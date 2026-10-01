# Exterior appraisal measurements from wall networks

The exterior-measurement command now recognizes the unique simple enclosing
perimeter in a straight wall layout containing partitions. Selecting a connected
interior wall can identify that shell. Explicit multi-selection remains the
candidate set. The review shows included wall count, excluded partition/branch
count, proposed exterior area, and all candidate baselines; exclusions appear
amber. Only whole perimeter-wall identities enter the retained v1 provenance.

Recognition analytically subdivides contacts for its derived planar graph,
removes bridges with iterative traversal, and walks faces. It does not change
source walls or choose an arbitrary largest room. It supports unsplit and split
T contacts, interior chords/crossings, concave outlines, contained interior
loops, and connected non-enclosing spurs. Disconnected geometry must lie strictly
inside the unique exterior. Overlapping walls, incomparable outlines, partial
source-wall contributions, curves, and unresolved precision reject explicitly.
Existing strict exterior offsets, source-current validation, and refresh remain
unchanged. Openings do not alter the measured building perimeter.

Discovery isolates resolved property/building/floor/layer context, phase, and
elevation. Core and desktop use resolved level placement and compare the entire
elevation range within the model tolerance, accepting arithmetic roundoff without
chaining offsets into different planes. Source entities and level graphs remain
unchanged. Limits bound candidates, intersection fragments/nodes, and containment
work; dense unsupported input raises an ordinary diagnostic rather than reducing
geometry or risking recursive stack exhaustion.

## Observed verification

The missing API first failed compilation. The new desktop review test then
failed on missing exclusion disclosure. Independent review found a raw-elevation
comparison that missed level offsets and legacy aliases; its regression failed
before correction. A further regression reproduced rejection of mathematically
equal level arithmetic, then passed with a complete-range tolerance check.

The final Release desktop, CLI and import-worker build succeeded. Six focused
CTest cases passed in 11.46 seconds: appraisal_document, wall_chain_connection,
connected_wall_canvas, wall_corner_canvas, wall_measurement_desktop, and
wall_measurement. They cover exact exterior areas and thickness offsets,
partition/chord/X discovery, reversed/shuffled and translated/rotated geometry,
source/context/elevation isolation, resource rejection, cancellation, dimensions,
square-foot totals, repeated-owner reuse, retained refresh, and save/reopen.
The user checklist adds practical task U294; its user acceptance remains untested.

Release vertex.exe SHA-256:
`BD931F40EFD3A62A6372B2FCAEF20A17F295B7300585C8241F5008C783779576`.
Package checks and actual mouse evidence are retained with runtime artifacts;
the automated evidence here does not claim those observations in advance.

## Remaining scope

A retained measurement follows its original source-wall IDs. Adding an enclosed
extension can produce a different perimeter and another measurement owner;
refresh does not rediscover or replace the accepted source list. A regression
proves that overlapping old/new owners with individually valid residential facts
withhold aggregate appraisal calculation rather than double-count it. A reviewed
replacement workflow preserving metadata and references still requires work.

Curved exterior derivation, partial-source contributions, changing thickness
along a collinear exterior run, measurement-standard qualification, Apex sample
migration, device testing, source/license closure, and clean-machine production
qualification remain open. This development checkpoint advances the active
production goal and does not certify full parity or a finished application.
