# Wall workflow repair

User report: walls do not draw or snap, dimensions are absent, and window
creation paths are inconsistent.

Native mouse input in the existing wall-appraisal package confirmed default
wall drawing, endpoint/alignment snapping, retained lengths and both quick
window insertion and visible-library double-click placement. Two different
application versions were open. The human's unsaved installed-build drawing
was inspected without editing it.

Source discovery found a concrete entry-path defect: menu and command-search
wall actions could remain in an architectural projection with wall snapping
disabled. They now route to the conventional drawing canvas, as the Library
already did. Direct boundary authoring uses the same routing. Draft/workspace
guards remain in place.

Returning a wall chain to its original anchor now finishes it. Newly authored
straight walls sharing exact endpoints on the same floor/layer in the active
phase receive persisted coincidence relations. Existing coincidence groups are
joined without redundant endpoint-pair cycles. Relation-only solver validation
must leave geometry unchanged; the new wall and relations commit atomically.
Length edits with connected movement preserve the shared corner. Deletion
removes relations referencing erased owners in the same command, retaining
relations explicitly rebound to surviving owners.
Connection records remain accessible through the constraint editor, without
raw internal Constraint rows in the layer navigator.

Library opening activation says to click a wall. The quick opening tool and
catalog variants use the same hosted model. Corner and oversized placement
messages explain the corrective action instead of exposing a negative-offset
exception. Invalid placement does not clamp or invent geometry.

Verification observed missing endpoint relations and missing chain termination
before implementation. The focused Release checks cover real Qt widget
interactions, ordinary wall clicks, preview/commit agreement, snapping, retained
lengths and PDF text, visible library double-click placement, context isolation,
atomic undo/redo, save/reopen, connected length editing/deletion, drawing entry
from architectural Plan/Elevation/Section, and exterior appraisal measurement.
The appraisal refusal fixture now also verifies that a raw coordinate edit
cannot silently break a persisted corner before explicitly removing relations
to construct its open-loop case.

The final Release run passed all five selected checks in 10.93 seconds:
wall/opening palette, wall chain connections, exterior measurement desktop,
source-kit allowlist and requirement schema. Runtime inspection found no
unresolved imports across 113 component binaries. The delivered bundle manifest
records the executable's SHA-256.

Manual checks U286–U288 describe these user workflows. General constrained
wall dragging/rotation, automatic physical miter presentation, curved wall
connections, large connected networks and complete production qualification
remain work to do. Existing projects do not receive inferred relations merely
by opening them; wall heights, thickness and appraisal eligibility retain their
separate semantics.
