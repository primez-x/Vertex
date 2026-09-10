param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$configName = $Configuration.ToLowerInvariant()
$executable = Join-Path $projectRoot "build\windows-$configName\property-studio.exe"
$qtPrefix = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$nativeSuffix = if ($Configuration -eq 'Debug') { 'debug\bin' } else { 'bin' }
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the desktop application first.' }
$outputDirectory = Join-Path $projectRoot "artifacts\desktop-smoke\$configName"
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$evidence = @()
foreach ($workspace in @('measurement', 'architectural')) {
    foreach ($case in @(@{ Size = '1366x768'; Scale = '1' },
                        @{ Size = '1920x1080'; Scale = '1' },
                        @{ Size = '1366x768'; Scale = '1.5' })) {
        $stem = "$workspace-$($case.Size)-$($case.Scale)"
        $imagePath = Join-Path $outputDirectory "$stem.png"
        $modelPath = Join-Path $outputDirectory "$stem-model.png"
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
        foreach ($argument in @('--smoke', '--smoke-workspace', $workspace,
                                '--smoke-size', $case.Size, '--smoke-output', $imagePath,
                                '--smoke-3d-output', $modelPath)) {
            $startInfo.ArgumentList.Add($argument)
        }
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
            $evidence += @{
                case = $stem
                exit_code = $testProcess.ExitCode
                images = @($images | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash } })
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
