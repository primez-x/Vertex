param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$Desktop,
    [switch]$Architecture,
    [switch]$SkipTests,
    [string[]]$Targets = @(),
    [string]$CadRuntimeRoot
)
$ErrorActionPreference = 'Stop'
if ($args.Count -gt 0) {
    throw 'Unexpected build arguments. Pass multiple targets as a PowerShell array: -Targets @(''target1'', ''target2'').'
}
if ($Targets.Count -gt 0 -and !$SkipTests) {
    throw 'For a focused build, use -SkipTests and run the corresponding tests explicitly.'
}
$projectRoot = Split-Path -Parent $PSScriptRoot
if ($CadRuntimeRoot -and !$Desktop) {
    throw '-CadRuntimeRoot requires -Desktop.'
}
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswherePath)) { throw 'Visual Studio Build Tools 2022 and the C++ workload are required.' }
$vsRoot = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'No supported Visual C++ installation was found.' }
$vcvarsPath = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
# Only the discovered VS installation is inserted into this fixed command.
if ($vcvarsPath.Contains('"') -or $vcvarsPath.Contains('&')) { throw 'Unsupported toolchain path.' }
$devEnvironment = & $env:ComSpec /d /s /c "`"`"$vcvarsPath`" >nul && set`""
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the x64 compiler environment.' }
foreach ($entry in $devEnvironment) {
    if ($entry -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
$ninjaDirectory = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = "$ninjaDirectory;$env:PATH"
$preset = if ($Desktop) { "windows-$($Configuration.ToLowerInvariant())" }
          else { "windows-headless-$($Configuration.ToLowerInvariant())" }
$configureArguments = @('--preset', $preset)
$nativePrefix = Join-Path $projectRoot '.deps\native\x64-windows'
# Saved workspace recovery validates exact area deductions through the shared
# geometry engine even without Qt. -Architecture remains a compatibility alias
# for the now-default headless engine build.
$configureArguments += @('-DSKETCH_BUILD_ARCHITECTURE=ON', "-DCMAKE_PREFIX_PATH=$nativePrefix")
if ($Desktop) {
    . (Join-Path $PSScriptRoot 'qt-selection.ps1')
    $qtSelection = Get-VertexQtSelection -ProjectRoot $projectRoot
    $qtPrefix = $qtSelection.prefix
    if (!(Test-Path -LiteralPath (Join-Path $qtPrefix 'lib\cmake\Qt6\Qt6Config.cmake'))) {
        throw "Pinned Qt $($qtSelection.version) is missing. Run the documented Qt bootstrap first."
    }
    $configureArguments += @('-DSKETCH_BUILD_DESKTOP=ON', "-DCMAKE_PREFIX_PATH=$qtPrefix;$nativePrefix")
    if ($CadRuntimeRoot) {
        $configureArguments += "-DVERTEX_CAD_RUNTIME_ROOT=$CadRuntimeRoot"
    }
} else {
    $configureArguments += '-DSKETCH_BUILD_DESKTOP=OFF'
}
$vertexBuildPreviousPath = $env:PATH
$vertexNativeBin = Join-Path $nativePrefix $(if ($Configuration -eq 'Debug') { 'debug\bin' } else { 'bin' })
$env:PATH = "$vertexNativeBin;$env:PATH"
Push-Location $projectRoot
try {
    & $cmakePath @configureArguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    $targetArguments = if ($Targets.Count -gt 0) { @('--target') + $Targets } else { @() }
    & $cmakePath --build --preset $preset --parallel 8 @targetArguments
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
    if (!$SkipTests) {
        & $ctestPath --preset $preset
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
} finally {
    Pop-Location
    $env:PATH = $vertexBuildPreviousPath
}
