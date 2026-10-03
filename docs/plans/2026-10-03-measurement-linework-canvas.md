# Measured lines, pen relocation and derived areas

Continue the accepted production plan with a real Draw First linework workflow.
Physical Wall remains the default hybrid canvas tool. Area retains its strict
closed-boundary authoring session. Measured lines represent independent strokes
without wall thickness, hosted openings, classification or a GLA contribution.

## Interaction and authority

Click to anchor, then click to commit successive measured edges. Every accepted
edge uses the authoritative receipt model and one ordinary document command.
Ending a stroke retains committed geometry. Pen-up relocation starts the next
stroke at the chosen point without joining it to the previous one. Point jumps
resolve a saved stable vertex under the exact source revision and drawing
context. Invalid or stale input must leave the document unchanged.

Saved strokes project into the retained canvas, dimensions and snap candidates.
Print/image/PDF use that shared projection. DXF exports their analytical lines
and circular arcs, with an explicit diagnostic for native input/identity loss.
An unsupported future receipt model is never converted to guessed geometry.

## Area graph

Area detection operates on a derived graph. It splits source lines and arcs at
crossings and T junctions, deduplicates retracing and partial overlaps, and
retains source ownership and parameter intervals for each derived edge. Source
receipts are not rewritten or quantized. Bounded faces retain analytical arcs.
Ambiguous or unrepresentable topology produces an error rather than incorrect
square footage. Defining areas must remain a deliberate operation; loose lines
alone do not change appraisal totals.

## Ownership and checks

- Desktop worker: MainWindow orchestration/header and native interaction tests.
- Geometry worker: new area graph header, implementation and core tests.
- Root: canvas alignment, DXF mapping, CMake, integration, documentation,
  verification, scoped commit/push and remote verification.

Reproduce missing behavior with compiling stubs and failing executable checks.
Freeze all native writers during builds and checks. Verify real native clicks,
exact typed entry, relocation/jump, stale rejection, Undo/Redo, save/reopen,
selection, dimensions and output. Test graph crossings, overlap provenance,
analytical curves, invalid topology and source preservation. Run only affected
existing checks. Inspect captured UI and output before reporting delivery.

This continues full production implementation. It does not certify Apex
shortcuts, native project compatibility, ANSI compliance or the complete release.
