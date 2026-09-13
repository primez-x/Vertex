[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InstallRoot,

    [ValidateSet('Install', 'Repair', 'Uninstall')]
    [string]$Action = 'Install'
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

function Resolve-InstallRoot([string]$SourceRoot, [string]$RequestedRoot) {
    try {
        $resolved = [IO.Path]::GetFullPath($RequestedRoot)
    } catch {
        Fail 'install root could not be resolved'
    }
    if ([string]::IsNullOrWhiteSpace($resolved) -or
        $resolved.Equals($SourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $resolved.StartsWith($SourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'install root cannot be the bundle directory or one of its children'
    }
    $parent = Split-Path -Path $resolved -Parent
    $leaf = Split-Path -Path $resolved -Leaf
    if ([string]::IsNullOrWhiteSpace($parent) -or [string]::IsNullOrWhiteSpace($leaf)) {
        Fail 'install root must name a directory below an existing parent'
    }
    if (-not (Test-Path -LiteralPath $parent)) {
        if ($Action -eq 'Uninstall') {
            Fail 'install root does not exist'
        }
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Assert-NoReparseChain $parent $parent 'install parent'
    return @{ Root = $resolved; Parent = $parent; Leaf = $leaf }
}

function Assert-RuntimeInstall([string]$RootPath, [string]$ManifestName) {
    $manifestPath = Resolve-SafeChildPath $RootPath $ManifestName 'installed runtime manifest path'
    $verifierPath = Resolve-SafeChildPath $RootPath 'verify-offline-bundle.ps1' 'installed verifier path'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $verifierPath -PathType Leaf)) {
        Fail 'install root is not a Vertex offline runtime'
    }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Fail 'installed runtime manifest could not be read'
    }
    if ($manifest.manifest_kind -ne 'runtime' -or $manifest.audit_status -ne 'incomplete') {
        Fail 'installed runtime manifest is unsupported'
    }
    foreach ($candidate in @(Get-ChildItem -LiteralPath $RootPath -Recurse -Force -ErrorAction Stop)) {
        if (($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "installed runtime contains a symlink or junction: $($candidate.FullName)"
        }
    }
    return @{ ManifestPath = $manifestPath; VerifierPath = $verifierPath }
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

    $target = Resolve-InstallRoot $sourceRoot $InstallRoot
    $targetRoot = $target.Root
    $targetParent = $target.Parent
    $targetLeaf = $target.Leaf

    $targetInitiallyExists = Test-Path -LiteralPath $targetRoot
    if ($targetInitiallyExists) {
        $targetItem = Get-Item -LiteralPath $targetRoot -Force
        if (-not $targetItem.PSIsContainer) {
            Fail 'install root is not a directory'
        }
        if (($targetItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail 'install root cannot be a symlink or junction'
        }
        if ($Action -eq 'Install' -and @(Get-ChildItem -LiteralPath $targetRoot -Force).Count -ne 0) {
            Fail 'install root must be missing or empty'
        }
    }

    if ($Action -eq 'Uninstall') {
        if (-not $targetInitiallyExists) {
            Fail 'install root does not exist'
        }
        $installed = Assert-RuntimeInstall $targetRoot $runtimeManifestName
        & $installed.VerifierPath -Root $targetRoot -ManifestName $runtimeManifestName
        if ($LASTEXITCODE -ne 0) {
            Fail 'installed runtime verification failed; refusing to remove it'
        }
        Remove-Item -LiteralPath $targetRoot -Recurse -Force
        if (Test-Path -LiteralPath $targetRoot) {
            Fail 'install root could not be removed'
        }
        Write-Output ("Removed the verified Vertex runtime from {0}." -f $targetRoot)
        exit 0
    }

    if ($Action -eq 'Repair') {
        if (-not $targetInitiallyExists) {
            Fail 'repair target does not exist'
        }
        # A repair may replace damaged payload bytes, but it must still prove
        # that the destination is an installation of this runtime family
        # before moving it aside.
        [void](Assert-RuntimeInstall $targetRoot $runtimeManifestName)
    }

    $installToken = [Guid]::NewGuid().ToString('N')
    $stagingRoot = Join-Path $targetParent ('.{0}.installing-{1}' -f $targetLeaf, $installToken)
    $backupRoot = Join-Path $targetParent ('.{0}.backup-{1}' -f $targetLeaf, $installToken)
    if ((Test-Path -LiteralPath $stagingRoot) -or (Test-Path -LiteralPath $backupRoot)) {
        Fail 'temporary install paths already exist'
    }
    $targetMovedToBackup = $false
    $publishedRootCreated = $false
    $published = $false
    try {
        New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
        Assert-NoReparseChain $targetParent $stagingRoot 'staging root'

        foreach ($entry in @($runtimeManifest.files)) {
            $relative = ($entry.path -replace '\\', '/')
            $sourcePath = Resolve-SafeChildPath $sourceRoot $relative 'runtime source path'
            $stagingPath = Resolve-SafeChildPath $stagingRoot $relative 'runtime staging path'
            if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
                Fail "runtime source file is missing: $relative"
            }
            New-Item -ItemType Directory -Path (Split-Path -Parent $stagingPath) -Force | Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination $stagingPath -Force
        }
        Copy-Item -LiteralPath $runtimeManifestPath -Destination (Join-Path $stagingRoot $runtimeManifestName) -Force
        Copy-Item -LiteralPath $sourceVerifier -Destination (Join-Path $stagingRoot 'verify-offline-bundle.ps1') -Force

        $stagingVerifier = Join-Path $stagingRoot 'verify-offline-bundle.ps1'
        & $stagingVerifier -Root $stagingRoot -ManifestName $runtimeManifestName
        if ($LASTEXITCODE -ne 0) {
            Fail 'staged runtime verification failed; no files were installed'
        }

        # Publish by directory rename after the complete staged tree verifies.
        # If an empty destination existed, retain it beside the staging tree
        # until the new install has passed its post-publish verification.
        if ($targetInitiallyExists) {
            Move-Item -LiteralPath $targetRoot -Destination $backupRoot
            $targetMovedToBackup = $true
        }
        Move-Item -LiteralPath $stagingRoot -Destination $targetRoot
        $publishedRootCreated = $true

        $targetVerifier = Join-Path $targetRoot 'verify-offline-bundle.ps1'
        & $targetVerifier -Root $targetRoot -ManifestName $runtimeManifestName
        if ($LASTEXITCODE -ne 0) {
            Fail 'installed runtime verification failed'
        }
        $published = $true
    } catch {
        if (Test-Path -LiteralPath $stagingRoot) {
            Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        if (-not $published) {
            if ($publishedRootCreated -and (Test-Path -LiteralPath $targetRoot)) {
                Remove-Item -LiteralPath $targetRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
            if ($targetMovedToBackup -and (Test-Path -LiteralPath $backupRoot) -and
                -not (Test-Path -LiteralPath $targetRoot)) {
                Move-Item -LiteralPath $backupRoot -Destination $targetRoot -Force
            }
        }
        throw
    }
    if (Test-Path -LiteralPath $backupRoot) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    $verb = if ($Action -eq 'Repair') { 'Repaired' } else { 'Installed' }
    Write-Output ("{0} {1} runtime files to {2}; qualification remains incomplete." -f $verb, @($runtimeManifest.files).Count, $targetRoot)
    exit 0
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
