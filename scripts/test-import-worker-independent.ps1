#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$Child,
    [switch]$CaptureSelfTest,
    [string]$CaptureDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$IsWindows) { throw 'This runner requires Windows.' }
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root ('build/windows-' + $Configuration.ToLowerInvariant())
$names = @('windows_import_worker_tests.exe', 'windows_import_worker_probe.exe',
           'assistance_workflow_tests.exe', 'property-studio-import-worker.exe')
$captureLimitBytes = 1MB
function Get-BinaryEvidence {
    @($names | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $build $_)
        [ordered]@{ path = $file.FullName; bytes = $file.Length;
            modified_utc = $file.LastWriteTimeUtc.ToString('o');
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    })
}
function Get-OptionalFileHash([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
    return $null
}
function Get-CaptureFailure($Stdout, $Stderr) {
    foreach ($stream in @(@('stdout', $Stdout), @('stderr', $Stderr))) {
        if ($stream[1].IsFaulted) { return ($stream[0] + '_capture_failed') }
        if ($stream[1].IsCompletedSuccessfully) {
            if ($stream[1].Result.Failed) { return ($stream[0] + '_capture_failed') }
            if ($stream[1].Result.Overflow) { return ($stream[0] + '_limit_exceeded') }
        }
    }
    return $null
}
if ($Child) {
    if (!(Test-Path -LiteralPath $CaptureDirectory -PathType Container)) { throw 'Missing capture directory.' }
    $report = [ordered]@{ configuration = $Configuration; started_utc = [DateTime]::UtcNow.ToString('o');
        host_pid = $PID; user = [Security.Principal.WindowsIdentity]::GetCurrent().Name;
        host_in_job = $null; binaries_before = @(); tests = @(); error = $null;
        capture_self_test = [bool]$CaptureSelfTest; capture_limit_bytes_per_stream = $captureLimitBytes;
        qualification_boundary = 'Development-host fixture evidence only; assistance permits synthetic fallback; no production or installed-runtime qualification.' }
    try {
        Add-Type @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
public static class IndependentHostJob {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
}
public static class BoundedWorkerCapture {
    public sealed class Result { public int Bytes; public bool Overflow; public bool Failed; }
    public static async Task<Result> CopyAsync(Stream input, string path, int limit) {
        var result = new Result();
        try {
            using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read)) {
                var buffer = new byte[8192];
                for (;;) {
                    int received = await input.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false);
                    if (received == 0) break;
                    int accepted = Math.Min(received, limit - result.Bytes);
                    await output.WriteAsync(buffer, 0, accepted).ConfigureAwait(false);
                    result.Bytes += accepted;
                    if (accepted != received) { result.Overflow = true; break; }
                }
            }
        } catch { result.Failed = true; }
        return result;
    }
}
'@
        $inJob = $false
        if (![IndependentHostJob]::IsProcessInJob([Diagnostics.Process]::GetCurrentProcess().Handle, [IntPtr]::Zero, [ref]$inJob)) {
            throw "IsProcessInJob failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        $report.host_in_job = $inJob
        $report.binaries_before = Get-BinaryEvidence
        $cases = if ($CaptureSelfTest) { @('capture-stdout-overflow', 'capture-stderr-overflow', 'capture-at-limit',
                'capture-empty', 'capture-open-failure') }
                 else { @('windows_import_worker_tests.exe', 'assistance_workflow_tests.exe') }
        foreach ($name in $cases) {
            $executable = if ($CaptureSelfTest) { (Get-Process -Id $PID).Path } else { Join-Path $build $name }
            $info = [Diagnostics.ProcessStartInfo]::new($executable)
            $info.UseShellExecute = $false
            $info.CreateNoWindow = $true
            $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
            $info.WorkingDirectory = $build
            $info.RedirectStandardOutput = $true
            $info.RedirectStandardError = $true
            $info.Environment['QT_QPA_PLATFORM'] = 'offscreen'
            $info.Environment['QT_PLUGIN_PATH'] = Join-Path $root '.deps/qt/6.8.3/msvc2022_64/plugins'
            $native = if ($Configuration -eq 'Debug') { 'debug/bin' } else { 'bin' }
            $info.Environment['PATH'] = (Join-Path $root '.deps/qt/6.8.3/msvc2022_64/bin') + ';' +
                (Join-Path $root ".deps/native/x64-windows/$native") + ';' + $env:PATH
            if ($name -eq 'windows_import_worker_tests.exe') {
                $info.ArgumentList.Add((Join-Path $build 'windows_import_worker_probe.exe'))
            }
            if ($CaptureSelfTest) {
                $body = switch ($name) {
                    'capture-stdout-overflow' { '[Console]::Out.Write(("x" * 1048577)); [Console]::Out.Flush(); Start-Sleep -Seconds 10' }
                    'capture-stderr-overflow' { '[Console]::Error.Write(("x" * 1048577)); [Console]::Error.Flush(); Start-Sleep -Seconds 10' }
                    'capture-at-limit' { '[Console]::Out.Write(("x" * 1048576)); [Console]::Error.Write(("x" * 1048576))' }
                    'capture-empty' { 'exit 0' }
                    'capture-open-failure' { 'Start-Sleep -Seconds 10' }
                }
                foreach ($argument in @('-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand',
                    [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body)))) { $info.ArgumentList.Add($argument) }
            }
            $process = [Diagnostics.Process]::new()
            $process.StartInfo = $info
            $started = $false
            try {
                if (!$process.Start()) { throw "Could not start $name" }
                $started = $true
                $stdoutPath = Join-Path $CaptureDirectory "$name.stdout.txt"
                $stderrPath = Join-Path $CaptureDirectory "$name.stderr.txt"
                if ($CaptureSelfTest -and $name -eq 'capture-open-failure') {
                    $null = New-Item -ItemType Directory -Path $stdoutPath
                }
                $stdout = [BoundedWorkerCapture]::CopyAsync($process.StandardOutput.BaseStream, $stdoutPath, $captureLimitBytes)
                $stderr = [BoundedWorkerCapture]::CopyAsync($process.StandardError.BaseStream, $stderrPath, $captureLimitBytes)
                $watch = [Diagnostics.Stopwatch]::StartNew()
                $timedOut = $false
                $captureFailure = $null
                for (;;) {
                    $captureFailure = Get-CaptureFailure $stdout $stderr
                    if ($captureFailure) { break }
                    if ($process.WaitForExit(25)) { break }
                    if ($watch.ElapsedMilliseconds -ge 60000) { $timedOut = $true; break }
                }
                if (($captureFailure -or $timedOut) -and !$process.HasExited) { $process.Kill($true) }
                if (!$process.WaitForExit(5000)) { throw "Could not confirm termination of $name" }
                if (!$stdout.Wait(5000) -or !$stderr.Wait(5000)) { throw "Output capture did not close for $name" }
                $captureFailure = Get-CaptureFailure $stdout $stderr
                $report.tests += [ordered]@{ name = $name; pid = $process.Id; exit_code = $process.ExitCode;
                    timed_out = $timedOut; stdout = $stdoutPath; stderr = $stderrPath;
                    capture_failure = $captureFailure; termination_confirmed = $process.HasExited;
                    captured_stdout_bytes = $stdout.Result.Bytes; captured_stderr_bytes = $stderr.Result.Bytes;
                    elapsed_ms = $watch.ElapsedMilliseconds;
                    stdout_sha256 = Get-OptionalFileHash $stdoutPath;
                    stderr_sha256 = Get-OptionalFileHash $stderrPath }
            } finally {
                if ($started -and !$process.HasExited) { $process.Kill($true); $null = $process.WaitForExit(5000) }
                $process.Dispose()
            }
        }
        $report.binaries_after = Get-BinaryEvidence
        if (($report.binaries_before | ConvertTo-Json -Depth 5 -Compress) -cne
            ($report.binaries_after | ConvertTo-Json -Depth 5 -Compress)) { throw 'Binary provenance changed during the run.' }
        if ($CaptureSelfTest) {
            foreach ($test in $report.tests) {
                $expected = switch ($test.name) {
                    'capture-stdout-overflow' { 'stdout_limit_exceeded' }
                    'capture-stderr-overflow' { 'stderr_limit_exceeded' }
                    'capture-open-failure' { 'stdout_capture_failed' }
                    default { $null }
                }
                $expectedStdoutBytes = if ($test.name -in @('capture-stdout-overflow', 'capture-at-limit')) { $captureLimitBytes } else { 0 }
                $expectedStderrBytes = if ($test.name -in @('capture-stderr-overflow', 'capture-at-limit')) { $captureLimitBytes } else { 0 }
                $stdoutLength = if (Test-Path -LiteralPath $test.stdout -PathType Leaf) {
                    (Get-Item -LiteralPath $test.stdout).Length
                } else { $null }
                $stderrLength = if (Test-Path -LiteralPath $test.stderr -PathType Leaf) {
                    (Get-Item -LiteralPath $test.stderr).Length
                } else { $null }
                if ($test.capture_failure -ne $expected -or $test.timed_out -or
                    !$test.termination_confirmed -or $test.captured_stdout_bytes -ne $expectedStdoutBytes -or
                    $test.captured_stderr_bytes -ne $expectedStderrBytes -or
                    (!$expected -and $test.exit_code -ne 0) -or ($expected -and $test.exit_code -eq 0) -or
                    ($test.name -eq 'capture-open-failure' -and ($null -ne $test.stdout_sha256 -or
                        $null -ne $stdoutLength)) -or
                    ($test.name -ne 'capture-open-failure' -and $stdoutLength -ne $expectedStdoutBytes) -or
                    $stderrLength -ne $expectedStderrBytes) {
                    throw "Output capture self-test failed: $($test.name)"
                }
            }
        }
    } catch { $report.error = $_.Exception.Message }
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $CaptureDirectory 'result.json') -Encoding utf8
    if ($report.error -or $report.host_in_job -or (!$CaptureSelfTest -and @($report.tests | Where-Object {
        $_.exit_code -ne 0 -or $_.timed_out -or $_.capture_failure }).Count)) { exit 1 }
    exit 0
}
if ($CaptureDirectory) { throw 'CaptureDirectory is reserved for the independent child.' }
$null = Get-BinaryEvidence
$runId = [guid]::NewGuid().ToString('N')
$CaptureDirectory = Join-Path $root "artifacts/import-worker-independent/$($Configuration.ToLowerInvariant())-$runId"
$null = New-Item -ItemType Directory -Path $CaptureDirectory
$pwsh = (Get-Process -Id $PID).Path
$capture = [ordered]@{ mechanism = 'Local Win32_Process.Create with current-user token and SW_HIDE';
    launch_return = $null; host_pid = $null; host_exit_code = $null; termination_confirmed = $false;
    result = $null; script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash;
    powershell_path = $pwsh; powershell_sha256 = (Get-FileHash -LiteralPath $pwsh).Hash; error = $null }
