#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$Child,
    [switch]$CaptureSelfTest,
    [switch]$PincOnly,
    [switch]$IfcOnly,
    [string]$CaptureDirectory,
    [string]$PackagedRuntimeRoot = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixtureParentOriginalAcl = $null
$fixtureParentPath = $null
if (!$IsWindows) { throw 'This runner requires Windows.' }
if ($PincOnly -and $CaptureSelfTest) { throw 'PincOnly cannot be combined with CaptureSelfTest.' }
if ($PincOnly -and $Configuration -ne 'Release') { throw 'PincOnly requires the inspected Release runtime.' }
if ($IfcOnly -and ($PincOnly -or $CaptureSelfTest -or $PackagedRuntimeRoot -or $Configuration -ne 'Release')) {
    throw 'IfcOnly requires the inspected Release runtime and cannot be combined with other fixture modes.'
}
if ($PackagedRuntimeRoot) {
    if ($CaptureSelfTest -or $Configuration -ne 'Release') { throw 'Packaged CAD checking requires Release and no capture self-test.' }
    if ($PackagedRuntimeRoot -match '["\x00-\x1f]') { throw 'Invalid packaged runtime path.' }
    $PackagedRuntimeRoot = (Resolve-Path -LiteralPath $PackagedRuntimeRoot).Path
    if (!(Test-Path -LiteralPath (Join-Path $PackagedRuntimeRoot 'vertex-import-worker.exe') -PathType Leaf)) {
        throw 'Packaged runtime must identify its bin directory.'
    }
}
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root ('build/windows-' + $Configuration.ToLowerInvariant())
$names = @('windows_import_worker_tests.exe', 'windows_import_worker_probe.exe',
           'assistance_workflow_tests.exe', 'assistance_ocr_isolation_tests.exe', 'dxf_desktop_workflow_tests.exe',
           'ifc_desktop_workflow_tests.exe', 'cad_library_worker_tests.exe', 'vertex-import-worker.exe')
