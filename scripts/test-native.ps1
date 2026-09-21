param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateCount(1, 3)][ValidateSet('1', '1.5', '2')][string[]]$Scales = @('1', '1.5', '2'),
    [ValidateCount(1, 5)][ValidateSet('geometry', 'forms', 'all', 'publication', 'gestures')][string[]]$Scenarios = @('geometry', 'forms', 'publication', 'gestures')
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$guardPath = Join-Path $PSScriptRoot 'native-process-guard.ps1'
if (!(Test-Path -LiteralPath $guardPath)) { throw 'Native process guard helper is missing.' }
. $guardPath
$configName = $Configuration.ToLowerInvariant()
$testPath = Join-Path $projectRoot "build\windows-$configName\native_view_tests.exe"
if (!(Test-Path -LiteralPath $testPath)) { throw 'Build the desktop test targets first.' }
$nativeSuffix = if ($Configuration -eq 'Debug') { 'debug\bin' } else { 'bin' }
$qtPrefix = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$outputDirectory = Join-Path $projectRoot 'artifacts\native-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$sourceEnvironment = [Environment]::GetEnvironmentVariables('Process')
$pathPrefixes = @((Join-Path $qtPrefix 'bin'),
                  (Join-Path $projectRoot ".deps\native\x64-windows\$nativeSuffix"))
foreach ($scale in $Scales) {
    foreach ($scenario in $Scenarios) {
            $captureDirectory = Join-Path $outputDirectory "captures\$configName-$scale-$scenario"
            New-Item -ItemType Directory -Force -Path $captureDirectory | Out-Null
            $stdout = Join-Path $outputDirectory "$configName-$scale-$scenario.stdout.txt"
            $stderr = Join-Path $outputDirectory "$configName-$scale-$scenario.stderr.txt"
            # Geometry captures at high DPI and the Debug publication fixture
            # perform many real OCCT redraws. This functional watchdog is
            # separate from the production performance qualification targets.
            $deadlineMilliseconds = if ($scenario -in @('geometry', 'all') -or
                ($Configuration -eq 'Debug' -and $scenario -eq 'publication')) { 60000 } else { 15000 }
            $deadlineSeconds = $deadlineMilliseconds / 1000
            $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
            $startInfo = [System.Diagnostics.ProcessStartInfo]::new($testPath)
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            Set-NativeProcessEnvironment -StartInfo $startInfo `
                -SourceEnvironment $sourceEnvironment -PathPrefixes $pathPrefixes
            $startInfo.Environment['QT_PLUGIN_PATH'] = Join-Path $qtPrefix 'plugins'
            $startInfo.Environment['QT_QPA_PLATFORM'] = 'windows'
            $startInfo.Environment['QT_ENABLE_HIGHDPI_SCALING'] = '0'
            $startInfo.Environment['QT_SCREEN_SCALE_FACTORS'] = '1'
            $startInfo.Environment['QT_SCALE_FACTOR'] = $scale
            $startInfo.Environment['SKETCH_TEST_ARTIFACT_DIR'] = $captureDirectory
            foreach ($argument in @('--scenario', $scenario, '--expected-dpr', $scale)) {
                $startInfo.ArgumentList.Add($argument)
            }
            $testProcess = [System.Diagnostics.Process]::new()
            $testProcess.StartInfo = $startInfo
            try {
                if (!$testProcess.Start()) { throw "Could not start native $scenario test at scale $scale." }
                $stdoutTask = $testProcess.StandardOutput.ReadToEndAsync()
                $stderrTask = $testProcess.StandardError.ReadToEndAsync()
                $guard = Wait-NativeProcess -Process $testProcess -Description "Native $scenario test at scale $scale" -DeadlineMilliseconds $deadlineMilliseconds
                [IO.File]::WriteAllText($stdout, [string]$stdoutTask.Result)
                [IO.File]::WriteAllText($stderr, [string]$stderrTask.Result)
                Write-Output "Native $scenario stdout:"
                if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout -Raw }
                else { Write-Output '<stdout file missing>' }
                Write-Output "Native $scenario stderr:"
                if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Raw }
                else { Write-Output '<stderr file missing>' }
                switch ($guard.State) {
                    'Exited' {
                        Write-Output "Native $scenario process DPR $scale completed in $($elapsed.ElapsedMilliseconds) ms"
                        if ($guard.ExitCode -ne 0) {
                            throw "Native $scenario test at scale $scale failed with exit $($guard.ExitCode)."
                        }
                    }
                    'Killed' {
                        throw "Native $scenario test at scale $scale exceeded $deadlineSeconds seconds; termination was confirmed after the deadline."
                    }
                    'AlreadyExited' {
                        throw "Native $scenario test at scale $scale exceeded $deadlineSeconds seconds; the process exited during timeout cleanup with exit $($guard.ExitCode)."
                    }
                    'TerminationFailed' {
                        throw "Native $scenario test at scale $scale exceeded $deadlineSeconds seconds, but the process could not be confirmed stopped (PID $($guard.ProcessId)): $($guard.Error)"
                    }
                    'WaitFailed' {
                        throw "Native $scenario process wait failed (PID $($guard.ProcessId)); deadline status is unavailable: $($guard.Error)"
                    }
                    'HandleFailed' {
                        throw "Native $scenario process handle failed (PID $($guard.ProcessId)); deadline status is unavailable: $($guard.Error)"
                    }
                    default { throw "Native $scenario process guard returned an unknown state '$($guard.State)'." }
                }
            } finally {
                if ($null -ne $testProcess) { $testProcess.Dispose() }
            }
    }
}
