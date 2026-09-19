$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $projectRoot 'scripts/native-process-guard.ps1')

function Assert-NativeGuard([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw "Native process guard test failed: $Message" }
}

function Start-NativeGuardChild([string]$Command, [string]$OutputPath, [string]$ErrorPath) {
    $shellPath = (Get-Process -Id $PID).Path
    if ([string]::IsNullOrWhiteSpace($shellPath)) { $shellPath = (Get-Command pwsh).Source }
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Command))
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $shellPath
    $startInfo.Arguments = "-NoLogo -NoProfile -NonInteractive -EncodedCommand $encodedCommand"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (!$process.Start()) { throw 'could not start hidden child' }
        $nativeGuardStreams[$process.Id] = @{
            stdout = $process.StandardOutput.ReadToEndAsync()
            stderr = $process.StandardError.ReadToEndAsync()
        }
        return $process
    }
    catch {
        $process.Dispose()
        throw
    }
}

function Save-NativeGuardOutput([System.Diagnostics.Process]$Process, [string]$OutputPath, [string]$ErrorPath) {
    $streams = $nativeGuardStreams[$Process.Id]
    if ($null -eq $streams) { throw "No retained output streams for PID $($Process.Id)." }
    if (!$Process.HasExited) { throw "Cannot collect completed output from running PID $($Process.Id)." }
    if (!$streams.stdout.Wait(2000) -or !$streams.stderr.Wait(2000)) {
        throw "Output collection did not complete within its bounded wait for PID $($Process.Id)."
    }
    [IO.File]::WriteAllText($OutputPath, [string]$streams.stdout.Result)
    [IO.File]::WriteAllText($ErrorPath, [string]$streams.stderr.Result)
}

