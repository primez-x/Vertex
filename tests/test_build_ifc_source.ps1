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
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('ifc-recipe-' + [guid]::NewGuid().ToString('N').Substring(0, 12))
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

    # External SDK selection must protect each immutable input before output
    # reservation. Defaults still resolve the original installed prefixes.
    $kernel = Join-Path $scratch 'kernel/x64-windows-ifc-static'
    $support = Join-Path $scratch 'support/x64-windows-ifc-static'
    foreach ($prefix in @($kernel, $support)) {
        [void][IO.Directory]::CreateDirectory($prefix)
        $metadata = Join-Path (Split-Path -Parent $prefix) 'vcpkg'
        [void][IO.Directory]::CreateDirectory($metadata)
        [IO.File]::WriteAllText((Join-Path $metadata 'status'), 'preserved SDK status')
    }
    foreach ($prefix in @($kernel, $support)) {
        Assert-Rejected { Assert-IfcCandidateRoot $prefix $workspace @($kernel, $support) } 'overlap'
        Assert-Rejected { Assert-IfcCandidateRoot (Join-Path $prefix 'candidate') $workspace @($kernel, $support) } 'overlap'
        Assert-Rejected { Assert-IfcCandidateRoot (Split-Path -Parent $prefix) $workspace @($kernel, $support) } 'overlap'
    }
    Assert-IfcCandidateRoot (Join-Path $scratch 'external-candidate') $workspace @($kernel, $support)
    Assert-IfcCandidateRoot ($kernel + '-candidate') $workspace @($kernel, $support)
    $roots = Get-IfcSdkRoots $workspace $kernel $support
    if ($roots.kernel -ne $kernel -or $roots.support -ne $support -or
        $roots.kernel_status -ne (Join-Path $scratch 'kernel/vcpkg/status') -or
        $roots.support_status -ne (Join-Path $scratch 'support/vcpkg/status')) { throw 'Explicit SDK roots/status escaped their selected prefixes.' }
    foreach ($name in @('ifc-kernel', 'ifc-support')) {
        [void][IO.Directory]::CreateDirectory((Join-Path $workspace ".deps/$name/x64-windows-ifc-static"))
        [void][IO.Directory]::CreateDirectory((Join-Path $workspace ".deps/$name/vcpkg"))
        [IO.File]::WriteAllText((Join-Path $workspace ".deps/$name/vcpkg/status"), 'default SDK status')
    }
    $defaults = Get-IfcSdkRoots $workspace
    if ($defaults.kernel -ne (Join-Path $workspace '.deps/ifc-kernel/x64-windows-ifc-static') -or
        $defaults.support -ne (Join-Path $workspace '.deps/ifc-support/x64-windows-ifc-static')) { throw 'Default SDK roots changed.' }
    Assert-Rejected { Get-IfcSdkRoots $workspace (Join-Path $scratch 'missing') $support } 'missing'
    Assert-Rejected { Get-IfcSdkRoots $workspace $kernel (Join-Path $scratch 'missing') } 'missing'
    [void][IO.Directory]::CreateDirectory((Join-Path $scratch 'without-status/prefix'))
    Assert-Rejected { Get-IfcSdkRoots $workspace (Join-Path $scratch 'without-status/prefix') $support } 'status'
    $driveRoot = [IO.Path]::GetPathRoot($scratch)
    foreach ($invalid in @('relative/sdk', '\\server\share\sdk', '//server/share/sdk', '\\?\C:\sdk', $driveRoot,
            "$scratch/../sdk", "$scratch/./sdk", "$scratch/sdk;foreign", "$scratch/sdk`nforeign",
            "$scratch/sdk:stream", "$scratch/sdk.", "$scratch/sdk ", "$scratch/sdk*/prefix")) {
        Assert-Rejected { Get-IfcSdkRoots $workspace $invalid $support } 'local'
        Assert-Rejected { Get-IfcSdkRoots $workspace $kernel $invalid } 'local'
        Assert-Rejected { Assert-IfcCandidateRoot $invalid $workspace @($kernel, $support) } 'local'
    }
    $linked = Join-Path $scratch 'sdk-junction'
    [void](New-Item -ItemType Junction -Path $linked -Target $kernel)
    Assert-Rejected { Get-IfcSdkRoots $workspace $linked $support } 'reparse'
    Assert-Rejected { Assert-IfcCandidateRoot (Join-Path $linked 'candidate') $workspace @($kernel, $support) } 'reparse'
    if ((Get-Content (Join-Path $scratch 'kernel/vcpkg/status') -Raw) -ne 'preserved SDK status' -or
        (Test-Path (Join-Path $kernel 'candidate'))) { throw 'Rejected output changed SDK inputs.' }

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

    $deriveArguments = @(Get-IfcDerivedSourceArguments $workspace (Join-Path $scratch 'candidate'))
    $deriveExpected = @('-I', '-B', (Join-Path $workspace 'scripts/prepare_ifc_derived_source.py'),
        '--workspace', $workspace, '--output', (Join-Path $scratch 'candidate/source'),
        '--manifest', (Join-Path $scratch 'candidate/source-derivation.json'))
    if (($deriveArguments -join "`0") -ne ($deriveExpected -join "`0")) { throw 'Derived source command escaped its exact controlled paths.' }

    $config = @(Get-IfcConfigureArguments 'src' 'build' 'kernel' 'support' 'python' 'swig' 'vs')
    if ($config -notcontains '-DSCHEMA_VERSIONS=2x3;4;4x1;4x2;4x3;4x3_tc1;4x3_add1;4x3_add2') { throw 'Full schema compatibility was lost.' }
    foreach ($entry in @('-DBUILD_IFCPYTHON=ON', '-DBUILD_IFCGEOM=ON', '-DWITH_OPENCASCADE=ON', '-DMINIMAL_BUILD=OFF', '-DBUILD_SHARED_LIBS=OFF', '-DBoost_NO_SYSTEM_PATHS=ON', '-DOCCT_STATIC=OFF')) {
        if ($config -notcontains $entry) { throw "Missing candidate setting: $entry" }
    }
    if ($config | Where-Object { $_ -match '^-DCMAKE_CXX_FLAGS(?:=|:)' }) { throw 'Recipe overwrites default exception flags.' }
    $externalConfig = @(Get-IfcConfigureArguments 'src' 'build' $kernel $support 'python' 'swig' 'vs')
    foreach ($setting in @("-DOCC_INCLUDE_DIR=$kernel/include/opencascade", "-DOCC_LIBRARY_DIR=$kernel/lib",
            "-DBOOST_ROOT=$support", "-DBoost_DIR=$support/share/boost", "-DEIGEN_DIR=$support/include/eigen3",
            "-DCMAKE_PREFIX_PATH=$kernel;$support")) {
        if ($externalConfig -notcontains $setting.Replace('\', '/')) { throw "Selected SDK configuration differs: $setting" }
    }
    $cachePath = Join-Path $scratch 'candidate-cache.txt'
    $cacheLines = @($config | Where-Object { $_ -match '^-D([^=]+)=(.*)$' } | ForEach-Object { $_ -replace '^-D([^=]+)=', '$1:STRING=' })
    $cacheLines += @('CMAKE_HOME_DIRECTORY:INTERNAL=src/cmake', 'CMAKE_CACHEFILE_DIR:INTERNAL=build/build', 'CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022', 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', 'CMAKE_CXX_FLAGS:STRING=/DWIN32 /EHsc', 'CMAKE_CXX_FLAGS_RELEASE:STRING=/O2 /DNDEBUG', 'libTKernel:FILEPATH=kernel/lib/TKernel.lib', 'Boost_REGEX_LIBRARY_RELEASE:FILEPATH=support/lib/boost_regex.lib')
    Set-Content $cachePath $cacheLines
    Assert-IfcConfiguredCache $cachePath $config 'build' 'src' 'kernel' 'support'
    Set-Content $cachePath ($cacheLines -replace 'support/lib/boost_regex.lib', 'native8/lib/boost_regex.lib')
    Assert-Rejected { Assert-IfcConfiguredCache $cachePath $config 'build' 'src' 'kernel' 'support' } 'escaped'
    Set-Content $cachePath ($cacheLines -replace 'kernel/lib/TKernel.lib', 'native8/lib/TKernel.lib')
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

    # Execute only refusal paths: missing/malformed SDKs and SDK-overlapping
    # outputs must stop before source/native commands or output reservation.
    $refusals = @(
        @{ name = 'missing-kernel'; kernel = (Join-Path $scratch 'missing-sdk'); support = $support; output = (Join-Path $scratch 'absent-parent/kernel-output'); reason = 'missing' },
        @{ name = 'missing-support'; kernel = $kernel; support = (Join-Path $scratch 'missing-sdk'); output = (Join-Path $scratch 'absent-parent/support-output'); reason = 'missing' },
        @{ name = 'malformed-sdk'; kernel = "$scratch/../sdk"; support = $support; output = (Join-Path $scratch 'absent-parent/malformed-output'); reason = 'local' },
        @{ name = 'overlap-sdk'; kernel = $kernel; support = $support; output = (Join-Path $kernel 'output'); reason = 'overlap' },
        @{ name = 'overlap-support'; kernel = $kernel; support = $support; output = (Join-Path $support 'output'); reason = 'overlap' }
    )
    foreach ($case in $refusals) {
        Assert-Rejected { Invoke-IfcCommand $case.name $pwsh @('-NoProfile', '-File', $recipe,
            '-BuildRoot', $case.output, '-KernelRoot', $case.kernel, '-SupportRoot', $case.support, '-ConfigureOnly') } 'exit 1'
        if ((Get-Content (Join-Path $scratch "$($case.name).stderr.log") -Raw) -notlike "*$($case.reason)*" -or
            (Test-Path -LiteralPath $case.output) -or (Test-Path -LiteralPath "$($case.output).reservation")) { throw "Entrypoint refusal failed: $($case.name)" }
    }
    if (Test-Path -LiteralPath (Join-Path $scratch 'absent-parent')) { throw 'SDK refusal created the output parent.' }

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
