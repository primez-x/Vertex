# Reference grids

Reference grids are persisted presentation entities that share the document's
model-space coordinates. They are useful for residential and light-commercial
floor plans, structural coordination, and field alignment without changing the
measurement boundary or architectural object model.

Each `reference_grid` entity has a `properties.model` object with the exact
version-1 shape below:

```json
{
  "version": 1,
  "origin_m": [10.0, -4.0],
  "rotation_radians": 0.25,
  "spacing_x_m": 2.0,
  "spacing_y_m": 3.0,
  "count_x": 2,
  "count_y": 1,
  "major_every": 2,
  "x_label": "Grid X",
  "y_label": "Grid Y",
  "visible": true
}
```

`origin_m`, rotation, and spacing are model values in metres and radians.
`count_x` and `count_y` describe the number of lines on each side of the
origin; the origin line is included, so a grid contains
`(2 * count_x + 1) + (2 * count_y + 1)` lines. Major lines are deterministic
multiples of `major_every`. Labels are retained as grid metadata for future
axis annotations and are validated as printable UTF-8 text.

The Windows **Reference grids** editor creates, edits, removes, and toggles
these entities through the ordinary revision-checked `Document` command path.
The editor preserves unknown entity properties, rejects invalid numeric or
label input before mutation, and reports stale dialog revisions instead of
overwriting newer work. Both measurement and architectural canvases render
the same validated line list; reference grids are never used as measurement
geometry and never alter saved world coordinates.

The current slice covers deterministic geometry, persistence, canvas display,
undo/redo, and save/reopen. Grid labels, site/terrain coordination, automatic
level-driven placement, and full cross-view production qualification remain
separate requirements.
