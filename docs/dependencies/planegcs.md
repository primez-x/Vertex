# PlaneGCS dependency qualification

## Pinned source

`property_planegcs` uses a bounded source extraction from FreeCAD `1.1.3`,
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
library `property_planegcs`, and builds the application adapter separately as
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
