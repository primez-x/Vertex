param([string[]]$EntryPoints = @())
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtPrefix = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$releaseDirectory = Join-Path $projectRoot 'build\windows-release'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual C++ Build Tools are required for PE import inspection.' }
$compilerVersion = (Get-Content -LiteralPath (Join-Path $vsRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$dumpbin = Join-Path $vsRoot "VC\Tools\MSVC\$compilerVersion\bin\Hostx64\x64\dumpbin.exe"
if (!(Test-Path -LiteralPath $dumpbin)) { throw "Missing PE inspection tool: $dumpbin" }
if ($EntryPoints.Count -eq 0) {
    $EntryPoints = @((Join-Path $releaseDirectory 'property-studio.exe'),
                    (Join-Path $releaseDirectory 'property-cli.exe'),
                    (Join-Path $releaseDirectory 'property_planegcs.dll'),
                    (Join-Path $qtPrefix 'plugins\platforms\qwindows.dll'),
                    (Join-Path $qtPrefix 'plugins\styles\qmodernwindowsstyle.dll'),
                    (Join-Path $qtPrefix 'plugins\imageformats\qgif.dll'),
                    (Join-Path $qtPrefix 'plugins\imageformats\qico.dll'),
                    (Join-Path $qtPrefix 'plugins\imageformats\qjpeg.dll'))
}
$searchDirectories = @($releaseDirectory, [Environment]::SystemDirectory,
                       (Join-Path $qtPrefix 'bin'),
                       (Join-Path $projectRoot '.deps\native\x64-windows\bin'))
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
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { (Get-Item -LiteralPath $candidate).FullName }
        } | Select-Object -Unique)
        if ($candidates.Count -eq 0) {
            $null = $unresolved.Add($name)
            $imports += @{ name = $name; kind = 'unresolved' }
            continue
        }
        $resolved = $candidates[0]
        $system = $resolved.StartsWith([Environment]::SystemDirectory + '\', [System.StringComparison]::OrdinalIgnoreCase)
        $imports += @{ name = $name; kind = $(if ($system) { 'installed-system-runtime' } else { 'local-component' }); resolved = $resolved; candidates = $candidates }
        if (!$system) { $pending.Enqueue($resolved) }
    }
    $modules += @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash; imports = $imports }
}
$outputDirectory = Join-Path $projectRoot 'artifacts\runtime'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$outputPath = Join-Path $outputDirectory 'release-imports.json'
@{
    recorded_utc = [DateTime]::UtcNow.ToString('o')
    entry_points = $EntryPoints
    modules = $modules
    unresolved = @($unresolved | Sort-Object)
    boundary = 'Static PE imports including reported delay imports, plus explicit plugin entry points. Does not prove dynamic LoadLibrary coverage, clean-machine runtime availability, offline behavior, licensing or redistributability.'
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $outputPath
Write-Output "Inspected $($modules.Count) component binaries; $($unresolved.Count) unresolved imports. Report: $outputPath"
if ($unresolved.Count -gt 0) { throw "Unresolved imports: $($unresolved -join ', ')" }
