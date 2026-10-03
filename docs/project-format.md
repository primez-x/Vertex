# Vertex project formats v1 through v22

## Original typed wall input

A physical wall created with exact drawing input can retain
`properties.original_drawing_input`, encoded with the existing standalone
ConstructionReceipt codec. Inline cardinal input uses `line_rise_run`; the
precision form also supports headings, relative turns, world coordinates and
the four analytical arc constructions. The receipt retains entered Quantity
and AngleInput expressions. Its segment ID is the initial wall ID. Creation
replays the receipt and requires its endpoints and sweep to match the new
authoritative baseline exactly.

Precision wall input also retains `properties.original_drawing_input_context`:
`version: 1`, `expected_start` as a two-element model-metre array,
`previous_segment` as the original canonical baseline or null,
`closure_anchor` as a two-element model-metre array or null, and
`tolerance_metres`. Only relative-turn receipts retain a `previous_segment`;
only closure receipts retain a `closure_anchor`. Other receipts store null
for these unused fields. This preserves the ending tangent needed to
replay a relative turn after a straight or curved wall. It is a historical
context, not a live dependency or a replacement for persistent endpoint
constraints. Older inline walls with a rise/run receipt need only its start
and the documented geometry tolerance to replay their original input.

This is historical input provenance. The wall's current `baseline` remains the
geometry authority; later moves or dimension edits can differ from the original
receipt. Readers must not substitute replayed original input for current
geometry. It is ordinary optional preserved metadata, requires no new project
format version and survives native history and save/reopen.

Reusable text templates use a separate [local text-library format](text-library-format.md).
Placed labels store their complete content and style in the project; opening or
editing them does not require that library file.

Annotation state version 5 adds optional label `model_plan: true`: its position
is a world XY anchor rendered only in horizontal plans, projected through each
plan frame. Rotation remains view-relative for readable, consistent text.
Absence keeps the legacy view-overlay convention. The field is refused in
versions 1–4; old readers refuse version 5 rather than silently misplacing it.
Version 5 retains the version 4 area-presentation and version 3 symbol schemas.
Encoding uses version 6 for wall measurements, otherwise version 5 when any
plan-anchored label exists, version 4 for area placement overrides, otherwise
version 3. Raw edits may retain a higher known
version after its last optional record is removed. The SQLite layout is unchanged.

## Derived wall measurement presentation

Annotation state v6 adds presentation overrides with `target_kind: "wall_dimension"`
and `target_id` equal to a wall's stable ID. The displayed value is always the
current analytical baseline length; no copied numeric measurement is stored.
`visible` suppresses only the measurement, never the wall or its openings.
Style controls text color, font and emphasis. `inherit_appearance: true` retains
the derived theme appearance for placement/visibility-only edits.

Optional `plan_label_offset_m: [x, y]` is a finite world XY offset from the
analytical midpoint. It follows that anchor as wall geometry changes; absence
restores automatic exterior placement. Optional `paper_text_height_mm` is a
finite positive value at most 100 mm (the editor permits 0.5–20 mm).
Optional `plan_label_rotation_radians` is a finite world-plan angle; absence
retains the upright wall-derived angle. A horizontal named view projects the
anchor, leader and angle. Pointer placement is inverse-projected before saving.
Wall measurement records cannot override wall outlines or hatches through
`paper_line_width_mm` or `hatch_scale`.

The target kind and new fields require v6. That version retains v4 area offsets,
v5 authored plan labels and v3 symbol transforms. Earlier readers refuse v6;
earlier states without wall measurements keep their previous encoding versions.
Raw updates preserve unrelated records, pinned artwork and opaque metadata.

## Exterior measurements derived from walls

An identified measurement boundary may carry `properties.wall_measurement_source`
with exactly `version: 1`, `basis: "exterior"` and `walls`. Each wall record has
exactly a stable `id` and a `context` object. Context may record nonempty
`property_id`, `building_id`, `floor_id`, `layer_id` and `phase_id` strings.
Records are unique and stored in ID order; they identify the complete perimeter
walls, excluding interior partitions and branches.

The boundary stores analytical line and circular-arc segments in the ordinary
identified-boundary format. Its construction receipts retain those curves.
The source record does not cache numeric area or perimeter. Currentness derives
the exterior again from current wall baselines and thicknesses, then checks
context and the complete analytical outline, including signed curvature.
Missing sources, changed geometry, malformed provenance or manual outline edits
withhold qualified appraisal quantities until repaired or refreshed. Openings
do not change this exterior outline. Refresh uses the recorded perimeter wall
IDs; an addition with different perimeter membership needs a new measurement.

## Unfinished Auto-Subtract drawing

An unfinished drawing with an explicit parent uses active recovery envelope
version 2, replay version 1. It adds the required nonempty
`auto_subtract_target_id` to `version`, `replay_version`, `source`, `checkpoint`
and `extensions`. The checkpoint and workspace-history schemas remain version 1.
Ordinary drawings retain their exact five-field active version 1 envelope.
Auto-Subtract and the desktop redraw operation cannot be combined. The chosen
parent participates in lifecycle/finish replay, retired-input validation and
resource accounting; it is not an inferred parent or a desktop-only annotation.
Unknown positive active versions remain opaque and make the owning recovery
history read-only. Completed areas use the existing parent `deduction_ids` field;
the final commit includes both new entities and the changed existing parent.

## Circular column selection orientation

Circular columns retain an optional finite `rotation_rad` property in radians.
It records the orientation of the selection frame; the cylinder's physical
geometry is rotationally symmetric. Older columns without this property read
as zero. Transform commands update it, while dimensional and property edits
preserve it. Undo/redo and project history retain the value alongside geometry.

## Symbol instance transforms

Area presentation overrides may carry `plan_label_offset_m: [x, y]`, two
finite model-space metre offsets from the owner's derived plan-label anchor.
An area uses `target_kind: "area"` for this field. It controls the placement of
derived names and quantities; it does not store a numeric area value or change
geometry. An explicit zero offset is a manual centered placement. Absence
restores automatic placement. Annotation state version 4 or later is required when an
offset is present; versions 1–3 cannot admit it. Placement-only records may
also carry `inherit_appearance: true`, so positioning
a label retains semantic colors, fills and linework. Explicit appearance edits
remove this flag. Version 4 retains the version 3 symbol representation. States
without offsets, appearance inheritance, plan-anchored labels or wall measurements encode as version 3,
and future unknown versions remain unsupported.

Area presentation overrides optionally carry `paper_line_width_mm` (finite,
0.05–10.0) and `hatch_scale` (finite, 0.1–10.0). Both fields are optional within
annotation state version 3: legacy records retain their existing defaults and
encode without the fields when absent. These values affect presentation only;
they do not change analytical geometry or area calculation facts. Desktop area
appearance edits preserve unrelated raw annotation records and metadata.

Annotation state version 3 adds `width_scale`, `depth_scale`, `flip_horizontal`
and `flip_vertical` to each saved symbol. Dimensions equal the saved definition's
physical width/depth multiplied by `placement.scale` and the corresponding axis
factor. Mirroring and axis scaling act about the saved definition anchor before
rotation and translation. Factors must be finite, positive and within the saved
definition's effective scale limits. Rotation remains a model-space radian angle.
Version 1 and 2 annotations migrate with axis factors of one and both flips false;
their pinned definition and exact SVG bytes remain unchanged. Labels retain their
existing placement model. Each resize, flip or rotation is a normal undoable
document command, and rendering and export consume the same saved transforms.

Vertex projects are standalone SQLite files containing one immutable logical
document snapshot and the complete command history known when that snapshot was captured.
The file is an interchange/save artifact. The current foundation keeps the working document
in memory; it does not claim to be a live SQLite working journal.

The bundled `vertex-cli` provides local format operations without a hosted service:
`inspect` reports document identity, revision, entity/asset counts, history, and editability;
`validate` loads and checks the storage, structural references, logical digest, and asset bytes;
`extract` writes a new JSON-and-assets directory; and `migrate <source> <destination>` loads a
supported project and writes a validated copy at the current storage version. Migration refuses
an existing destination, reports source and destination SHA-256 fingerprints, and verifies that
the source hash is unchanged. A failed migration leaves the source and any existing destination
untouched.

These CLI operations also recognize supported ordinary and recovery-copy
archives. Inspection reports their role and recovery record counts; validation
checks the archive and its replayable recovery state. Migration preserves the
complete ledger and its role through `save_archive`, rather than saving only
the document. Extraction writes exchange version 12 with the complete raw
recovery records alongside document history and assets. The SQLite format
version has a v4 minimum for recovery-bearing archives; retained document
proofs can require a higher version.

An unknown recovery kind or version can be inspected only as an opaque archive:
the CLI reports its source fingerprint, diagnostic and record metadata with
`editable: false` and `validation_complete: false`. It does not expose a
forkable document snapshot. Validate, migrate and extract refuse that state
before creating an output. Damaged storage or a failed digest remains an error.

Retained typed `ApplyBoundaryConstraintChanges` endpoint commands replay through the same analytical
topology checks used by interactive authoring. A matching command/result pair
and recomputed logical digest cannot authorize new undeclared wall contacts,
crossings, overlap or reversal of a protected closed loop. Unsafe history
rejects with an integrity diagnostic; the loader preserves the original bytes.
Ordinary explicit construction/transform commands retain their separate policy.
New straight wall-only endpoint authoring retains this typed intent. Historical
straight wall-only authoring stored as generic `ApplyEntityChanges`
does not encode endpoint-edit intent and is not covered by this typed replay
policy; its missing intent is not inferred retrospectively.

