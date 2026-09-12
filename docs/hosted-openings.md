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

Door handedness/swing, type-driven door/window assemblies, frame/leaf/glazing
solids, and the full ARCH-MOD-002 export acceptance remain open. The current
architectural solid is the cut in the host wall, not a complete manufactured
door or window assembly.
