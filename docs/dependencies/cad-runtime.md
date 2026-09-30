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

The next integration embeds the interpreter inside `vertex-import-worker`; the
existing AppContainer policy permits one process and prohibits a Python child.
It must retain bounded IPC, native geometry agreement, source preservation and
atomic document admission. Native library stdout/stderr must not contaminate
the candidate pipe. ezdxf also reads its own configuration independently of
Python's isolated mode, so its configuration/home paths must point to the
worker's private directory, with no inherited `EZDXF_CONFIG_FILE`.

Still required: the actual adapter bridge, bounded nested-block traversal,
foreign IFC contour mapping preserving holes/components, runtime staging and
immutable ACL admission, all static/dynamic DLL dependencies, exact notice and
corresponding-source closure (including IfcOpenShell and bundled native code),
SBOM/installer integration and clean Windows offline execution. The lock and
runtime manifests explicitly retain incomplete qualification. LGPL/GPL license
texts and package-provided notices are present; that alone does not certify
redistribution or fulfill every source obligation.
