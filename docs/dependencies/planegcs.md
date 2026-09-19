# PlaneGCS dependency qualification

## Pinned source

`vertex_planegcs` uses a bounded source extraction from FreeCAD `1.1.3`,
commit [`145529fe741292ff0b3977a01195bf0247425794`](https://github.com/FreeCAD/FreeCAD/commit/145529fe741292ff0b3977a01195bf0247425794).
The exact upstream file list and hashes are recorded in
`third_party/planegcs/SOURCE.md`. No FreeCAD application, UI, document, or Qt
source is included.

The extracted PlaneGCS files carry the original GNU Library General Public
License version 2-or-later notices. The pinned FreeCAD repository license text
is retained verbatim at `third_party/planegcs/upstream/LICENSE`. The small
FreeCAD Boost adjacency wrapper carries `SPDX-License-Identifier:
LGPL-2.1-or-later`. The PlaneGCS files at this pinned revision use their full
license notice rather than an SPDX line.

The local compatibility shims provide only the export macro, no-op console
logging, and elapsed-time surface needed by the extracted sources. They do not
reimplement FreeCAD application behavior.

## Build and replacement boundary

`cmake/PlaneGCS.cmake` builds the extracted solver as the standalone shared
library `vertex_planegcs`, and builds the application adapter separately as
`sketch_constraints`. This keeps the solver DLL replaceable without linking
PlaneGCS source into the application-owned constraint model. Distributions
must ship the retained license and corresponding source/provenance required by
the solver's license.

The fragment requires the prepared native prefix at
`.deps/native/x64-windows`. It resolves Eigen with:

```cmake
find_package(Eigen3 5.0.1 CONFIG REQUIRED NO_DEFAULT_PATH)
```

The extracted sources compile successfully with MSVC 19.44, C++20, Eigen
5.0.1, and Boost 1.92. The qualification build defined `EIGEN_MPL2_ONLY`, so
Eigen modules outside the MPL2-only surface fail at compile time rather than
being pulled into the DLL. There were no Eigen 5 API errors. The untouched
upstream sources emitted seven MSVC C4267 `size_t`-to-`int` warnings in
`GCS.cpp`; the vendored source is deliberately not edited to hide them.

### Replacement build procedure and unverified handoff boundary

In a separate copy of the source/build kit with the documented MSVC toolchain
and prepared native prefix, build the shared library and its focused test:

```powershell
./scripts/build.ps1 -Configuration Release -SkipTests `
  -Targets @('vertex_planegcs', 'constraints_tests')
```

Run `ctest --test-dir build/windows-release -R '^constraints$'
--output-on-failure` using the CTest executable from the configured toolchain.
The generated DLL is `build/windows-release/vertex-planegcs.dll`; the runtime
inventory maps it to `bin/vertex-planegcs.dll`. An ABI-compatible replacement
can be evaluated in a separate portable-package copy with the application
closed. Preserve the original DLL and record both hashes. An intentional
replacement makes the original package integrity manifest stale; a failed
integrity check must not be presented as an unchanged, verified distribution.
If exported interfaces or compiler ABI change, rebuild dependent application
targets too. A replacement build and application launch have not been
qualified by this documentation or by the SBOM exporter tests.

The local build definition makes `vertex_planegcs` shared and
`sketch_constraints` static; the latter is application-owned adapter code.
Eigen and Boost headers contribute code to the solver build. The separate
application SQLite amalgamation is also compiled statically and nlohmann JSON
is header-only. Replacing a DLL does not replace these compiled contributions;
their modification requires rebuilding the consuming targets. Source-kit
allowlisting excludes dependency caches, so possession of that kit alone does
not establish a complete offline rebuild environment.

Remaining review evidence includes complete matching sources and build inputs
for shipped dependencies (including Qt's embedded third-party contributions),
verified replacement/rebuild results, notice completeness, and review of the
applicable distribution terms. The local vendor SPDX may identify a source
release tag rather than an immutable commit; the inventory must not relabel a
vcpkg recipe revision as that upstream commit. This procedure describes the
technical replacement boundary only and does not clear COMP-LIC-001.

## Application trust boundary

PlaneGCS returns a candidate only. `solve_planar_constraints` independently:

- rejects malformed IDs, missing references, non-finite values, unrepresentable
  displacements, and degenerate direction segments before solver entry;
- requests PlaneGCS's least-norm DogLeg step to minimize movement from the
  immutable input geometry;
- reports degrees of freedom and stable application constraint IDs for
  conflicting and redundant solver tags;
- checks every hard linear residual against `1e-6` metre and angular residual
  against `1e-8` radian;
- checks fixed anchors independently;
- checks each supplied winding invariant after solving; and
- returns the original input points for every rejection.

An accepted result is a revision-bound preview. The adapter carries
`expected_revision` through unchanged and has no mutation or auto-apply API.
The caller must compare the revision again before publishing a preview.

The initial adapter is intentionally planar and point-based. It supports
horizontal, vertical, coincident, fixed-length, parallel, perpendicular, and
fixed-anchor constraints. Curve constraints and application persistence remain
outside this extraction.
