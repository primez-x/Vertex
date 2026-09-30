# Curved connected editing checkpoint

Existing point constraints now accept analytical curved walls and identified
measurement/room boundaries. Signed sweeps remain fixed during endpoint solving.
Direction relations use chords; Endpoint distance measures the straight distance
between chosen points, independently of the physical arc measurement.

Both workspaces expose the same preview/Apply workflow. Native previews draw
true arcs with analytical bounds. Corner dragging and physical boundary edge
edits use connected authoring for curved owners, including rotated saved plans.
Original curve construction expressions and opaque input metadata remain in
validated derivation evidence. Subsequent full curve reconstruction preserves
the original archive and prior operations.

Curved wall proofs use inner version 2 / command envelope 3. Project format 10
and exchange format 7 cover proofs, actual curved bindings and wall derivations
through all retained states, including undone history and imported derivations
without originating command history. Historical straight payloads retain their
original representations and reader floors.

## Observed verification

- The initial native regression failed because curved room boundaries were
  excluded from the constraint editor.
- The Release app and affected targets built. A missing export-library link and
  invalid test calls to the private restore API were corrected during compilation.
- The focused runtime checks exposed and corrected sparse legacy wall handling,
  undo-history provenance validation, and test-fixture ordering.
- Fourteen final CTest checks passed in 40.20 seconds: constraint authoring,
  workspace preview adapters, symbol transforms, saved-plan corner editing,
  building receipts, constraint dialog, boundary canvas, curve construction,
  geometry operations, architectural document adapter, constraint integrity,
  document commands, project storage and project exchange.
- Native pointer regressions cover both saved horizontal plan normals and true
  curved boundary/wall propagation. Dialog regressions cover both workspaces,
  source immutability, explicit reconstruction, undo/redo and save/reopen of
  shorter/original derivation states. Light/dark raster probes verify signed
  semicircle extrema without substituting chord strokes.
- Source and runtime evidence: `artifacts/curved-constraints-20260930/`.
  Root visually inspected the curved wall and boundary dialog captures.
- Independent review identified the undo/reopen defect before delivery; the
  corrected reader retains exact source-state and navigation-stack checks.
- The final review identified stale curve provenance in the transform editor.
  Rotation/translation now rebase the preserved derivation; reflection appends
  a validated construction before cloning remaps identities. The reviewer
  approved this correction. All four affected native checks then passed in
  34.60 seconds, covering transforms, saved-plan corner editing, building
  receipts and the constraint dialog. Evidence is in
  `artifacts/curved-constraints-20260930/transform-final-ctest.log`.

## Qualification boundary

The offline bundle contains 3,758 declared files, including 1,106 source-kit
files. Installation verified 2,645 runtime files at
`artifacts/installed/vertex-20260930-curved-editing/`. The installed executable
matches the tested Release build: SHA-256
`e05b0bb3ab3f2ceb6256b684be91dc9e1df1ce2e15ac55a3adb4b0e328d72fdc`.
Six sampled installed launches passed the create/save/reopen pairs for
measurement, residential architecture and light commercial architecture.
Evidence is in
`artifacts/installed-runtime-20260930-curved-editing/run-20260930-172314-af446728/report.json`.
Source checkpoint `bf572455da1ed15379d484fea2accfb9a5ab546d` was committed,
pushed and verified against `origin/main`. This subsequent evidence update
does not change the executable or packaged source checkpoint.

Full independently validated curve construction is permitted as a distinct
recorded operation; it is not claimed to carry the solver topology proof.
Authoring validates analytical winding, crossings, overlap, reversal, collapse,
hosted openings and constraint conflicts. Shared topology admission for direct
typed command replay remains a production qualification gap, including older
straight command paths. Physical arc-length locks, tangency, full Apex parity
and clean-machine/network-denied qualification remain open. This checkpoint
does not complete the production goal.
