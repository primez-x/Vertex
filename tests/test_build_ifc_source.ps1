#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$recipe = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts/build-ifc-source.ps1'
if (!(Test-Path -LiteralPath $recipe)) { throw 'Candidate build recipe is missing.' }
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($recipe, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors.Message -join "`n") }
# Load only pure guards/configuration functions, never the build entrypoint.
foreach ($function in $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($function.Extent.Text))
}
function Assert-Rejected([scriptblock]$Action, [string]$Message) {
    try { & $Action } catch { if ($_.Exception.Message -like "*$Message*") { return }; throw }
    throw "Expected rejection containing: $Message"
}
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('ifc-recipe-test-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($scratch)
try {
    $workspace = Join-Path $scratch 'repo'
    [void][IO.Directory]::CreateDirectory($workspace)
    Assert-Rejected { Assert-IfcCandidateRoot $workspace $workspace } 'overlap'
    Assert-Rejected { Assert-IfcCandidateRoot (Join-Path $workspace 'build') $workspace } 'overlap'
    Assert-Rejected { Assert-IfcCandidateRoot $scratch $workspace } 'overlap'
    $foreign = Join-Path $scratch 'foreign'
    [void][IO.Directory]::CreateDirectory($foreign)
    Set-Content (Join-Path $foreign 'CMakeCache.txt') 'foreign cache'
    Assert-Rejected { Assert-IfcCandidateRoot $foreign $workspace } 'already exists'
    if ((Get-Content (Join-Path $foreign 'CMakeCache.txt')) -ne 'foreign cache') { throw 'Foreign cache was changed.' }
    Assert-IfcCandidateRoot (Join-Path $scratch 'fresh') $workspace

    $gitStatus = Join-Path $scratch 'git-status.log'
    [IO.File]::WriteAllText($gitStatus, '')
    Assert-IfcCleanGitStatus $gitStatus
    [IO.File]::WriteAllText($gitStatus, " M ports/example/portfile.cmake`n")
    Assert-Rejected { Assert-IfcCleanGitStatus $gitStatus } 'tracked edits'

    $sourceEnvironment = @{ PATH = 'tools'; OCC_INCLUDE_DIR = 'native8'; CXXFLAGS = '/MT'; CL = '/EHs-'; BUILD_IFCPYTHON = 'OFF'; PYTHONPATH = 'foreign'; VCPKG_ROOT = 'foreign'; SystemRoot = 'windows' }
    $environment = Get-IfcBuildEnvironment $sourceEnvironment @('BUILD_IFCPYTHON')
    foreach ($name in @('OCC_INCLUDE_DIR', 'CXXFLAGS', 'CL', 'BUILD_IFCPYTHON', 'PYTHONPATH', 'VCPKG_ROOT')) {
        if ($environment.Contains($name)) { throw "Inherited build override survived: $name" }
    }
    if ($environment['SystemRoot'] -ne 'windows' -or $sourceEnvironment['CL'] -ne '/EHs-') { throw 'Environment isolation mutated the caller.' }

    $config = @(Get-IfcConfigureArguments 'src' 'build' 'kernel' 'support' 'python' 'swig' 'vs')
    if ($config -notcontains '-DSCHEMA_VERSIONS=2x3;4;4x1;4x2;4x3;4x3_tc1;4x3_add1;4x3_add2') { throw 'Full schema compatibility was lost.' }
    foreach ($entry in @('-DBUILD_IFCPYTHON=ON', '-DBUILD_IFCGEOM=ON', '-DWITH_OPENCASCADE=ON', '-DMINIMAL_BUILD=OFF', '-DBUILD_SHARED_LIBS=OFF', '-DBoost_NO_SYSTEM_PATHS=ON', '-DOCCT_STATIC=OFF')) {
        if ($config -notcontains $entry) { throw "Missing candidate setting: $entry" }
    }
    if ($config | Where-Object { $_ -match '^-DCMAKE_CXX_FLAGS(?:=|:)' }) { throw 'Recipe overwrites default exception flags.' }
    $cachePath = Join-Path $scratch 'candidate-cache.txt'
    $cacheLines = @($config | Where-Object { $_ -match '^-D([^=]+)=(.*)$' } | ForEach-Object { $_ -replace '^-D([^=]+)=', '$1:STRING=' })
    $cacheLines += @('CMAKE_HOME_DIRECTORY:INTERNAL=src/cmake', 'CMAKE_CACHEFILE_DIR:INTERNAL=build/build', 'CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022', 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', 'CMAKE_CXX_FLAGS:STRING=/DWIN32 /EHsc', 'CMAKE_CXX_FLAGS_RELEASE:STRING=/O2 /DNDEBUG', 'libTKernel:FILEPATH=kernel/lib/TKernel.lib', 'Boost_REGEX_LIBRARY_RELEASE:FILEPATH=support/lib/boost_regex.lib')
    Set-Content $cachePath $cacheLines
    Assert-IfcConfiguredCache $cachePath $config 'build' 'src' 'kernel' 'support'
    Set-Content $cachePath ($cacheLines -replace 'support/lib/boost_regex.lib', 'native8/lib/boost_regex.lib')
    Assert-Rejected { Assert-IfcConfiguredCache $cachePath $config 'build' 'src' 'kernel' 'support' } 'escaped'
    Set-Content $cachePath ($cacheLines -replace '/EHsc', '/EHs-')
    Assert-Rejected { Assert-IfcConfiguredCache $cachePath $config 'build' 'src' 'kernel' 'support' } '/EHsc'

    # A real no-window child proves argument boundaries, separate stream
    # capture, numeric nonzero exit evidence and failure propagation.
    $script:candidatePath = $scratch; $script:projectRoot = $workspace
    $script:childEnvironment = Get-IfcBuildEnvironment ([Environment]::GetEnvironmentVariables()) @()
    $script:evidence = @{ commands = [Collections.Generic.List[object]]::new() }
    $pwsh = (Get-Command pwsh -CommandType Application).Source
    Invoke-IfcCommand 'probe-ok' $pwsh @('-NoProfile', '-Command', '[Console]::Out.Write("argument with spaces"); [Console]::Error.Write("diagnostic")')
    if ((Get-Content "$scratch/probe-ok.stdout.log" -Raw) -ne 'argument with spaces' -or (Get-Content "$scratch/probe-ok.stderr.log" -Raw) -ne 'diagnostic') { throw 'Child stream/argument capture failed.' }
    Assert-Rejected { Invoke-IfcCommand 'probe-error' $pwsh @('-NoProfile', '-Command', 'exit 7') } 'exit 7'
    if ($script:evidence.commands[1].exit_code -ne 7) { throw 'Nonzero exit evidence was lost.' }

    $status = Join-Path $scratch 'status'
    Set-Content $status "Package: opencascade`nVersion: 7.8.1`nPort-Version: 1`nArchitecture: x64-windows-ifc-static`nStatus: install ok installed`n"
    Assert-IfcSdkStatus $status 'opencascade' '7.8.1' 1
    Assert-Rejected { Assert-IfcSdkStatus $status 'opencascade' '8.0.1' 1 } 'version'
    Set-Content $status "Package: opencascade`nVersion: 7.8.1`nPort-Version: 1`nArchitecture: x64-windows`nStatus: install ok installed`n"
    Assert-Rejected { Assert-IfcSdkStatus $status 'opencascade' '7.8.1' 1 } 'installed'
    Write-Output 'IFC candidate recipe guards passed.'
} finally {
    # Exact invocation-owned scratch directory only.
    if ([IO.Path]::GetFullPath($scratch).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}
