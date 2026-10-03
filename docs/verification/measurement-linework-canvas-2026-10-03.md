# Measured-line canvas and derived-area checkpoint

This checkpoint implements the bounded canvas plan in
[2026-10-03-measurement-linework-canvas.md](../plans/2026-10-03-measurement-linework-canvas.md).
APX-WF-001 remains in progress; this is not production, Apex compatibility or
ANSI certification. Manual checklist results remain Not tested.

## Exposed workflow

Draw offers Wall (the default), Area and Measured lines. Measured lines retain
one exact receipt-backed edge per accepted click. D accepts typed distance and
heading. Enter, stationary right-click and Escape finish while retaining committed
edges. Tools → Lift measured pen starts an independent stroke; selecting a saved
stroke and using Tools → Jump to measured or boundary vertex resolves its stable
exact vertex without a joining edge. A closes; Enter accepts alignment proposals.
Retained strokes supply canvas geometry, dimensions, snap targets and shared output.

Select a stroke, finish any active drawing, and choose Tools → Detect closed areas.
After classification, the app creates real measurement_boundary entities in one
undoable command from all measured strokes in that exact drawing context. It keeps
source strokes and lineage (owner, segment, analytical parameter interval and
direction). Repetition skips identical derived geometry; altered geometry with
the same retained lineage requires review before another area can be created.

The analytical graph nodes line/arc crossings, T junctions and coincident overlaps,
deduplicates retraced edges with all source lineage, and ignores bridges/stubs for
face extraction. Nested cycles requiring holes reject. Limits are 2048 sources,
16384 represented stations/derived edges and 65536 contacts. Invalid or uncertain
geometry rejects rather than guessing square footage. DXF exports analytical
lines/arcs with an explicit native typed-expression/identity loss diagnostic;
unsupported receipt models remain opaque.

GLA and area dimensions are visible in Details, the third left-panel tab beside
Layers and Library. The prior implementation and verification are recorded in
[drawing-and-appraisal-details-2026-10-02.md](drawing-and-appraisal-details-2026-10-02.md).
Area definition alone does not certify GLA; project setup and actual recorded
observations remain authoritative.

## Recorded technical verification

Nine distinct focused checks have exit code 0 in
`artifacts/measurement-linework-canvas-20261003/green-checks.json` and
`remaining-green-checks.json`: measurement_area_graph_tests,
measurement_linework_exchange_tests, measurement_linework_desktop_tests,
measurement_linework_area_desktop_tests, directional_alignment_canvas_tests,
dxf_project_exchange_tests, wall_chain_connection_tests,
drawing_measurement_desktop_tests and measurement_linework_document_tests.
The first green-checks record has a failed directional_alignment_canvas_tests
run; its subsequent remaining-green record passes. Initial failing checks are
retained in the red, integration, closure-red and review records. These are
technical fixtures, not completed user manual testing.

The final native rebuild passed. All five affected desktop checks passed again
in `final-checks.json`, including the existing appraisal Details check. Root
inspected `ui/measured-lines-defined-areas.png` and the rendered actual PDF,
`ui/measured-lines-output.png`. Bundled fonts render normally, closed-stroke
dimensions face outward, and permanent dimensions use readable project units.
The ANSI-profile check retains tenths-of-foot presentation even in a metric
workspace, without changing exact measured geometry. The Details tab tooltip
now identifies GLA, dimensions, deductions and the appraisal report.

## Remaining scoped gaps

Same-stroke Undo continuation; stroke transforms and editing; combined rise/run,
relative-turn and curve authoring UI; nested holes; real native Apex-file roundtrips; complete
production qualification and ANSI certification remain open. Shared output and
the passing focused checks do not establish those broader acceptance gates.