Version 2 retains the v1 table structure and adds a mandatory compatibility
boundary for identified geometry. Any identified boundary, boundary draft or
dimension in retained history requires v2, including an undone or deleted
identified boundary. Both SQLite `user_version` and `metadata.format_version`
must agree, and the logical digest includes that version. The reader accepts
v1 legacy history, v2 identity history, v3 construction-receipt history, v5
translation history, v6 transform history, v7 boundary-coordinate edit
history, v8 boundary-constraint transactions, v9 measured group translations,
v10 curved endpoint constraints, v11 straight wall-only intent, v12 physical
arc-length locks, v13 direct physical curve-length inputs, and v14 rigid curve
construction transforms, v15 fixed-chord boundary curvature edits, v16
reviewed exterior wall-source replacements, and v17 explicit fresh-topology
boundary replacements, v18 measured group rigid transforms, and v19 coordinated
physical wall/exterior measurement changes, and v20 mixed ordinary object/asset
and exterior changes, v21 ANSI-oriented appraisal evidence, and v22 coordinated
exterior-corner edits, plus v4 through v22 archives through
recovery-aware APIs.
Under-versioned semantic data and versions above 22 reject. Legacy-only history
is still written as v1. Unknown boundary entity
versions in v2 remain preserved read-only. See `boundary-entity-format.md`.
Version 2 also recognizes `dimension` entities. Segment-length dimensions refer
to one stable child ID; angle dimensions refer to two stable child IDs and their
shared vertex; area dimensions refer to the complete identified closed boundary.
Every retained state validates the supported references. Unknown dimension
versions and kinds remain opaque and make the project read-only. A supported
dimension on an unknown boundary version is preserved read-only without
guessing its geometry.
See `boundary-dimensions.md` for the typed dimension contract.

Version 3 retains the v1 and v2 tables and adds a storage guard for the reserved
`boundary_authoring` property. The guard first qualifies an explicit identified
boundary: a recognized boundary type with supported integer
`boundary_model_version: 1`. A `boundary_authoring` property on that owner
requires v3 anywhere in retained history, including an undone or deleted
entity, regardless of the envelope version or shape. Generic entities and
anonymous legacy boundaries may retain a vendor collision in v1. An unknown
positive boundary model remains v2 and opaque. The property is preserved as
opaque JSON through save and load; receipt envelope validation, replay
semantics, and editability belong to the document and receipt codec layers.
See `boundary-authoring.md` for the authoring contract.

## Boundary construction envelopes v1 and v2

On a supported identified boundary, `properties.boundary_authoring` has exactly
these fields: `version` (1 or 2), `replay_version: 1`, `boundary_id`, `anchor`,
`segments`, and `extensions`. `anchor` is a finite `[x, y]` point in metres;
`boundary_id` equals the owning entity ID. `extensions` is an opaque JSON
object. Both known schemas implement replay algorithm version 1. An unknown
positive replay version preserves the complete envelope and makes the retained
document read-only, as does an unknown positive schema version. Missing, zero,
negative or noninteger version fields reject for recognized schemas. Unknown
schemas remain opaque without assuming their payload shape.

`segments` is a nonempty ordered array. Each member contains exactly
`segment_id`, `start_vertex_id`, `end_vertex_id`, and `receipt`. Its identities
must match the corresponding canonical boundary segment. The receipt repeats
`segment_id` and contains `kind`, `start`, `clockwise`, and the fields below.
There are no optional or additional fields for a known kind.

| `kind` | Additional required fields |
| --- | --- |
| `line_heading` | `distance`, `heading` |
| `line_rise_run` | `rise`, `run` |
| `line_relative_turn` | `distance`, `turn` |
| `line_closure` | `closure_delta` |
| `arc_chord_angle` | `chord_end`, `angle` |
| `arc_chord_height` | `chord_end`, `height` |
| `arc_chord_length` | `chord_end`, `arc_length` |
| `arc_start_tangent` | `tangent`, `arc_length`, `sweep` |

Schema v2 adds `line_to_point`, requiring only `chord_end` beyond the common
receipt fields. Schema v1 remains closed to the eight kinds above and rejects
`line_to_point`. Current authoring sessions emit schema v2, while older schema
v1 records retain their version when re-encoded. The SQLite storage version
remains 3 for either envelope.

Receipt schema v3 adds a required `transforms` array. Each entry has exactly
`pivot`, `rotation_radians`, `flip_horizontal`, `flip_vertical`, and `offset`.
Points are finite two-number arrays, rotation is a finite number, and flips are
Booleans. Replay first reconstructs the original local receipts using schema-v2
rules, then applies each transform in order: rotate about the pivot, reflect X
and Y about the pivot as requested, and translate. An odd number of reflections
reverses arc sweep. Each resulting boundary must remain valid. Replay rejects
non-finite residuals, endpoint or analytical-length drift beyond its geometry
tolerance, and accumulated pure-translation rounding beyond that tolerance;
see `geometry-operations.md` for the precision contract.

The stored anchor, receipt coordinates, closure vectors, exact quantities, and
entered expressions remain local and unchanged. Replayed edges and anchor are
world coordinates; replayed receipts still contain their original local inputs.
Copies may remap typed identities. Extensions are preserved without interpreting
identifier-shaped user data. An empty transform array is valid. Schema v1/v2
reject the transforms field and retain their original encodings; new drawing
sessions still emit v2. The SQLite receipt storage minimum remains format 3;
explicit in-place translation history independently requires format 5.
Explicit in-place rotation/reflection history requires format 6.
Direct stable-ID vertex moves and segment-length changes require format 7.

Point construction copies the finite endpoint directly into a straight segment
after checking its exact start and minimum chord length. It performs no angle
conversion or synthetic quantity parsing. It records a coordinate-defined edge;
it does not establish click origin, a typed measurement, a snap relationship or
a geometric constraint. Snapping resolves coordinates before this command.

Points and deltas are finite two-number arrays. `clockwise` is a Boolean; it
must be false except for chord-length construction, whose unsigned length
needs this direction choice. Signed angle, height or sweep inputs carry
direction for the other arc forms.

A quantity object has exactly `metres`, `exact_metres`, `entered_unit`, and
`original_expression`. `exact_metres` contains signed 64-bit `numerator` and
positive signed 64-bit `denominator`. `entered_unit` is one of `metre`,
`millimetre`, `centimetre`, `foot`, or `inch`. Parsing the nonempty expression
with that default unit must reproduce the stored quantity exactly.

An angle object has exactly `radians`, `original_expression`, and
`normalized_expression`. Both nonempty expressions must parse to the stored
finite radians without a tolerance. The normalized expression is the canonical
round-trip decimal radians string produced by the v1 codec.

Replay derives each edge from these inputs, using the preceding edge for a
relative turn and the initial anchor for generated closure. It must reproduce
the canonical ordered topology and analytical coordinates exactly, and the
result must be a valid closed boundary. Receipts document a reproducible
construction; they do not prove that a person entered an expression. Display
rounding never changes these stored values.

## Document contract

### Typed command envelopes

Application edits cross the workspace boundary as a version-1 JSON command
envelope. `sketch::command_to_json` and `sketch::command_from_json` preserve
the command kind, expected revision, entity and asset changes, quantities,
metadata, and boundary transforms. Known envelopes reject unknown fields,
unsupported versions, invalid identifiers, non-finite coordinates, malformed
asset hex, and asset digest mismatches before a command can be applied.

The supported `kind` values are `apply_entity_changes`, `name_revision`,
`translate_boundary`, `translate_boundaries`, `transform_boundary`,
`edit_boundary_geometry`, and `apply_boundary_constraint_changes`. An apply envelope contains
typed `entity_changes` and `asset_changes`; an upsert carries the complete
entity or asset payload and an erase carries its stable ID. Assets use a
lowercase `bytes_hex` representation and retain their SHA-256. Translation
and transform envelopes carry explicit finite `offset`, `pivot`, rotation, and
reflection fields. Boundary geometry envelopes carry the strict edit intent
described above. `ProjectWorkspace::prepare` round-trips each command
through this codec on an isolated immutable fork before staging one document
revision, so one accepted compound operation has one undoable history entry.

Every semantic entity has a stable ID, a type, a JSON `properties` object, a `required` flag,
and a JSON `extensions` object. IDs are document identity and are never derived from geometry.
The v1 known types are:

`property`, `building`, `floor`, `layer`, `boundary`, `measurement_boundary`, `room_boundary`,
`wall`, `wall_join`, `opening`, `room`, `slab`, `roof`, `roof_join`, `stair`, `railing`, `column`, `beam`, `label`, `sheet`, `view`,
`constraint`, `reference_grid`, `terrain_surface`, `dxf_source`, and `ifc_source`.

All geometry properties use metres and radians. A wall and opening can be represented as:

The root `property` entity may persist `calculation_workflow` as `"measurement"`
or `"appraisal"`. Its `calculation_profile` contains `id`, positive `version`,
`display_unit`, `decimal_places`, and a `classifications` object. Every
classification rule stores boolean `building_total` and `living_total` values
plus an `appraisal_category` token. Supported appraisal tokens are `none`,
`above_grade_finished`, `above_grade_unfinished`, `below_grade_finished`,
`below_grade_unfinished`, `garage`, `carport`, `porch`, `patio`, `deck`, and
`other_non_living`. Appraisal workflow uses the versioned `vertex-appraisal`
profile; a prior measurement profile is retained separately as
`measurement_calculation_profile` so switching workflows is reversible. Missing
workflow or appraisal-category fields migrate to measurement and `none`, which
prevents an older project from silently acquiring GLA classifications.

