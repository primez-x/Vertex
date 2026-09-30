# Bundled CAD library runtime

The selected production adapters are IfcOpenShell and ezdxf. They currently run
in independent validation tooling; the native application still uses its bounded
C++ adapters. This bootstrap prepares their production dependency runtime. It
does not claim that worker integration, distribution closure, source obligations
or external-format qualification are finished.

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
ACL provisioning, all static/dynamic DLL dependencies, exact notice and
corresponding-source closure (including IfcOpenShell and bundled native code),
SBOM/installer integration and clean Windows offline execution. The lock and
runtime manifests explicitly retain incomplete qualification. LGPL/GPL license
texts and package-provided notices are present; that alone does not certify
redistribution or fulfill every source obligation.
