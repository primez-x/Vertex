# Saved measured-stroke vertex and length editing

This delivery continues the unified production plan. It does not establish
finished Apex parity, production acceptance or ANSI approval. User checklist
results remain Not tested.

## Implemented behavior

Selected saved measured strokes expose stable endpoint/vertex handles, including
the final endpoint of an open stroke. Dragging one vertex updates all occurrences
of its identity. Double-click quick properties and the canvas context menu expose
Geometry: anchored edge-length editing, an explicit connected movement choice,
and exact vertex X/Y entry. Original and proposed geometry and edge lengths are
shown before Apply. Cancel and invalid/stale edits do not alter the document.

Schema/replay three retains original construction receipts, quantities, angles,
local anchor and extensions. Its ordered operations record geometry edits and
rigid transforms in their actual order. Open, retraced and self-crossing strokes
remain valid; signed arc sweep is preserved. No parallel editable geometry is
persisted. Effective API no-ops preserve the original encoding. Strict replay
rejects invalid identities, irrelevant fields, inconsistent exact quantities,
degeneracy and precision loss.

Source edits and unambiguous derived-area consequences commit together in one
revision. Classification facts remain assigned, current area measurements and
GLA recalculate, and Undo restores the complete edit. A released endpoint waits
for its exact final native projection; absent/invalid projection, cancellation
and stale completion cannot publish a released edit. Native format 29 and
extraction 27 protect all retained version-three history, including Undo and
deleted strokes. Earlier recognized dialects retain their existing encoding and
legacy-read policy.

## Observed verification

The initial core and desktop checks reproduced missing edit behavior and missing
endpoint handles. A separate release regression reproduced premature admission
of an endpoint whose exact final projection was pending. A connected-translation
rounding regression reproduced shape drift and now rejects the lossy operation.

The final native Release build passed (`final-build.log`). Twelve focused suites
passed in `artifacts/measured-stroke-geometry-20261003/final-checks.json`:
measurement_linework_edit_tests, measurement_linework_tests,
measurement_linework_transform_tests, measurement_linework_storage_tests,
measurement_linework_source_tests, measurement_linework_area_desktop_tests,
boundary_canvas_tests, appraisal_details_panel_tests, appraisal_document_tests,
project_store_tests, project_exchange_tests and measurement_linework_document_tests.

The actual mouse fixture moves one separator endpoint, updates both derived
faces, preserves their combined sixteen-square-metre coverage and recalculates
declared dwelling GLA from eight to ten square metres. It verifies a release
without an intermediate motion event, one-step Undo, typed eight-metre anchored
length, stale-edit rejection and native save/reopen. The dialog fixture verifies
its exact proposal, invalid input, Cancel, Apply and Undo. Root inspected actual
native captures `final-ui/measured-stroke-vertex-edit.png` and
`final-ui/measured-stroke-length-preview.png`.

Root reviewed the integrated source, interfaces, compatibility floors and
verification evidence. An additional read-only advisor dispatch was unavailable
because the agent runtime refused new assignments at its thread limit.

## Remaining production requirements

Changed or ambiguous face topology still needs the complete explicit area
review/redefinition workflow. Scaling and combined curve/relative-turn measured
authoring, nested holes, large-project responsiveness, installed Apex native-file
compatibility, integration/device certification, clean-machine network-denied
qualification and normative ANSI validation remain in scope. Passing these
focused checks does not replace that full release gate.