Appraisal display uses only `calculation_profile.decimal_places` (integer 0–6,
default 2) and the current workspace unit. Its saved `version` is a positive local
display-configuration revision, separate from the fixed appraisal policy version.
An Area display edit updates only `decimal_places` and `version` when the profile
object exists, retaining other fields and extension metadata verbatim. New
configuration objects use the built-in profile defaults. This uses existing
property metadata; no SQLite format migration is required. Malformed decimal
settings visibly withhold numeric appraisal reports. A malformed or exhausted
configuration revision prevents a display edit without changing geometry or
qualifying/withholding a report that has valid decimal settings.

Generated appraisal schedule cells carry optional `display_decimal_places`
presentation metadata. Their `ScheduleQuantity` values remain unrounded SI;
the metadata is rebuilt from the property and is not a stored schedule format.

Closed area entities retain workflow-specific meanings independently.
`measurement_classification` stores the user-selected measurement rule and
`appraisal_category` stores the explicit appraisal category. `classification`
remains the compatibility measurement value. Projects written by the initial
appraisal preview, which placed an appraisal token in `classification`, are
recognized and migrated on a workflow change. An area created for the first
time while Appraisal is active receives the neutral `measurement` rule when the
user later enters Measurement; Vertex never infers an appraisal category from
its name, floor, or geometry.

```json
{
  "id": "wall-1",
  "type": "wall",
  "required": false,
  "properties": {
    "floor_id": "floor-1",
    "baseline": {
      "start": [0.0, 0.0],
      "end": [5.0, 0.0],
      "sweep_radians": 0.0
    },
    "thickness_m": 0.14,
    "height_m": 2.4,
    "elevation_m": 0.0,
    "slope_rise_m": 0.3,
    "layers": [
      {"id": "outer", "thickness_m": 0.02},
      {"id": "core", "thickness_m": 0.10,
       "material_assignment": {"version": 1, "catalog_id": "assemblies", "material_id": "brick"}},
      {"id": "inner", "thickness_m": 0.02}
    ]
  },
  "extensions": {}
}
```

`properties.layers` is optional. When present it is an ordered, contiguous
wall assembly from the negative to positive side of the wall baseline normal.
Each layer has exactly `id` and `thickness_m`, plus an optional
`material_assignment` object with exactly `version: 1`, `catalog_id`, and
`material_id`. Layer IDs are stable document-local identifiers; thicknesses are
positive metres and must sum to the parent `thickness_m` within the document
precision tolerance. A layer material points to an `assembly_model` entity and
its cataloged material. The same hosted opening geometry is cut through every
layer, while the layer stack remains available for schedules and future
assembly editing. An empty array is equivalent to a monolithic wall.
An optional signed `slope_rise_m` changes the wall-top height linearly from
the baseline start to its end while keeping the bottom at `elevation_m`;
`height_m` is the start height. Nonzero sloped walls currently require a
straight baseline, and hosted openings must fit below the local sloped top.

A `wall_join` is a version-1 architectural relationship that preserves the
source wall entities while providing one derived fused solid for coordinated
views. Its properties contain exactly `version: 1`, `style: "fused"`, and a
`wall_ids` array of two to thirty-two unique wall IDs. Every referenced wall
must exist and be a `wall`; one wall may not belong to more than one join. The
native geometry builder additionally requires every member to share a
connected endpoint with another member and to have overlapping vertical
extents. The fused shape is a derived cache: source wall dimensions, hosted
openings, classifications, and schedule quantities remain authoritative and
are never replaced by the join record.

An opening may carry an optional `properties.opening_assembly` object. The
object is exactly:

```json
{
  "version": 1,
  "kind": "door",
  "frame_width_m": 0.08,
  "frame_depth_m": 0.12,
  "panel_thickness_m": 0.04,
  "glazing_thickness_m": 0.0,
  "inset_m": 0.0
}
```

`kind` must match the opening's `opening_kind` (`door` or `window`). All
dimensions are finite metres; frame and panel dimensions are positive, glazing
is nonnegative, and the panel cannot be deeper than its frame. Window profiles
require positive glazing. `inset_m` is signed toward the wall's left-hand
normal and the profile must fit within the host wall thickness. The profile is
an instance presentation contract: the wall cut, hosted dimensions, and
handing remain the source of truth, while frame, leaf/sash, and glazing solids
are derived for coordinated views.

For windows, profile version 2 contains those same seven fields and exactly
five additional fields: `window_layout` (`fixed`, `double_fixed`, `triple_fixed`,
`casement`, or `sliding`), boolean `window_hinge_at_end`, boolean
`window_open_left`, numeric `window_angle_degrees` in [0,180], and numeric
`window_slide_fraction` in [0,1]. Casement uses the jamb, side and angle; sliding
uses the moving half, track side and fraction. Casement requires zero slide
fraction; sliding requires the canonical angle 90. Fixed layouts require the
canonical movement fields (`false`, `true`, `90`, `0`). Canonical single-pane
profiles serialize as version 1. Version 2 is window-only. Missing or extra
fields, unsupported layouts and dormant conflicting movement values reject.
Both moving layouts currently require a straight host. Split fixed panes
preserve curved-host geometry. Their manufactured parts, plans and exchange
derive from this profile; the original hosted wall cut remains authoritative.

Bay windows use profile version 3: exactly the twelve v2 fields plus
`window_bay_projection_m` and `window_bay_front_fraction`. Only a window with
`window_layout: "bay"` may use v3, and bay layouts cannot use v2. Projection is
positive, finite, bounded to 10 metres, and measured beyond the selected host
wall face. The front fraction is strictly between zero and one and describes
front-face width relative to the full wall-opening width. Geometry admission
also requires room for its mitered frames and glazing. `window_open_left`
selects the projecting side relative to the host direction; bay hinge, angle
and travel remain `false`, `90`, and `0`. Non-bay profiles cannot carry dormant
bay dimensions. Earlier v1/v2 shapes and serialized fields remain unchanged.

A bay is a fixed, directly glazed assembly with three frame facets, sealed
top/bottom plates and mounting shoulders. Its existing panel-thickness field
is retained for profile compatibility and bounds glazing thickness; it does
not describe a separate operable sash. A bay alone adds no floor or appraisal
measurement area. Curved-host bay geometry remains a required unimplemented
feature; current admission explains the straight-host requirement.

An opening may also carry `properties.door_operation`. Version 1 has exactly
`version: 1`, `hinge: "start" | "end"`, `side: "left" | "right"`, and a finite
`angle_degrees` in (0,180]. Version 2 adds exactly `kind` (`hinged`,
`double_hinged`, or `sliding`) and numeric `slide_fraction` in [0,1]. Only a
slider may have nonzero travel. Ordinary hinged operations serialize in the
original version-1 form. Unknown, missing and extra fields are rejected.
The slider's hinge identifies the movable half-panel's jamb, side chooses its
track, and fraction one stacks it behind the fixed half-panel. A profiled
slider requires a straight host and sufficient frame depth for two tracks.
Double doors retain two opposing jamb leaves and two analytic swings. Profiled
double admission rejects leaf/frame/host intersections at the requested pose.
These records remain dormant when the opening is classified as a window;
reclassifying it as a door restores their use. Detailed physical and legacy
limits are documented in [hosted openings](hosted-openings.md).

A `roof_join` is a version-1 architectural relationship that preserves the
source roof entities while providing one derived fused solid for coordinated
views. Its properties contain exactly `version: 1`, `style: "fused"`, and a
`roof_ids` array of two to sixteen unique roof IDs. Every referenced roof must
exist and be a `roof`; one roof may not belong to more than one join. The
native geometry builder additionally requires every source solid to touch at
least one other member. Source roof parameters, openings, materials, and
schedule quantities remain authoritative and are never replaced by the join.

```json
{
  "id": "opening-1",
  "type": "opening",
  "required": false,
  "properties": {
    "wall_id": "wall-1",
    "offset_m": 1.2,
    "width_m": 0.9,
    "sill_m": 0.0,
    "height_m": 2.0
  },
  "extensions": {}
}
```

A slab uses `boundary`, an array of the same `{start,end,sweep_radians}` segments; `holes` is
an array of boundary arrays. Its scalar fields are `thickness_m` and `elevation_m`. An optional
`element_kind` is one of `slab`, `floor`, `ceiling`, or `foundation`; the four kinds share the
same geometry and quantity rules. An optional `layers` array uses the same strict layer object
shape as walls (`id`, `thickness_m`, and an optional version-1 `material_assignment`) but orders
layers from the lower surface to the upper surface. Layer thicknesses must be positive and sum
to the parent `thickness_m` within the document precision tolerance. Each material assignment
resolves to an `assembly_model` catalog and is retained for per-layer quantities and schedules.
The native solid reader and all plan, elevation, section, and 3D projections decode this stack;
each layer receives the same boundary holes at its own elevation so the compound volume remains
the authoritative sum of the layer solids.
Entered unit text and exact quantity fields are separate semantic properties; display units do
not change the metre geometry.

