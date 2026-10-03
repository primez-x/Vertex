# Coordinated exterior-corner editing

## Outcome

Editing a current wall-derived measured exterior corner, through numeric X/Y
or a canvas handle, changes the physical source walls and regenerates measured
consumers in one reversible transaction. Geometry, dimensions and appraisal
calculations use the same proposed and committed document. This closes an
editing gap in the accepted production scope; it is not production signoff.

## Ownership

- Core worker: analytical inverse construction, persistent constraint solve,
  source correspondence, command proof/replay, native format compatibility,
  and core regression coverage.
- Root: desktop presentation and interaction coverage, documentation,
  integration, focused builds/checks, source distribution, Git and delivery.
- Independent advisor: challenge numerical, topology and provenance invariants.

## Geometry and integrity

Reconstruct physical centerlines by inward analytical offsets using each source
wall's thickness and retained edge lineage. Validate the forward-derived
exterior against the desired outline within the existing model tolerance
(1e-7 metre). Persist the authoritative forward-derived geometry, not rounded
screen coordinates. Preserve other exterior corners, stable identifiers,
manual dimensions, original construction inputs and historical command replay.

Preserve existing partition contacts across drawing layers on the same physical
floor and active phase. Endpoint joints use temporary coincidences; T attachments
retain fractional station through temporary affine solver relations to their
physical host endpoints. Saved measurements and anchors solve simultaneously.
Unavailable alternatives and demolished walls retain their original bytes.
Hosted openings retain their physical station and
must fit the resulting host. Solve saved constraints and regenerate all affected
current source consumers. Ambiguous joins, collapsed geometry, conflicts,
invalid openings or stale original sources are refused without mutation.
Changing a physical arc requires a qualified reconstruction proof, not a
relaxation of the existing fixed-sweep wall-edit command.

## Verification

Exercise unequal thickness, line/arc and arc/arc geometry, winding and reversed
walls, attached partitions, openings, conflicting constraints and invalid
offsets. Check exact preview/commit equality, current source correspondence,
calculation changes, stable dimensions, one Undo/Redo and persisted reopening.
Check command tampering and historical native/exchange compatibility. Inspect
native numeric and canvas previews; run required repository contracts and
diff checks before the scoped commit and remote verification.
