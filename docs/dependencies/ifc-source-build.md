# Controlled IFC source build inputs

`scripts/prepare_ifc_source.py` prepares a candidate source tree and SWIG tool.
It does not compile, install or replace the CAD runtime. The existing locked
IfcOpenShell wheel has not been bound to this source revision; a successful
preparation cannot establish that wheel's corresponding source.

The lock is [ifc-source-lock.json](../../third_party/ifc-source-lock.json).
It pins the official IfcOpenShell repository to
`ff3c5b849eee2ef6343b537c885b971ae6bba452`, three source submodules to their
gitlink revisions, and the official Windows SWIG 4.3.1 ZIP by SHA-512. The SWIG
checksum comes from the recorded vcpkg manager revision and its
`vcpkg_find_acquire_program(SWIG).cmake`; it is independent of the local archive.

Run from the repository root with Python 3.11 or newer and Git available:

```powershell
python scripts/prepare_ifc_source.py
# With the exact source/submodules and SWIG ZIP already cached:
python scripts/prepare_ifc_source.py --offline
python scripts/prepare_ifc_source.py --offline --check
python tests/test_prepare_ifc_source.py
```

Preparation uses fixed local paths:

| Input or result | Location |
| --- | --- |
| IfcOpenShell checkout | `.deps/ifc-src` |
| Prepared source submodules | Their locked paths inside `.deps/ifc-src` |
| SWIG cached ZIP | `.cache/ifc-source/swigwin-4.3.1.zip` |
| Extracted SWIG | `.deps/ifc-tools/swigwin-4.3.1` |
| Input evidence | `.deps/ifc-source-preparation.json` |

The online command clones only missing repositories into private stages and
checks out the exact commits. It does not fetch, reset, clean or update an
existing checkout. An empty gitlink placeholder can receive its prepared
submodule; other foreign content is rejected and retained. Cached/offline mode
never downloads or clones, and `--check` requires the existing preparation
manifest and tools. Git queries disable hooks, filesystem monitors, global
configuration, lazy object fetching and replacement objects. Standalone Git
directories and normal submodule gitfiles are accepted; linked worktrees and
`commondir` redirection are rejected. Both `config` and `config.worktree` are
inspected for executable clean filters and config includes before any Git query
for that repository. Index `assume-unchanged` and `skip-worktree` flags are
rejected before status, since they can hide altered or missing source files.
Status never recurses into child repositories: selected children receive their
own metadata, identity, index and status checks. Nonempty unselected gitlinks
are rejected and preserved.

Existing sources must have the exact origin, HEAD and gitlinks, with no dirty,
untracked or ignored content. Links and Windows reparse points are rejected in
source trees, metadata, cache paths and tools. SWIG extraction uses one immutable
checksum-verified byte snapshot, validates the complete ZIP table before writes,
and rejects unsafe Windows names, links, nonregular members, encrypted members,
duplicate and ancestor/case collisions. Compressed size, expanded size and
member count are bounded. Only invocation-owned scratch paths are cleaned up.
Existing tool bytes must match the locked archive; editing their manifest cannot
forge that comparison. Unexpected existing files or manifests are preserved.

The generated evidence records lock and archive hashes, official origins and
exact revisions, the source file SHA-256 table (excluding Git metadata), and
the archive-derived SWIG file table. Both `build_qualified` and
`source_closure_qualified` remain `false`. It is a selected-input preparation
record, not a complete transitive source inventory or license conclusion.

## Candidate build configuration

The pinned CMake entrypoint is `.deps/ifc-src/cmake/CMakeLists.txt`. Keep
`BUILD_IFCGEOM=ON`, `BUILD_IFCPYTHON=ON`, `WITH_OPENCASCADE=ON`,
`MINIMAL_BUILD=OFF` and `BUILD_SHARED_LIBS=OFF`. `MINIMAL_BUILD=ON` explicitly
disables the Python wrapper. Preserve the default schema set; reducing schemas
would change file compatibility.

For the candidate OCCT-only wrapper, disable these ancillary switches:

