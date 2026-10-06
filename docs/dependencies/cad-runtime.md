# Bundled CAD library runtime

The application import worker embeds IfcOpenShell and ezdxf behind the bounded
C++ admission adapters. This bootstrap prepares their pinned dependency SDK;
its manifest describes preparation, not production qualification. Distribution
closure, source obligations and external-format qualification remain separate.

## Current selected source-built runtime

The current Release source batch selects
`.deps/cad-runtime/3.13.15-ifc-source-ff3c5b849eee` explicitly. It replaces only
the IfcOpenShell wheel cohort with the controlled eight-schema build from
revision `ff3c5b849eee2ef6343b537c885b971ae6bba452`; the interpreter and other
locked packages retain their verified original bytes. The extension SHA-256 is
`710c14599b1243d7a032668f6cc21796af2399c72b2a515f75e325d7a6f58cdf`
and the SDK manifest SHA-256 is
`606e69273eb7a1799b41b785af2f8ee4e278ce8b9bca9b7482529fb042d1a587`.
See [ifc-source-build.md](ifc-source-build.md) for original/derived sources,
build controls and the separately rebuilt Boost thread input.

Build and derive the matching packaging inputs with the same explicit selection:

```powershell
& scripts/build.ps1 -Desktop -Configuration Release -SkipTests `
  -CadRuntimeRoot .deps/cad-runtime/3.13.15-ifc-source-ff3c5b849eee

python -B scripts/prepare_cad_distribution.py `
  --runtime-root .deps/cad-runtime/3.13.15-ifc-source-ff3c5b849eee `
  --staged-root build/windows-release/cad-runtime `
  --selection-path build/windows-release/cad-runtime-selection.json `
  --corresponding-source-manifest .deps/source-closure/ifc-controlled/corresponding-source-paths-v2.json