function Get-NativeGuardProcessOrNull([int]$ProcessId) {
    try { return Get-Process -Id $ProcessId -ErrorAction Stop }
    catch {
        if ($_.Exception.Message -match 'Cannot find a process|No process|not found') { return $null }
        throw
    }
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("native-process-guard-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temporary | Out-Null
$children = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$nativeGuardStreams = @{}
$createdFiles = [System.Collections.Generic.List[string]]::new()
$cleanupIssues = [System.Collections.Generic.List[string]]::new()
$testSucceeded = $false
try {
    $duplicateEnvironment = [System.Collections.Specialized.OrderedDictionary]::new(
        [System.StringComparer]::Ordinal)
    $duplicateEnvironment.Add('Path', 'stale-process-path')
    $duplicateEnvironment.Add('PATH', 'conflicting-process-path')
    $duplicateEnvironment.Add('VERTEX_ENVIRONMENT_SENTINEL', 'preserved')
    $environmentProbe = [System.Diagnostics.ProcessStartInfo]::new()
    Set-NativeProcessEnvironment -StartInfo $environmentProbe `
        -SourceEnvironment $duplicateEnvironment `
        -PathPrefixes @('qt-bin', 'native-bin') -BasePath 'system-bin;user-bin'
    $pathKeys = @($environmentProbe.Environment.Keys | Where-Object { $_ -ieq 'Path' })
    Assert-NativeGuard ($pathKeys.Count -eq 1 -and $pathKeys[0] -ceq 'Path') `
        'child environment must contain one canonical Path key'
    Assert-NativeGuard ($environmentProbe.Environment['Path'] -eq
        'qt-bin;native-bin;system-bin;user-bin') `
        'child Path must use explicit runtime prefixes and the stable base path'
    Assert-NativeGuard ($environmentProbe.Environment['VERTEX_ENVIRONMENT_SENTINEL'] -eq 'preserved') `
        'non-Path process environment entries must be preserved'

    $timeoutStdout = Join-Path $temporary 'timeout.stdout.txt'
    $timeoutStderr = Join-Path $temporary 'timeout.stderr.txt'
    [void]$createdFiles.Add($timeoutStdout)
    [void]$createdFiles.Add($timeoutStderr)
    $timeoutChild = Start-NativeGuardChild `
        'Write-Output ''guard stdout''; [Console]::Error.WriteLine(''guard stderr''); Start-Sleep -Seconds 30' `
        $timeoutStdout $timeoutStderr
    [void]$children.Add($timeoutChild)
    $timeoutResult = Wait-NativeProcess -Process $timeoutChild -Description 'guard sleeper' `
        -DeadlineMilliseconds 1500 -TerminationWaitMilliseconds 2000
    Save-NativeGuardOutput $timeoutChild $timeoutStdout $timeoutStderr
    Assert-NativeGuard ($timeoutResult.TimedOut) 'deadline expiry must be observed as an error'
    Assert-NativeGuard ($timeoutResult.State -eq 'Killed') 'deadline child must be killed and confirmed exited'
    Assert-NativeGuard ($timeoutResult.TerminationConfirmed) 'deadline child termination must be confirmed'
    Assert-NativeGuard ($timeoutChild.HasExited) 'deadline child handle must report exit'
    $timeoutProcess = Get-NativeGuardProcessOrNull $timeoutChild.Id
    Assert-NativeGuard ($null -eq $timeoutProcess) 'deadline child PID must be gone'
    Assert-NativeGuard (([string](Get-Content -Raw -LiteralPath $timeoutStdout)) -match 'guard stdout') `
        'deadline stdout must be retained'
    Assert-NativeGuard (([string](Get-Content -Raw -LiteralPath $timeoutStderr)) -match 'guard stderr') `
        'deadline stderr must be retained'

    $normalStdout = Join-Path $temporary 'normal.stdout.txt'
    $normalStderr = Join-Path $temporary 'normal.stderr.txt'
    [void]$createdFiles.Add($normalStdout)
    [void]$createdFiles.Add($normalStderr)
    $normalChild = Start-NativeGuardChild `
        'Write-Output ''normal stdout''; [Console]::Error.WriteLine(''normal stderr'')' `
        $normalStdout $normalStderr
    [void]$children.Add($normalChild)
    $normalResult = Wait-NativeProcess -Process $normalChild -Description 'normal child' `
        -DeadlineMilliseconds 5000
    Save-NativeGuardOutput $normalChild $normalStdout $normalStderr
    Assert-NativeGuard ($normalResult.State -eq 'Exited') 'normal child must exit normally'
    Assert-NativeGuard ($normalResult.ExitCode -eq 0) 'normal child exit code must be zero'

    $nonzeroStdout = Join-Path $temporary 'nonzero.stdout.txt'
    $nonzeroStderr = Join-Path $temporary 'nonzero.stderr.txt'
    [void]$createdFiles.Add($nonzeroStdout)
    [void]$createdFiles.Add($nonzeroStderr)
    $nonzeroChild = Start-NativeGuardChild `
        'Write-Output ''nonzero stdout''; [Console]::Error.WriteLine(''nonzero stderr''); exit 7' `
        $nonzeroStdout $nonzeroStderr
    [void]$children.Add($nonzeroChild)
    $nonzeroResult = Wait-NativeProcess -Process $nonzeroChild -Description 'nonzero child' `
        -DeadlineMilliseconds 5000
    Save-NativeGuardOutput $nonzeroChild $nonzeroStdout $nonzeroStderr
    Assert-NativeGuard ($nonzeroResult.State -eq 'Exited') 'nonzero child must still be observed as exited'
    Assert-NativeGuard ($nonzeroResult.ExitCode -eq 7) 'nonzero child exit code must be preserved'

    $alreadyStdout = Join-Path $temporary 'already.stdout.txt'
    $alreadyStderr = Join-Path $temporary 'already.stderr.txt'
    [void]$createdFiles.Add($alreadyStdout)
    [void]$createdFiles.Add($alreadyStderr)
    $alreadyChild = Start-NativeGuardChild 'exit 3' $alreadyStdout $alreadyStderr
    [void]$children.Add($alreadyChild)
    Assert-NativeGuard ($alreadyChild.WaitForExit(5000)) 'already-exited child must finish before guard call'
    $alreadyResult = Wait-NativeProcess -Process $alreadyChild -Description 'already-exited child' `
        -DeadlineMilliseconds 5000
    Assert-NativeGuard ($alreadyResult.State -eq 'Exited') 'already-exited child must remain a normal exit'
    Assert-NativeGuard ($alreadyResult.ExitCode -eq 3) 'already-exited child code must be preserved'

    $disposedStdout = Join-Path $temporary 'disposed.stdout.txt'
    $disposedStderr = Join-Path $temporary 'disposed.stderr.txt'
    [void]$createdFiles.Add($disposedStdout)
    [void]$createdFiles.Add($disposedStderr)
    $disposedChild = Start-NativeGuardChild 'exit 0' $disposedStdout $disposedStderr
    [void]$children.Add($disposedChild)
    Assert-NativeGuard ($disposedChild.WaitForExit(5000)) 'disposed-handle child must finish before disposal'
    $disposedChild.Dispose()
    [void]$children.Remove($disposedChild)
    $disposedResult = Wait-NativeProcess -Process $disposedChild -Description 'disposed child' `
        -DeadlineMilliseconds 5000
    Assert-NativeGuard (@('HandleFailed', 'WaitFailed') -contains $disposedResult.State) `
        'disposed handle must fail explicitly'
    Assert-NativeGuard (![string]::IsNullOrWhiteSpace($disposedResult.Error)) 'disposed handle failure must retain its reason'
    Assert-NativeGuard (!$disposedResult.TimedOut) 'disposed handle must not be labeled as a deadline'
    Assert-NativeGuard (!$disposedResult.TerminationConfirmed) 'disposed handle must not claim termination confirmation'

    $testSucceeded = $true
    Write-Output 'Native process guard checks passed.'
}
finally {
    foreach ($child in $children) {
        $childId = 0
        try { $childId = $child.Id }
        catch {
            [void]$cleanupIssues.Add("process cleanup handle could not be read: $($_.Exception.Message)")
            continue
        }
        try {
            $hasExited = $false
            $initialStateReadFailed = $false
            try { $hasExited = $child.HasExited }
            catch {
                $initialStateReadFailed = $true
                [void]$cleanupIssues.Add("PID $childId state read failed: $($_.Exception.Message)")
            }
            if (!$hasExited) {
                $killError = $null
                try { $child.Kill() }
                catch { $killError = $_.Exception }
                $confirmed = $false
                if ($null -eq $killError) {
                    try { $confirmed = $child.WaitForExit(2000) }
                    catch { [void]$cleanupIssues.Add("PID $childId bounded cleanup wait failed: $($_.Exception.Message)") }
                }
                $hasExitedAfterCleanup = $false
                $finalStateReadFailed = $false
                try { $hasExitedAfterCleanup = $child.HasExited }
                catch {
                    $finalStateReadFailed = $true
                    [void]$cleanupIssues.Add("PID $childId final state read failed: $($_.Exception.Message)")
                }
                if (!$confirmed -and !$hasExitedAfterCleanup) {
                    if ($initialStateReadFailed -or $finalStateReadFailed) {
                        [void]$cleanupIssues.Add("could not confirm PID $childId stopped")
                    }
                    elseif ($killError) {
                        [void]$cleanupIssues.Add("PID $childId remains running; retained-handle Kill failed: $($killError.Message)")
                    }
                    else { [void]$cleanupIssues.Add("PID $childId remains running after bounded cleanup confirmation") }
                }
            }
        }
        catch { [void]$cleanupIssues.Add("PID $childId cleanup failed: $($_.Exception.Message)") }
        finally {
            try { $child.Dispose() }
            catch { [void]$cleanupIssues.Add("PID $childId handle disposal failed: $($_.Exception.Message)") }
        }
    }

    if (!$testSucceeded) {
        [void]$cleanupIssues.Add("test evidence retained at $temporary")
    }
    elseif ($cleanupIssues.Count -eq 0) {
        foreach ($path in $createdFiles) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                try { Remove-Item -LiteralPath $path -Force -ErrorAction Stop }
                catch { [void]$cleanupIssues.Add("file cleanup failed for ${path}: $($_.Exception.Message)") }
            }
        }
        if (Test-Path -LiteralPath $temporary -PathType Container) {
            try {
                $remaining = @(Get-ChildItem -LiteralPath $temporary -Force -ErrorAction Stop)
                if ($remaining.Count -eq 0) {
                    Remove-Item -LiteralPath $temporary -Force -ErrorAction Stop
                }
                else {
                    [void]$cleanupIssues.Add("owned evidence directory is not empty; retained at $temporary")
                }
            }
            catch { [void]$cleanupIssues.Add("evidence directory cleanup failed for ${temporary}: $($_.Exception.Message)") }
        }
    }
    else {
        [void]$cleanupIssues.Add("test evidence retained at $temporary")
    }

    foreach ($issue in $cleanupIssues) { [Console]::Error.WriteLine("Native process guard cleanup: $issue") }
    if ($testSucceeded -and $cleanupIssues.Count -gt 0) {
        throw 'Native process guard cleanup failed; see diagnostics above.'
    }
}
