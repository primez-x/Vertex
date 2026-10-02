# Boundary editing and appraisal diagnostics

This is an internal production-goal checkpoint. Apex parity, compatibility, and
the complete architectural release remain required and are not certified by it.

## Outcomes and ownership

- Root verifies the requested zoom- and unit-aware grid against visible paint,
  cursor snapping, exact retained geometry, and imperial/metric increments.
- The appraisal worker owns the report builder and its core tests. Missing policy
  must withhold qualified totals while retaining valid diagnostic measurements.
  Malformed in-scope areas must retain selectable source rows without numbers.
- The geometry worker owns the typed fixed-endpoint arc intent, geometry helper,
  and pure geometry checks. Original expressions, stable child IDs, and endpoints
  must survive curvature reconstruction.
- Root owns document replay integration, the actual dimension/curve controls,
  desktop verification, documentation, Git, and offline delivery.

## Implementation and verification

1. Verify adaptive grid in both unit systems at close and wide zoom levels.
2. Establish failing regression cases for appraisal diagnostics and arc edits;
   implement bounded core changes without fabricating qualified results.
3. Expose edge-length dimension creation/restoration and analytical curvature
   editing through existing boundary tools, with preview and normal undo history.
4. Check source identity, dependent dimensions, constraints, undo/redo, and saved
   project replay. Exercise real desktop controls, not only public helper APIs.
5. Review the integrated diff, resolve required findings, run affected checks and
   diff checks, then commit, push, and verify the remote commit.
6. Deliver a fresh offline install and verify the Desktop shortcut and executable
   hashes. Preserve prior installations and unrelated dirty files.