An architectural room volume uses a `room` entity with `boundary` (or the
migration-compatible `segments`) and an optional `holes` array of boundary arrays.
It requires positive `height_m` and finite `elevation_m`; all values are stored in
metres. The same analytical room volume is used by plan, elevation, section,
native 3D, and quantity consumers. `room_boundary` remains a separate 2D
appraisal/space boundary type. A legacy `room` row without height or elevation
can still be displayed as a plan boundary, but solid-driven views report the
missing volume fields instead of inventing a default height.
Complete room volumes are validated when created, changed, or restored from a
native project, including historical revisions. Invalid dimensions, malformed
boundaries, and outside, touching, intersecting, overlapping, duplicate, nested,
or numerically indeterminate holes are rejected before loading succeeds. This
uses the shared analytical boundary validator without requiring a 3D renderer.
Legacy `segments`, `height`, and `elevation` aliases remain accepted; canonical
fields take precedence when both forms are present.

Floors may carry an optional version-1 `vertical_level_binding` object with exactly
`graph_id` and `level_id` (plus `version: 1`). `graph_id` resolves to a `vertical_levels`
entity and `level_id` resolves inside that graph. The Document validator admits the binding
only on floors and rejects a missing graph, wrong graph type, malformed model, or missing level.
Removing the property clears the association without changing world-coordinate geometry.
See [explicit vertical levels](vertical-levels.md).

Walls, slabs, and architectural objects may also carry an optional version-1
`vertical_placement` object with `mode` (`"level"` or `"absolute"`) and a finite
`offset_m`. New objects authored on a floor with a level binding default to
`mode: "level"`; projection resolves the bound elevation plus the offset from
the current snapshot without rewriting the stored coordinates. Absolute mode,
including an omitted placement record, preserves source elevation for imported
or explicitly fixed geometry. The resolver is shared by plan, elevation,
section, and native 3D output and rejects malformed or unbound level requests.

Reference grids are optional `reference_grid` entities. Their `properties.model`
is a strict version-1 object containing `origin_m`, `rotation_radians`,
independent X/Y spacing and extents, `major_every`, axis labels, and `visible`;
all geometry is stored in metres/radians. The model is validated at the
Document boundary and rendered by both desktop canvases from the same line
list. Grids are presentation aids only: they do not participate in area
totals, wall geometry, or measurement truth. See
[reference grids](reference-grids.md).

Terrain surfaces are optional `terrain_surface` entities. Their `properties.model` is a strict
version-1 local triangulated irregular network (TIN) with `provenance`, bounded `points`,
`triangles`, `contour_interval_m`, and `visible` fields. Each point stores a stable `id`,
`x_m`, `y_m`, and `elevation_m`; triangle indices refer to that point array and each edge may
belong to at most two triangles. The model is the measurement authority for the surface:
plan edges and contour segments are derived from it, while native 3D uses the same triangles
as OCCT faces. A terrain entity carries the ordinary property/building/floor/layer placement
links when it belongs to a drawing context. The source boundary and any entered elevation
expressions are retained as authoring metadata; editing the terrain never rewrites the
source boundary. Terrain remains a local surface tool and does not imply a survey provider,
georeferencing service, native Apex compatibility, or production qualification. See
[terrain surfaces](terrain-surfaces.md).

An imported DXF may retain its original bytes as an asset referenced by a
`dxf_source` entity. The entity records the source filename, declared DXF
version, mapped candidate count, and stable fidelity diagnostics; it is a
provenance record and is not drawing geometry. Editable candidates keep their
source layer and primitive in `extensions.dxf_source`. Removing or replacing
the source record never changes the mapped geometry.

An imported IFC may retain its original STEP bytes as an asset referenced by an
`ifc_source` entity. The record follows the same provenance shape as
`dxf_source`, with IFC4 format, source filename, mapped candidate count, and
stable fidelity diagnostics; mapped candidates retain an `extensions.ifc_source`
record identity/type.

For known entity types, the document validates `refs` and `references` arrays as generic entity
references. It also validates canonical singular and plural reference fields for each known type,
such as `property_id`, `building_id`, `floor_id`, `wall_id`, `column_id`, `beam_id`, and `sheet_id`;
a referenced entity must exist and have the named type. `boundary_id` continues to mean the exact
`boundary` type; the distinct `measurement_boundary` and `room_boundary` types carry their own
`floor_id` and `layer_id` links. `parent_id`, `host_id`, `target_id`, `entity_id`, and
`source_entity_id` are generic
references. `asset_id` and `asset_ids` resolve against the pinned asset map; a missing asset or
deletion of an asset still in use rejects the complete command. This is structural referential
validation for ordinary entities. Known persisted wall constraints additionally enforce their
semantic bindings, hard residuals and host validity at the document boundary; see
[persistent wall constraints](constraint-entity-format.md). Full geometric, topology and solver
workflow validation remains incomplete.

Commands are the only mutable interface. `ApplyEntityChanges` atomically applies entity and asset
upserts/deletes against an exact expected revision. Duplicate operations, stale revisions,
invalid JSON, asset checksum failures, and dangling or mistyped references reject the whole
command without advancing the revision. Persisted action text is limited to 1,024 bytes and
revision names to 256 bytes; both require valid UTF-8 without embedded NUL. Media types have the
same encoding requirement and reject CR/LF. JSON floating-point NaN and infinity are rejected
before encoding because JSON cannot preserve them. `NameRevision` records an immutable named
revision.

Undo and redo create new, monotonically increasing revisions. An edit after undo clears the
navigation redo stack but retains every old revision and named branch in history. Each revision
currently stores a full entity and asset state. This is intentionally simple and lossless, but
large histories can use substantial space; delta compaction is a future format change.

`DocumentSnapshot` owns copies of every entity, history record, and asset byte. Its getters are
const-only. Saving a snapshot captured at revision R always writes R, even if the working document
has advanced to R+1. After publication, `mark_saved(R)` leaves an R+1 head dirty.

Unknown optional entity types and unknown JSON fields survive unrelated commands and a v1
save/load cycle. Property and extension JSON is encoded deterministically by nlohmann JSON, so
v1-generated unknown values re-encode with the same JSON value types and canonical bytes. Input
whitespace, source object-key order, and alternate numeric spellings are not retained. An unknown
required entity is loaded and exposed for inspection, but the document becomes read-only. An
unknown project `format_version` is rejected rather than opened unsafely.

## SQLite schema

The SQLite `application_id` is `0x50535444` (`PSTD`). `user_version` and metadata
`format_version` are equal and range from `1` through `22`, according to the
retained semantics. The baseline application tables below are shared; later
versions add the proof columns and recovery data documented in this file.

| Table | Purpose |
| --- | --- |
| `metadata` | `format_version`, `document_id`, `head_revision`, `saved_revision`, and `logical_digest` |
| `revisions` | Revision identity, event parent/source, action/name, and undo/redo navigation stacks |
| `revision_entities` | Full entity state for each revision, including properties and extension JSON |
| `revision_assets` | Full pinned asset bytes, media type, metadata JSON, and SHA-256 for each revision |
| `named_revisions` | Stable name-to-revision mappings, including retained branches |

All application tables are SQLite `STRICT` tables with primary and foreign keys. A standalone
v1–3 file must have `saved_revision == head_revision`, use SQLite `DELETE` journal mode, and have no
`-journal`, `-wal`, or `-shm` sidecar. Load verifies the exact metadata-key set, `user_version`,
table columns, primary keys, foreign keys, and `STRICT` flags. It rejects missing or additional
schema objects, invalid column types, non-contiguous or impossible history transitions, a
non-bijective named-revision index, duplicate IDs, invalid JSON, broken references, oversized
data, asset hash mismatches, and a logical digest mismatch.

The logical digest is BCrypt SHA-256 over a deterministic JSON manifest of metadata, revision
structure, entities, asset metadata, asset sizes, and asset SHA-256 values. Each asset's SHA-256
binds its bytes to that manifest. This detects corruption and uncoordinated modification. It is
not a signature or message authentication code and does not prove who created a file.

Current safety bounds are 128 bytes for IDs, 64 bytes for entity types, 1 MiB per JSON object,
64 levels and 100,000 values per JSON object, 256 MiB per asset, 4 GiB per project file, 10,000
revisions, 250,000 entity rows, 100,000 asset rows, 64 MiB of encoded JSON, 2,000,000 aggregate
JSON values, and 512 MiB of aggregate decoded asset bytes. Physical size and SQL aggregates are
checked before graph/blob allocation; allocation failures are translated to typed resource-limit
errors. The SQL preflight measures JSON as UTF-8 bytes (`length(CAST(value AS BLOB))`), rather than
SQLite text characters. IDs allow ASCII letters, digits, dash, underscore, dot, and colon. Project
paths must name ordinary files under an existing, non-reparse-point parent directory. Windows
device names and alternate data streams are rejected; ambiguous trailing-dot or trailing-space
names are rejected when creating a destination.

The separate recovery-aware v4 format adds a recovery-record table and preserves
the optional captured saved revision. Its complete schema, digest and opaque
load contract are documented in [project-archive-v4.md](project-archive-v4.md).
Document-only APIs refuse v4 rather than discard its recovery ledger.

