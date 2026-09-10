[CmdletBinding()]
param(
    [switch]$VerifyOnly,
    [string]$PythonPath = 'C:\Program Files\Python312\python.exe'
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$depsRoot = Join-Path $projectRoot '.deps'
$toolingRoot = Join-Path $depsRoot 'tooling'
$qtRoot = Join-Path $depsRoot 'qt'
$downloadRoot = Join-Path $depsRoot 'downloads'
$venvRoot = Join-Path $toolingRoot 'qt-venv'
$venvPython = Join-Path $venvRoot 'Scripts\python.exe'

$qtVersion = '6.8.3'
$qtArchitecture = 'win64_msvc2022_64'
$aqtVersion = '3.3.0'
$qtPrefix = Join-Path $qtRoot "$qtVersion\msvc2022_64"

function Assert-File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Test-QtInstallation {
    Assert-File (Join-Path $qtPrefix 'bin\qmake.exe') 'Qt qmake'
    Assert-File (Join-Path $qtPrefix 'bin\qtpaths.exe') 'Qt qtpaths'

    $qtpathsPath = Join-Path $qtPrefix 'bin\qtpaths.exe'
    $reportedVersion = (& $qtpathsPath --query QT_VERSION).Trim()
    if ($LASTEXITCODE -ne 0 -or $reportedVersion -ne $qtVersion) {
        throw "Expected Qt $qtVersion, but qtpaths reported '$reportedVersion'."
    }

    $reportedPrefix = (& $qtpathsPath --query QT_INSTALL_PREFIX).Trim()
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $reportedPrefix -PathType Container)) {
        throw "qtpaths reported an invalid Qt prefix: $reportedPrefix"
    }
    $resolvedReportedPrefix = (Resolve-Path -LiteralPath $reportedPrefix).Path
    $resolvedQtPrefix = (Resolve-Path -LiteralPath $qtPrefix).Path
    if ($resolvedReportedPrefix -ne $resolvedQtPrefix) {
        throw "qtpaths prefix '$resolvedReportedPrefix' does not match '$resolvedQtPrefix'."
    }

    $components = @('Core', 'Gui', 'Widgets', 'OpenGL', 'OpenGLWidgets', 'PrintSupport', 'Pdf', 'Svg')
    $missing = [System.Collections.Generic.List[string]]::new()
    foreach ($component in $components) {
        $releaseDll = Join-Path $qtPrefix "bin\Qt6$component.dll"
        $releaseImportLibrary = Join-Path $qtPrefix "lib\Qt6$component.lib"
        $cmakeConfig = Join-Path $qtPrefix "lib\cmake\Qt6$component\Qt6${component}Config.cmake"
        foreach ($path in @($releaseDll, $releaseImportLibrary, $cmakeConfig)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                $missing.Add($path)
            }
        }
    }
    if ($missing.Count -gt 0) {
        throw "Qt installation is missing required files:`n$($missing -join "`n")"
    }

    Write-Host "Qt $reportedVersion verified at $resolvedQtPrefix"
    Write-Host "CMake prefix: $resolvedQtPrefix"
    Write-Host "Components: $($components -join ', ')"
}

if (-not $VerifyOnly) {
    Assert-File $pythonPath 'Python 3.12 interpreter'

    New-Item -ItemType Directory -Force -Path $toolingRoot, $qtRoot, $downloadRoot | Out-Null
    if (-not (Test-Path -LiteralPath $venvPython -PathType Leaf)) {
        Invoke-Checked $pythonPath @('-m', 'venv', $venvRoot)
    }
    Assert-File $venvPython 'project-local Qt tooling Python'

    Invoke-Checked $venvPython @(
        '-m', 'pip', 'install', '--disable-pip-version-check', '--upgrade',
        "aqtinstall==$aqtVersion"
    )

    $qtAlreadyReady = $false
    if (Test-Path -LiteralPath $qtPrefix -PathType Container) {
        try {
            Test-QtInstallation
            $qtAlreadyReady = $true
            Write-Host 'Existing Qt installation is complete; skipping reinstallation.'
        } catch {
            Write-Host "Existing Qt installation is incomplete; aqt will repair it. $($_.Exception.Message)"
        }
    }

    if (-not $qtAlreadyReady) {
        $aqtInstallArguments = @(
            '-m', 'aqt', 'install-qt',
            'windows', 'desktop', $qtVersion, $qtArchitecture,
            '--outputdir', $qtRoot,
            '--archive-dest', $downloadRoot,
            '--keep',
            '--timeout', '60',
            '--archives', 'qtbase', 'qtsvg',
            '--modules', 'qtpdf'
        )
        Push-Location $toolingRoot
        try {
            Invoke-Checked $venvPython $aqtInstallArguments
        } finally {
            Pop-Location
        }
    }
}

Test-QtInstallation
