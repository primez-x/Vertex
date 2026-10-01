# Physical curve-length editing checkpoint

Curve length is now separate from Endpoint distance. An anchored constraint
target edits an analytical curved wall or identified boundary edge while retaining
its signed sweep. The exact entered quantity measures arc length. The solver
receives an equivalent chord target; independent document admission checks the
actual analytical length and the owner/segment/vertex identities.

Version-3 constraint entities require project format 12 and exchange version 9
throughout retained history. Earlier codecs and formats retain their semantics.
Undone/deleted relations cannot lose their reader floor through later commands.

## Observed verification

- Baseline codec probe built and failed because physical arc-length semantics
  were unsupported. The new codec check passed after implementation.
- Release engine checks: constraint entities 0.10 s, integrity 0.18 s,
  storage 3.91 s, exchange 0.28 s. Authoring passed in 0.52 s after correcting
  a test fixture that had expected measured-input archival from angle-only input.
  The corrected fixture uses a genuine measured 7 m arc and independently
  verifies its source geometry before checking preservation of its input.
- Signed minor/major arcs, anchored connected movement, stable boundary edges,
  arc-versus-chord conflicts, atomic rejection, exact quantity/vendor metadata,
  persistent freedom, replay, undo/redo and save/reopen have focused coverage.
- Persistence rejects downgraded format markers with a recomputed digest. It
  also rejects matching forged wall geometry and command proof that substitute
  a 6 m chord for a 6 m semicircular arc. Rejected files remain unchanged.
- Release Vertex, CLI and native constraint dialog built. Native dialog workflows
  passed in 3.75 s: physical prefill, wall start/end anchors, non-first curved
  boundary edge, edit/reload/removal, cancel, conflict and invalid binding cases.
- Root inspected wall and boundary preview captures: dashed current geometry,
  blue proposed arcs and anchored endpoint indicators are visible. The native
  widget fixtures do not reproduce the full application's inherited theme.
- Independent scoped source review approved the measurement and persistence
  changes. Its numerical uncertainty prompted additional checks: signed sweeps
  within 0.00001 radian of a full turn and moderate arcs translated to
  (1000000, -1000000) m passed anchored 7 m to 5 m editing, independent physical
  length checks, immutable preview and typed replay. An unrepresentable chord
  target rejects atomically. The final conditioned authoring run passed in 0.52 s;
  this does not qualify every extreme coordinate/angle combination.

Local evidence is under `artifacts/physical-curve-length-20260930`:
`core-ctest.log`, `authoring-final-ctest.log`, `conditioning-ctest.log`,
`native-ctest.log`, `contracts-ctest.log`, and the
`physical-curve-*` / `physical-boundary-curve-length.png` captures. The first
combined log retains the fixture failure; it is not reported as a wholly passing
run. The final authoring log resolves that failure without changing product code.

## Remaining scope

Direct curved-wall resize receipts, tangent relationships, level dependency
propagation and full production qualification remain required work. This is
development-machine evidence, not Apex file/device compatibility certification,
clean-machine installation qualification, or completion of the production goal.
