# Wall placement coherence

The repeated wall report was checked against current source and a packaged native
application. An older installed transform-controls executable was also open;
it was preserved. Its SHA-256 differs from the current build.

## Corrections

- Analytical snapping included ordinary walls but omitted the sloped-wall tool.
  A real Qt regression failed at the snapped-start assertion before the fix;
  it now checks Snap enabled/disabled, preview length and accepted rise commit.
- Door/Window quick actions and catalog activation now share preset preparation
  and one Style/width/height/sill editor. Library double-click arms the hosted
  placement preview immediately; drag-drop uses the same admission path.
- Window defaults to Fixed 1200 and Door to Hinged 910 Left. Changing Style loads
  the selected preset's dimensions. The catalog categories explicitly identify
  wall openings; furniture remains freely placed.
- A regression failed because the shared chooser was absent, then passed with
  style selection, physical width, invalid overlap refusal and existing
  catalog placement/save/reopen/undo/redo coverage.
- Independent review found grid rounding could move an exact click off a thin,
  off-grid diagonal host. A real Library double-click regression failed with
  the old rounded input. Hosted placement now consumes raw input and performs
  its existing analytical host projection; the user's Snap preference is
  retained. Drag-drop had the same failure independently; it now uses raw
  input for catalog openings while ordinary component drops keep grid snapping.
- Native inspection caught binary floating-point noise in the width field.
  A regression reproduced it; preset widths now use shortest round-trip decimal
  formatting (`0.6 m`) without reducing stored measurement precision.

The first integrated three desktop checks passed in 14.52 seconds. Final build
and native interaction evidence is retained under
`artifacts/wall-placement-coherence-20261001`.
The final seven focused checks passed in 25.98 seconds: SVG desktop placement,
wall/opening palette, wall chains, exterior measurement desktop, canvas input,
source-kit contract and requirements contract. Independent review accepted the
raw-input and drop corrections; package/native verification is recorded in the
artifact directory separately.

## Observed native workflow

The exterior-network package was exercised by actual mouse clicks: four walls
formed a closed outline, displayed lengths and endpoint markers, and a fifth
partition snapped to the host wall. Escape retained the partition. Selecting
that partition and measuring exterior showed four included walls, one excluded
partition and 223.1 ft². Creating the measurement retained all five walls.
Screenshots are under `artifacts/exterior-wall-networks-20261001`.

The current Style-chooser executable has SHA-256
`C14E06A7F99704EE8FA3330A1ED36E100723C6BFA1CD5E03AD42087896591FF2`.
This records an internal build, not complete production qualification.

## Outstanding scope

Full catalog subtype behavior still requires qualification: bay window and
roof-hosted skylight construction, and pocket/sliding/bifold door mechanisms,
are not certified by a width preset or generic hosted cut. These are explicit
production gaps. The retained source-replacement workflow after additions,
curved exterior networks, Apex fixtures/adapters and the full production gate
also remain open.
