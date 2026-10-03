# Measurement linework for disconnected Draw First construction

## Outcome and scope

Implement a durable measurement-linework model as the prerequisite for pen-up
relocation, independent separator lines and later area definition. The official
[Apex Draw First tutorial](https://apexwin.com/support/ApexSketchv7/ApexSketchv7-DrawFirst.pdf),
pages 6–8, demonstrates these operations. The current closed-boundary authoring
session cannot represent them: lifting its pen retains the active connected
chain, and another anchor requires the current chain to close. A cursor-only
jump does not change the next edge's origin.

Each linework entity holds one connected open or closed stroke. Its ordered
stable topology and exact construction receipts are authoritative; analytical
segments are derived using individual receipt replay. It has no wall thickness,
hosted openings, area classification or standalone GLA contribution. Crossings
and retraced edges are legitimate authored linework, even when they cannot yet
be converted into a valid area.

This stage includes typed model/codec/replay, document admission, drawing-context
integrity, source-format documentation and history/storage coverage. It does
not finish U049 or Draw First parity. Canvas authoring, point picking, dimension
placement, edit/transform/output adapters, recovery, derived graph noding and
area definition remain required implementation stages. Existing strict closed
boundary invariants remain intact.

## Ownership

- Core worker: new measurement-linework header, implementation and core tests.
- Root: document/organization integration, CMake, integration tests, format and
  requirement documentation, source-kit inventory, verification and Git.
- Read-only advisor: approach challenge; root resolves findings from source
  and verification evidence.

No native source changes occur during native builds or checks. Preserve
unrelated work and leave the production goal active.

## Admission and persistence

Known schema/replay versions validate normalized receipts, finite nondegenerate
segments, stable IDs and exact joins. Repeated vertex IDs must refer to the same
point. Unknown positive versions preserve their opaque model and make the
document read-only. New linework is required geometry so older binaries cannot
silently edit around it. The entity's stroke ID matches its document identity;
its property/building/floor/layer references resolve through the same drawing
context contract as other plan geometry. Command rejection is atomic.

## Verification

Reproduce the absent behavior with failing core tests before implementing the
model. Verify open/closed strokes, disconnected entities, all applicable
analytical line/arc construction methods, exact input retention, relative turns,
malformed receipts/identity/coordinates, extension round trips and unsupported
version handling. Integration checks create actual documents and use ordinary
command history and project storage for Undo/Redo and save/reopen. Check context
rejection and that loose linework adds no GLA area. Retain relevant existing
boundary and document checks; run source-kit coverage and diff checks. Commit,
push and verify the remote ref. This remains an internal production checkpoint.
