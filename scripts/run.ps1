param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string[]]$ApplicationArguments = @()
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtPrefix = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$nativeSuffix = if ($Configuration -eq 'Debug') { 'debug\bin' } else { 'bin' }
$nativeBin = Join-Path $projectRoot ".deps\native\x64-windows\$nativeSuffix"
$applicationPath = Join-Path $projectRoot "build\windows-$($Configuration.ToLowerInvariant())\vertex.exe"
foreach ($requiredPath in @($applicationPath, $nativeBin, (Join-Path $qtPrefix 'bin'), (Join-Path $qtPrefix 'plugins'))) {
    if (!(Test-Path -LiteralPath $requiredPath)) {
        throw "Build the desktop application and prepare its dependencies first: $requiredPath"
    }
}
$previousPath = $env:PATH
$previousPluginPath = $env:QT_PLUGIN_PATH
try {
    $env:PATH = "$(Join-Path $qtPrefix 'bin');$nativeBin;$previousPath"
    $env:QT_PLUGIN_PATH = Join-Path $qtPrefix 'plugins'
    # Array arguments remain separate strings; no shell-generated command text.
    & $applicationPath @ApplicationArguments
} finally {
    $env:PATH = $previousPath
    $env:QT_PLUGIN_PATH = $previousPluginPath
}