Version 5 is required when any retained revision contains an explicit boundary
translation proof, including an undone command or an abandoned branch. It adds
one nullable `TEXT` column, `revisions.boundary_translation_json`, after
`redo_stack_json`. SQL `NULL` means no proof; a JSON `null` value is invalid.
A present proof has exactly `{"version":1,"boundary_id":"...","offset":[x,y]}`.
The version is an integer, the boundary ID obeys the ordinary identifier rules,
and both offsets are finite numbers in metres. Unknown versions, extra keys,
duplicate keys and malformed values reject. Proof JSON participates in the
aggregate byte, value and recovery string budgets.

A v5 file may contain the same `project_recovery_records` table as v4. Its
presence makes the file an archive: recovery-aware APIs preserve its ledger and
optional saved revision, and document-only load or replacement rejects it.
Without that table, v5 has the standalone document saved-revision rules. The
exact expected schema is checked in both cases. Histories without proofs retain
the existing v1-v4 schemas and digest encodings.

The proof is included in logical and document/source digests. On restore the
document recomputes the complete next entity state from the previous revision
and the offset; it rejects unrelated edits, changed assets, forged offsets or
missing proofs even after an attacker recomputes the logical digest. Human
action text grants no authority. Exact undo/redo references retain the original
proof on its command revision rather than copying it onto navigation records.
JSON/assets extraction uses exchange version 2 when proofs occur and emits
`boundary_translation` on the corresponding revision; proof-free extraction
remains exchange version 1.

Version 6 is required when any retained revision contains an explicit boundary
transform proof, including undone commands and abandoned branches. It retains
the v5 translation column and adds nullable `TEXT`
`revisions.boundary_transform_json`. A transform proof has exactly `version`
(integer 1), `boundary_id`, `pivot`, `rotation_radians`, `flip_horizontal`,
`flip_vertical`, and `offset`. Points are finite two-number arrays in metres,
the angle is finite radians, and both flip fields are Booleans. Unknown versions,
duplicate or extra keys, and malformed values reject. SQL NULL is absence;
JSON null is invalid. A revision cannot contain more than one boundary
derivation proof.

Transform proofs participate in the same digests, resource budgets, complete
state reconstruction, and navigation restrictions as translation proofs.
The command preserves identities and local receipt inputs, appends an ordered
schema-3 frame, and transforms attached dimension positions. Unrelated entities
and assets must remain identical. A v6 archive is distinguished by its recovery
table, as in v5; document-only APIs cannot discard that ledger. Histories without
transform proofs keep their earlier minimum format and digest representation.
JSON/assets extraction uses exchange version 3 when a transform proof occurs
and emits `boundary_transform` on its command revision. Translation-only and
proof-free histories retain exchange versions 2 and 1, respectively.

Version 7 adds nullable `revisions.boundary_edit_json`. A present value is a
strict version-1 `move_vertex`, `resize_segment`, `insert_vertex`, or `redefine_boundary` intent, or a version-2 redraw reference intent described below. Vertex moves store the
boundary ID, stable vertex ID, and absolute finite position. Segment resize
intents store the boundary ID, stable segment ID, positive analytical length in
metres, fixed endpoint (`start` or `end`), and the explicit connected-chain
choice. Insertion stores the boundary and target segment IDs, a finite fraction
strictly between zero and one, fresh `new_vertex_id` and `new_segment_id`, and
`new_dimension_id` (an empty string when no automatic second-piece dimension
is requested). The original segment ID remains on the first piece; its end
changes to the inserted vertex, and the new second piece ends at the original
end vertex. The edit retains all existing boundary, segment, and vertex IDs.
Only a validated typed insertion may authorize this endpoint ownership change;
raw entity replacement cannot rebind that ID. New IDs must not collide with
current or retired topology IDs in that boundary; the new dimension ID must
also be unused in retained entity history. Readers predating insertion support
reject its unknown intent rather than silently discarding it.

Redefinition stores ordered identified `replacement_segments`, optional exact
raw `replacement_authoring`, restricted `replacement_properties`, and explicit
`replacement_dimension_ids`. Equal edge counts retain every ordered child ID;
changed counts allocate fresh children and regenerate automatic edge dimensions.
Owner-only area references remain attached. Changed-count redraws with child
references require explicit decisions; missing or incompatible decisions reject
without changing the source. A nonempty reference plan uses strict intent version
2 and adds both `replacement_child_mapping` (an object with `segments` and
`vertices` objects, each mapping old child IDs to new IDs) and
`replacement_removed_reference_ids` (an ordered array of reference
entity IDs). Empty plans retain the exact version-1 encoding and size policy.
Maps connect existing segments to replacement segments and vertices to vertices,
with distinct destinations within each namespace. Equal ID strings in the segment
and vertex namespaces remain separate. A mapping with no entries is encoded as
`{}`; nonempty mappings include both typed groups. Every map entry must be used by a retained reference.
Every retained child reference must be fully mapped, including angle dimensions'
second edge and common vertex. Constraint endpoint roles follow the new vertex's
incidence on the mapped segment; relation, length, anchor, other owners and opaque
binding metadata remain unchanged. Only affected supported manual child
dimensions or endpoint constraints may be explicitly removed. Automatic edge
dimensions regenerate; automatic angle dimensions may be mapped but not removed.
Area dimensions, unrelated entities and unsupported references cannot be removed
through this plan. Canonical constraint checks still reject conflicting locks.
Non-null construction inputs must replay to the exact replacement geometry before
their temporary child IDs are mapped by order. Raw API geometry uses null
construction evidence. Receipt-free drawn replacements acquire verified new
construction evidence; existing receipts and derivation prefixes remain exact.
The proof field has a 1 MiB read budget with aggregate JSON resource accounting;
replacement payload validation reserves encoding overhead within that limit.

A workspace redraw is one `boundary_finish` operation: the typed replacement,
input archival and draft retirement publish together. The archived strict
`desktop_operation` identifies the target. Version 1 contains exactly `version`,
`kind` (`redefine`) and `target_id`; nonempty accepted plans use version 2 and add
the two reference-plan fields above and `replacement_segments_sha256`, the SHA-256
of the exact ordered replacement-segment JSON encoding. This binds fresh IDs to
their reviewed geometry without duplicating the geometry payload. Atomic finish and history restoration require
the entire envelope to match the typed command exactly, including ordered removal
IDs. Editing the draft invalidates previous review choices. Restoration checks source revision,
resolved drawing context, exactly one accepted classified chain, exact new
construction input, canonical classification/category updates and the complete
replayed entity map. One Undo restores prior geometry and retains the redraw
input as a retired recoverable view; Redo restores the exact replacement.

The first direct edit of a receipt-backed boundary moves the exact original
`boundary_authoring` envelope into
`extensions.boundary_geometry_derivation.source_boundary_authoring` and appends
the edit intent to its ordered `operations` array. Subsequent coordinate edits,
insertions, translations, rotations, and reflections append in command order. Load and
document restoration replay the original construction and every operation, then
require an exact match with canonical geometry and command history.
The original construction evidence is therefore preserved without pretending
that it produced the manually edited coordinates. A missing, forged, reordered,
or incompatible edit proof rejects the project. A v7 archive is distinguished
by its recovery table exactly like v5 and v6. JSON/assets extraction uses
exchange version 4 and writes `boundary_geometry_edit` on each corresponding
command revision, including retained undone and abandoned history.

## Save and replacement protocol

`ProjectStore::save` takes an immutable snapshot. If the destination exists,
`expected_destination_sha256` is mandatory and must equal the fingerprint returned by the
preceding load or save. A missing, replaced, or modified destination fails closed.

The save sequence is:

1. Open the parent directory and reject a reparse point. For an existing destination, open the
   complete destination and use its handle-resolved, volume-backed normalized path; this expands
   extended, trailing-dot, and available DOS 8.3 aliases. For a missing destination, append only a
   non-ambiguous final filename to the handle-resolved parent. Apply Windows invariant case folding
   and acquire the corresponding `Global` named mutex. Failure to resolve or access the identity
   fails closed. One expected fingerprint authorizes at most one cooperating save.
2. Reserve a new, unique file with Windows `CREATE_NEW` in the destination directory and retain
   that file-object handle through SQLite writing, flush, validation, and publication upgrades.
3. Create the complete SQLite schema and content in one `BEGIN IMMEDIATE` transaction with
   `synchronous=FULL` and `journal_mode=DELETE`, flush SQLite's page cache, close SQLite, and call
   `FlushFileBuffers` before sealing the staging bytes against further writes.
4. Hold a read handle to the staging file while SQLite runs `integrity_check` and
   `foreign_key_check`, decodes and validates the complete document, and verifies asset and logical
   SHA-256 values. After SQLite closes, `ReOpenFile` acquires `DELETE` access to that same file
   object while denying other writes, rename, and deletion. The publication handle is hashed again
   and must equal the validation digest.
5. Recheck the destination fingerprint while holding its file identity read-only, with deletion
   sharing enabled for replacement. Copy that locked expected destination by handle to a unique
   `.bak.<uuid>` sibling, flush it, and verify its SHA-256. Immediately before publication, open the
   current destination name again, require the same Windows file identity and fingerprint, and keep
   both destination read handles through replacement so content writes remain denied. Keep the
   verified backup read handle with no write/delete sharing through publication and receipt return,
   so its exact bytes cannot be changed, renamed, deleted, or replaced in that interval.
6. Atomically rename the validated staging file object on the same volume with
   `SetFileInformationByHandle(FileRenameInfoEx)`. Replacement uses the Windows replace and POSIX
   flags so the verified destination may remain read-open; new-target saves request neither flag.
