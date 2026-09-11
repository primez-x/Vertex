[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Root = $PSScriptRoot,

    [Parameter(Position = 1)]
    [string]$ManifestName = 'offline-bundle-manifest.json'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw "offline bundle verification: $Message"
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
    $rootFull = [IO.Path]::GetFullPath($Root)
    $rootItem = Get-Item -LiteralPath $rootFull -Force -ErrorAction Stop
    if (-not $rootItem.PSIsContainer) {
        Fail "root is not a directory: $Root"
    }
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail "root cannot be a symlink or junction: $Root"
    }

    $manifestRelative = ($ManifestName -replace '\\', '/')
    $manifestPath = Resolve-SafeChildPath $rootFull $manifestRelative 'manifest path'
    $manifestItem = Get-Item -LiteralPath $manifestPath -Force -ErrorAction Stop
    if ($manifestItem.PSIsContainer) {
        Fail "manifest is a directory: $manifestRelative"
    }
    if (($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail "manifest cannot be a symlink or junction: $manifestRelative"
    }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Fail "manifest JSON could not be read: $($_.Exception.Message)"
    }
    if ($null -eq $manifest -or $manifest.schema_version -ne 1 -or $manifest.manifest_version -ne 1) {
        Fail 'manifest schema or version is unsupported'
    }
    if ($manifest.manifest_kind -notin @('offline-bundle', 'runtime')) {
        Fail "unsupported manifest kind: $($manifest.manifest_kind)"
    }
    if ($manifest.audit_status -ne 'incomplete') {
        Fail 'manifest audit_status must remain incomplete'
    }
    if ($null -ne $manifest.installer_qualified -and $manifest.installer_qualified) {
        Fail 'manifest cannot claim installer qualification'
    }
    if ($null -ne $manifest.offline_qualified -and $manifest.offline_qualified) {
        Fail 'manifest cannot claim offline qualification'
    }
    if ($null -ne $manifest.qualification) {
        if ($manifest.qualification.installer_qualified -ne $false -or
            $manifest.qualification.offline_qualified -ne $false) {
            Fail 'manifest qualification flags must remain false'
        }
    }

    $files = @($manifest.files)
    if ($files.Count -eq 0) {
        Fail 'manifest files must be nonempty'
    }
    $seen = @{}
    $allowed = @{}
    $allowed[$manifestRelative.ToLowerInvariant()] = $true
    foreach ($entry in $files) {
        $relative = ($entry.path -replace '\\', '/')
        $filePath = Resolve-SafeChildPath $rootFull $relative 'file path'
        $key = $relative.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            Fail "manifest contains duplicate file path: $relative"
        }
        $seen[$key] = $true
        $item = Get-Item -LiteralPath $filePath -Force -ErrorAction Stop
        if ($item.PSIsContainer) {
            Fail "declared file is a directory: $relative"
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "declared file cannot be a symlink or junction: $relative"
        }
        $expected = ([string]$entry.sha256).ToLowerInvariant()
        if ($expected -notmatch '^[0-9a-f]{64}$') {
            Fail "declared hash is invalid: $relative"
        }
        $actual = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            Fail "hash mismatch for $relative"
        }
        if ([int64]$entry.size -ne [int64]$item.Length) {
            Fail "size mismatch for $relative"
        }
        $allowed[$key] = $true
    }
    if ($manifest.manifest_kind -eq 'offline-bundle') {
        foreach ($candidate in @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -ErrorAction Stop)) {
            if (($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Fail "bundle contains a symlink or junction: $($candidate.FullName)"
            }
            if (-not $candidate.PSIsContainer) {
                $relative = $candidate.FullName.Substring($rootFull.Length).TrimStart('\').Replace('\', '/')
                if (-not $allowed.ContainsKey($relative.ToLowerInvariant())) {
                    Fail "bundle contains an unlisted file: $relative"
                }
            }
        }
    }
    Write-Output ("Verified {0} declared files ({1}); qualification remains incomplete." -f $files.Count, $manifest.manifest_kind)
    exit 0
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