```text
BUILD_CONVERT=OFF BUILD_GEOMSERVER=OFF BUILD_EXAMPLES=OFF
BUILD_DOCUMENTATION=OFF BUILD_IFCMAX=OFF BUILD_QTVIEWER=OFF BUILD_PACKAGE=OFF
WITH_CGAL=OFF COLLADA_SUPPORT=OFF GLTF_SUPPORT=OFF HDF5_SUPPORT=OFF
WITH_PROJ=OFF IFCXML_SUPPORT=OFF USD_SUPPORT=OFF CITYJSON_SUPPORT=OFF
WITH_RELATIONSHIP_VALIDATION=OFF USE_MMAP=OFF USE_VLD=OFF WASM_BUILD=OFF
```

At this revision, `src/svgfill` is compiled only inside `WITH_CGAL`; its locked
preparation is retained without claiming it is required by the OCCT-only build.
The `mvd` and `simple_spf` submodules contribute Python package source covered
by the wrapper's recursive install glob. They are package features rather than
C++ link dependencies. Test fixtures, documentation assets, converter CityJSON
and Pyodide submodules are outside this selected preparation. The install glob
also needs a reviewed packaging filter for nested Git metadata; do not treat
an unreviewed upstream install tree as the finished runtime payload.

The first candidate used the application's OCCT 8.0.1, Boost 1.92 and
Eigen 5 prefix. Configuration preserved all eight schemas, but compilation
failed on IFC's uses of `Handle_*` aliases removed in OCCT 8. The task-owned
build was stopped after that definite compiler failure. Its log is retained at
`C:/Build/Vertex/ifc-candidate/release-source-build-2.log` on the development
host. Do not reuse that candidate's CMake cache or its replacement
`CMAKE_CXX_FLAGS`: the latter also omitted the default `/EHsc` option.

The replacement candidate uses separate pinned dependency manifests:

| Inputs | Manifest | Installed prefix |
| --- | --- | --- |
| OCCT 7.8.1, port revision 1 | `third_party/ifc-source/vcpkg.json` | `.deps/ifc-kernel/x64-windows-ifc-static` |
| Boost 1.86, Eigen 3.3.9 | `third_party/ifc-source/support/vcpkg.json` | `.deps/ifc-support/x64-windows-ifc-static` |

Both use `third_party/ifc-source/triplets/x64-windows-ifc-static.cmake`:
Release static libraries with the dynamic MSVC CRT. A static candidate kernel
avoids loading OCCT 7 and 8 DLLs with colliding names; the resulting wrapper
still needs actual import and geometry verification.

With the pinned vcpkg checkout prepared, run these installs serially. vcpkg
holds a shared package lock even when install prefixes differ.
Its source HEAD must be `0397b75952a7c874b68a7d4f9baf8d3c5951b371`;
the support manifest's older baseline selects Boost 1.86 independently of the
manager checkout. Do not run these installs concurrently or reuse a buildtree
from another dependency configuration.

```powershell
$ifcRoot = (Resolve-Path .).Path
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_MAX_CONCURRENCY = '8'
& .deps/vcpkg/vcpkg.exe install "--x-manifest-root=$ifcRoot/third_party/ifc-source" `
  "--overlay-triplets=$ifcRoot/third_party/ifc-source/triplets" `
  --triplet=x64-windows-ifc-static "--x-install-root=$ifcRoot/.deps/ifc-kernel" `
  --x-buildtrees-root=C:/Build/Vertex/ifc-kernel-build --disable-metrics
if ($LASTEXITCODE -ne 0) { throw 'IFC kernel dependency build failed' }
& .deps/vcpkg/vcpkg.exe install "--x-manifest-root=$ifcRoot/third_party/ifc-source/support" `
  "--overlay-triplets=$ifcRoot/third_party/ifc-source/triplets" `
  --triplet=x64-windows-ifc-static "--x-install-root=$ifcRoot/.deps/ifc-support" `
  --x-buildtrees-root=C:/Build/Vertex/ifc-support-build --disable-metrics
if ($LASTEXITCODE -ne 0) { throw 'IFC support dependency build failed' }
```

Use a fresh short candidate build path, all eight default schemas, the switches
above, CPython 3.13.15's executable/include/library in
`.deps/cad-runtime/3.13.15`, and SWIG 4.3.1 from the prepared tool directory.
Resolve OCC include/library paths only from the isolated kernel prefix,
Boost and Eigen only from the isolated support prefix. Preserve MSVC's
exception-handling and dynamic CRT settings. The application's newer native
prefix must not supply fallback libraries.

