# Building entity codec

`sketch/building_entity.hpp` is the semantic bridge between the supported
architectural parameter structs and the document layer's `Entity`.  It is the
single contract used by authoring, derived-shape creation, and future save or
viewer adapters.  The codec is deterministic and offline; the geometry in an
entity is always reconstructed by `make_building_shape` from its parameters.

The canonical entity types are:

| `Entity.type` | `properties.form` values |
| --- | --- |
| `column` | `rectangular_column`, `circular_column` |
| `beam` | `straight_beam` |
| `stair` | `straight_stair_flight` |
| `roof` | `sloped_roof_panel`, `gable_roof`, `hip_roof` |

Every encoded properties object contains integer `version` and a string
`form`.  Required lengths use an `_m` suffix, angles use `_rad`, and vectors
are exact arrays of three finite numbers.  The remaining canonical fields are:

Version 1 remains the format for objects without roof openings. Version 2 is
reserved for roofs carrying `roof_openings`, an array of at most 256 objects
with `id`, `x_m`, `y_m`, `width_m`, and `depth_m`. IDs must be unique and obey
the building ID syntax. Version 1 with this reserved field is rejected;
readers limited to version 1 reject a roof with openings instead of losing
its cuts. Removing all openings through the editor returns the roof to
version 1 and removes the field. A version-2 empty array can be decoded and
normalizes to the uncut version-1 representation when edited.

Opening X/Y coordinates are in the roof's horizontal local frame: the panel
origin is its un-overhung lower-left footprint corner, while gable/hip origins
are the footprint centre. Width and depth are horizontal projected distances;
the cut is vertical through the entire solid, including across a ridge.
Openings cannot overlap or touch and must stay inside the un-overhung footprint
with clearance of the roof thickness plus geometry tolerance on every edge.
These are geometric through-openings, not hosted skylight products, curbs,
flashing, framing, or structural checks.

The desktop **Openings** editor applies all rows atomically. Changed coordinate
expressions, default units and exact metre rationals are retained by opening ID
under `extensions.roof_opening_input` (version 1, `entries`). These receipts are
historical input metadata, never authoritative geometry. Removing an opening
removes its receipt; untouched canonical coordinates retain their precision.
Reopening displays an original expression only when its stored default unit,
exact rational and canonical value validate together. A suffixless expression
gets an explicit unit when necessary after a workspace-unit change. Invalid
receipts fall back to canonical metre text and do not alter the document merely
by being viewed. Entering an equivalent expression can create a new input
receipt without changing the opening geometry. Quantity errors identify the
opening row and coordinate column.

```json
{
  "version": 1,
  "form": "rectangular_column",
  "base_center_m": [1.0, 2.0, 0.0],
  "width_m": 0.4,
  "depth_m": 0.6,
  "height_m": 3.0,
  "rotation_rad": 0.0
}
```

The circular form uses `base_center_m`, `radius_m`, and `height_m`.  A straight
beam uses `start_m`, `end_m`, `up_dir` (a dimensionless direction), `width_m`,
and `depth_m`.  A stair flight uses `base_position_m`, `orientation_rad`,
integer `riser_count`, `total_rise_m`, `going_m`, `width_m`, and a required
`top_landing` key.  `top_landing` is either `null` or an object containing
`depth_m` and `thickness_m`.

A sloped panel uses `base_position_m`, `orientation_rad`, `run_m`, `span_m`,
`rise_m`, `pitch_rad`, `overhang_m`, and `thickness_m`.  A gable roof uses the
same orientation, rise, pitch, overhang, and thickness fields with
`length_m` and full `span_m` in place of `run_m`.  Pitch/rise consistency and
all solid-builder constraints are checked during decoding, before a caller can
create an authoring command.

The equal-pitch `hip_roof` uses the same property names as `gable_roof`, with
`length_m >= span_m`; equality denotes a square pyramidal roof. Rise and pitch
describe half the span. Its thickness is normal to each slope with vertical
trims at eaves and joins. Older readers that do not support `hip_roof` must
reject it as an unsupported form rather than reinterpret it as a gable.

`encode_building_entity(object, metadata)` calls `Entity::create` for a fresh
entity.  A nonempty semantic object ID replaces the generated factory ID;
empty IDs retain the generated ID.  `metadata` must be a JSON object and is
copied to `Entity.extensions`.  The encoder emits only canonical semantic
properties and ignores no input because the variant is already typed.

`decode_building_entity(entity)` requires a recognized canonical type, version,
form, every required field, finite numeric values, exact vector lengths, and
valid derived geometry.  It preserves `entity.id` in the returned semantic
object.  Decoding is read-only: unrelated properties and extension metadata
are ignored while the original `Entity` remains available to the caller.  An
edit caller should merge changed canonical fields into that existing entity
and retain its unknown properties and extensions; re-encoding the returned
variant intentionally emits only the known canonical fields unless the caller
supplies metadata again.

`can_recognize_building_entity_type` answers only whether the type vocabulary
is known.  It returns true for `column`, `beam`, `stair`, and `roof` even when
the entity's version, form, or geometry is malformed.  Decode success is a
separate, stricter operation.  Unknown types, versions, and forms fail with
`std::invalid_argument` and an explanatory message.

The codec does not expand the generic document validator's semantic model,
does not claim production-complete stair or roof authoring, and does not
replace command atomicity, stable links, persistence, material assemblies, or
regulatory/structural checks. All six current forms have parameter editors;
the codec remains usable independently of the UI and supplies the geometry
and field contract to every caller.
