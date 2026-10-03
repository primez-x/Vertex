# Measurement-linework foundation — 2026-10-03

## Gap established from source and implementation

The [official Apex Draw First tutorial](https://apexwin.com/support/ApexSketchv7/ApexSketchv7-DrawFirst.pdf),
pages 6–8, draws independent separator lines after pen-up relocation and later
defines areas. Vertex's existing boundary session only represents connected
closed outlines. Its selected-vertex jump changes the pointer, not the next
segment's origin. A wall-only relocation would leave measured linework missing.

APX-WF-001 is therefore in progress. Its earlier verified status covered closed
outline fixtures, not the full documented workflow. U049 and production parity
remain open; historical acceptance evidence has not been regenerated as proof
of the missing workflow.

## Implemented foundation

`measurement_linework` is a required entity with one open or closed connected
stroke. Stable topology and normalized construction receipts are authoritative.
Individual receipt replay supports measured headings, rise/run, relative turns,
point entry and analytical arc construction. Crossings, retracing and exact
vertex revisits remain valid. No wall thickness, hosted openings or area total
is invented for loose linework.

The strict codec retains original expressions and extensions. Known invalid
geometry, identity aliases and inconsistent joins reject. Positive future model
or replay versions retain their original JSON and make the document read-only.
Document admission uses the existing organization resolver for all four drawing
context IDs. Child identities cannot alias their owner, another stroke's child,
an entity or an identified closed-boundary child in the current state.

## Verification and review

The initial compiling stub failed all fourteen core groups with the expected
missing-model error. With the model implemented, four isolated document cases
failed against the original registry/admission behavior. Independent review
then identified contradictory parent references and owner-child ID aliases.
Both additional regressions failed before correction. Parent admission now uses
the shared resolver, and core replay/encode/decode reject owner-child aliases.

The final Windows Release build, including `vertex`, succeeded. All seven
affected checks passed: measurement linework, linework document integration,
document, boundary integrity, project organization, project storage and appraisal
document. Coverage includes real Undo/Redo and save/reopen, atomic rejected
parent edits, exact coordinate/input preservation, future-version read-only
storage and unchanged GLA after adding loose linework. Existing boundary and
document regression checks passed. Runtime imports are inspected separately;
local receipts are in `artifacts/measurement-linework-20261003/`.

Root resolved the two independent review findings and reran affected checks.
These results establish the model and admission stage, not user-observed canvas
resolution or a production release.

## Required next stages

Connect the model to canvas pen-up relocation and authoring, selection,
dimensions, edit/transform/output adapters and recoverable unfinished work.
Area definition also needs a derived graph that nodes T-junctions/crossings and
deduplicates retraced geometry while preserving authored source lineage. The
current endpoint-only face detector is not sufficient for that complete flow.
Physical Apex shortcut behavior and native project compatibility remain open.
