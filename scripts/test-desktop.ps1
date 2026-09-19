param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
      [switch]$IncludeReference)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$configName = $Configuration.ToLowerInvariant()
$executable = Join-Path $projectRoot "build\windows-$configName\property-studio.exe"
$qtPrefix = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$nativeSuffix = if ($Configuration -eq 'Debug') { 'debug\bin' } else { 'bin' }
$addProcessArguments = {
    param([System.Diagnostics.ProcessStartInfo]$StartInfo, [string[]]$Arguments)
    $argumentListProperty = $StartInfo.PSObject.Properties['ArgumentList']
    if ($null -ne $argumentListProperty) {
        foreach ($argument in $Arguments) { $StartInfo.ArgumentList.Add($argument) }
        return
    }
    # Windows PowerShell 5.1 does not expose ArgumentList. The smoke
    # arguments contain no embedded quotes, so quoting whitespace-bearing
    # values produces the same argv for the native process.
    $StartInfo.Arguments = (($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ }
    }) -join ' ')
}
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the desktop application first.' }
$python = (Get-Command python -CommandType Application -ErrorAction Stop).Source
$outputDirectory = Join-Path $projectRoot "artifacts\desktop-smoke\$configName"
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$evidence = @()
foreach ($workspace in @('measurement', 'architectural')) {
    foreach ($case in @(@{ Size = '1366x768'; Scale = '1' },
                        @{ Size = '1920x1080'; Scale = '1' },
                        @{ Size = '1366x768'; Scale = '1.5' },
                        @{ Size = '1366x768'; Scale = '2' })) {
        $stem = "$workspace-$($case.Size)-$($case.Scale)"
        if ($IncludeReference) { $stem += '-reference' }
        $imagePath = Join-Path $outputDirectory "$stem.png"
        $modelPath = Join-Path $outputDirectory "$stem-model.png"
        $performancePath = Join-Path $outputDirectory "$stem-performance.json"
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new($executable)
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.Environment['PATH'] = "$(Join-Path $qtPrefix 'bin');$(Join-Path $projectRoot ".deps\native\x64-windows\$nativeSuffix");$env:PATH"
        $startInfo.Environment['QT_PLUGIN_PATH'] = Join-Path $qtPrefix 'plugins'
        $startInfo.Environment['QT_QPA_PLATFORM'] = 'windows'
        $startInfo.Environment['QT_ENABLE_HIGHDPI_SCALING'] = '0'
        $startInfo.Environment['QT_SCREEN_SCALE_FACTORS'] = '1'
        $startInfo.Environment['QT_SCALE_FACTOR'] = $case.Scale
        $arguments = @('--smoke', '--smoke-workspace', $workspace,
                       '--smoke-size', $case.Size, '--smoke-output', $imagePath,
                       '--smoke-3d-output', $modelPath,
                       '--smoke-performance-output', $performancePath)
        if ($IncludeReference) { $arguments += '--smoke-reference' }
        & $addProcessArguments $startInfo $arguments
        $testProcess = [System.Diagnostics.Process]::new()
        $testProcess.StartInfo = $startInfo
        try {
            if (!$testProcess.Start()) { throw "Could not start $stem." }
            $stdout = $testProcess.StandardOutput.ReadToEndAsync()
            $stderr = $testProcess.StandardError.ReadToEndAsync()
            if (!$testProcess.WaitForExit(15000)) {
                $testProcess.Kill($true)
                $testProcess.WaitForExit()
                throw "Desktop smoke $stem exceeded 15 seconds and was stopped."
            }
            $stdout.Result | Set-Content -LiteralPath (Join-Path $outputDirectory "$stem.stdout.txt")
            $stderr.Result | Set-Content -LiteralPath (Join-Path $outputDirectory "$stem.stderr.txt")
            if ($testProcess.ExitCode -ne 0) {
                throw "Desktop smoke $stem failed with exit $($testProcess.ExitCode): $($stderr.Result)"
            }
            $images = @($imagePath)
            if ($workspace -eq 'architectural') { $images += $modelPath }
            foreach ($path in $images) {
                if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -eq 0) {
                    throw "Desktop smoke produced no image at $path."
                }
            }
            if (!(Test-Path -LiteralPath $performancePath) -or
                (Get-Item -LiteralPath $performancePath).Length -eq 0) {
                throw "Desktop smoke produced no performance report at $performancePath."
            }
            try {
                # Use the same strict parser as the installed-runtime runner.
                # ConvertFrom-Json alone accepts duplicate keys and coercible schema values.
                $validationInfo = [System.Diagnostics.ProcessStartInfo]::new($python)
                $validationInfo.UseShellExecute = $false
                $validationInfo.CreateNoWindow = $true
                $validationInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
                $validationInfo.RedirectStandardOutput = $true
                $validationInfo.RedirectStandardError = $true
                & $addProcessArguments $validationInfo @((Join-Path $PSScriptRoot 'performance_report.py'), $performancePath)
                $validator = [System.Diagnostics.Process]::new()
                $validator.StartInfo = $validationInfo
                try {
                    if (!$validator.Start()) { throw 'Could not start performance report validator.' }
                    $validationOutput = $validator.StandardOutput.ReadToEndAsync()
                    $validationError = $validator.StandardError.ReadToEndAsync()
                    if (!$validator.WaitForExit(15000)) {
                        $validator.Kill()
                        $validator.WaitForExit()
                        throw 'Performance report validation exceeded 15 seconds.'
                    }
                    if ($validator.ExitCode -ne 0) { throw $validationError.Result }
                    $performance = $validationOutput.Result | ConvertFrom-Json
                } finally { $validator.Dispose() }
            } catch {
                throw "Desktop smoke produced an invalid performance report: $($_.Exception.Message)"
            }
            $evidence += @{
                case = $stem
                exit_code = $testProcess.ExitCode
                images = @($images | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash } })
                performance = @{
                    path = $performancePath
                    sha256 = [string]$performance.sha256
                    threshold_status = [string]$performance.threshold_status
                }
            }
            Write-Output "Desktop smoke passed: $stem"
        } finally { $testProcess.Dispose() }
    }
}
@{
    recorded_utc = [DateTime]::UtcNow.ToString('o')
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    cases = $evidence
    qualification = 'Internal visual captures; manual image inspection required. No production certification.'
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputDirectory 'evidence.json')