7. Return the already verified publication-handle digest, the exact saved revision, and the backup
   path.

Failures before publication run checked cleanup for the exact temporary database, its exact
`-journal`, `-wal`, and `-shm` siblings, and any not-yet-published backup. If Windows prevents
removal, the save error retains the original failure and reports the exact residual paths; it does
not claim cleanup succeeded. When more than one removal fails, the diagnostic lists every exact
path and `StorageError::residual_paths()` exposes the complete structured list while preserving the
original error code and message. A process termination can leave a uniquely named staging file or
backup for later recovery cleanup. The fault-injection stages `after_journal_creation`,
`after_database_write`, `after_validation`, and `before_publish`, plus a validation barrier, exist
for deterministic rollback and lock tests. Coverage includes denial of external writes,
rename/deletion, and path replacement while the publication handle is held; exact published digest
and backup bytes; destination write denial plus rename-and-plant revalidation after backup; stale
external fingerprints; backup write/delete/rename/replacement denial; simultaneous locked backup
and staging-sidecar cleanup residuals; multibyte JSON budgets; and cooperating writers split across
normal, extended, trailing-dot, Unicode, and available DOS 8.3 aliases.

A noncooperating process cannot write destination content while the prepublication read handles are
held, but it can still rename or delete a deletion-shared destination in the small interval after
the final pathname-identity check and before handle-based replacement. Windows does not expose a
generic path-level compare-and-swap replace. The verified prior-file backup is the recovery boundary
for that rename-only OS race. Antivirus or file-indexer handles can also make replacement fail, in
which case the original stays in place and the save reports an error.

## Explicitly pending

The in-memory `Document` is a serialized single writer but does not yet have a durable live working
journal. A WAL/FULL edit journal, recovery after process termination during editing, and compaction
remain future work. The desktop now has a broker-backed edit-session lease that reserves the
normalized project path and, for an existing file, its volume/file identity. A cooperating second
open receives an explicit read-only document. The owner rechecks path identity and content digest
before save and records the new identity after an atomic publication, so an external edit or
replacement becomes a visible read-only/save-as boundary instead of a silent overwrite. This
lease is a cooperating-session guard; hostile writers, restart recovery, and clean-machine
qualification remain production work.

The atomic replacement path has deterministic injected-failure coverage, but it has not been
qualified against real machine power loss, filesystem filter drivers, or disk-full conditions at
every write. Save fails closed unless Windows identifies the destination as a local fixed disk
using NTFS or ReFS; UNC paths, mapped network drives, removable media, and other filesystems are
outside the durability boundary and are rejected before staging.

## Survey source corrections

Survey measurement boundaries carry `extensions.survey_source.version = 1`.
`report` contains the currently entered calls and their recomputed results;
`added_closing_segment`, `adjusted_final_endpoint`, and `endpoint_adjustment_m`
record the closure choice. The first call-based correction retains
`original_report` and `original_closure`. Corrections also write `placement`
with `version: 1`, metre `anchor_m: [x,y]`, and
`orientation: "called_north_bearings"`. Other source extensions are preserved.
These optional metadata fields use existing entity/history storage and do not
raise the storage version. Undo and redo restore the geometry and source
metadata together. See [survey contracts](survey-georeferencing-contracts.md)
for input validation and dependent-target rules.

## Boundary-constraint transaction history (v8)

Version 8 adds nullable `revisions.boundary_constraint_changes_json`. A present
value is the strict versioned `apply_boundary_constraint_changes` command
envelope, including expected revision, ordered geometry edits, constraint entity
changes, and message. It belongs only to the originating transaction revision;
undo and redo retain that revision and its proof without copying the proof onto
navigation records. Any retained proof requires v8, including undone history.

The column participates in aggregate JSON byte and value limits, the project
logical digest, document snapshot digests, and authoring source history digests.
Loading decodes the exact command kind and validates its replay against the
parent and resulting entity state. Missing, malformed, or forged proof rejects
even when the file's logical digest has been recomputed. Recovery-aware APIs
preserve the column and recovery ledger together; document-only APIs continue
to reject recovery-bearing files. JSON/assets exchange uses version 5 and emits
`boundary_constraint_changes` on the originating revision.

Older supported files load without this optional proof and retain their existing
minimum storage version when saved. No source file is modified by loading or
migration; a new save containing the command writes both format markers as 8
and the v8 schema. Absent proofs remain omitted from digest manifests, preserving
legacy digest vectors.

Mixed straight-wall/boundary transactions use version 2 of the same command
envelope when wall geometry also changes. Version 2 adds a nonempty ordered
`wall_edits` array. Each entry has exactly `wall_id`, `baseline` and
`length_entry`. `baseline` has finite `start` and `end` coordinate pairs and
`sweep_radians: 0`. `length_entry` is null for a connected wall movement or an
object with `original_expression`, `entered_unit` and `exact_metres`; the latter
has integer `numerator` and `denominator`. Recognized units are `m`, `mm`, `cm`,
`ft` and `in`. Re-parsing the expression must reproduce the entered unit and
exact rational length, which must match the proposed baseline.

Replay reconstructs each existing straight wall from its source entity, changing only
its baseline and recognized length receipt. It retains other wall fields and
hosted-opening records. Duplicate wall edits, curved geometry in these historical proofs,
invalid receipts or hosted openings, and incompatible final relationships
reject the complete transaction. Boundary geometry and its derivation proof
are replayed together with the walls before the final relationship checks.
Version 1 remains unchanged for transactions without wall edits. These nested
command versions continue using the v8 storage column and all its digest and
history admission checks.

## Measured group translation history (v9)

Version 9 adds nullable `revisions.boundary_translations_json`. A present value
is the strict version-1 `translate_boundaries` command envelope. It contains
`expected_revision`, a nonempty ordered `translations` array of `{boundary_id,
offset}`, ordinary `entity_changes`, and `message`. It is retained only on the
originating command revision. Any retained batch requires v9, including a batch
that was later undone. Earlier histories keep their existing minimum version.

The document reconstructs every measured translation from its construction or
geometry-derivation evidence before applying ordinary changes. Stable owners,
vertices, segments, exact measurement entries, and dependent dimension targets
survive; dimension text anchors receive the same offset. Duplicate owners and
ordinary changes overlapping translated owners or their dependent dimensions
are rejected. Ordinary boundary changes pass the usual transition admission;
the message cannot grant an exception. Persistent constraints are checked on
the complete final state, allowing joined owners to move together while a
conflicting partial move refuses without publishing any part of the group.

The proof participates in aggregate storage/recovery budgets and logical and
snapshot digests. Loading replays it against its parent and requires the exact
resulting entities and unchanged assets. Forged or misplaced proof, or missing
proof needed to explain a measured geometry change, rejects the history.
An all-zero translation with ordinary supplemental edits is equivalent to an
ordinary edit; removing that redundant proof does not change its admission.
Undo/redo navigation records do not copy the proof. Exchange
version 6 emits `boundary_translations`; mixed older/newer proof histories retain
the highest required exchange version. Absent batch fields remain omitted from
older digest representations.

## Curved endpoint constraints and wall proofs (v10)

Version 10 raises the minimum reader version without adding SQLite columns.
It uses the v9 schema and the existing `boundary_constraint_changes_json`
column. Both format markers and the logical digest advertise version 10 when
any retained revision contains a curved wall proof or a supported constraint
whose endpoint binding resolves to an actual curved wall baseline or identified
boundary segment. A wall carrying `extensions.curve_input_derivation` also
requires v10, including an imported unconstrained wall with no originating
command in its retained history. This includes already satisfied relations that need no
geometry edit, undone relations, and abandoned history. A straight bound edge
on a boundary containing a different curved edge retains its historical floor.
Opaque unknown constraint versions or relations do not acquire curve semantics.
Generic entities may retain a vendor `curve_input_derivation` extension
collision opaquely without changing their format floor; wall derivation
envelopes are reserved and checked by the curve reconstruction validator.

Curved wall edits carry explicit proof `version: 2` and use version 3 of the
`apply_boundary_constraint_changes` command. Historical straight wall entries
retain their exact unversioned representation, including when accompanied by a
curved entry. The signed sweep is fixed while existing relations constrain
endpoints and chord distance. Wall-only curved commands may contain an empty
`boundary_edits` array. Replay preserves original construction evidence and
validates analytical geometry and hosted openings before publishing the
complete transaction. Historical command versions retain their earlier rules.

Loading recomputes the required floor from all retained proof and entity states
before accepting a file. Downgrading both markers to v9 rejects even with a
recomputed logical digest. Older histories retain their previous minimum
formats, digest encodings, migration behavior, and proof representations.
Recovery-aware APIs preserve v10 documents and their ledgers together;
document-only load or replacement cannot discard a v10 recovery ledger.

JSON/assets extraction uses exchange version 7 for the same retained curved
proofs, bound relations, and wall derivation envelopes. It writes the complete versioned command only on
the originating revision. Later ordinary commands and undo records cannot
lower the exchange version; curve-free histories keep exchange versions 1
through 6 according to their existing proofs.

## Straight wall-only endpoint intent (v11)

Version 11 uses the existing v9 SQLite columns. It protects version 4 of
`apply_boundary_constraint_changes`: an empty `boundary_edits` array, a nonempty
`wall_edits` array containing only historical unversioned straight proofs, and
optional constraint entity changes. Version 4 rejects boundary edits or curved
wall proofs. Versions 1/2/3 retain their earlier decoding and encoding rules.

