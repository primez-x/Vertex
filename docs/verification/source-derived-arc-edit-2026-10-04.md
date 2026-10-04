# Editing physical-wall-derived exterior curves

The original native RED fixture reproduced a real source synchronization defect:
the curve editor changed the measured exterior while its physical source walls
retained their old geometry. Angle, arc height and arc length now use a typed
source-aware reconstruction. Both measured chord endpoints stay fixed; the
physical perimeter, current measured consumers, attached dimensions and declared
GLA update together. Movement of other related objects remains an explicit choice.
Incompatible fixed measurements or frozen physical contacts refuse atomically.

Command envelope 14 retains the exact measured ConstructionReceipt and
independently reconstructs the physical perimeter during admission and replay.
Final verification checks the entire source-derived outline, stable identities,
source wall identities and thicknesses. Requested coordinates cannot overwrite
the actual forward-derived geometry. Analytical roundoff is bounded, and
ill-conditioned or incompatible offset joins refuse without topology exceptions.

Physical curve inputs differ from measured inputs after offset joins. They retain
separate derived construction records. Straight-to-arc conversion uses a v3 archive
with a null original curve input and its true original straight baseline. Later
construction, endpoint, rigid and split operations preserve that origin and every
earlier operation. Retained commands and entity-only line-origin archives require
native 40 / extraction 38, including undone and deleted history. Earlier proof
dialects and reader floors remain unchanged.

Integration exposed two additional issues: missing native40 metadata-reader
admission and a semicircle fixture whose unequal-thickness tangent join could not
reproduce the requested outline. The reader now admits its own new version.
Semicircle and major-arc tests use physically admissible equal-thickness joins;
the ordinary 96-case inverse matrix still covers unequal thicknesses. Impossible
joins continue to refuse.

The independent review requested an interior T-station contact case and complete
v3 lifecycle coverage. The contact follows the physical arc station when allowed
and refuses frozen movement. Rotation/translation, splitting, save/reopen and
exact Undo/Redo pass through complete commands. Reflection passes through the
actual editor's grouped TransformBoundaries path, rather than endpoint authoring
that intentionally retains original ring adjacency. Fixtures were corrected to
use curved-only rigid authority, supply a fresh automatic split dimension ID and
allow the established v1-to-v2 rigid archive upgrade while retaining v3 at v3.
Those fixture corrections did not weaken production topology or provenance checks.
An initial Windows fixture helper also collided with the `near` macro and was
renamed before native execution.

Final Release application build and all 23 affected native suites passed
(`green7-build.json`, `green7-check.json`; native checks: 61.13 seconds). Six actual
editor cases cover each construction, straight and curved sources, both directions
and unit systems, Preview/Cancel/Apply, current physical walls, visible dimensions,
GLA changes, atomic Undo/Redo and editable save/reopen. Each case also rotates and
reflects the result through the real editor, verifies unchanged GLA and archived
source history, then saves, reopens and replays that transform. Fourteen Python
transfer-package checks pass through native40 and reject future native41.

Root visually inspected the imperial straight-to-curve Details view and the
metric clockwise arc-length preview. Final native, transfer, packaging,
installation and runtime evidence is retained locally under
`artifacts/exterior-measured-arc-edit-20261004`. Earlier failed runs remain failed
evidence. Manual U428 remains unchecked. This is an internal implementation
checkpoint, not full Apex compatibility, normative ANSI certification,
clean-machine/network-denied qualification or user acceptance.
