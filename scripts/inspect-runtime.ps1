param([string[]]$EntryPoints = @(), [string]$CadPayloadManifest = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtPrefix = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$releaseDirectory = Join-Path $projectRoot 'build\windows-release'

function Get-VerifiedCadPayload {
    param([string]$ManifestPath)

    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if (($manifest.schema_version -isnot [int] -and $manifest.schema_version -isnot [long]) -or
        $manifest.schema_version -ne 1 -or $null -eq $manifest.files -or
        $manifest.files -isnot [System.Array]) {
        throw 'CAD payload manifest must use schema_version 1 and a files array.'
    }
    $payloadRoot = [System.IO.Path]::GetFullPath((Join-Path $releaseDirectory 'cad-runtime'))
    $verifiedFiles = @{}
    $peNames = @{}
    $pePaths = @()
    $directories = @()
    foreach ($record in $manifest.files) {
        if ($record.runtime -isnot [bool]) { throw 'CAD payload manifest runtime must be a JSON boolean.' }
        if (!$record.runtime) { continue }
        if ($record.path -isnot [string] -or [string]::IsNullOrWhiteSpace($record.path) -or
            [System.IO.Path]::IsPathRooted($record.path) -or $record.path.Contains(':')) {
            throw "CAD payload path must be repository-relative: $($record.path)"
        }
        $segments = @($record.path -split '[\\/]')
        if (@($segments | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) {
            throw "CAD payload path must not contain empty or traversal segments: $($record.path)"
        }
        if (@($segments | Where-Object { $_.EndsWith('.') -or $_.EndsWith(' ') }).Count -gt 0) {
            throw "CAD payload path must not contain Windows trailing-dot or trailing-space aliases: $($record.path)"
        }
        $path = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $record.path))
        if (!$path.StartsWith($payloadRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "CAD payload file must be inside build/windows-release/cad-runtime: $($record.path)"
        }
        # Check every repository-local ancestor as well as the file. GetFullPath
        # alone cannot establish containment when a junction redirects a directory.
        $current = $projectRoot
        foreach ($segment in @('') + $segments) {
            if ($segment -ne '') { $current = Join-Path $current $segment }
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "CAD payload path contains a reparse point: $current"
            }
        }
        if ($item -isnot [System.IO.FileInfo]) { throw "CAD payload file is not a regular file: $path" }
        if ($record.sha256 -isnot [string] -or $record.sha256 -notmatch '^[a-fA-F0-9]{64}$') {
            throw "CAD payload file needs a SHA256 hash: $($record.path)"
        }
        if ($verifiedFiles.ContainsKey($path)) { throw "Duplicate CAD payload path: $($record.path)" }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -ne $record.sha256) { throw "CAD payload SHA256 mismatch: $($record.path)" }
        $verifiedFiles[$path] = $hash
        if ([System.IO.Path]::GetExtension($path) -notmatch '^\.(dll|pyd|exe)$') { continue }
        $name = [System.IO.Path]::GetFileName($path)
        if ($peNames.ContainsKey($name)) { throw "Ambiguous CAD payload PE basename: $name" }
        $peNames[$name] = $path
        $pePaths += $path
        $directories += [System.IO.Path]::GetDirectoryName($path)
    }
    return @{ files = $verifiedFiles; entry_points = $pePaths; directories = @($directories | Select-Object -Unique) }
}

