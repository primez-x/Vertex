# Corner responsiveness and DXF layer review

This delivery closes two verified gaps in the accepted production scope. It
does not change the full release criteria or certify production completeness.

## Outcomes and ownership

- Core worker owns project organization, wall contact resolution, constraint
  authoring/integrity and its core regression tests. Eliminate repeated whole
  project organization during one immutable placement pass. Preserve derived
  entity bytes, validation order/error behavior, physical contacts, topology,
  provenance, calculation and command replay invariants.
- Desktop worker owns MainWindow and its DXF desktop workflow tests. The actual
  Import DXF action reviews each source CAD layer and its destination project
  floor/layer before mutation. Preserve source organization through atomic
  creation of missing layers, allow explicit reassignment, map annotations,
  preserve hosted-opening floor consistency, retain original bytes and report
  import limitations. Keep the direct programmatic import API compatible.
- Root owns integration, documentation, generators, builds, verification, Git,
  source distribution and installed delivery. Freeze writers before builds.
- An independent read-only advisor challenges snapshot validity, placement
  equivalence and import data integrity after implementation.

## Evidence and acceptance

The existing 366-wall corner fixture, including 360 unrelated level-bound
walls, took 2191.2 ms on this machine. Six placement passes repeatedly rebuild
the organization per wall: four physical-contact passes spend 340-366 ms each,
and the before/after topology pass spends 700.9 ms. The solver took 0.551 ms.
Measure the same fixture after the fix, keeping its geometry assertions. This
measurement is local evidence, not an agreed reference-hardware qualification.

Compare batch placement to the existing singular path for exact derived
entities, absent/absolute placements, valid floor levels, malformed placement,
binding/graph/coordinate failures and a changed level in a fresh snapshot.
Run source-corner, wall, constraint, project-organization and document checks.
Verify the native editing path and appraisal totals remain correct.

Exercise the actual DXF review dialog with multiple source layers, existing
and newly created destinations, annotations, different floors, cancellation,
stale document context and incompatible host/opening destinations. Verify
source retention, fidelity mapping, atomic Undo/Redo and save/reopen. Preserve
existing single-layer programmatic import and editable opening-profile checks.
Inspect a native capture of the dialog and run relevant exchange checks.

Run repository contracts, source-kit and runtime distribution checks, and
`git diff --check`. Commit only reviewed task paths, push and verify the remote
ref. Deliver the verified offline package without launching or closing the
user's application. Report any remaining responsiveness, source-lineage or
compatibility limitations explicitly.
