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

## Build configuration to qualify next

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
development host. The separate OCCT 7.8.1 SDK compile has started; the support
SDK and replacement IFC wrapper build remain pending. These manifests and
commands describe candidate inputs, not a completed source-build qualification.

Remaining evidence is a recorded successful Release wrapper/geometry build,
reviewed package contents and notices, source/dependency/license closure,
binary-to-source binding for the replacement, and existing IFC behavior checks
under the actual independent sandbox worker. Candidate outputs must stay
separate from `.deps/cad-runtime/3.13.15` and the installed application until
those checks justify replacement. No successful source build is claimed here.
