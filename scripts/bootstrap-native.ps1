param(
    [string]$BuildRoot = 'C:\Build\PropertyStudio\vcpkg-build',
    [ValidateRange(1, 64)][int]$Concurrency = 16,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'third_party\dependencies.json') -Raw | ConvertFrom-Json
$manager = $manifest.native_package_manager
$vcpkgRoot = Join-Path $projectRoot '.deps\vcpkg'
$binaryPath = Join-Path $vcpkgRoot 'vcpkg.exe'
function Assert-Exit([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed with exit code $LASTEXITCODE." }
}
if (!(Test-Path -LiteralPath (Join-Path $vcpkgRoot '.git'))) {
    if ($Offline) { throw 'The pinned vcpkg source cache is missing.' }
    & git clone --filter=blob:none --no-checkout $manager.repository $vcpkgRoot
    Assert-Exit 'vcpkg source clone'
    & git -C $vcpkgRoot fetch --depth 1 origin $manager.commit
    Assert-Exit 'Pinned vcpkg revision fetch'
    & git -C $vcpkgRoot checkout --detach $manager.commit
    Assert-Exit 'Pinned vcpkg checkout'
}
$actualCommit = (& git -C $vcpkgRoot rev-parse HEAD).Trim()
Assert-Exit 'vcpkg revision check'
if ($actualCommit -ne $manager.commit) { throw "vcpkg revision mismatch: $actualCommit" }
if (!(Test-Path -LiteralPath $binaryPath)) {
    if ($Offline) { throw 'The cached vcpkg executable is missing.' }
    & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    Assert-Exit 'vcpkg executable bootstrap'
}
# OCCT contains generated object names that exceed MAX_PATH in a deeply nested
# checkout. Keep dependency compilation in a short dedicated directory. Source
# and downloaded archives remain in the documented caches; nothing is deleted.
$absoluteBuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($absoluteBuildRoot.Length -gt 70) { throw 'Choose a dependency build directory shorter than 70 characters.' }
New-Item -ItemType Directory -Path $absoluteBuildRoot -Force | Out-Null
$env:VCPKG_MAX_CONCURRENCY = [string]$Concurrency
$env:VCPKG_DISABLE_METRICS = '1'
$arguments = @('install', '--triplet', 'x64-windows',
    "--x-install-root=$(Join-Path $projectRoot '.deps\native')",
    "--x-buildtrees-root=$absoluteBuildRoot", '--disable-metrics')
if ($Offline) { $arguments += '--no-downloads' }
Push-Location $projectRoot
try {
    & $binaryPath @arguments
    Assert-Exit 'Native dependency build'
} finally { Pop-Location }