```

The selected-runtime inspector verifies every SDK and staged file before the
bridge is compiled. CMake binds the manifest, controlled provenance and adapter
hashes into the worker. The packaging generator retains that exact selection,
rejects stale wheel metadata, and declares the verified preferred-source paths.
The materialized IFC source payload contains 4,744 original/derived/control
files; the current generated CAD payload contains 2,569 files. SDK probes and
actual worker results are recorded separately in
[the execution record](../delivery/progress.md). Neither a successful build nor
these counts establishes complete transitive source, licensing or offline
rebuild qualification. The previously installed candidate still uses its older
payload until a coherent replacement candidate is staged and checked.

The baseline bootstrap and earlier wheel observations below remain available
for reproducibility; they do not identify the current selected Release runtime.

`third_party/cad-runtime-lock.json` pins Windows x64 CPython 3.13.15, the matching
development package, IfcOpenShell 0.8.3.post2, ezdxf 1.4.3 and eleven total Python
distributions including all unconditional dependencies. Optional drawing/GUI and
development extras are excluded. The lock records official download URLs and
SHA-256 values. The CPython interpreter and source archive hashes were checked
against the [official release table](https://www.python.org/downloads/release/python-31315/).
Wheels use their release-specific PyPI checksums.

Prepare the cache and runtime explicitly:

```powershell
python scripts/bootstrap_cad_runtime.py
# With all sixteen pinned archives already in .deps/downloads/cad-runtime:
python scripts/bootstrap_cad_runtime.py --offline
python scripts/bootstrap_cad_runtime.py --offline --check
python scripts/check_cad_runtime.py --output artifacts/cad-runtime-probe.json
```

Normal CMake builds never download or invoke this bootstrap. It requires Python
3.11 or later as a developer tool. Application users will use the bundled runtime
after worker integration; they will not install Python or packages themselves.
The default prepared runtime is `.deps/cad-runtime/3.13.15`. Builds that need a
different locked runtime use a new child under `.deps/cad-runtime`; existing
targets are verified and never overwritten on mismatch.

CMake inspects the selected SDK's complete manifest-bound inventory before
configuration. `scripts/stage_cad_runtime.py` stages that exact inventory and
the workspace adapter instead of merging directories. Changed stages retain
one verified rollback; interrupted rotation uses a bounded identity-bound
transaction record. Unmarked legacy output is refused for a separate verified
migration. Build callers must freeze their inputs, serialize staging/build jobs
and keep the output unused during replacement.

An explicitly selected controlled source-built IFC SDK also requires its exact
manifest, native extension and generated-wrapper SHA-256 identities compiled
into the worker. Its upstream `0.0.0` Python text is informative. A source build
does not claim the former wheel's version or provenance. Source-built selection
does not waive source, notice, sandbox or interchange qualification.

The bootstrap does not use pip or execute wheel installation hooks. It verifies
cached archives, takes an immutable hash-checked byte snapshot for extraction,
rejects unsafe Windows paths, links, junctions and collisions, and bounds archive
expansion. A private staging directory is published atomically after extraction.
Interrupted downloads remove only their unique owned scratch file. `--check`
reconstructs the expected file table from the locked archives; editing both an
installed file and its generated manifest cannot forge a passing check.

The fixed `python313._pth` includes only the bundled standard library, runtime
root and bundled site-packages, without `import site`. User sites, registry
Python paths and `PYTHONPATH` are excluded. Matching C headers and the x64 import
library are prepared for `PyConfig_InitIsolatedConfig` embedding. FontTools man
pages and wheel console scripts are preserved as inert wheel data; no external
launcher is installed or executed.

`check_cad_runtime.py` runs generated developer fixtures in a hidden isolated
process with private configuration/home/temp locations. Poisoned `PYTHONPATH`
and user customization modules must not load. Its checks exercise binary DXF,
nested INSERT world coordinates, IFC4 parsing and a triangulated cube's world
geometry. This is SDK evidence, not an application import fallback.

## Current evidence and remaining implementation

On the Windows development host, offline preparation and archive-derived
verification passed for 2,802 files. The SDK probes passed under CPython 3.13.15.
A native C++ probe compiled against the matching headers/import library and
initialized the same interpreter in isolated mode before importing both libraries.
Seven bootstrap regressions passed, including an actual Windows junction,
interrupted-download retry, cache replacement and a forged installed manifest.
Evidence is under `artifacts/tooling/cad-bootstrap`.

The worker now embeds the interpreter lazily; the AppContainer policy continues
to permit one process and prohibits a Python child. The bridge uses isolated
paths, an absolute delay-loaded interpreter DLL and fixed library versions.
Native and Python stdout/stderr are suppressed through interpreter shutdown.
Configuration/home paths point to the worker's private directory. OpenBLAS and
OpenMP use one thread within the existing memory budget.

Foreign DXF normalization supports binary input, legacy polylines and bounded
nested INSERT/MINSERT expansion. Original native DXF metadata goes through the
native parser before any library normalization. IFC products without native
metadata use world-coordinate midheight sections with explicit approximation
diagnostics. Outer contours, holes and disconnected components retain their
source identity; unresolved length units retain source without inventing metre
coordinates. They do not acquire invented architectural semantics or living
area classifications. Verified native architectural entities remain intact.
The exact source is still retained and candidate validation precedes output.

The build stages the SDK and trusted adapter beside the worker. Adapter changes
restage independently of worker relinking. Run the focused library checks with:

```powershell
& .deps/cad-runtime/3.13.15/python.exe -I -B tests/test_cad_library_adapter.py
& scripts/test-import-worker-independent.ps1 -Configuration Release
```

Application integration qualification requires the actual independent sandbox
run; developer SDK probes alone do not prove it. On the development host, all
five independent fixtures passed: broker, assistance, native DXF desktop,
native IFC desktop and the new CAD library worker. The latter verifies binary
nested INSERT coordinates, rotated hollow sections, disconnected meshes and a
FLOOR slab void and unresolved-unit source retention. Twenty-five real-library regressions and both core exchange
suites also passed. These are development-host integration results, not clean
machine installation or complete file compatibility certification.

The worker uses explicit extended Windows paths for Python imports, since a
restricted token may be unable to query the machine's long-path policy.
Runtime paths beyond 260 characters are exercised by the independent fixture.
The SDK preparation manifest remains a standalone dependency manifest; its
`production_worker_integrated: false` does not describe the compiled application
bridge. Application evidence is the independent worker report.

Still required: full format compatibility qualification, production immutable
ACL qualification, complete dynamic-load coverage, exact notice and
corresponding-source closure (including IfcOpenShell and bundled native code),
clean Windows offline execution. The lock and
runtime manifests explicitly retain incomplete qualification. LGPL/GPL license
texts and package-provided notices are present; that alone does not certify
redistribution or fulfill every source obligation.

## Offline distribution payload

`prepare_cad_distribution.py` derives explicit component, file and portable
allowlists from the pinned interpreter ZIP and wheels. It verifies the build's
staged bytes, including the fixed LF-only `_pth` configuration, against immutable
archive snapshots. The distribution inventory repeats archive/member checks
and records the digest of the same lock bytes it parsed. Its payload includes
2,527 library files, with Python code, data, metadata and notices kept in their
original package layout beside the embedded interpreter. It does not install
pip, launchers, development headers or inert wheel scripts/manpages.

The duplicate interpreter copies of MSVC runtime and SQLite DLLs are excluded;
the package uses the application's separately inventoried copies in `bin/`.
NumPy and Shapely's uniquely named native DLLs remain in their wheel directories.
Static inspection includes all declared `.pyd` and DLL entrypoints and accepts
only verified CAD payload files from those directories. This is an import table
check; actual worker execution remains necessary.

The generated component manifest includes GNU license text hashes from the
lock. IfcOpenShell's wheel does not supply its own license file, so the package
also preserves its embedded copyright text. The SBOM preserves publisher
license prose while using `NOASSERTION` for declarations that are not a reviewed
SPDX expression; it does not promote that prose into a license conclusion.
IfcOpenShell wheel-to-source provenance and bundled native corresponding source
remain unqualified.

The official build catalog's Windows CPython 3.13 candidate
`ifcopenshell-python-313-v0.8.4-ff3c5b8-win64.zip` was inspected without executing
or installing it. Its native extension SHA-256 is
`040b46112ddb72b615c4fb4d3a689a4983804e47e66427d12d2b494d1b0714c0`;
the locked PyPI wheel's native extension is
`24849f9f6dbf73b9cac41aba98ea68a66f4f6cd458fb99ea9d565168bae962c7`.
They differ, so that catalog commit cannot establish provenance for the shipped
wheel. The PyPI integrity provenance endpoint for that exact wheel returned
HTTP 404. Neither observation proves that source is unavailable; exact
publisher-to-binary binding or a controlled source build remains required.
Local inspection evidence is under `artifacts/source-provenance/`.

For a controlled replacement build, the official short ref resolves to
[`ff3c5b849eee2ef6343b537c885b971ae6bba452`](https://github.com/IfcOpenShell/IfcOpenShell/commit/ff3c5b849eee2ef6343b537c885b971ae6bba452).
Its CMake entrypoint is `cmake/CMakeLists.txt`; the required wrapper/geometry
options are `BUILD_IFCPYTHON=ON`, `BUILD_IFCGEOM=ON`, and
`WITH_OPENCASCADE=ON`. `MINIMAL_BUILD` disables the Python wrapper and is
unsuitable. Ancillary converter/server/viewer and optional format engines can
be disabled, but the actual transitive source and build dependency closure
still needs verification. The upstream Windows scripts at that commit use
OCCT 7.8.1, Boost 1.86.0, Eigen 3.3.9 and SWIG 3.0.12; the application's current
vcpkg prefix uses newer versions and is not yet qualified for that build.
The locked CPython 3.13.15 headers/import library can supply the Python build
inputs. Do not run upstream download/update scripts as a substitute for pinned,
hash-verified source preparation and recorded build inputs.

Use the workflow in [offline-installer.md](offline-installer.md). After installing
an internal bundle, run the actual installed worker without replacing its files:

```powershell
& scripts/test-import-worker-independent.ps1 -Configuration Release `
  -PackagedRuntimeRoot <installation>/bin
```