Actual source preparation and offline verification have passed on the
development host. The separate OCCT 7.8.1 static Release SDK build completed
successfully, as did the Boost 1.86/Eigen 3.3.9 support SDK install. The
replacement IFC wrapper configured successfully with all eight schemas and
entered Release compilation in `C:/Build/Vertex/ifc-candidate-784`, then failed
because `SvgSerializer.cpp` requires `boost/format.hpp`. The support manifest
now explicitly includes `boost-format`; its pinned 1.86.0 support SDK install
passed. The fresh `C:/Build/Vertex/ifc-candidate-785` configured successfully
and compiled the schema/geometry dependencies, but the Python wrapper failed
with three MSVC C2248 errors: generated code attempts to copy
`SwigValueWrapper<IfcGeom::OpaqueCoordinate<3>>` and
`SwigValueWrapper<IfcGeom::OpaqueCoordinate<4>>`, whose copy constructors are
private. Its terminal `build-evidence.json` records `state: failed` and a
nonzero `build-release` exit. It cannot be packaged as a successful candidate.
Both failed candidates' logs and `build-evidence.json` are preserved. Diagnose
the wrapper generation before starting another full build; no runtime
replacement or source qualification follows from compiled intermediate libraries.
These manifests and commands describe
candidate inputs, not a completed source-build
qualification. Earlier failed preflight/configure attempts are retained in
their separate candidate directories; they are not successful build evidence.

The wrapper failure was reproduced independently with SWIG 4.3.1 and the
candidate's MSVC C++17/Python 3.13 headers. The default output allocation
passes a `SwigValueWrapper` directly to `OpaqueCoordinate`'s unrestricted
variadic constructor, selecting an inaccessible wrapper copy. A scoped output
typemap using SWIG's `%new_copy` casts to the coordinate's const reference and
selects its public deep-copy constructor:

```swig
%typemap(out, noblock=1)
IfcGeom::OpaqueCoordinate<3>,
IfcGeom::OpaqueCoordinate<4> {
  $result = SWIG_NewPointerObj(
      %new_copy($1, $1_ltype), $&descriptor,
      SWIG_POINTER_OWN | %newpointer_flags);
}
```

The small generated wrapper reproduced the same three C2248 errors without
this typemap and compiled with it. Separate executable probes passed deep-copy
and destruction checks for both dimensions, with and without
`/Zc:__cplusplus`. Local probe sources/logs are under
`artifacts/ifc-wrapper-probe`. This is causal compiler evidence, not a full
IFC build, link, import, or geometry qualification. The correction still needs
to enter a hash-bound derived source input before the next full candidate;
the pristine locked checkout and both failed builds remain unchanged.

After both SDK installs finish, run the actual candidate recipe from the
repository root with PowerShell 7:

```powershell
pwsh -NoProfile -File scripts/build-ifc-source.ps1 `
  -BuildRoot C:/Build/Vertex/ifc-candidate-7 -Parallel 4
# Optional configure-only probe (also requires a new directory):
pwsh -NoProfile -File scripts/build-ifc-source.ps1 `
  -BuildRoot C:/Build/Vertex/ifc-configure-7 -ConfigureOnly
pwsh -NoProfile -File tests/test_build_ifc_source.ps1
```

Omitting `-BuildRoot` selects a unique short path beneath `C:/Build/Vertex`.
The root must be outside this workspace, at most 120 characters, and entirely
new. Existing directories, CMake caches and foreign outputs are rejected and
preserved; rerunning requires another new root. No cleanup or resume operation
is offered. `-CMakeExecutable` can select an explicit CMake executable; by
default the recipe uses the discovered Visual Studio 2022 CMake. CMake 3.31 or
newer is required by policies used in this pinned source.

The script invokes the locked Python executable for
`prepare_ifc_source.py --offline --check`, checks the official vcpkg origin,
pinned HEAD and tracked cleanliness, and requires installed package metadata
and headers for OCCT 7.8.1#1, every declared Boost package at 1.86.0, and Eigen
3.3.9#1 in their dedicated prefixes. It checks all OCCT libraries named by
upstream CMake, the Python executable/header version and 64-bit architecture,
and SWIG 4.3.1. It removes inherited feature and dependency overrides from the
child environment, passes exact include/library/tool paths, and disables
Boost system search and CMake package registries. Boost is resolved through
the dedicated SDK's `share/boost/BoostConfig.cmake` exports, which name its
static libraries directly instead of guessing their filename prefixes.
The caller's environment is unchanged.

