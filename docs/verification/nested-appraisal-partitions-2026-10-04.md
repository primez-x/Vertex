# Nested appraisal partition authoring

Detect closed areas can now author a floor, a contained measured room and an
internal exclusion together under an explicit supported ANSI appraisal profile.
The shared preparation helper also validates the existing deductions editor.
This exposes relationships the appraisal report already supported; it does not
introduce a new calculation policy or project schema.

Each parent deducts immediate-child gross footprints. Eligible children
contribute their own net area. The executed three-level fixture uses gross
100/36/4 m², giving floor net64 plus room net32 and qualified GLA96 m², displayed
as 1033 whole square feet. Actual declarations remain necessary: labels and
containment do not supply missing inspection, grade, finish or ceiling facts.

## Verification

The behavioral RED records show the old same-TYPE rejection and disabled native
Apply. The final Release build and eight affected registered CTest checks pass:
appraisal_area_partition, measurement_area_definition,
nested_measured_area_desktop, boundary_commit, appraisal_document,
appraisal_details_panel, appraisal_desktop_workflow and ansi_appraisal_desktop.
Logs and native captures are in
`artifacts/nested-appraisal-partitions-20261004/final-*`.

Checks cover the complete proposed link overlay, metadata and stable identity,
immediate gross deduction rather than child net, void union, shared descendants,
cyclic/context/source/phase refusals, removal repairs, supported policy versions,
no inferred qualification, re-detection, atomic Undo/Redo and native save/reopen.
Saved floor-owned boundaries without a drawing layer remain supported. Supplied
contradictory owners are refused. Native Document admission rejects a non-layer
object used as layer_id without changing entities, revision or history; the
helper also retains a node-type guard.

Native checks use the actual review and deductions dialogs. They remove and
restore an internal exclusion, verify exact retained geometry and declarations,
then record V2 sloped-room observations against the authored room and leaf
geometry. Details and the actual Full report PDF agree on 1033 sq ft. The
ordinary drawing-sheet PDF is separately loaded and rendered; it is not assumed
to contain an appraisal summary without a configured sheet schedule.

Root inspected the actual light/dark review and Details captures and the
rendered appraisal PDF. The review now shows all three complete rows without
vertical scrolling. Independent review required preserved layerless ownership
and an actual layer-reference type check; both were corrected. Failed runs are
retained, including two invalid fixture attempts and the earlier test that
expected report totals in a drawing-sheet export.

## Requirement gap check

The helper is shared by measured-area definition and deduction editing. Legacy
Auto-Subtract TYPE and one-level rules remain unchanged. No retained history is
rewritten. User checklist U407 gives a practical floor/room/void test; all user
results remain Not tested. U052 and U383 distinguish this ANSI authoring path
from ordinary measurement subtraction.

Independent copy of a parent with nested descendants remains unsupported.
Native Apex compatibility, full keyboard/device/integration parity, final ANSI
publisher-standard validation, clean-machine offline qualification and unified
production acceptance remain unproven. The runnable package and installed
observation are recorded separately in local delivery evidence. This is an
internal checkpoint, not the completed production replacement.
