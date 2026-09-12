# Hosted opening authoring

Select a wall and run **Create door opening** or **Create window opening**.
One editor provides offset along the wall, width, sill height, and height, using
the current input unit and the shared exact measurement parser. Explicit units
and fractions are supported. A live elevation preview shows existing openings
and the proposed opening. Curved walls are shown as an explicitly labeled
unrolled elevation; horizontal distance follows the wall's centreline arc length.

The editor validates the complete host and its openings with
`validate_wall_semantics`. Invalid input, out-of-bounds openings, and overlaps
disable creation and display an inline explanation. Invalid drafts are removed
from the preview. Correcting the fields restores the preview and Create button.
Cancellation leaves the document unchanged.

The main window captures the source document, revision, selection, layer, and
units before opening the editor. Submission checks that context, then uses the
existing hosted-opening command and solid validation. A successful creation is
one undoable document operation. The object retains its host, dimensions, and
door/window classification through the ordinary project format and schedules.

Verification: `desktop_smoke` exercises the actual command palette, invalid and
corrected drafts, fractional input, creation, undo/redo, and curved-host overlap
checks. `modal_authoring_tests` intervenes with project replacement, selection,
layer, units, and revision changes while the new editor is open.

Door creation optionally enables a 90-degree swing. The door inspector's
**Door swing** editor changes the start/end jamb, left/right side, and angle
(greater than zero, at most 180 degrees), or removes the symbol. Jamb order and
swing side are relative to the host wall's drawing direction. The optional
`door_operation` property stores `version: 1`, `hinge: "start" | "end"`,
`side: "left" | "right"`, and numeric `angle_degrees`. Unknown versions, invalid
fields, and invalid angles are rejected by Document validation. Unspecified
existing openings receive no inferred swing.

Plan output uses a straight leaf and an analytic circular arc, sharing geometry
between the canvas and printed/exported plan scene. On curved walls, the closed
leaf follows the chord between jambs. Schedules expose read-only hinge, side, and
angle with source provenance. Ordinary dimension edits preserve operation data;
conversion to a window retains it as dormant metadata and suppresses its symbol
and schedule cells. Undo or conversion back to a door restores its use.

Type-driven door/window assemblies, frame/leaf/glazing solids, and the full
ARCH-MOD-002 export acceptance remain open. The current
architectural solid is the cut in the host wall, not a complete manufactured
door or window assembly.
