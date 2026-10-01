param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string[]]$ApplicationArguments = @()
)
$ErrorActionPreference = 'Stop'
$vertexProjectRoot = Split-Path -Parent $PSScriptRoot
$vertexCliPath = Join-Path $vertexProjectRoot "build\windows-headless-$($Configuration.ToLowerInvariant())\vertex-cli.exe"
$vertexNativeSuffix = if ($Configuration -eq 'Debug') { 'debug\bin' } else { 'bin' }
$vertexNativeBin = Join-Path $vertexProjectRoot ".deps\native\x64-windows\$vertexNativeSuffix"
foreach ($vertexRequiredPath in @($vertexCliPath, $vertexNativeBin)) {
    if (!(Test-Path -LiteralPath $vertexRequiredPath)) {
        throw "Prepare native dependencies and build the headless configuration first: $vertexRequiredPath"
    }
}
$vertexCliPreviousPath = $env:PATH
try {
    $env:PATH = "$vertexNativeBin;$vertexCliPreviousPath"
    & $vertexCliPath @ApplicationArguments
    $vertexCliExitCode = $LASTEXITCODE
} finally {
    $env:PATH = $vertexCliPreviousPath
}
if ($vertexCliExitCode -ne 0) {
    throw "Vertex CLI exited with code $vertexCliExitCode."
}
