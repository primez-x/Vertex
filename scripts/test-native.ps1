param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateCount(1, 3)][ValidateSet('1', '1.5', '2')][string[]]$Scales = @('1', '1.5', '2'),
    [ValidateCount(1, 3)][ValidateSet('geometry', 'forms', 'all')][string[]]$Scenarios = @('geometry', 'forms')
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
$savedEnvironment = @{}
foreach ($name in @('PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_ENABLE_HIGHDPI_SCALING','QT_SCREEN_SCALE_FACTORS','QT_SCALE_FACTOR','SKETCH_TEST_ARTIFACT_DIR')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
}
try {
    $env:PATH = "$(Join-Path $qtPrefix 'bin');$(Join-Path $projectRoot ".deps\native\x64-windows\$nativeSuffix");$env:PATH"
    $env:QT_PLUGIN_PATH = Join-Path $qtPrefix 'plugins'
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_ENABLE_HIGHDPI_SCALING = '0'
    $env:QT_SCREEN_SCALE_FACTORS = '1'
    foreach ($scale in $Scales) {
        $env:QT_SCALE_FACTOR = $scale
        foreach ($scenario in $Scenarios) {
            $captureDirectory = Join-Path $outputDirectory "captures\$configName-$scale-$scenario"
            New-Item -ItemType Directory -Force -Path $captureDirectory | Out-Null
            $env:SKETCH_TEST_ARTIFACT_DIR = $captureDirectory
            $stdout = Join-Path $outputDirectory "$configName-$scale-$scenario.stdout.txt"
            $stderr = Join-Path $outputDirectory "$configName-$scale-$scenario.stderr.txt"
            $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
            $testProcess = Start-Process -FilePath $testPath -ArgumentList @(
                '--scenario', $scenario, '--expected-dpr', $scale
            ) -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            try {
                $guard = Wait-NativeProcess -Process $testProcess -Description "Native $scenario test at scale $scale" -DeadlineMilliseconds 15000
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
                        throw "Native $scenario test at scale $scale exceeded 15 seconds; termination was confirmed after the deadline."
                    }
                    'AlreadyExited' {
                        throw "Native $scenario test at scale $scale exceeded 15 seconds; the process exited during timeout cleanup with exit $($guard.ExitCode)."
                    }
                    'TerminationFailed' {
                        throw "Native $scenario test at scale $scale exceeded 15 seconds, but the process could not be confirmed stopped (PID $($guard.ProcessId)): $($guard.Error)"
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
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process')
    }
}
