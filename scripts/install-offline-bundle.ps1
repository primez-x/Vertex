[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InstallRoot
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw "offline bundle installation: $Message"
}

function Assert-NoReparseChain([string]$RootPath, [string]$Candidate, [string]$Field) {
    $cursor = $Candidate
    while ($true) {
        if (Test-Path -LiteralPath $cursor) {
            try {
                $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            } catch {
                Fail "$Field could not be inspected: $Candidate"
            }
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Fail "$Field cannot contain a symlink or junction: $Candidate"
            }
        }
        if ($cursor.Equals($RootPath, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $parent = Split-Path -Path $cursor -Parent
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent.Equals($cursor, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $cursor = $parent
    }
}

function Resolve-SafeChildPath([string]$RootPath, [string]$RelativePath, [string]$Field) {
    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        Fail "$Field must be a nonempty relative path"
    }
    if ([IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath -match '(^|[\/])\.\.([\/]|$)' -or
        $RelativePath.Contains(':') -or
        $RelativePath.IndexOf([char]0) -ge 0) {
        Fail "$Field contains an unsafe path"
    }
    try {
        $candidate = [IO.Path]::GetFullPath((Join-Path -Path $RootPath -ChildPath ($RelativePath -replace '/', '\')))
    } catch {
        Fail "$Field could not be resolved: $RelativePath"
    }
    $prefix = $RootPath.TrimEnd('\') + '\'
    if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "$Field escapes the selected root: $RelativePath"
    }
    Assert-NoReparseChain $RootPath $candidate $Field
    return $candidate
}

try {
    $sourceRoot = [IO.Path]::GetFullPath($PSScriptRoot)
    $sourceVerifier = Join-Path $sourceRoot 'verify-offline-bundle.ps1'
    if (-not (Test-Path -LiteralPath $sourceVerifier -PathType Leaf)) {
        Fail 'the bundle verifier is missing'
    }
    & $sourceVerifier -Root $sourceRoot -ManifestName 'offline-bundle-manifest.json'
    if ($LASTEXITCODE -ne 0) {
        Fail 'bundle verification failed; no files were installed'
    }

    $bundleManifestPath = Join-Path $sourceRoot 'offline-bundle-manifest.json'
    $bundleManifest = Get-Content -LiteralPath $bundleManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $runtimeManifestName = [string]$bundleManifest.installer.runtime_manifest
    if ([string]::IsNullOrWhiteSpace($runtimeManifestName)) {
        $runtimeManifestName = 'runtime-manifest.json'
    }
    $runtimeManifestPath = Resolve-SafeChildPath $sourceRoot $runtimeManifestName 'runtime manifest path'
    $runtimeManifest = Get-Content -LiteralPath $runtimeManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($runtimeManifest.manifest_kind -ne 'runtime' -or $runtimeManifest.audit_status -ne 'incomplete') {
        Fail 'runtime manifest is unsupported'
    }

    $targetRoot = [IO.Path]::GetFullPath($InstallRoot)
    if ($targetRoot.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $targetRoot.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'install root cannot be the bundle directory or one of its children'
    }
    Assert-NoReparseChain $targetRoot $targetRoot 'install root'
    if (Test-Path -LiteralPath $targetRoot) {
        $targetItem = Get-Item -LiteralPath $targetRoot -Force
        if (-not $targetItem.PSIsContainer) {
            Fail 'install root is not a directory'
        }
        if (($targetItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail 'install root cannot be a symlink or junction'
        }
        if (@(Get-ChildItem -LiteralPath $targetRoot -Force).Count -ne 0) {
            Fail 'install root must be missing or empty'
        }
    } else {
        New-Item -ItemType Directory -Path $targetRoot -Force | Out-Null
    }

    foreach ($entry in @($runtimeManifest.files)) {
        $relative = ($entry.path -replace '\\', '/')
        $sourcePath = Resolve-SafeChildPath $sourceRoot $relative 'runtime source path'
        $targetPath = Resolve-SafeChildPath $targetRoot $relative 'runtime install path'
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            Fail "runtime source file is missing: $relative"
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $targetPath -Force
    }
    Copy-Item -LiteralPath $runtimeManifestPath -Destination (Join-Path $targetRoot $runtimeManifestName) -Force
    Copy-Item -LiteralPath $sourceVerifier -Destination (Join-Path $targetRoot 'verify-offline-bundle.ps1') -Force

    $targetVerifier = Join-Path $targetRoot 'verify-offline-bundle.ps1'
    & $targetVerifier -Root $targetRoot -ManifestName $runtimeManifestName
    if ($LASTEXITCODE -ne 0) {
        Fail 'installed runtime verification failed'
    }
    Write-Output ("Installed {0} runtime files to {1}; qualification remains incomplete." -f @($runtimeManifest.files).Count, $targetRoot)
    exit 0
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