$cadPayload = $null
if ($CadPayloadManifest) {
    # Verify the entire declared runtime before invoking dumpbin on any binary.
    $cadPayload = Get-VerifiedCadPayload -ManifestPath $CadPayloadManifest
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual C++ Build Tools are required for PE import inspection.' }
$compilerVersion = (Get-Content -LiteralPath (Join-Path $vsRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$dumpbin = Join-Path $vsRoot "VC\Tools\MSVC\$compilerVersion\bin\Hostx64\x64\dumpbin.exe"
if (!(Test-Path -LiteralPath $dumpbin)) { throw "Missing PE inspection tool: $dumpbin" }
if ($EntryPoints.Count -eq 0) {
    $EntryPoints = @((Join-Path $releaseDirectory 'vertex.exe'),
                    (Join-Path $releaseDirectory 'vertex-import-worker.exe'),
                    (Join-Path $releaseDirectory 'vertex-cli.exe'),
                    (Join-Path $releaseDirectory 'vertex-planegcs.dll'),
                    (Join-Path $qtPrefix 'plugins\platforms\qwindows.dll'),
                    (Join-Path $qtPrefix 'plugins\styles\qmodernwindowsstyle.dll'),
                    (Join-Path $qtPrefix 'plugins\imageformats\qgif.dll'),
                    (Join-Path $qtPrefix 'plugins\imageformats\qico.dll'),
                    (Join-Path $qtPrefix 'plugins\imageformats\qjpeg.dll'))
}
if ($null -eq $cadPayload) {
    $searchDirectories = @($releaseDirectory,
                           (Join-Path $releaseDirectory 'cad-runtime'),
                           (Join-Path $projectRoot '.deps\msvc-runtime\14.44.35211.0\bin'),
                           [Environment]::SystemDirectory,
                           (Join-Path $qtPrefix 'bin'),
                           (Join-Path $projectRoot '.deps\native\x64-windows\bin'))
} else {
    $EntryPoints = @($EntryPoints) + @($cadPayload.entry_points)
    $searchDirectories = @($releaseDirectory,
                           (Join-Path $projectRoot '.deps\msvc-runtime\14.44.35211.0\bin'),
                           (Join-Path $qtPrefix 'bin'),
                           (Join-Path $projectRoot '.deps\native\x64-windows\bin')) +
                         @($cadPayload.directories) + @([Environment]::SystemDirectory)
}
$cadSearchDirectories = @{}
if ($null -ne $cadPayload) {
    foreach ($directory in $cadPayload.directories) { $cadSearchDirectories[$directory] = $true }
}
$pending = [System.Collections.Generic.Queue[string]]::new()
foreach ($entry in $EntryPoints) {
    $pending.Enqueue((Get-Item -LiteralPath $entry).FullName)
}
$seen = @{}
$modules = @()
$unresolved = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
while ($pending.Count -gt 0) {
    $path = $pending.Dequeue()
    if ($seen.ContainsKey($path)) { continue }
    $seen[$path] = $true
    $moduleHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($null -ne $cadPayload -and $cadPayload.files.ContainsKey($path) -and
        $moduleHash -ne $cadPayload.files[$path]) {
        throw "CAD payload changed after manifest verification: $path"
    }
    $lines = & $dumpbin /dependents $path
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect PE imports: $path" }
    $imports = @()
    foreach ($name in @($lines | ForEach-Object {
        if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1] }
    } | Sort-Object -Unique)) {
        if ($name -match '^(api-ms-win-|ext-ms-win-)') {
            $imports += @{ name = $name; kind = 'windows-api-contract' }
            continue
        }
        $candidates = @($searchDirectories | ForEach-Object {
            $candidate = Join-Path $_ $name
            # A neighboring DLL is not payload provenance merely because its
            # directory contains a verified file.
            if ($cadSearchDirectories.ContainsKey($_) -and !$cadPayload.files.ContainsKey($candidate)) { return }
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { (Get-Item -LiteralPath $candidate).FullName }
        } | Select-Object -Unique)
        if ($candidates.Count -eq 0) {
            $null = $unresolved.Add($name)
            $imports += @{ name = $name; kind = 'unresolved' }
            continue
        }
        $resolved = $candidates[0]
        $system = $resolved.StartsWith([Environment]::SystemDirectory + '\', [System.StringComparison]::OrdinalIgnoreCase)
        # Only package-local alternatives participate in local provenance.
        # An installed Windows fallback is observed separately, never copied.
        $localCandidates = @($candidates | Where-Object {
            !$_.StartsWith([Environment]::SystemDirectory + '\', [System.StringComparison]::OrdinalIgnoreCase)
        })
        $imports += @{ name = $name; kind = $(if ($system) { 'installed-system-runtime' } else { 'local-component' });
            resolved = $resolved; candidates = @(if ($system) { $candidates } else { $localCandidates });
            system_fallback_available = ($localCandidates.Count -lt $candidates.Count) }
        if (!$system) { $pending.Enqueue($resolved) }
    }
    $modules += @{ path = $path; sha256 = $moduleHash; imports = $imports }
}
$outputDirectory = Join-Path $projectRoot 'artifacts\runtime'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$outputPath = Join-Path $outputDirectory 'release-imports.json'
$evidence = @{
    recorded_utc = [DateTime]::UtcNow.ToString('o')
    entry_points = $EntryPoints
    modules = $modules
    unresolved = @($unresolved | Sort-Object)
    boundary = 'Static PE imports including reported delay imports, plus explicit plugin entry points. Does not prove dynamic LoadLibrary coverage, clean-machine runtime availability, offline behavior, licensing or redistributability.'
}
if ($null -ne $cadPayload) { $evidence.cad_payload_manifest = $CadPayloadManifest }
$evidence | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $outputPath
Write-Output "Inspected $($modules.Count) component binaries; $($unresolved.Count) unresolved imports. Report: $outputPath"
if ($unresolved.Count -gt 0) { throw "Unresolved imports: $($unresolved -join ', ')" }
