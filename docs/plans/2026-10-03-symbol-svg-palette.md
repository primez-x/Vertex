# Per-instance SVG drawing colors

Implement the remaining SVG-color authoring gap within APX-ANNO-003 without
changing the detailed symbol catalog, pinned source bytes, hashes or geometry.
The existing selection quick properties expose Colors. An instance inherits
the library appearance until an explicit outline/surface palette is accepted.
Use library colors removes only that instance's override. Cancel and unchanged
Apply do not create history; one accepted change has one Undo step.

## Presentation contract

The supported `white-outline-2` profile has explicit paint roles: `#111111`
outline strokes; white and `#f2f2f2` primary flat surfaces; primary gradients
ceramic, rim, steel, upholstery, cushion, linen, wood, counter, foliage, leaf
and tread. A primary white stop becomes the selected surface color and its
`#f2f2f2` shading retains the 242/255 channel ratio. Well, steelwell, glass and
dark gradients, dark fills, gray detail strokes, opacity, transparency, `none`
paint, gradient layout, geometry and all other colors remain unchanged.
This profile is a documented color vocabulary, not a guess from luminance in
arbitrary artwork. Unsupported profiles refuse editing with a useful error.

Create only a derived render copy with a safe XML parser. Never rewrite a
project's pinned SVG or catalog asset. The cache identity includes original
artwork SHA and explicit palette profile/version/colors; transforms and
selection do not change that identity. Canvas and shared printed/PDF output
use the same derivative. Invalid persisted palette rendering must be reported
and must not silently authorize incorrect output.

## Storage

Append optional SymbolSvgPalette to SymbolInstance. Its JSON envelope has
version 1, profile, outline_color and surface_color. Unknown/null/malformed
fields, invalid colors and palettes on procedural symbols reject. Absence
preserves old behavior; explicit default-valued colors remain authored intent.
Any explicit palette emits annotation state 7. Native format 25 and extraction
23 qualify typed state 7 across all retained history, including undone/deleted
records. Old readers refuse unsupported intent rather than discard it.
Existing raw owner policies, unrelated records, metadata and artwork survive
targeted edits, native save/reopen and clipboard operations.

## Ownership and verification

The model worker owns annotation_catalog.hpp/.cpp and core codec fixtures.
The rendering worker owns plan_canvas.hpp/.cpp, the Qt palette helper and its
focused tests. Root owns desktop editing, native workflow fixture, storage and
extraction qualification, CMake/generators, docs, integrated review, Git and
packaging. Writers return ownership and freeze before any build.

Verify old absent behavior, exact artwork and geometry preservation, strict
envelopes, all catalog profile assets, protected recess/detail paint, distinct
palettes of identical artwork in both cache orders, selected transparency,
actual UI controls and color pixels, shared canvas/sheet/PDF, reset/history,
native save/reopen, clipboard, stale/read-only/unsupported refusal and format
downgrade refusal with a recomputed digest. Inspect actual captures. Finish
with the scoped commit, push and remote verification and updated offline build.
This does not complete production acceptance or ANSI/Apex qualification.
