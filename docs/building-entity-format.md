# Building entity codec

`sketch/building_entity.hpp` is the semantic bridge between the six
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
| `roof` | `sloped_roof_panel`, `gable_roof` |

Every encoded properties object contains integer `version: 1` and a string
`form`.  Required lengths use an `_m` suffix, angles use `_rad`, and vectors
are exact arrays of three finite numbers.  The remaining canonical fields are:

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