Endpoint authoring emits this typed command when it changes straight walls
without changing a boundary. Replay reconstructs geometry and measurements
from each proof, then applies the shared analytical topology admission and
independent hard-relation/host validation before accepting the complete state.
Undo/redo and abandoned history retain the originating proof. Deleting its wall
later does not reduce the required reader version.

Both SQLite format markers and the logical digest use v11 whenever any retained
revision contains this capability. Loading independently recomputes that
minimum; changing both markers and recalculating the digest cannot downgrade a
v11 history to v10. Mixed curved and straight-only histories retain the highest
minimum version. Projects without a straight wall-only proof retain their
earlier format requirements. JSON/assets extraction advertises exchange
version 8 and retains each exact command on its originating revision.

These records do not retrofit missing endpoint-edit intent into old generic
`ApplyEntityChanges` history. Explicit construction and object transforms
remain distinct commands with their own validation policy.

## Physical arc-length relationships (v12)

Version 12 retains the existing SQLite columns. A supported version-3 constraint
entity with relation `fixed_arc_length` locks the physical length of a single
curved wall baseline or identified boundary edge. Its exact quantity measures
the analytical arc, not the chord. Bindings identify the same owner and segment,
with opposite endpoint roles and verified stable boundary vertex identities.
The signed sweep remains fixed while endpoint coordinates are solved.

Any retained revision containing this relation requires project format 12,
including undone commands and relations or owners deleted later. A subsequent
straight wall-only proof cannot lower that requirement. Both SQLite markers and
the logical digest carry version 12; loading recomputes the minimum independently
and rejects a downgraded file even if its digest was recalculated. Histories
without this relation retain their previous minimum formats.

JSON/assets extraction advertises exchange version 9 for the same retained
semantics. Earlier constraint entity versions 1 and 2 retain their original
encodings. They do not interpret the new relation spelling as endpoint distance;
unrecognized geometric constraints retain their opaque payload and make the
document read-only.

## Direct physical curve-length inputs (v13)

Version 13 retains the existing SQLite columns. A wall edit proof with
`version: 3` requires a positive exact `length_entry` matching the analytical
arc length, and preserves the source's signed sweep. It uses version 5 of
`apply_boundary_constraint_changes`, which may also contain historical straight
proofs, version-2 curved endpoint proofs and boundary edits. An older command
envelope cannot interpret a version-3 proof; version 5 without that proof rejects.
Historical command and proof versions retain their earlier encodings and rules.

The resulting wall records `extensions.constraint_authoring.version: 1` and
`last_length_entry.version: 2`. Its five fields are `version`,
`original_expression`, `entered_unit`, `exact_metres`, and `baseline`. The exact
rational measures physical arc length; the recorded baseline includes the
signed sweep and must exactly match the current wall. Restore independently
checks recognized receipts even when imported without originating history.
Rigid moves and rotations rebase the receipt; they preserve physical length
and signed sweep. Reflection rebases the receipt's signed sweep while preserving
its magnitude and physical length. Existing measured construction provenance
has separate validation: the shared rigid-transform helper now preserves fresh
and existing measured inputs through the version-2 archive described below.
Subsequent typed
endpoint deformation clears a stale known
receipt unless another explicit length was entered. Unknown nested metadata is
preserved on explicit resize and cannot be silently discarded. Future optional
receipt versions remain opaque on open and cannot be edited by this reader.

Any retained version-3 wall proof or known physical input receipt requires
project format 13, including abandoned, undone and deleted history. The loader
recomputes that minimum and refuses a downgrade despite a recomputed digest.
Generic vendor entities do not acquire this wall-specific extension semantics.
Recovery-aware APIs retain the document and ledger together. JSON/assets
extraction advertises exchange version 10 and preserves exact proof/receipt
payloads; extraction does not constitute a JSON project importer.

## Rigid curve construction transforms (v14)

A measured curved wall's first nonidentity rigid transform retains its exact
original `curve_input` and baseline, including unknown nested metadata, in
`extensions.curve_input_derivation.version: 2`. The archive has four fields:
`version`, `source_input`, `source_baseline`, and a nonempty `operations` array.
Historical version-1 archives keep their encoding; adding a rigid transform
upgrades their version and retains every previous operation verbatim.

A rigid operation has exactly three fields: `kind: "rigid_transform"`,
`transform`, and `baseline`. The transform has six fields: `version: 1`,
`pivot: [x,y]`, finite `rotation_radians`, boolean `flip_horizontal` and
`flip_vertical`, and `offset: [x,y]`. Replay rotates about the pivot, reflects
about the pivot, then translates. An odd number of reflections reverses signed
sweep; physical length and sweep magnitude remain unchanged. The recorded
baseline must match that independent reconstruction. Original measurement
expressions remain archived even when reflection changes the active input to a
canonical angle construction. No identity transform adds an archive operation.

Version 2 requires at least one rigid operation and also supports the earlier
fixed-sweep endpoint and explicit reconstruction operations. Admission compares
an appended rigid operation against the exact source entity; source input,
baseline, and prior operations cannot be rewritten or dropped. The existing
typed endpoint proof rules are unchanged.

Every retained or imported wall with a known version-2 archive requires project
format 14, including undone and deleted history. Recovery-aware APIs retain the
same floor. Older format markers reject even with a recomputed logical digest.
JSON/assets extraction advertises exchange version 11 and retains the complete
archive; it does not supply a JSON project importer. Measured-curve scaling
remains unsupported and rejects without changing the document.

## Fixed-chord boundary curvature edits (v15)

`BoundaryGeometryEdit.kind: "reconstruct_arc"` keeps both endpoints and all
segment/vertex identities. Its version-1 JSON envelope contains exactly
`version`, `kind`, `boundary_id`, `segment_id`, and `construction`.
The construction is a strict receipt of kind `arc_chord_angle`,
`arc_chord_height`, or `arc_chord_length`; its segment ID, start, and chord end
must match the selected edge exactly. Angle and height are signed; arc length
uses the receipt's clockwise flag. The receipt kernel validates the retained
original and normalized expressions and reconstructs analytical geometry.
Degenerate curves and invalid closed topology are refused.

The source `boundary_authoring` receipt is archived unchanged in
`extensions.boundary_geometry_derivation`, followed by a replayable
`geometry_edit` operation carrying the complete intent. Existing derivation
operations remain unchanged. Known constraints and dependent dimensions are
validated by the normal document transaction; automatic length dimensions
follow the new arc and winding, while manual placements remain unchanged.

The new intent in any retained revision, or in an imported identified boundary's
derivation, requires format 15. Undoing or deleting the current curve does not
lower that floor. Older format markers reject the retained semantics. Files
without this intent retain their previous required format and serialization.
JSON/assets extraction advertises exchange version 13, including recovery
archives carrying this intent. Extraction is not a JSON project importer.

## Reviewed exterior wall-source replacement (v16)

An explicit source repair is a `redefine_boundary` intent encoded as strict
`BoundaryGeometryEdit` version 3. In addition to the existing redefinition fields,
it requires nonempty `replacement_wall_source_ids` and both reference-plan fields
(`replacement_child_mapping` and `replacement_removed_reference_ids`, possibly
empty). The IDs must identify 3 to 2048 distinct walls and count toward the
persisted proof budget. Other edit kinds cannot carry source replacements. The
existing version-1 and version-2 edit representations are unchanged.

The core independently derives the analytical exterior from those walls. The
replacement outline must match that exterior, allowing cyclic ordering or
reversed direction; a caller cannot provide arbitrary source JSON. The owner and
sources must resolve to the same property/building/floor/layer, consistent eligible
phase and effective elevation plane. Retained deductions must fit the replacement.
Geometry and `wall_measurement_source` change in one transaction; facts, factors,
name, styling, custom attributes and deduction links are preserved. Ordinary
Refresh remains tied to the originally recorded sources.

An authored owner retains the existing version-1 geometry derivation and its
original construction record. An identified owner without a construction record
uses strict `boundary_geometry_derivation` version 2 with exactly `version`,
`source_boundary` and `operations`. `source_boundary` contains only the original
`boundary_model_version` and identified `segments`. No construction receipt is
invented. Historical replay uses this archived geometry and its typed operations,
without rederiving from today's walls. Later wall deletion or editing can make
source freshness false while the project and its history remain readable. The
final source IDs must agree with the latest retained source-replacement intent.

Supported identified measurement owners cannot acquire, change or remove their
wall-source metadata through a raw surviving-entity edit. Generic and legacy
vendor metadata remain separate. Original source v1 does not archive elevation;
when no original walls survive and the owner has no explicit elevation, the
original plane is unavailable. Replacement walls still must share one coherent
effective plane and the resolved original hierarchy and phase.

### Fresh-topology redefinition (v17)

Redefinition intent version 4 adds `fresh_topology: true` and requires both
reference-plan fields plus `replacement_wall_source_ids`; the source ID array may
be empty for a generic redraw. Earlier intent versions retain their existing
wire shapes and format floors. A v4 intent is valid only for boundary
redefinition. It assigns fresh segment and vertex IDs to the replacement
topology, disjoint from every old child ID, rather than guessing correspondence
by order. For changed geometry with equal edge count, the desktop opens the
reference planner so supported manual dimensions and constraints can be mapped
or removed explicitly; automatic dimensions are regenerated. Different edge
counts continue to use explicit reference mapping/removal decisions and regenerate
automatic length dimensions.

