param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$configName = $Configuration.ToLowerInvariant()
$executable = Join-Path $projectRoot "build/windows-$configName/visibility_workflow_tests.exe"
$qtPrefix = Join-Path $projectRoot '.deps/qt/6.8.3/msvc2022_64'
$nativeSuffix = if ($Configuration -eq 'Debug') { 'debug/bin' } else { 'bin' }
$evidence = @()
foreach ($scale in @('1', '1.5')) {
    $outputDirectory = Join-Path $projectRoot "artifacts/visibility-workspace/$configName/$scale"
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new($executable)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Environment['PATH'] = "$(Join-Path $qtPrefix 'bin');$(Join-Path $projectRoot ".deps/native/x64-windows/$nativeSuffix");$env:PATH"
    $startInfo.Environment['QT_PLUGIN_PATH'] = Join-Path $qtPrefix 'plugins'
    $startInfo.Environment['QT_QPA_PLATFORM'] = 'offscreen'
    $startInfo.Environment['QT_SCALE_FACTOR'] = $scale
    $startInfo.Environment['QT_SCREEN_SCALE_FACTORS'] = '1'
    $startInfo.Environment['SKETCH_VISIBILITY_CAPTURE_DIR'] = $outputDirectory
    $testProcess = [System.Diagnostics.Process]::new()
    $testProcess.StartInfo = $startInfo
    try {
        if (!$testProcess.Start()) { throw 'Could not start visibility workflow test.' }
        $stdout = $testProcess.StandardOutput.ReadToEndAsync()
        $stderr = $testProcess.StandardError.ReadToEndAsync()
        if (!$testProcess.WaitForExit(15000)) {
            $testProcess.Kill($true)
            $testProcess.WaitForExit()
            throw 'Visibility workflow test exceeded 15 seconds and was stopped.'
        }
        $stdout.Result | Set-Content -LiteralPath (Join-Path $outputDirectory 'stdout.txt')
        $stderr.Result | Set-Content -LiteralPath (Join-Path $outputDirectory 'stderr.txt')
        if ($testProcess.ExitCode -ne 0) { throw "Visibility workflow failed: $($stderr.Result)" }
        $images = @()
        foreach ($name in @('measurement', 'architectural')) {
            $path = Join-Path $outputDirectory "$name.png"
            if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -eq 0) {
                throw "Missing visibility capture: $name"
            }
            $images += @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
        }
        $pdfPath = Join-Path $outputDirectory 'filtered-view.pdf'
        if (!(Test-Path -LiteralPath $pdfPath) -or (Get-Item -LiteralPath $pdfPath).Length -eq 0) {
            throw 'Missing filtered draft PDF capture.'
        }
        $evidence += @{
            scale = $scale
            exit_code = $testProcess.ExitCode
            images = $images
            filtered_pdf = @{ path = $pdfPath; sha256 = (Get-FileHash -LiteralPath $pdfPath -Algorithm SHA256).Hash }
        }
        Write-Output "Visibility workspace captures passed at scale $scale."
    } finally { $testProcess.Dispose() }
}
@{
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    recorded_utc = [DateTime]::UtcNow.ToString('o')
    cases = $evidence
    qualification = 'Offscreen workspace controls and plan captures; native framebuffer tests are separate.'
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $projectRoot "artifacts/visibility-workspace/$configName/evidence.json")