Configuration uses Visual Studio 17 2022 x64, Release, static IFC libraries,
the dynamic MSVC CRT and all eight schemas. It preserves default
`CMAKE_CXX_FLAGS`, including `/EHsc`, and verifies the resulting cache settings,
cache/source roots and discovered Boost/OCCT libraries before compilation.
`OCCT_STATIC=OFF` avoids upstream's GNU archive-group flags on MSVC; the
dedicated SDK supplies static `.lib` files and upstream sets `HAVE_NO_DLL`.
The script rejects generated MSVC projects containing GNU linker flags.
The build targets `ifcopenshell_wrapper` and its geometry dependencies. The
extension remains under `<BuildRoot>/build/ifcwrap/Release`; generated wrapper
Python source remains under `<BuildRoot>/build/ifcwrap`. The recipe never runs
`cmake --install`, copies packages, imports the candidate, or replaces the
runtime. Its candidate install destinations are precautionary only: the
upstream recursive Python install glob still needs a reviewed packaging
filter before any installation, including removal of nested `.git` metadata.

Each command runs without a visible child window and records separate stdout
and stderr logs, arguments, timestamps and exit codes. `build-evidence.json`
records success or failure, exact inputs and SHA-256 hashes, SDK status and
version headers, linked SDK library hashes, the configured cache, log hashes,
and the candidate extension hash after a successful build. The source
preparation manifest supplies the locked source file table. This evidence
does not inventory every transitive dependency source or assert license
closure. `build_qualified` and `source_closure_qualified` remain `false`, even
after compilation succeeds.

## Isolated Python candidate staging

After a successful `built-unqualified` build, `scripts/prepare_ifc_candidate.py`
can stage an import candidate outside the workspace and build directory:

```powershell
.deps/cad-runtime/3.13.15/python.exe -I -B scripts/prepare_ifc_candidate.py `
  --build-evidence C:/Build/Vertex/ifc-success/build-evidence.json `
  --output C:/Build/Vertex/ifc-success-package
.deps/cad-runtime/3.13.15/python.exe -I -B tests/test_prepare_ifc_candidate.py
```

Use the actual successful build path. The output must be fresh with an existing
parent. Failed, configured-only, or unfinished builds are rejected before output
creation. The helper checks the bound source snapshot, build inputs, configured
cache, CMake and Python commands, all eight schemas, Release/x64 settings, and
the extension's hash and structural platform fields. It copies the selected
Python package and submodules, generated wrapper, extension, original notices,
and local provenance; a final exact file/directory/hash check precedes manifest
publication. Git metadata, cache files, unreviewed source files, and reparse
paths are rejected. Partial output is preserved after a copy failure.

Static SDK inputs have separate bounds (2 GB per file, 16 GB aggregate); they
are verified but not copied into the Python package. Source and staged payloads
remain bounded at 512 MB per file and 2 GB aggregate. If the tracked build recipe
changed after invocation, `--recipe-snapshot` accepts only the original matching
bytes saved as `<BuildRoot>/build-recipe.ps1`.

This stage does not import or install the package, replace the product runtime,
create a wheel, or deliver the C++ corresponding source and dependency license
closure. The generated wrapper is hashed at staging rather than bound in the
original build evidence. All qualification flags remain false. Fourteen
synthetic tests pass on Python 3.12 and the locked 3.13.15 interpreter. The actual
failed candidate 784 is rejected without creating an output, and its bound
cache/CMake/Python coherence checks pass independently; neither result proves a
successful native package. No actual successful candidate has been staged yet.

Remaining evidence is a recorded successful Release wrapper/geometry build,
reviewed package contents and notices, source/dependency/license closure,
binary-to-source binding for the replacement, and existing IFC behavior checks
under the actual independent sandbox worker. Candidate outputs must stay
separate from `.deps/cad-runtime/3.13.15` and the installed application until
those checks justify replacement. No successful source build is claimed here.