This mode runs the CAD fixtures against that installed immutable root in the
independent AppContainer host. Developer SDK code generates only the input test
files; the worker loads its own installed runtime. It remains development-host
evidence, not clean-machine or complete compatibility certification.

The internal `vertex-offline-20260929-cad-runtime-rights` bundle was installed
without substituting SDK modules. All 12 native access probes denied their
requested rights: file creation, directory creation, deletion and their combined
mask on both `bin` and `plugins`, plus writes to the application, Python DLL,
IFC extension and Qt platform plugin. The earlier installation's deletion
failure is preserved separately rather than replaced by this result.
The installed CAD fixture exited successfully in the independent AppContainer
host. Six application runs also passed with a private profile and a System-only
search path: measurement/residential and architectural/residential and
light-commercial, each followed by reopening. Project bytes, sampled outputs,
screenshots and applicable native 3D images matched their save/reopen pairs.
These small fixtures establish installed execution and sampled persistence;
they do not certify a complete architectural workflow or Apex compatibility.
Local evidence is under `artifacts/runtime/installed-module-root-write-probes-rights.json`,
`artifacts/import-worker-independent/release-ed329136726d43f7928a3b9acd1964d4/`,
and `artifacts/installed-runtime-cad-rights-check/run-20260929-231420-814a058e/`.