This intent anywhere in retained, undone or deleted history, or in an imported
derivation, requires native format 17. Under-versioned archives reject even when
their digests are recomputed. JSON/assets extraction advertises exchange version
15. Files without these new semantics retain their previous required format and
representation; extraction remains separate from project import.

### Measured group rigid transforms (v18)

`TransformBoundaries` retains one shared numerical planar transform and a list
of distinct measured owner IDs. Its version-one command envelope has exactly
`version`, `kind: "transform_boundaries"`, `expected_revision`, `message`,
`transformations` and `entity_changes`. Each transformation uses the existing
single-boundary transform representation. Supplemental changes preserve existing
identities and cannot substitute raw boundary or bound-dimension geometry.

The complete candidate reconstructs all measured owners, dimensions, source walls,
hosted openings and internal relationships together before final admission. Plain
identified boundaries retain an exact topology origin instead of invented input
receipts. Source exterior normalization preserves child identities and is admitted
only with unique machine-precision analytical correspondence and typed replayable
evidence. Incompatible external relationships and invalidated unchanged consumers
are refused; they are not silently dropped or refreshed.

A present group proof uses `revisions.boundary_transforms_json` in SQLite and
`boundary_transforms` in revision JSON/digest data. Absent proofs are omitted from
logical representations to preserve previous digest values. Retained group history
requires native format 18 and extraction version 16, including after Undo or
deletion. Recomputed digests do not authorize lowering those format markers.

New exterior derivations use stable tangent intersection arithmetic. Retained
replacement commands may instead validate against the complete exact original
version-one offset result, including its original gap allowance. This historical
route is separate from live replacement admission and never rounds or rewrites
stored geometry, command bytes or digests. Exact original outlines can also
remain current against their unchanged sources. Old records retain this
compatibility when a later command raises the archive to format 18.

### Coordinated physical wall and exterior updates (v19)

Constraint-command envelope version 6 retains the preceding geometry and
constraint fields, plus `physical_entity_changes` and `exterior_source_edits`.
Physical changes are admitted under their ordinary wall-edit rules; they do not
inherit typed endpoint-edit authority. Exterior edits are explicit typed boundary
redefinitions applied after the physical wall candidate has been reconstructed.
The envelope version remains part of command identity even when testing omission
or tampering; removing a required exterior update cannot downgrade its meaning.

Every automatic exterior update is recomputed from the original physical walls
and measured owner. A unique cyclic or reversed analytical correspondence binds
existing edge and corner identities to their recorded source walls. Proposed
geometry must retain those wall identities and their cyclic adjacency. Equal
edge counts are insufficient. The owner retains its metadata, appraisal facts,
appearance and deduction references; dimensions resolve against the retained
child identities. Final constraints and deduction containment remain mandatory.

Consumer discovery uses the complete original document, including unselected
owners. Initially current supported owners are coordinated together. Previously
stale sources, generic imported boundaries and explicit source deletion retain
their repair contract and cannot produce qualified totals merely from this proof.
Changing source topology still requires reviewed replacement and reference mapping.

The proof occupies the existing `boundary_constraint_changes_json` revision
column and corresponding logical/extraction data; no new SQLite column is
required. Retained v6 history requires native format 19 and extraction version 17,
including after Undo or deletion. Existing v1-v5 command envelopes retain their
original replay order and digest bytes.

### Mixed ordinary objects and exterior updates (v20)

Constraint-command envelope version 7 additionally records
`supplemental_entity_changes` and `supplemental_asset_changes`. These lanes retain
ordinary object and asset intent alongside the version-six physical wall and
exterior update proof. Version seven can also retain an ordinary physical wall
edit with additional author-supplied wall metadata. Ordinary construction
provenance and source context remain subject to their existing checks; this does
not grant permission to replace a retained construction record. Version-six
physical metadata restrictions remain unchanged. The version marker remains retained even if supplemental
vectors are emptied; removing intent cannot reinterpret a recorded operation as
an older command. Version-six commands keep their original representation.

The original ordinary command must be admitted before source completion. A
supplement cannot overlap another lane or inject raw measured-owner or bound
dimension geometry. Ordinary provenance, constraints, references and asset
validation remain mandatory. The complete candidate carries exact entity and
asset changes through preview, atomic Apply, Undo/Redo and retained history.
Exterior reconstruction still covers every eligible current consumer and requires
its existing source-wall lineage; unrelated objects confer no additional geometry
authority.

Retained v7 history requires native format 20 and extraction version 18, including
after Undo or deletion. Its proof uses the existing revision JSON column. Lowering
format markers remains invalid even when digests are recomputed. Earlier formats
and command digests retain their prior semantics.

The retained command keeps the existing one-MiB encoded proof ceiling. Asset
bytes in supplemental intent are hex-encoded and count toward that ceiling;
this is smaller than the standalone asset size limit. Exact unchanged asset
upserts need no retained supplement. Large changed assets combined with a wall
edit therefore require further storage work; this is an implementation gap,
not removal of portable reference imagery from the production scope.

### ANSI-oriented appraisal evidence (v21)

The opt-in `appraisal_policy.policy_kind: "ansi_z765_2021"` has policy version
one and calculation profile `vertex-ansi-z765-2021-v1`. Existing declared
residential and light-commercial profiles retain their previous behavior.
This is a versioned Vertex rule implementation, not ANSI approval or a complete
UAD reporting contract. Rule evidence and unresolved normative interpretations
are part of the measurement summary.

The property policy's `ansi` object records `interior_inspected` and
`direct_measurement` booleans, `acquisition_increment` (`inch` or `tenth_foot`),
and `limitations_statement`. A floor's `appraisal_facts.ansi` records
`any_part_below_grade`; it must agree with the declared whole-level `grade`.
Missing observations remain undeclared; they are not assumed true.

An area's `appraisal_facts.ansi` records `year_round_suitable`,
`finish_matches_dwelling`, `dwelling_identity` (`primary`, `attached_adu`,
`detached_adu`, `detached_other`) and `ceiling`. Ceiling `kind` is `flat`,
`sloped` or `stairs`. Flat evidence has `minimum_height_m`. Sloped evidence has
`at_least_7ft_area_m2`, `room_floor_area_m2`, `room_boundary_id`,
`source_geometry_sha256`, and `below_5ft_deduction_ids`. Heights and areas are
stored in metres and square metres, independently of workspace display units.
Stairs have `stair_from_floor_id` and must have `stair_footprint` boundary role.
Access additionally supports `through_unfinished` for the new profile.

Sloped evidence binds the complete room boundary and deduction geometry using
the versioned deterministic ceiling geometry digest. Referenced low-height
areas must be real contained geometric deductions in `deduction_ids`; scalar
area declarations never replace exclusion geometry. Geometry changes invalidate
the evidence rather than silently preserving measured proportions. ANSI room
partitions can have nested deductions; cyclic graphs, incompatible floor context
and overlaps that would double-count area remain errors. A parent removes the
whole child footprint and the child contributes only its own net category area.

Primary, ADU and detached-other categories are separate. The canonical report
rounds aggregate square feet once to whole square feet; boundary dimensions use
tenths of a foot. Display rounding does not change stored geometry. The sloped
denominator currently uses gross room geometry and remains explicitly
provisional pending verification against the final publisher standard.

These semantics use existing entity JSON and require native format 21 and
extraction version 19 even when retained only in undone or deleted history.
Earlier readers must refuse them rather than edit away the observations. A
recomputed digest does not permit lowering the reader floor. Unrelated vendor
properties outside the property/floor/boundary appraisal namespaces remain
opaque and do not raise this floor. Older documents retain their existing
minimum format and digest representations. Extraction is a documented data
export and does not itself provide project import.

### Coordinated exterior-corner edits (v22)

Constraint-command envelope version 8 retains an `exterior_corner_move` intent.
Its strict version-1 object contains `version`, `boundary_id`, `vertex_id`,
`position` (finite X/Y in model metres), and `move_connected_objects`.
The boundary and vertex IDs identify a current physical-wall-derived measured
exterior. A stale or ambiguous source cannot authorize this operation.

Replay independently reconstructs physical walls from the requested analytical
outline, wall thicknesses and retained source lineage, then regenerates affected
measured owners. It validates the desired outline against the forward derivation
within the existing 1e-7-metre geometry tolerance. Stored boundary bytes remain
the forward-derived authority; the requested coordinate is never substituted
into derived geometry merely to make a source look current. Stable measured
edge/vertex IDs remain retained.

This intent supplies narrowly scoped authority for physical arc reconstruction.
Original measured curve inputs and previous derivation operations remain
archived. Existing fixed-sweep wall proof versions keep their original rules.
Unproved ordinary physical or asset supplements cannot use version 8's corner
authority. Saved constraints, hosted-opening fit, physical contact topology,
deductions and all previously-current source consumers require final validation.
Preview and Apply use the same command; the complete edit occupies one revision.

Endpoint joints and fractional T stations are reconstructed as temporary solver
relations from the original physical contacts. They do not introduce a persisted
constraint kind. The version-8 topology check uses resolved elevations and active
physical phases, preserving unavailable wall payloads. Earlier envelopes keep
their historical topology policy.

Every retained version-8 command requires native format 22, including undone or
deleted history. JSON/assets extraction advertises exchange version 20.
Older document semantics retain their existing format floors and command bytes.
