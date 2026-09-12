param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$configName = $Configuration.ToLowerInvariant()
$executable = Join-Path $projectRoot "build/windows-$configName/building_object_dialog_tests.exe"
$qtPrefix = Join-Path $projectRoot '.deps/qt/6.8.3/msvc2022_64'
$nativeSuffix = if ($Configuration -eq 'Debug') { 'debug/bin' } else { 'bin' }
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
$evidence = @()
foreach ($scale in @('1', '1.5')) {
    $outputDirectory = Join-Path $projectRoot "artifacts/building-editor/$configName/$scale"
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new($executable)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Environment['PATH'] = "$(Join-Path $qtPrefix 'bin');$(Join-Path $projectRoot ".deps/native/x64-windows/$nativeSuffix");$env:PATH"
    $startInfo.Environment['QT_PLUGIN_PATH'] = Join-Path $qtPrefix 'plugins'
    $startInfo.Environment['QT_QPA_PLATFORM'] = 'windows'
    $startInfo.Environment['QT_SCREEN_SCALE_FACTORS'] = '1'
    $startInfo.Environment['QT_SCALE_FACTOR'] = $scale
    & $addProcessArguments $startInfo @('--capture-directory', $outputDirectory)
    $testProcess = [System.Diagnostics.Process]::new()
    $testProcess.StartInfo = $startInfo
    try {
        if (!$testProcess.Start()) { throw 'Could not start editor visual test.' }
        $stdout = $testProcess.StandardOutput.ReadToEndAsync()
        $stderr = $testProcess.StandardError.ReadToEndAsync()
        if (!$testProcess.WaitForExit(15000)) {
            $testProcess.Kill($true)
            $testProcess.WaitForExit()
            throw 'Editor visual test exceeded 15 seconds and was stopped.'
        }
        $stdout.Result | Set-Content -LiteralPath (Join-Path $outputDirectory 'stdout.txt')
        $stderr.Result | Set-Content -LiteralPath (Join-Path $outputDirectory 'stderr.txt')
        if ($testProcess.ExitCode -ne 0) { throw "Editor visual test failed: $($stderr.Result)" }
        $images = @()
        foreach ($name in @('rectangular-column', 'circular-column', 'straight-beam',
                            'straight-stair-flight', 'sloped-roof-panel', 'gable-roof', 'invalid-input',
                            'imperial-default-stair', 'quantity-unit-switch')) {
            $path = Join-Path $outputDirectory "$name.png"
            if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -eq 0) {
                throw "Editor visual test produced no $name image."
            }
            $images += @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
        }
        $evidence += @{ scale = $scale; exit_code = $testProcess.ExitCode; images = $images }
        Write-Output "Editor captures passed at scale $scale."
    } finally { $testProcess.Dispose() }
}
@{
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    recorded_utc = [DateTime]::UtcNow.ToString('o')
    cases = $evidence
    qualification = 'Internal hidden dialog captures; image inspection required.'
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $projectRoot "artifacts/building-editor/$configName/evidence.json")
