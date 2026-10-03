# Saved measured-line editing and current area calculations

This continues the unified production plan; it does not certify the finished
Apex replacement or ANSI compliance. User checklist results remain Not tested.

## Implemented behavior

Saved measured strokes support native canvas body movement and rotation. Their
local typed receipts and stable segment/vertex identities stay unchanged. An
ordered rigid frame supplies authoritative world geometry to drawing, snapping,
dimensions, calculations and output. Selection controls retain the stroke's
orientation after release. Horizontal saved plans also display measured strokes
with their actual view origin/orientation and corresponding snap targets.

Moving a source updates previously current derived faces when their retained
source identities identify one unambiguous face with the same edge count. Moving
a derived area includes its supporting measured strokes and updates other affected
faces sharing those sources. The whole operation is one undoable revision; area
classification facts are preserved. Preview does not mutate the document.

Qualification recomputes source freshness from actual graph geometry and lineage.
Changed, missing, unsupported, context-mismatched or phase-hidden sources withhold
stale area measurements and property GLA. Direct and ANSI recursive deduction
dependencies receive the same check. Ordinary workspace visibility stays separate
from semantic design-phase availability.

Native format 28 and extraction 26 protect the new semantics throughout retained
history. Published version-one lineage projects still open without modification;
their next save upgrades while retaining an exact original backup. A falsely
lowered format cannot admit recognized version-two stroke frames.

## Verification evidence

All ten focused checks passed in
`artifacts/measurement-linework-editing-20261003/final-checks.json`:
measurement_linework_transform_tests, measurement_linework_source_tests,
measurement_linework_tests, measurement_linework_storage_tests,
measurement_linework_area_desktop_tests, appraisal_document_tests,
project_store_tests, project_exchange_tests, measurement_linework_document_tests
and appraisal_details_panel_tests. The final native Release application build
passed in `final-canvas-build.log`; supporting targets were rebuilt in
`final-build.log` and `review-green-build.log`.

The native mouse fixture moves a separator from X=2 to X=3, updates dwelling
GLA from 8 to 12 square metres, retains garage classification, and verifies
preview, one-revision release, Undo/Redo and save/reopen. Details displays the
updated 129.17-square-foot GLA in the declared residential profile. Root inspected
the actual capture `final-ui/measured-line-live-area-move.png`. This is a fixture
under the declared Vertex policy, not an ANSI approval demonstration.

The rotation fixture verifies an actual quarter-turn gesture and persistent
handle orientation. A translated, rotated saved-plan fixture compares selection
frame position/orientation before and after release. A derived-area body drag
verifies that supporting strokes move and all shared faces stay current.

Independent read-only review found two actionable defects: common coordinate
drift during large-pivot rotation/reflection, and double projection of a move
preview frame. Root fixed both and added regressions. Compensated affine
evaluation handles MSVC's double-width long double without a new dependency.
Initial failed fixtures and storage admission failures remain in the local logs;
only the final passing evidence supports this checkpoint.

## Remaining work

Ambiguous or changed topology needs a complete explicit face review/redefinition
workflow; withholding a stale total is not completion of that workflow. Typed
vertex/length edits, scaling, combined rise/run and curve/relative-turn UI,
same-stroke Undo continuation, nested holes, large-project responsiveness,
native Apex compatibility and full production/ANSI qualification remain in scope.
This checkpoint does not replace the full acceptance gate with its focused tests.