if ($PincOnly) {
    $names = @('windows_import_worker_tests.exe', 'windows_import_worker_probe.exe',
               'pinc_project_desktop_tests.exe', 'vertex-import-worker.exe')
}
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
function Get-RuntimeEvidence([string]$RuntimeRoot) {
    $paths = @($RuntimeRoot)
    if ($PincOnly) { $paths += Join-Path (Split-Path -Parent $RuntimeRoot) 'plugins' }
    @(Get-ChildItem -LiteralPath $paths -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName; bytes = $_.Length;
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
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
function New-ImmutableDesktopFixtureRoot([string]$CaptureRoot) {
    $runtime = Join-Path $CaptureRoot 'immutable-desktop-runtime'
    $protectedRoot = $runtime
    if ($PincOnly) { $runtime = Join-Path $runtime 'bin' }
    $null = New-Item -ItemType Directory -Path $runtime -Force
    $fixtureNames = if ($PincOnly) { @('pinc_project_desktop_tests.exe', 'vertex-import-worker.exe', 'vertex-planegcs.dll') }
        else { @('assistance_ocr_isolation_tests.exe', 'dxf_desktop_workflow_tests.exe', 'ifc_desktop_workflow_tests.exe', 'cad_library_worker_tests.exe',
                 'vertex-import-worker.exe', 'vertex-planegcs.dll') }
    foreach ($name in $fixtureNames) {
        Copy-Item -LiteralPath (Join-Path $build $name) -Destination $runtime
    }
    # The CAD bridge loads its pinned interpreter lazily from this exact sibling
    # directory. Include native extensions before assigning immutable ACLs.
    if (!$PincOnly) { Copy-Item -LiteralPath (Join-Path $build 'cad-runtime') -Destination $runtime -Recurse }
    if (!$PincOnly) {
        # OCR runs the real sibling worker against these pinned resources in
        # the same immutable root. It never searches the source checkout.
        foreach ($relative in @('assets/assistance/ocr-engine-v1.json', 'assets/assistance/ocr/eng.traineddata',
                               'assets/assistance/ocr/LICENSE')) {
            $destination = Join-Path $runtime $relative
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
            Copy-Item -LiteralPath (Join-Path $build $relative) -Destination $destination
        }
        $fontDirectory = Join-Path $runtime 'assets/fonts'
        $null = New-Item -ItemType Directory -Path $fontDirectory -Force
        Copy-Item -LiteralPath (Join-Path $root 'assets/fonts/Inter.ttf') -Destination $fontDirectory
    }
    $qtBin = Join-Path $root '.deps/qt/6.8.3/msvc2022_64/bin'
    $qtSuffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
    foreach ($name in @("Qt6Core$qtSuffix.dll", "Qt6Gui$qtSuffix.dll",
                         "Qt6Network$qtSuffix.dll", "Qt6Pdf$qtSuffix.dll")) {
        Copy-Item -LiteralPath (Join-Path $qtBin $name) -Destination $runtime
    }
    # Native IFC reconstruction now uses the solid kernel inside the worker.
    # Stage its inspected DLL closure before making the fixture immutable.
    $entryPoints = @((Join-Path $build 'vertex-import-worker.exe'))
    if ($PincOnly) {
        $qtPrefix = Split-Path -Parent $qtBin
        foreach ($module in @('Widgets', 'PrintSupport', 'Svg', 'OpenGL', 'OpenGLWidgets')) {
            $entryPoints += Join-Path $qtBin "Qt6$module.dll"
        }
        $entryPoints += Join-Path $build 'pinc_project_desktop_tests.exe'
        foreach ($plugin in @('platforms/qoffscreen.dll', 'platforms/qwindows.dll',
                             'styles/qmodernwindowsstyle.dll', 'imageformats/qgif.dll',
                             'imageformats/qico.dll', 'imageformats/qjpeg.dll', 'imageformats/qsvg.dll')) {
            $source = Join-Path $qtPrefix "plugins/$plugin"
            $destination = Join-Path $protectedRoot "plugins/$plugin"
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
            Copy-Item -LiteralPath $source -Destination $destination
            $entryPoints += $source
        }
    }
    & (Join-Path $PSScriptRoot 'inspect-runtime.ps1') -EntryPoints $entryPoints | Out-Null
    $inventory = Get-Content -LiteralPath (Join-Path $root 'artifacts/runtime/release-imports.json') -Raw | ConvertFrom-Json
    foreach ($module in $inventory.modules) {
        if ([IO.Path]::GetExtension($module.path) -ne '.dll') { continue }
        Copy-Item -LiteralPath $module.path -Destination $runtime -Force
    }
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
                   [Security.AccessControl.InheritanceFlags]::ObjectInherit
    $none = [Security.AccessControl.PropagationFlags]::None
    $allow = [Security.AccessControl.AccessControlType]::Allow
    $read = [Security.AccessControl.FileSystemRights]::ReadAndExecute -bor
            [Security.AccessControl.FileSystemRights]::Synchronize
    $full = [Security.AccessControl.FileSystemRights]::FullControl
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent().User, $read, $inheritance, $none, $allow))
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Security.Principal;
public static class VertexAppContainerFixture {
    [DllImport("userenv.dll", CharSet=CharSet.Unicode)]
    static extern int CreateAppContainerProfile(string name, string displayName, string description,
        IntPtr capabilities, uint capabilityCount, out IntPtr sid);
    [DllImport("userenv.dll", CharSet=CharSet.Unicode)]
    static extern int DeriveAppContainerSidFromAppContainerName(string name, out IntPtr sid);
    [DllImport("advapi32.dll")]
    static extern IntPtr FreeSid(IntPtr sid);
    public static SecurityIdentifier Sid() {
        IntPtr sid;
        int result = CreateAppContainerProfile("Vertex.ImportWorker", "Vertex import worker",
            "Local offline import isolation", IntPtr.Zero, 0, out sid);
        if (result == unchecked((int)0x800700B7))
            result = DeriveAppContainerSidFromAppContainerName("Vertex.ImportWorker", out sid);
        if (result < 0 || sid == IntPtr.Zero) Marshal.ThrowExceptionForHR(result);
        try { return new SecurityIdentifier(sid); }
        finally { FreeSid(sid); }
    }
}
'@
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [VertexAppContainerFixture]::Sid(), $read, $inheritance, $none, $allow))
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), $full, $inheritance, $none, $allow))
    }
    Set-Acl -LiteralPath $protectedRoot -AclObject $acl
    # The containing capture directory otherwise grants DELETE on the runtime
    # through FILE_DELETE_CHILD. This fixture-owned parent stays writable for
    # stdout/stderr capture; remove only the alternate child-deletion route
    # during the tests and restore its original access ACL in the child finally.
    $existing = Get-Acl -LiteralPath $CaptureRoot
    $script:fixtureParentOriginalAcl = [Security.AccessControl.DirectorySecurity]::new()
    $script:fixtureParentOriginalAcl.SetSecurityDescriptorSddlForm(
        $existing.Sddl, [Security.AccessControl.AccessControlSections]::Access)
    $script:fixtureParentPath = $CaptureRoot
    $parentAcl = [Security.AccessControl.DirectorySecurity]::new()
    $parentAcl.SetSecurityDescriptorSddlForm($existing.Sddl, [Security.AccessControl.AccessControlSections]::Access)
    $parentAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent().User, 'DeleteSubdirectoriesAndFiles', 'Deny'))
    [IO.FileSystemAclExtensions]::SetAccessControl([IO.DirectoryInfo]::new($CaptureRoot), $parentAcl)
    return $runtime
}
if ($Child) {
    if (!(Test-Path -LiteralPath $CaptureDirectory -PathType Container)) { throw 'Missing capture directory.' }
    $report = [ordered]@{ configuration = $Configuration; started_utc = [DateTime]::UtcNow.ToString('o');
        host_pid = $PID; user = [Security.Principal.WindowsIdentity]::GetCurrent().Name;
        host_in_job = $null; binaries_before = @(); tests = @(); error = $null;
        capture_self_test = [bool]$CaptureSelfTest; capture_limit_bytes_per_stream = $captureLimitBytes;
        pinc_only = [bool]$PincOnly;
        ifc_only = [bool]$IfcOnly;
        packaged_runtime_root = $PackagedRuntimeRoot;
        qualification_boundary = 'Development-host fixture evidence only; packaged CAD mode uses the supplied immutable installed bin directory; no clean-machine, complete compatibility or production qualification.' }
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
        if (!$CaptureSelfTest -and !$PincOnly) {
            & (Join-Path $root '.deps/cad-runtime/3.13.15/python.exe') -I -B (
                Join-Path $root 'tests/generate_cad_library_worker_fixtures.py') (
                Join-Path $CaptureDirectory 'cad-fixtures')
            if ($LASTEXITCODE -ne 0) { throw 'CAD library fixture generation failed.' }
        }
        $desktopFixtureRoot = if ($PackagedRuntimeRoot -and $PincOnly) { $PackagedRuntimeRoot }
            elseif ($PackagedRuntimeRoot) { $build } elseif ($CaptureSelfTest) { $null } else {
            New-ImmutableDesktopFixtureRoot $CaptureDirectory
        }
        if ($PincOnly) { $report.runtime_before = Get-RuntimeEvidence $desktopFixtureRoot }
        if (!$PincOnly -and !$CaptureSelfTest -and !$PackagedRuntimeRoot) {
            $report.runtime_before = Get-RuntimeEvidence $desktopFixtureRoot
        }
        $cases = if ($PincOnly) { @('windows_import_worker_tests.exe', 'pinc_project_desktop_tests.exe') }
                 elseif ($IfcOnly) { @('ifc_desktop_workflow_tests.exe') }
                 elseif ($PackagedRuntimeRoot) { @('cad_library_worker_tests.exe') }
                 elseif ($CaptureSelfTest) { @('capture-stdout-overflow', 'capture-stderr-overflow', 'capture-at-limit',
                'capture-empty', 'capture-open-failure') }
                 else { @('windows_import_worker_tests.exe', 'assistance_workflow_tests.exe', 'assistance_ocr_isolation_tests.exe',
                          'dxf_desktop_workflow_tests.exe', 'ifc_desktop_workflow_tests.exe', 'cad_library_worker_tests.exe') }
        foreach ($name in $cases) {
            $executable = if ($CaptureSelfTest) { (Get-Process -Id $PID).Path }
                elseif ($name -in @('pinc_project_desktop_tests.exe', 'assistance_ocr_isolation_tests.exe', 'dxf_desktop_workflow_tests.exe', 'ifc_desktop_workflow_tests.exe', 'cad_library_worker_tests.exe')) {
                    Join-Path $desktopFixtureRoot $name
                } else { Join-Path $build $name }
            $info = [Diagnostics.ProcessStartInfo]::new($executable)
            $info.UseShellExecute = $false
            $info.CreateNoWindow = $true
            $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
            $info.WorkingDirectory = $build
            $info.RedirectStandardOutput = $true
            $info.RedirectStandardError = $true
            $info.Environment['QT_QPA_PLATFORM'] = 'offscreen'
            $info.Environment['VERTEX_TEST_CAPTURE_DIR'] = $CaptureDirectory
            $info.Environment.Remove('VERTEX_TEST_RUNTIME_ROOT') | Out-Null
            if ($PackagedRuntimeRoot) { $info.Environment['VERTEX_TEST_RUNTIME_ROOT'] = $PackagedRuntimeRoot }
            $info.Environment['QT_PLUGIN_PATH'] = Join-Path $root '.deps/qt/6.8.3/msvc2022_64/plugins'
            $native = if ($Configuration -eq 'Debug') { 'debug/bin' } else { 'bin' }
            $info.Environment['PATH'] = (Join-Path $root '.deps/qt/6.8.3/msvc2022_64/bin') + ';' +
                (Join-Path $root ".deps/native/x64-windows/$native") + ';' + $env:PATH
            if ($PincOnly -and $name -eq 'pinc_project_desktop_tests.exe') {
                $info.WorkingDirectory = $desktopFixtureRoot
                $info.Environment['QT_PLUGIN_PATH'] = Join-Path (Split-Path -Parent $desktopFixtureRoot) 'plugins'
                $info.Environment['PATH'] = $desktopFixtureRoot + ';' + [Environment]::SystemDirectory
                $info.Environment['VERTEX_TEST_CAPTURE_DIR'] = Join-Path $CaptureDirectory 'pinc-desktop-captures'
            }
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
        if (!$PincOnly -and !$CaptureSelfTest -and !$PackagedRuntimeRoot) {
            $report.runtime_after = Get-RuntimeEvidence $desktopFixtureRoot
            if (($report.runtime_before | ConvertTo-Json -Depth 5 -Compress) -cne
                ($report.runtime_after | ConvertTo-Json -Depth 5 -Compress)) { throw 'Frozen worker runtime provenance changed during the run.' }
        }
        if ($PincOnly) {
            $report.runtime_after = Get-RuntimeEvidence $desktopFixtureRoot
            if (($report.runtime_before | ConvertTo-Json -Depth 5 -Compress) -cne
                ($report.runtime_after | ConvertTo-Json -Depth 5 -Compress)) { throw 'Pinc runtime provenance changed during the run.' }
            foreach ($test in $report.tests) {
                $expected = if ($test.name -eq 'windows_import_worker_tests.exe') { 'windows import worker tests passed' }
                    else { 'Pinc project desktop tests passed' }
                $output = Get-Content -LiteralPath $test.stdout -Raw
                if (!$test.termination_confirmed -or $test.exit_code -ne 0 -or $test.timed_out -or
                    $test.capture_failure -or $output -notmatch [regex]::Escape($expected) -or
                    $output -match '(?i)\bskip(?:ped)?\b') { throw "Pinc fixture failed or produced incomplete evidence: $($test.name)" }
            }
        }
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
    finally {
        if ($null -ne $fixtureParentOriginalAcl) {
            try {
                [IO.FileSystemAclExtensions]::SetAccessControl(
                    [IO.DirectoryInfo]::new($fixtureParentPath), $fixtureParentOriginalAcl)
            } catch {
                $report.error = "$($report.error); fixture parent ACL restoration failed: $($_.Exception.Message)"
            }
        }
    }
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $CaptureDirectory 'result.json') -Encoding utf8
    if ($report.error -or (!$CaptureSelfTest -and @($report.tests | Where-Object {
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
    if ($PincOnly) { $command += ' -PincOnly' }
    if ($IfcOnly) { $command += ' -IfcOnly' }
    if ($PackagedRuntimeRoot) { $command += ' -PackagedRuntimeRoot "' + $PackagedRuntimeRoot + '"' }
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
$expectedCount = if ($PincOnly) { 2 } elseif ($IfcOnly -or $PackagedRuntimeRoot) { 1 } elseif ($CaptureSelfTest) { 5 } else { 6 }
if (!$capture.termination_confirmed -or $capture.host_exit_code -ne 0 -or
    $result.error -or @($result.tests).Count -ne $expectedCount -or (!$CaptureSelfTest -and @($result.tests | Where-Object {
        $_.exit_code -ne 0 -or $_.timed_out -or $_.capture_failure }).Count)) {
    throw "Independent fixtures did not pass; inspect $CaptureDirectory"
}
exit 0