$independent = $null
try {
    # WMI's no-window creation flag is rejected on some hosts; SW_HIDE controls
    # the newly created console from launch, without touching any shared console.
    $startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
    $command = '"' + $pwsh + '" -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -File "' + $PSCommandPath +
        '" -Configuration ' + $Configuration + ' -Child -CaptureDirectory "' + $CaptureDirectory + '"'
    if ($CaptureSelfTest) { $command += ' -CaptureSelfTest' }
    $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine = $command; CurrentDirectory = $root; ProcessStartupInformation = $startup }
    $capture.launch_return = $created.ReturnValue
    if ($created.ReturnValue -ne 0) { throw "WMI creation failed: $($created.ReturnValue)" }
    $capture.host_pid = $created.ProcessId
    $independent = [Diagnostics.Process]::GetProcessById($created.ProcessId)
    $null = $independent.Handle # Retain the exact process handle through exit.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (!$independent.WaitForExit(250)) {
        if ($watch.Elapsed.TotalSeconds -gt 150) { throw 'Independent fixture exceeded its parent deadline.' }
    }
    $capture.host_exit_code = $independent.ExitCode
    $capture.termination_confirmed = $true
    $resultPath = Join-Path $CaptureDirectory 'result.json'
    if (!(Test-Path -LiteralPath $resultPath)) { throw 'Independent fixture produced no result.' }
    $capture.result = $resultPath
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if ($result.host_pid -ne $created.ProcessId -or $result.user -ne [Security.Principal.WindowsIdentity]::GetCurrent().Name) {
        throw 'Independent host identity did not match the launched current-user process.'
    }
} catch { $capture.error = $_.Exception.Message }
finally {
    if ($null -ne $independent) {
        try {
            if (!$independent.HasExited) { $independent.Kill($true) }
            $capture.termination_confirmed = $independent.WaitForExit(5000)
        } finally {
            $independent.Dispose()
        }
    }
    $capture | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $CaptureDirectory 'launcher.json') -Encoding utf8
}
Write-Output $CaptureDirectory
if ($capture.error) { throw $capture.error }
$expectedCount = if ($CaptureSelfTest) { 5 } else { 2 }
if (!$capture.termination_confirmed -or $capture.host_exit_code -ne 0 -or $result.host_in_job -ne $false -or
    $result.error -or @($result.tests).Count -ne $expectedCount -or (!$CaptureSelfTest -and @($result.tests | Where-Object {
        $_.exit_code -ne 0 -or $_.timed_out -or $_.capture_failure }).Count)) {
    throw "Independent fixtures did not pass; inspect $CaptureDirectory"
}
exit 0
