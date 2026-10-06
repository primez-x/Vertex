#requires -Version 7.0
<#
Build a separate, unqualified IFC wrapper candidate. Never install or package it.
Dependencies must already be prepared; see docs/dependencies/ifc-source-build.md.
#>
[CmdletBinding()]
param(
    [string]$BuildRoot = ('C:/Build/Vertex/ifc-' + [guid]::NewGuid().ToString('N').Substring(0, 12)),
    [ValidateNotNullOrEmpty()][string]$KernelRoot,
    [ValidateNotNullOrEmpty()][string]$SupportRoot,
    [ValidateRange(1, 32)][int]$Parallel = 4,
    [string]$CMakeExecutable,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ((Test-Path variable:args) -and $args.Count) { throw 'Unexpected arguments; use named recipe parameters.' }

function Assert-IfcNoReparse([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Path crosses a link or reparse point: $Path"
            }
        }
        $current = Split-Path -Parent $current
    }
}

function Get-IfcLocalDirectoryPath([string]$Path) {
    # Reject ambiguous spellings before normalization, including UNC/device
    # namespaces, traversal, alternate streams and Windows-trimmed names.
    if ($Path -notmatch '^[A-Za-z]:[\\/]' -or $Path -match '[;\x00-\x1f<>"|?*]' -or $Path.Substring(2).Contains(':')) {
        throw 'Path must be an absolute local directory without ambiguous characters.'
    }
    $segments = $Path.Substring(3).TrimEnd('\', '/') -split '[\\/]'
    foreach ($segment in $segments) {
        if (!$segment -or $segment -in @('.', '..') -or $segment -match '[. ]$' -or
            $segment -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw 'Path must be an unambiguous local directory without traversal or reserved names.'
        }
    }
    $normalized = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    Assert-IfcNoReparse $normalized
    return $normalized
}

function Assert-IfcCandidateRoot([string]$Candidate, [string]$Workspace, [string[]]$SdkRoots = @()) {
    $candidatePath = Get-IfcLocalDirectoryPath $Candidate
    if ($candidatePath.Length -gt 120) { throw 'BuildRoot must be a short local directory, at most 120 characters.' }
    foreach ($inputRoot in (@($Workspace) + $SdkRoots)) {
        $inputPath = [IO.Path]::GetFullPath($inputRoot).TrimEnd('\', '/')
        if ($candidatePath.Equals($inputPath, [StringComparison]::OrdinalIgnoreCase) -or
            $candidatePath.StartsWith($inputPath + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $inputPath.StartsWith($candidatePath + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'BuildRoot must not overlap the workspace, selected SDK inputs or product runtime.'
        }
    }
    Assert-IfcNoReparse $candidatePath
    if (Test-Path -LiteralPath $candidatePath) { throw 'BuildRoot already exists; use a fresh path. Existing caches and foreign outputs are preserved.' }
}

function Get-IfcSdkRoots([string]$Workspace, [string]$KernelRoot, [string]$SupportRoot) {
    if (!$KernelRoot) { $KernelRoot = Join-Path $Workspace '.deps/ifc-kernel/x64-windows-ifc-static' }
    if (!$SupportRoot) { $SupportRoot = Join-Path $Workspace '.deps/ifc-support/x64-windows-ifc-static' }
    $roots = [ordered]@{}
    foreach ($entry in @(@{ name = 'kernel'; path = $KernelRoot }, @{ name = 'support'; path = $SupportRoot })) {
        $prefix = Get-IfcLocalDirectoryPath $entry.path
        if (!(Test-Path -LiteralPath $prefix -PathType Container)) { throw "Selected $($entry.name) SDK prefix is missing: $prefix" }
        $status = Join-Path (Split-Path -Parent $prefix) 'vcpkg/status'
        Assert-IfcNoReparse $status
        if (!(Test-Path -LiteralPath $status -PathType Leaf)) { throw "Selected $($entry.name) SDK installed status is missing: $status" }
        $roots[$entry.name] = $prefix
        $roots["$($entry.name)_status"] = $status
    }
    return $roots
}

function Get-IfcBuildEnvironment([System.Collections.IDictionary]$Source, [string[]]$OptionNames) {
    $clean = @{}
    foreach ($entry in $Source.GetEnumerator()) {
        $name = [string]$entry.Key
        if ($name -match '^(?:CMAKE.*|VCPKG.*|BOOST.*|Boost.*|OCC_.*|EIGEN.*|PYTHON.*|SWIG.*|CGAL.*|GMP.*|MPFR.*|HDF5.*|LIBXML.*|OPENCOLLADA.*|PCRE.*|USD.*|CONDA.*|CC|CXX|CFLAGS|CXXFLAGS|CPPFLAGS|LDFLAGS|CL|_CL_|LINK|_LINK_|LIB|LIBPATH|INCLUDE)$' -or $name -in $OptionNames) { continue }
        $clean[$name] = [string]$entry.Value
    }
    # Git queries cannot trigger global/system configuration, hooks or fetching.
    foreach ($name in @($clean.Keys)) { if ($name -like 'GIT_*') { $clean.Remove($name) } }
    $clean['GIT_CONFIG_NOSYSTEM'] = '1'; $clean['GIT_CONFIG_GLOBAL'] = 'NUL'
    $clean['GIT_TERMINAL_PROMPT'] = '0'; $clean['GIT_OPTIONAL_LOCKS'] = '0'
    $clean['GIT_NO_LAZY_FETCH'] = '1'; $clean['GIT_NO_REPLACE_OBJECTS'] = '1'
    return $clean
}

function Get-IfcConfigureArguments([string]$Source, [string]$Candidate, [string]$Kernel, [string]$Support, [string]$Python, [string]$Swig, [string]$VisualStudio) {
    $Source = $Source.Replace('\', '/'); $Candidate = $Candidate.Replace('\', '/')
    $Kernel = $Kernel.Replace('\', '/'); $Support = $Support.Replace('\', '/')
    $Python = $Python.Replace('\', '/'); $Swig = $Swig.Replace('\', '/'); $VisualStudio = $VisualStudio.Replace('\', '/')
    $off = @('MINIMAL_BUILD', 'BUILD_SHARED_LIBS', 'BUILD_CONVERT', 'BUILD_GEOMSERVER', 'BUILD_EXAMPLES', 'BUILD_DOCUMENTATION', 'BUILD_IFCMAX', 'BUILD_QTVIEWER', 'BUILD_PACKAGE', 'WITH_CGAL', 'COLLADA_SUPPORT', 'GLTF_SUPPORT', 'HDF5_SUPPORT', 'WITH_PROJ', 'IFCXML_SUPPORT', 'USD_SUPPORT', 'CITYJSON_SUPPORT', 'WITH_RELATIONSHIP_VALIDATION', 'USE_MMAP', 'USE_VLD', 'WASM_BUILD', 'ADD_COMMIT_SHA', 'MSVC_PARALLEL_BUILD', 'ENABLE_BUILD_OPTIMIZATIONS')
    @('-S', "$Source/cmake", '-B', "$Candidate/build", '-G', 'Visual Studio 17 2022', '-A', 'x64',
      "-DCMAKE_GENERATOR_INSTANCE=$VisualStudio", '-DCMAKE_CONFIGURATION_TYPES=Release',
      '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL', '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
      '-DBUILD_IFCGEOM=ON', '-DBUILD_IFCPYTHON=ON', '-DWITH_OPENCASCADE=ON',
      '-DSCHEMA_VERSIONS=2x3;4;4x1;4x2;4x3;4x3_tc1;4x3_add1;4x3_add2',
      "-DOCC_INCLUDE_DIR=$Kernel/include/opencascade", "-DOCC_LIBRARY_DIR=$Kernel/lib",
      # Upstream OCCT_STATIC enables GNU archive-group flags on Windows.
      # Static .lib inputs and upstream HAVE_NO_DLL provide MSVC static linking.
      '-DOCCT_STATIC=OFF', "-DBOOST_ROOT=$Support", "-DBOOST_LIBRARYDIR=$Support/lib",
      "-DBoost_INCLUDE_DIR=$Support/include", '-DBoost_NO_SYSTEM_PATHS=ON', '-DBoost_NO_BOOST_CMAKE=OFF',
      "-DBoost_DIR=$Support/share/boost",
      '-DBoost_USE_STATIC_LIBS=ON', '-DBoost_USE_STATIC_RUNTIME=OFF', "-DEIGEN_DIR=$Support/include/eigen3",
      "-DPYTHON_EXECUTABLE=$Python/python.exe", "-DPYTHON_INCLUDE_DIR=$Python/include", "-DPYTHON_LIBRARY=$Python/libs/python313.lib",
      "-DSWIG_EXECUTABLE=$Swig/swig.exe", "-DSWIG_DIR=$Swig/Lib",
      "-DCMAKE_PREFIX_PATH=$Kernel;$Support", '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
      '-DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF', '-DCCACHE_FOUND=OFF',
      "-DCMAKE_INSTALL_PREFIX=$Candidate/uninstalled", "-DPYTHON_MODULE_INSTALL_DIR=$Candidate/uninstalled/python") +
      @($off | ForEach-Object { "-D$_=OFF" })
}

function Assert-IfcConfiguredCache([string]$Path, [string[]]$Configuration, [string]$Candidate, [string]$Source, [string]$Kernel, [string]$Support) {
    $cache = @{}
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -match '^([^/#][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    # Check every explicit cache setting, including the full schema set and
    # paths. Normalize separators only; do not accept another prefix/version.
    foreach ($argument in $Configuration) {
        if ($argument -match '^-D([^=]+)=(.*)$') {
            $name = $Matches[1]; $expected = $Matches[2].Replace('\', '/')
            if (!$cache.ContainsKey($name) -or $cache[$name].Replace('\', '/') -cne $expected) { throw "Configured cache differs for $name." }
        }
    }
    foreach ($entry in @{ CMAKE_HOME_DIRECTORY = "$Source/cmake"; CMAKE_CACHEFILE_DIR = "$Candidate/build" }.GetEnumerator()) {
        if (!$cache.ContainsKey($entry.Key) -or $cache[$entry.Key].Replace('\', '/') -ine $entry.Value.Replace('\', '/')) { throw "Configured cache root differs for $($entry.Key)." }
    }
    if ($cache['CMAKE_GENERATOR'] -ne 'Visual Studio 17 2022' -or $cache['CMAKE_GENERATOR_PLATFORM'] -ne 'x64') { throw 'Configured generator differs from VS 2022 x64.' }
    if ($cache['CMAKE_CXX_FLAGS'] -notmatch '(?:^|\s)/EHsc(?:\s|$)' -or ($cache['CMAKE_CXX_FLAGS'] + ' ' + $cache['CMAKE_CXX_FLAGS_RELEASE']) -match '(?:^|\s)/MT(?:d)?(?:\s|$)') { throw 'CMake lost /EHsc or selected a static CRT.' }
    foreach ($entry in $cache.GetEnumerator()) {
        if ($entry.Key -match '^Boost_.*LIBRARY_(?:RELEASE|DEBUG)$' -and $entry.Value -notmatch 'NOTFOUND$') {
            if (!$entry.Value.Replace('\', '/').StartsWith($Support.Replace('\', '/') + '/lib/', [StringComparison]::OrdinalIgnoreCase)) { throw "Boost library escaped controlled support prefix: $($entry.Key)" }
        }
    }
    if (!$cache.ContainsKey('libTKernel') -or $cache['libTKernel'].Replace('\', '/') -ine "$Kernel/lib/TKernel.lib".Replace('\', '/')) { throw 'OCCT library escaped controlled kernel prefix.' }
}

function Assert-IfcSdkStatus([string]$Path, [string]$Package, [string]$Version, [int]$PortVersion = -1) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "SDK installed status is missing: $Path" }
    $found = @()
    foreach ($paragraph in ((Get-Content -LiteralPath $Path -Raw) -split '\r?\n\s*\r?\n')) {
        $fields = @{}
        foreach ($line in ($paragraph -split '\r?\n')) { if ($line -match '^([A-Za-z-]+): (.*)$') { $fields[$Matches[1]] = $Matches[2] } }
        if ($fields['Package'] -eq $Package -and $fields['Architecture'] -eq 'x64-windows-ifc-static' -and $fields['Status'] -eq 'install ok installed') { $found += ,$fields }
    }
    if ($found.Count -ne 1) { throw "Expected one installed $Package for x64-windows-ifc-static." }
    if ($found[0]['Version'] -ne $Version -or ($PortVersion -ge 0 -and $found[0]['Port-Version'] -ne "$PortVersion")) { throw "SDK package version mismatch: $Package" }
}

function Get-IfcDerivedSourceArguments([string]$Workspace, [string]$Candidate) {
    return @('-I', '-B', (Join-Path $Workspace 'scripts/prepare_ifc_derived_source.py'),
        '--workspace', $Workspace, '--output', (Join-Path $Candidate 'source'),
        '--manifest', (Join-Path $Candidate 'source-derivation.json'))
}

function Invoke-IfcCommand([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $record = [ordered]@{ name = $Name; executable = $Executable; arguments = $Arguments; started_utc = [DateTime]::UtcNow.ToString('o'); exit_code = $null }
    $stdout = Join-Path $script:candidatePath "$Name.stdout.log"
    $stderr = Join-Path $script:candidatePath "$Name.stderr.log"
    $record['stdout'] = $stdout; $record['stderr'] = $stderr
    [void]$script:evidence.commands.Add($record)
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.WorkingDirectory = $script:projectRoot
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.Environment.Clear()
    foreach ($entry in $script:childEnvironment.GetEnumerator()) { $info.Environment[$entry.Key] = $entry.Value }
    $outStream = [IO.File]::Open($stdout, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $errStream = [IO.File]::Open($stderr, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info
    try {
        Write-Host "IFC candidate: $Name (logs in $script:candidatePath)"
        if (!$process.Start()) { throw "Could not start $Name" }
        $outTask = $process.StandardOutput.BaseStream.CopyToAsync($outStream)
        $errTask = $process.StandardError.BaseStream.CopyToAsync($errStream)
        $process.WaitForExit()
        [void]$outTask.GetAwaiter().GetResult(); [void]$errTask.GetAwaiter().GetResult()
        $record.exit_code = $process.ExitCode
        if ($process.ExitCode -ne 0) { throw "$Name failed with exit $($process.ExitCode); see $stdout and $stderr" }
    } finally {
        $record['finished_utc'] = [DateTime]::UtcNow.ToString('o')
        $outStream.Dispose(); $errStream.Dispose(); $process.Dispose()
    }
}

function Add-IfcFileEvidence([string]$Path) {
    Assert-IfcNoReparse $Path
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required input missing: $Path" }
    [void]$script:evidence.inputs.Add([ordered]@{ path = $Path; bytes = (Get-Item -LiteralPath $Path).Length; sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() })
}

function Assert-IfcCleanGitStatus([string]$Path) {
    if (![string]::IsNullOrWhiteSpace((Get-Content -LiteralPath $Path -Raw))) {
        throw 'vcpkg contains tracked edits; inputs are preserved.'
    }
}

$script:projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$sdkRoots = Get-IfcSdkRoots $script:projectRoot $KernelRoot $SupportRoot
Assert-IfcCandidateRoot $BuildRoot $script:projectRoot @($sdkRoots.kernel, $sdkRoots.support)
$script:candidatePath = Get-IfcLocalDirectoryPath $BuildRoot
$pristineSource = Join-Path $script:projectRoot '.deps/ifc-src'
$source = Join-Path $script:candidatePath 'source'
$kernel = $sdkRoots.kernel
$support = $sdkRoots.support
$python = Join-Path $script:projectRoot '.deps/cad-runtime/3.13.15'
$swig = Join-Path $script:projectRoot '.deps/ifc-tools/swigwin-4.3.1'
$vcpkg = Join-Path $script:projectRoot '.deps/vcpkg'
$lockPath = Join-Path $script:projectRoot 'third_party/ifc-source-lock.json'
$optionNames = @([regex]::Matches((Get-Content "$pristineSource/cmake/CMakeLists.txt" -Raw), '(?im)^option\(\s*(\w+)') | ForEach-Object { $_.Groups[1].Value })
$script:childEnvironment = Get-IfcBuildEnvironment ([Environment]::GetEnvironmentVariables()) $optionNames
$script:evidence = [ordered]@{ schema_version = 1; build_qualified = $false; source_closure_qualified = $false; product_runtime_replaced = $false; state = 'preflight'; started_utc = [DateTime]::UtcNow.ToString('o'); workspace = $script:projectRoot; build_root = $script:candidatePath; configuration = 'Release'; inputs = [Collections.Generic.List[object]]::new(); commands = [Collections.Generic.List[object]]::new(); outputs = @() }
$script:evidence['sdk_roots'] = [ordered]@{ kernel = $kernel; support = $support }
# No directory/cache is reused, deleted or reset. Reserve this run exclusively.
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $script:candidatePath))
$reservation = "$script:candidatePath.reservation"
$reservationStream = [IO.File]::Open($reservation, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $sdkRoots = Get-IfcSdkRoots $script:projectRoot $kernel $support
    Assert-IfcCandidateRoot $script:candidatePath $script:projectRoot @($kernel, $support)
    [void][IO.Directory]::CreateDirectory($script:candidatePath)
    try {
        foreach ($path in @($PSCommandPath, $lockPath, "$script:projectRoot/scripts/prepare_ifc_source.py", "$script:projectRoot/.deps/ifc-source-preparation.json", "$script:projectRoot/third_party/ifc-source/vcpkg.json", "$script:projectRoot/third_party/ifc-source/support/vcpkg.json", "$script:projectRoot/third_party/ifc-source/triplets/x64-windows-ifc-static.cmake", "$python/python.exe", "$python/libs/python313.lib", "$python/include/Python.h", "$python/include/patchlevel.h", "$python/runtime-manifest.json", "$swig/swig.exe")) { Add-IfcFileEvidence $path }
        $lock = Get-Content $lockPath -Raw | ConvertFrom-Json
        if ($lock.build_qualified -ne $false -or $lock.source_closure_qualified -ne $false) { throw 'Source lock must remain unqualified.' }
        Invoke-IfcCommand 'source-check' "$python/python.exe" @('-I', '-B', "$script:projectRoot/scripts/prepare_ifc_source.py", '--offline', '--check')
        foreach ($path in @("$script:projectRoot/scripts/prepare_ifc_derived_source.py", "$script:projectRoot/third_party/ifc-source/patches/opaque-coordinate-output.i")) { Add-IfcFileEvidence $path }
        Invoke-IfcCommand 'source-derive' "$python/python.exe" (Get-IfcDerivedSourceArguments $script:projectRoot $script:candidatePath)
        $script:evidence['source_derivation'] = Join-Path $script:candidatePath 'source-derivation.json'
        Add-IfcFileEvidence $script:evidence.source_derivation
        Invoke-IfcCommand 'python-version' "$python/python.exe" @('-I', '-B', '-c', 'import sys,struct; assert sys.version_info[:3] == (3,13,15) and struct.calcsize("P") == 8; print(sys.version)')
        if ((Get-Content "$python/include/patchlevel.h" -Raw) -notmatch '#define\s+PY_VERSION\s+"3\.13\.15"') { throw 'CPython development header version differs from 3.13.15.' }
        Invoke-IfcCommand 'swig-version' "$swig/swig.exe" @('-version')
        if ((Get-Content "$script:candidatePath/swig-version.stdout.log" -Raw) -notmatch 'SWIG Version 4\.3\.1\b') { throw 'Expected SWIG 4.3.1.' }

        Assert-IfcNoReparse "$vcpkg/.git"
        if (!(Test-Path "$vcpkg/.git" -PathType Container) -or (Test-Path "$vcpkg/.git/commondir")) { throw 'vcpkg requires standalone controlled Git metadata.' }
        foreach ($config in @("$vcpkg/.git/config", "$vcpkg/.git/config.worktree")) {
            if ((Test-Path $config) -and (Get-Content $config -Raw) -match '(?im)^\s*\[\s*(?:filter|include|includeIf)\b') { throw 'vcpkg Git config contains filters or external includes.' }
        }
        $git = (Get-Command git.exe -CommandType Application | Select-Object -First 1).Source
        Add-IfcFileEvidence $git
        $gitOptions = @('--no-replace-objects', '-c', 'core.fsmonitor=false', '-c', 'core.untrackedCache=false', '-c', 'core.hooksPath=NUL', '-C', $vcpkg)
        Invoke-IfcCommand 'vcpkg-head' $git ($gitOptions + @('rev-parse', 'HEAD'))
        $managerHead = (Get-Content "$script:candidatePath/vcpkg-head.stdout.log" -Raw).Trim()
        if ($managerHead -ne $lock.swig.hash_provenance.revision) { throw "Pinned vcpkg source HEAD differs: $managerHead" }
        Invoke-IfcCommand 'vcpkg-origin' $git ($gitOptions + @('config', '--local', '--no-includes', '--get-all', 'remote.origin.url'))
        if ((Get-Content "$script:candidatePath/vcpkg-origin.stdout.log" -Raw).Trim() -ne $lock.swig.hash_provenance.repository) { throw 'vcpkg origin differs from the locked official source.' }
        Invoke-IfcCommand 'vcpkg-status' $git ($gitOptions + @('status', '--porcelain=v1', '--untracked-files=no', '--ignore-submodules=all'))
        Assert-IfcCleanGitStatus "$script:candidatePath/vcpkg-status.stdout.log"
        $script:evidence['vcpkg_revision'] = $managerHead

        $kernelStatus = $sdkRoots.kernel_status
        $supportStatus = $sdkRoots.support_status
        Assert-IfcSdkStatus $kernelStatus 'opencascade' '7.8.1' 1
        Assert-IfcSdkStatus $supportStatus 'eigen3' '3.3.9' 1
        $supportManifest = Get-Content "$script:projectRoot/third_party/ifc-source/support/vcpkg.json" -Raw | ConvertFrom-Json
        foreach ($package in $supportManifest.dependencies) { if ($package -like 'boost-*') { Assert-IfcSdkStatus $supportStatus $package '1.86.0' } }
        foreach ($path in @($kernelStatus, $supportStatus, "$kernel/include/opencascade/Standard_Version.hxx", "$support/include/boost/version.hpp", "$support/include/eigen3/Eigen/src/Core/util/Macros.h")) { Add-IfcFileEvidence $path }
        if ((Get-Content "$kernel/include/opencascade/Standard_Version.hxx" -Raw) -notmatch '#define\s+OCC_VERSION_COMPLETE\s+"7\.8\.1"') { throw 'Expected OCCT 7.8.1 headers.' }
        if ((Get-Content "$support/include/boost/version.hpp" -Raw) -notmatch '#define\s+BOOST_VERSION\s+108600\b') { throw 'Expected Boost 1.86 headers.' }
        $eigenHeader = Get-Content "$support/include/eigen3/Eigen/src/Core/util/Macros.h" -Raw
        foreach ($macro in @('EIGEN_WORLD_VERSION\s+3', 'EIGEN_MAJOR_VERSION\s+3', 'EIGEN_MINOR_VERSION\s+9')) { if ($eigenHeader -notmatch "#define\s+$macro\b") { throw 'Expected Eigen 3.3.9 headers.' } }
        # Upstream derives all OCCT link paths from TKernel; check the full set.
        foreach ($library in @('TKernel', 'TKMath', 'TKBRep', 'TKGeomBase', 'TKGeomAlgo', 'TKG3d', 'TKG2d', 'TKShHealing', 'TKTopAlgo', 'TKMesh', 'TKPrim', 'TKBool', 'TKBO', 'TKFillet', 'TKXSBase', 'TKOffset', 'TKHLR', 'TKBin', 'TKDESTEP', 'TKDEIGES')) { Add-IfcFileEvidence "$kernel/lib/$library.lib" }
        $boostLibraries = @(Get-ChildItem "$support/lib" -Filter '*boost*.lib' -File)
        if (!$boostLibraries.Count) { throw 'Static Boost Release libraries are missing.' }
        foreach ($library in $boostLibraries) { Add-IfcFileEvidence $library.FullName }
        if (@(Get-ChildItem "$kernel/bin" -Filter 'TK*.dll' -ErrorAction SilentlyContinue).Count) { throw 'OCCT prefix contains DLLs; expected static kernel.' }

        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        Invoke-IfcCommand 'visual-studio' $vswhere @('-latest', '-version', '[17.0,18.0)', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
        $vs = (Get-Content "$script:candidatePath/visual-studio.stdout.log" -Raw).Trim()
        if (!$vs -or !(Test-Path -LiteralPath $vs -PathType Container)) { throw 'Visual Studio 2022 with the x64 C++ workload is required.' }
        if (!$CMakeExecutable) { $CMakeExecutable = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' }
        $CMakeExecutable = [IO.Path]::GetFullPath($CMakeExecutable)
        Add-IfcFileEvidence $CMakeExecutable
        Invoke-IfcCommand 'cmake-version' $CMakeExecutable @('--version')
        $cmakeVersion = [regex]::Match((Get-Content "$script:candidatePath/cmake-version.stdout.log" -Raw), 'cmake version (\d+\.\d+\.\d+)')
        if (!$cmakeVersion.Success -or [version]$cmakeVersion.Groups[1].Value -lt [version]'3.31.0') { throw 'Pinned upstream policies require CMake 3.31 or newer.' }
        $configuration = @(Get-IfcConfigureArguments $source $script:candidatePath $kernel $support $python $swig $vs)
        $script:evidence['configure_arguments'] = $configuration
        $script:evidence.state = 'configuring'
        Invoke-IfcCommand 'configure' $CMakeExecutable $configuration
        Add-IfcFileEvidence "$script:candidatePath/build/CMakeCache.txt"
        Assert-IfcConfiguredCache "$script:candidatePath/build/CMakeCache.txt" $configuration $script:candidatePath $source $kernel $support
        foreach ($project in (Get-ChildItem "$script:candidatePath/build" -Filter '*.vcxproj' -Recurse -File)) {
            if ((Get-Content $project.FullName -Raw) -match '(?:-Wl,|--start-group|--end-group)') { throw "GNU link flags appeared in MSVC project: $($project.FullName)" }
        }
        if (!$ConfigureOnly) {
            $script:evidence.state = 'building'
            Invoke-IfcCommand 'build-release' $CMakeExecutable @('--build', "$script:candidatePath/build", '--config', 'Release', '--target', 'ifcopenshell_wrapper', '--parallel', "$Parallel")
            $outputs = @(Get-ChildItem "$script:candidatePath/build/ifcwrap/Release" -Filter '*.pyd' -File)
            if ($outputs.Count -ne 1) { throw 'Expected exactly one candidate Python extension.' }
            $script:evidence.outputs = @($outputs | ForEach-Object { [ordered]@{ path = $_.FullName; bytes = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
            $generatedWrapper = Get-Item -LiteralPath "$script:candidatePath/build/ifcwrap/ifcopenshell_wrapper.py"
            Assert-IfcNoReparse $generatedWrapper.FullName
            $script:evidence['generated_wrapper'] = [ordered]@{ path = $generatedWrapper.FullName;
                bytes = $generatedWrapper.Length;
                sha256 = (Get-FileHash -LiteralPath $generatedWrapper.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
        $script:evidence.state = if ($ConfigureOnly) { 'configured' } else { 'built-unqualified' }
    } catch {
        $script:evidence.state = 'failed'; $script:evidence['error'] = $_.Exception.Message
        throw
    } finally {
        $script:evidence['finished_utc'] = [DateTime]::UtcNow.ToString('o')
        $script:evidence['logs'] = @(Get-ChildItem $script:candidatePath -Filter '*.log' -File | ForEach-Object { [ordered]@{ path = $_.FullName; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
        $script:evidence | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$script:candidatePath/build-evidence.json" -Encoding utf8
        Write-Host "Candidate evidence: $script:candidatePath/build-evidence.json"
    }
} finally {
    $reservationStream.Dispose()
    Remove-Item -LiteralPath $reservation
}
