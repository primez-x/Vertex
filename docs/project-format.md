# Vertex project formats v1 through v60

## Model-plan furniture and equipment symbols (v60)

Native format 60 and JSON/assets extraction version 58 protect annotation state
version 10. A symbol can opt into physical model XY coordinates with the Boolean
`model_plan: true`. Its placement, rotation, width/depth scales, local-axis flips,
pinned definition, SVG and palette remain in the existing symbol record. The
outer annotation entity envelope and its owning layer contract do not change.

The marker is optional in state v10: absent or false retains the legacy view
overlay convention. Earlier annotation states reject the marker, including
explicit false, and non-Boolean values are invalid. V10 retains the extended
styles admitted by v9. The writer selects v10 when a model-plan symbol exists;
states containing only earlier semantics keep their conditional older version.
Loading a legacy symbol never reinterprets its saved coordinates as model XY.

Model-plan symbols use the same horizontal plan projection as authored geometry.
The complete physical footprint and SVG orientation follow the plan axes,
including reflected views. A Site annotation instead resolves its saved child's
layer and declared presentation frame once; it does not add a second conventional
plan transform. View-overlay symbols retain their existing presentation path.
Joint connected translation applies the model offset to model-plan symbols and
labels, and the separate presentation offset to legacy overlays. Translation
cannot change the coordinate mode or any unrelated symbol property.

The native reader floor includes every retained revision, including undone
creation, deleted symbols and abandoned branches. Such an archive cannot be
relabelled below v60 even when its current head has no model-plan symbol. Older
projects without these semantics retain their existing format floors and remain
readable. Extraction uses version 58 for this floor in both ordinary and compact
asset output.

## Atomic geometry and device observations (v59)

Native format 59 and JSON/assets extraction version 57 retain command-envelope
19: a typed geometry edit followed by one `disto_measurement` attachment in the
same history event. The outer envelope contains `expected_revision`, `message`,
`disto_measurement_completion: true`, `disto_measurement` and `proof`. The
attachment has exactly `owner_id`, `record` and Boolean `replace_existing`;
`proof` is the original typed command envelope without an attachment. Nested
attachments are refused. Existing geometry dialects keep their admission rules.

Replay first reconstructs the complete original geometry, then attaches the
record to the same existing owner. The record's field must match the owner type
and its value, converted from the declared unit, must agree with the completed
dimension. Wall lengths use the physical straight or curved axis with only
floating-point representation allowance; scalar fields use the exact converted
value. Display rounding and solver tolerance do not qualify a different reading.
The attachment cannot alter geometry, properties, identities, receipts, assets
or unrelated extensions. Replacing an existing record requires
`replace_existing: true` and a valid prior record for that field.

The observation remains in the existing version-one `disto_measurements`
extension, indexed by its target field. It records the reading, declared unit,
device identity, timestamp, transport and provenance. A later ordinary geometry
edit may leave this historical observation unchanged; an old reading is not a
claim about the current dimension. Keyboard provenance retains the received text
and explicit unit/decimal declarations without claiming device discovery or
physical qualification.

The reader floor includes every retained command, including an undone edit or a
deleted owner. Relabeling an archive below v59 cannot make envelope 19 readable
by an older reader. Ordinary entity commands can retain measurement metadata
through their existing admission path and do not acquire typed geometry
authority from that metadata. Older projects keep their existing format floors.

## Site presentation frames, terrain datum bindings and framed annotations (v58)

Native format 58 and JSON/assets extraction version 56 protect explicit
site-coordinate contracts: `property.site_frame`, `building.site_placement`,
`terrain_surface.terrain_elevation_binding`, and `presentation_frame` on
non-container entities. Annotation entity version 3 requires its own strict
`presentation_frame`; each label or symbol still uses its own saved layer to
determine the source context.

Site and building transforms use metres, XY yaw and Z translation. They do not
scale, shear or rewrite authored geometry, measurements, or vertical-level
facts. Building geometry enters the property-site and relative building
transform only when `site_placement` is explicitly present. Terrain ignores
building placement: relative elevations bind to the site origin, while
absolute elevations require the property's matching vertical datum. Independent
assembly geometry remains in its existing world frame unless its
`presentation_frame` explicitly enrolls it. Placement is resolved for
presentation after local hosts, joins, assembly expansion and vertical
placement; the saved source remains local and unchanged.

The reader floor scans every retained revision, including undone, deleted and
abandoned owners. Unknown or malformed future markers raise the conservative
reader floor but gain no typed authority. Container entities cannot override
their presentation frame. Older projects without these explicit markers keep
their existing world coordinates; loading or extracting them does not infer a
site placement or migrate geometry. Their recovery ledger and original source
bytes remain intact.

## Independent assemblies, roof materials and living units (v57)

Native format 57 and JSON/assets extraction version 55 protect independent
`assembly_instance` entities, `assembly_model.model` catalogs with schema
`sketch.assemblies.v4`, `roof_join` version 2 or its `material_assignment`, and
property/boundary `appraisal_reporting` version 2. Reporting markers containing
`living_units`, `living_unit_id`, or a room's `other_description` also require
this floor. The scan includes every retained revision, including undone creation
and deleted owners. Relabeling such an archive as v56 refuses even with a
recomputed logical digest; refusal preserves its bytes. Existing v56/exchange54
reporting and earlier format contracts continue to apply.

Independent instances use the entity envelope `version: 1`,
`form: independent_assembly_instance`, `assembly_catalog_id` and `instance`.
The nested instance uses `sketch.assembly-instance.v1`, has the same identity
as its entity, and requires a world `root_transform`. Legacy host placement and
top-level placement/type aliases are forbidden. Catalog references, type IDs,
material slots and nested part paths are validated against the actual catalog.
V4 catalogs retain compact extrusion profiles, nested parts, independent root
transforms and structured nested overrides; geometry remains derived from
those authored values. Unrelated entity metadata remains preserved.

Authored profile and part order remains stable. Expanded geometry retains its
root identity and typed part path, type and profile provenance. Each expansion
is limited to 4,096 nodes and 262,144 profile segments; the same limits span all
placed roots in a document. Validation of unused definitions has a separate
aggregate limit of 16,384 nodes and 1,048,576 profile segments. A shared nested
definition cannot evade those limits by expanding through several branches.
Rejected graph changes preserve the previous document and retained history.

Roof joins retain their ordered `roof_ids` and fused style. Version 2 adds
`material_assignment` with `version: 1`, `catalog_id` and `material_id`; both
the assembly catalog and its material must exist. This assignment belongs to
the join and does not replace the retained member roof geometry.

Property reporting v2 adds one to 64 `living_units` with unique bounded
`unit_id` and appraiser `identifier`, declared `role`, and optional level
declarations. Boundary reporting v2 can reference a declared `living_unit_id`
and adds the expanded room-use vocabulary; `other` requires a nonblank
`other_description`. The existing source observation digest still fences
reporting facts. These declarations are authored facts, not inferred from
decorative geometry or owner names. Typed codecs reject unsupported fields
and malformed declarations; a reader-floor marker does not grant authority.

## Appraisal reporting and conditional declarations (v56)

Native format 56 and JSON/assets extraction version 54 protect either an
`appraisal_reporting` marker on a property or measurement boundary, or
`appraisal_policy.ansi.limitation_declarations` on a property. The reader floor
inspects all retained history, including undone creation, deletion and abandoned
branches. Future or malformed markers still require the floor; they do not
acquire executable authority. An identically named vendor field on another
entity type remains ordinary opaque data. Lower format labels are refused even
when their logical digest has been recomputed.

Property `appraisal_reporting` version 1 contains `contract` (`legacy_uad_2_6`
or `uad_3_6`) and an explicit Boolean `room_inventory_complete`. Boundary
reporting contains `version: 1`, `source_geometry_sha256`, optional Boolean
`contained_within_primary`, and `rooms`. Each room has a unique, bounded
`room_id`, a declared `use` (`bedroom`, `bathroom_full`, `bathroom_half` or
`other`), and optional Boolean `legacy_total_room`. Unknown fields, versions,
uses and malformed values refuse typed interpretation. Room identifiers are
membership identities; names and decorative fixture symbols are not room facts.

The persisted digest field binds current geometry, deductions, relevant area
and inherited floor observations, property measurement policy and ownership.
Reporting's own fields, names and appearance are excluded. Changed observations
require explicit reconfirmation; a new transaction fingerprint does not renew
old declarations. Applying reporting uses one undoable command fenced by the
entire captured snapshot, including retained history, and the same semantic
design-phase scope used in the editor. Presentation hiding does not remove an
area from appraisal calculations.

UAD 3.6 primary room counts use declared original room types across grades and
finish categories. ADUs and noncontinuous area retain separate summaries.
Combined ADU summaries are not per-unit form fields. Legacy room mappings that
remain unresolved withhold primary counts while preserving supported separate
summaries. Report projection never changes canonical independent ADU areas.
Absent reporting configuration leaves older projects in measurement-summary
mode. This schema does not certify ANSI, lender forms or vendor interchange.

Conditional ANSI declarations are a bounded array of `{kind, statement}` records.
Kinds are `interior_not_inspected`, `based_on_plans`, and
`direct_measurement_not_possible`; each requires an explicit nonblank statement
when its measurement condition applies. Generic `limitations_statement` remains
supplemental notes. Duplicate, unknown and inapplicable kinds are rejected for
qualification. Saved incomplete facts remain editable but cannot produce
qualified totals. Presence checks do not verify prescribed publisher wording.

## Physical wall axis dimensions (v55)

Canonical dimensions with `dimension_version: 4` and
`dimension_kind: wall_axis_length` target the actual physical wall through
`target.entity_id`. They carry no segment, vertex or segment-chain target.
Resolution reads the current physical axis and its analytical length; a stored
observation is not measurement authority. Only this version-4 kind is supported:
version-4 prior kinds and future versions or kinds remain opaque and retain the
complete source entity. Malformed known typed data is rejected.

Native format 55 and JSON/assets extraction version 53 are required when a
supported wall-axis dimension occurs in any retained revision, including undone
creation or deletion. Existing dimension versions 1–3 keep their contracts.

## Owned stair landing railings (v54)

Canonical `railing` properties with `version: 3` and
`form: stair_landing_railing` persist height, thickness and post spacing plus a
`host`. The host names `stair_id`, `role` (`connecting` or `top`),
`incoming_flight_id`, `edge_index` (0–3), and ordered start/end station fractions.
A connecting host also names `landing_id` and `outgoing_flight_id`; a top host
cannot carry those fields. Hosted geometry derives from the stair landing;
independent base position, orientation and length are rejected, as are flight
host fields on a landing rail. Unknown forms and future integer versions retain
their opaque data without acquiring canonical attachment authority.

Native format 54 and JSON/assets extraction version 52 apply across retained
history, including deleted, undone and abandoned records. The version-2 flight
railing contract remains unchanged; extra version-2 host keys confer no landing
authority. These schema additions introduce no separate replay-version field.

## Assistance previews and extraction limits

Assistance proposals use `schema_version: 1`, `status: unverified`, and
`requires_explicit_acceptance: true`. Their source observations, resource
provenance and command preview are not executable document commands. Acceptance
creates a request for normal command validation, permission checks and an Undo
transaction; it does not verify measurements. The decoder rejects unsupported
schemas, unknown kinds and unexpected envelope fields rather than granting them
opaque executable meaning. This preview schema has no replay-version field and
does not define document-history authority.

JSON/assets extraction preserves retained revisions and, when an archive is
supplied, its recovery records. Its exchange version follows the highest native
reader floor required by that history. Extraction remains separate from a JSON
project importer; an assistance preview or extracted record does not itself
establish a replayable accepted command.

## Exact physical-source translation lineage (v53)

A measured exterior translated with its physical walls can retain
`wall_measurement_source.version: 2`. Its frozen kernel, original wall
baselines/thickness/context, original analytical outline, and flat ordered
translation list reproduce exact coordinates without summing offsets or
subtracting a translation to guess original values. Captured physics must
independently derive the original outline. Each translation is replayed over
both the physical baselines and the outline; live source-currentness also
requires exact current wall records. Intrinsic lineage remains valid when a
physical source has subsequently changed or disappeared, so stale values are
withheld without discarding valid history.

Typed boundary edit v8 retains the exact translation offset and includes
genesis captures only when upgrading a v1 source. Further translations append
to the existing validated lineage. The document reconstructs the complete
submitted edit from its actual source and physical result. Ordinary shape,
thickness, or source replacement derives a fresh v1 source. Limits are 2,048
captured walls, 4,096 finite nonzero translations and a bounded encoded proof.
The combined replay budget is 8,388,608 wall-pair checks:
`W * (W - 1) / 2 * (T + 2)`, where `W` is the captured wall count and `T`
the retained translation count. The independent limits cannot be multiplied
into an unbounded replay workload. Exhaustion refuses the edit atomically
rather than changing coordinates.

Native format 53 and extraction 51 cover this authority in every retained
revision, including undone/deleted history and recovery. Lower format labels
are rejected even when their logical digest has been recomputed.

## Typed stair topology and owned flight railings (v51/v52)

Canonical `stair` properties with `version: 2` and `form: multi_flight_stair`
require native format 51 and extraction 49. Flights and connecting landings have
stable child identities owned by the stair. A retained child ID cannot be reused
for another owner or role, or become an entity ID; retirement and abandoned
Redo history preserve that lifetime. Ordinary Undo/Redo restores the original
identities. New topology and clones allocate new identities.

Canonical `railing` properties with `version: 2` and
`form: stair_flight_railing` require native format 52 and extraction 50. The rail
owns a reference to its current stair and typed flight, side, and ordered
start/end station fractions. Geometry follows that flight's tread pitch line;
it has no independent base position, orientation, length, or vertical placement.
The host and rail require complete compatible property/building/floor/layer
organization and compatible phase membership. Attachment and identity checks
apply to restored document history as well as current edits.

These reader floors inspect every retained revision, including deleted stairs,
removed rails, undone commands and abandoned branches. Both native markers and
the logical digest carry the selected floor; recomputing a digest after lowering
markers does not permit opening that history with an older reader. Version-1
straight stairs and independent railings, and unrelated opaque forms, retain
their previous reader floors. No SQLite columns change.

## Source-qualified physical-room dimensions (v50)

Existing dimension versions 1–3 can target an identified physical-wall-derived
room. Structural admission retains stable edge, corner and chain references;
it does not certify a stored room outline as current. Numeric resolution requires
the complete current entity map or snapshot. It rederives clear spaces from the
physical walls, checks exact source lineage, outer geometry, holes, drawing
context, active phase and effective plane, and subtracts holes from clear area.
A changed source withholds numeric values until explicit room review.

Any such dimension anywhere in retained history requires native format 50 and
extraction 48, including undone or deleted dimensions. This changes neither
SQLite tables nor the dimension schema. Entity-only numeric resolution remains
unsupported for physical rooms; anonymous legacy or unqualified room geometry
cannot substitute for the physical source.

## Atomic reviewed physical-room dispositions (v49)

Command envelope 18 carries a strict version-one semantic review intent and an
explicit completion marker. It contains complete retained-room and fresh-space
decisions for one captured physical context and plane, explicit identities and
metadata for created rooms, interior witnesses, retained descriptor digests,
complete Keep/Remove choices for affected supported references, explicit child
mapping for kept edge/corner references, fresh automatic dimension IDs, and
acknowledged room-relationship removals. Complete reviewed retained owners can
exchange destinations; ordinary single-room repair still refuses an occupied
destination. Regenerated automatic dimensions preserve their template's style,
context and opaque metadata while taking fresh identities and analytical targets.
Heterogeneous kept automatic templates refuse rather than silently choosing one
style. Unknown incoming references to removed dimensions or constraints also
refuse. The new batch policy does not reinterpret older ordinary-command proofs.

The authoritative replay rederives physical clear spaces and all resulting
entities from the preceding source. Ordinary geometry, entity or asset lanes
cannot be combined with this authority. Live application checks the full captured
snapshot. Historical replay validates the exact preceding entity map, a version-2
authoring-source digest, and the complete captured snapshot reconstructed from
the retained prefix and captured saved revision. Both include every typed proof,
assets, names and navigation. The frozen version-1 authoring digest retains its
original representation and does not authorize this review. New identities cannot
reuse retained history. Opaque references in JSON keys or values refuse when
retirement or child replacement would detach them. Cancel
publishes nothing; the complete accepted batch is one history event. Native
format 49 and extraction 47 retain this authority after Undo/Redo and reopen.

## Persistent analytical tangent junctions (v48)

Constraint entity version 5 is exclusive to the `tangent` relation. Its four
bindings are the first contact, first opposite endpoint, second contact, and
second opposite endpoint. Each pair identifies one full native segment with
opposite endpoint roles. At least one segment is a circular arc. Signed sweeps
and endpoint derivatives come from authoritative owner geometry.

A smooth junction requires coincident contacts and opposite outward tangent
directions within the defined linear and angular tolerances. Chord parallelism
does not establish tangency. The solver enforces this relation during subsequent
edits; existing fixed anchors remain hard constraints. Earlier entity versions
cannot interpret tangent, and v5 cannot reinterpret earlier relations. Native
format 48 and extraction 46 cover the head and retained history.

## Joint connected translation (v47)

Command envelope 17 carries a version-one joint translation intent and one
ordinary geometry proof. It records the selected rigid boundary and measured
stroke IDs, selected partial physical-wall IDs, finite nonzero offset, and
connected-movement choice, selected dimension IDs, and nullable finite
`presentation_offset`. The analytical offset applies to model geometry and
model-space labels; a separate presentation offset can move selected overlay
symbols, labels and references through a projected view. All selected points participate in one hard-connected
solve. Persisted fixed anchors and relations retain their original authority.

Replay reconstructs that solve from the preceding source, rederives measured
owners and callouts, and requires the submitted proof to produce the exact same
entities. Existing annotation/reference supplements may change placement only;
their types, child identities, size, content, styling, context and opaque data
remain unchanged. Geometry supplements cannot borrow presentation authority.
Independent disjoint composition remains envelope 16. A missing intent,
false completion marker, nested authority, changed relation, or forged result is
refused. Native format 47 and extraction 45 apply to retained history, including
undone edits and stripped proofs; earlier readers cannot ignore this intent.

## Scoped annotations and explicit presentation (v46)

Annotation state version 9 adds optional `style.fill_opacity` in `[0,1]`,
`style.line_pattern` (`solid`, `dash`, `dot`, `dashdot`), and `cross`,
`horizontal`, and `dots` fill patterns. Absent opacity preserves earlier screen
and output alpha defaults. Explicit opacity, including 0 and 1, is retained.
The optional `use_model_text_height` callout flag uses the style's metre height
instead of the default paper height, without changing earlier callouts. It
cannot coexist with explicit paper text height. Earlier states reject new keys.

Outer annotation entity version 2 retains a complete property/building/floor/layer
context, with an optional level. Outer version 1 remains unchanged. Reusable
text library version 3 carries these styles; earlier library formats refuse
unsupported fields. Native format 46 and extraction 44 apply to retained history,
including removed or undone annotations.

Pinc imports retain the exact source file once as an `application/x-pincsketch`
asset. Source pointers and page display mappings are provenance in extensions;
native measured-line receipts and source-derived areas remain the geometry
authority. Decoded underlays keep original encoded assets and PNG previews.
Source categories never establish appraisal eligibility or physical wall depth.

## Independent live area callouts and text alignment (v45)

Annotation state version 8 adds `style.text_alignment` (`left`, `center`, or
`right`) and presentation override roles `area_name` and `area_calculation`.
Both reference the existing area owner through `target_id`. Each role retains
its own text style, visibility, model-space anchor offset, optional paper text
height and rotation. It cannot supply geometry outline or hatch-spacing fields.
The area name and calculated string are derived at render time; these overrides
contain presentation only. A hidden callout does not hide its analytical area.

The encoder uses version 8 when a role or noncenter alignment is authored.
Centered legacy annotations retain their prior schema and appearance. Earlier
annotation versions reject the new alignment field and role names; version-8
siblings without an alignment field default to centered. The outer annotation
entity schema remains version 1. Reusable text-library version 2 retains
alignment; older centered library records keep their existing wire format.

Native format 45 and extraction 43 are required wherever version-8 annotations
appear in retained history, including Undo or deleted presentation owners.
Both native format markers and the portable-package validator recognize this
reader floor. Older readers must refuse these files before losing callout
semantics. No SQLite table change is required.

## Reviewed same-ID physical room repair (v44)

Boundary geometry edit dialect 7 adds `physical_wall_room_repair`: a selected
current wall ID, finite strictly interior witness, exact reviewed detector
lineage and SHA-256 of the retained physical-room descriptor. It is an exclusive
fresh-topology boundary redefinition: no unrelated source lane, authoring data or
classification override is allowed. Fresh child identities and explicit existing
reference mapping/removal contracts remain required.

Document apply and retained-history restore independently detect the destination
from the preceding entity map. They verify context, phase, old descriptor digest,
exact outer geometry, current lineage, strict interior membership and destination
ownership. Holes are regenerated from that source; they are not supplied by a
caller. Only the verified same-ID edit can replace a retained room's marker and
outline. Name, classification and unrelated metadata remain intact; previous
evidence remains in history. Undo and Redo navigate that validated history.

This is explicit reviewed reassignment, not automatic cell correspondence. A
split assigns only the chosen space; other pieces remain unclassified. A merge
does not acquire other rooms' metadata, and other affected owners remain stale.
Automatic correspondence and atomic multi-room dispositions remain open.
Entity-only dimensions on physical rooms remain refused until their
snapshot-aware resolution is implemented.

Retained dialect-7 repair intent requires native format 44 / extraction 42,
including a deleted owner, retained history or an entity-only geometry-derivation
carrier. Unsupported older readers must refuse before accepting the history.
An entity-only derivation carrier preserves the higher reader floor; it does not
by itself verify live source authority. Current room queries still rederive it.

## Source-bound clear rooms (v43)

An identified `room_boundary` can carry `extensions.physical_wall_room`.
Its four fields are `version` (1), `selected_wall_id`, `source_lineage` and
`holes`. Version 1 requires a detector-produced version-one lineage, a valid selected wall ID, and
bounded analytical holes encoded as segment arrays with `[x,y]` start/end points
and signed `sweep_radians`. Exact adjacent endpoints are required. The outer
identified boundary remains the owner's authoritative retained topology.

The canonical query rebuilds the complete source context/elevation/semantic phase
and checks exact outer, holes and lineage before exposing clear room area.
Unknown positive descriptor versions are retained read-only. Stale evidence has
no current quantity. Generic edits cannot strip or replace the descriptor or
independently move the outer. Classification is separate from appraisal facts;
these room owners do not enter exterior GLA.

The marker in any retained revision requires native format 43 / extraction 41,
including after deletion. No new command-envelope version is needed for this
creation/stale checkpoint. Historical centerline rooms are not automatically
converted. The reviewed same-ID repair dialect is documented above.

IFC source carriers exceeding the legacy small-text limit use private
`Pset_VertexExchange_v2`: one owner, a count/byte/SHA-256 manifest and ordered
text chunks. Reconstruction is bounded and checksum-verified; imported carriers
retain opaque native evidence through repeated exchange without activating a
physical room. Supported flat footprint loops and source retention are distinct
from IFC semantic room or full architectural interoperability certification.

## Optional linked floor tracing

A floor may retain `properties.tracing_reference` as a presentation preference:

```json
{
  "version": 1,
  "source_floor_id": "source-floor-id",
  "visible": true,
  "opacity": 0.25,
  "offset_m": { "x": 0.0, "y": 0.0 }
}
```

Version 1 requires exactly these fields, a nonempty source ID, boolean visibility,
opacity from 0.05 through 0.75 and finite metre offsets within +/-1,000,000. The
source and destination must be distinct floors with resolved organization in the
same building and property. Unknown versions or malformed fields are preserved
as metadata but refused by the tracing projector, with a repair diagnostic.
Clearing the reference removes the preference in one undoable revision.

The source is resolved live from the current document; geometry, annotations and
assets are not copied into the destination floor. The XY offset is applied only
to screen rendering. While the destination is active in the 2D workspace, its
ordinary canvas focuses on that floor independently of reference visibility.
The reference does not participate in quantities, selection, snapping, extents,
sheet output or sketch export. The link persists through native save/reopen and
Undo/Redo. Existing v42 generic entity properties already preserve this optional
presentation metadata; it introduces no new geometry or command dialect.
Earlier clients may preserve the preference without offering its tracing UI.

## Mixed rigid group and connected geometry completion (v42)

Constraint-command envelope 16 retains an optional `rigid_group_transform`
typed transform group and the `rigid_group_completion` discriminator. This
authority composes a rigid group move with connected geometry consequences in
one revision. The child transform keeps its existing typed representation;
earlier command envelopes 1 through 15 keep their historical rules.

Retained mixed completion intent requires native format 42 / extraction 40,
including undone commands and deleted states. Either the completion marker or
the optional transform child sets this reader floor. Clearing the child cannot
downgrade a retained completion marker. Readers reject a downgraded native or
extraction format instead of dropping the proof. Compact supplemental asset
references remain supported in envelope 16 through their existing discriminator.

## Connected geometry with explicit dimension placement (v41)

Constraint-command envelope 15 retains `dimension_placement_moves`, a list of
dimension IDs and model-space offsets, and its explicit completion discriminator.
This narrow authority composes a connected geometry edit with selected callout
placement in one revision. It does not permit raw dimension supplements, source
replacement or removal of geometric constraints.

Replay resolves each dimension against the original and reconstructed geometry.
After exterior redraw and measured-source consequences, it uses the original
text position plus the retained offset, sets manual placement and clears the
automatic-placement version. Stable analytical target identities, presentation
style and opaque metadata remain intact. Invalid or retired targets, conflicting
raw edits, duplicate intents and nonfinite/overflowed coordinates refuse the
complete command. Unselected automatic dimensions retain normal source reflow.
Ordinary selected labels or reference images may accompany this typed placement
lane without implying an exterior-source redraw. Explicit exterior and measured
completion flags still require their corresponding reconstruction proofs. The
legacy supplemental inference for envelopes 1 through 14 remains unchanged.

Retained envelope-15 history requires native 41 / extraction 39, including Undo
and deleted states. Emptying the placement vector cannot downgrade the envelope;
an empty completion cannot authorize a live edit. Entity-only materialization
without retained placement intent keeps its existing reader requirements. Earlier
command dialects and the isolated geometry importer protocol remain unchanged.

## Physical-source measured curve reconstruction (v40)

Command envelope 14 retains an `exterior_segment_arc` intent containing the
stable measured boundary and segment IDs, the exact chord ConstructionReceipt,
and the related-object movement choice. Angle, height and arc-length inputs
retain their expressions and exact values. Both measured chord endpoints and
all unselected measured geometry remain fixed within analytical roundoff.
Physical wall baselines are reconstructed from that requested exterior using
their original unequal thicknesses. Their derived curve inputs describe the
physical baselines, independently of the measured construction receipt.

Replay reconstructs the physical perimeter independently and checks the entire
final measured outline after source completion. Competing raw physical,
supplemental, rigid, split, corner or resize authority is forbidden; frozen
related objects cannot carry dependent wall, boundary or measured-stroke edits.
Current measured consumers and attached dimensions derive from the resulting
walls. Ordinary topology, host, source-currentness and constraint checks remain.

Straight-to-arc conversion uses curve_input_derivation version 3 with null
source_input and the exact original straight source_baseline. Its first operation
records the derived physical arc input. Later ordinary, construction and rigid
operations preserve that line origin and archive prefix. Earlier versions keep
their historical curve-origin rules. Retained envelope-14 commands and v3 line
origins require native 40 / extraction 38, including undone and deleted history.
Entity-only snapshots with that origin require the same reader floor; histories
without the new proof or provenance retain their previous minimum versions.

## Physical-source measured edge resizing (v39)

Constraint-command envelope 13 retains an `exterior_segment_resize` intent:
the current measured boundary and stable segment IDs, exact entered length
receipt, fixed start/end endpoint, local boundary-chain movement and movement
of other connected objects. The two movement choices are independent.
Reconstruction of the measured boundary's physical perimeter is mandatory.

Replay reconstructs the requested analytical outline, inverts its wall offsets,
validates physical contacts and constraints, and regenerates current measured
consumers. The final selected edge must retain its requested analytical length,
anchor and signed sweep. Physical wall arc sweeps can change when their offset
joins are trimmed. Derived boundary coordinates remain the actual forward result;
the requested outline cannot substitute for source-derived geometry.

The new intent cannot borrow ordinary physical, asset, split, corner or rigid
mutation authority. Dependent measured-stroke completion retains envelope 13.
All retained commands with this intent require native 39 / extraction 37,
including undone and deleted histories. Entity-only materialized imports with
no retained resize intent keep their existing entity/archive reader floors;
they do not claim to preserve the absent entered-length receipt. Earlier command
dialects retain their existing representation and replay semantics.

## Whole-wall relationship membership (v38)

Room relationship schema two retains ordered physical members for one logical
architectural wall. Splitting a member rewrites the owned list without changing
relation endpoints or meanings. Native format 38 and extraction version 36 are
required when schema-two or future relationship models appear in any retained
state, including Undo, deleted records and entity-only extraction. Ordinary
schema-one records and vendor properties on unrelated types keep prior floors.
Downgraded native/extraction markers reject instead of dropping membership.

Known schemas validate all member roles, current physical wall geometry and the
combined graph across records. Positive future models remain opaque and make
the project read-only. Retained known wall-split proofs are still reconstructed:
only future relationship records are removed from the detached replay input,
then restored exactly before full reconstructed-state comparison. Authored
splits retain strict reference protection; this restoration exception cannot
authorize a new split against unknown references.

## Wall point insertion and whole-span dimensions (v37)

Wall split command authority, physical arc-chain relations and full-span
dimensions require native format 37 and extraction version 35. The typed split
reconstructs both baselines, hosted opening stations, supported constraint and
measurement consumers, and retained archive evidence. Unknown references and
openings crossing the seam refuse atomically. Reconstructed full-state equality
precedes detached history normalization.

## Measured stroke dimensions (v36)

Saved dimensions targeting identified measured strokes require native format 36
and extraction version 34 across retained history. Stable target identities and
authoring provenance remain part of the document; display rounding does not
replace analytical geometry.

## Persistent measured-stroke constraints (v35)

Measured linework schema/replay v5 adds ordered `vertex_batch` operations. Each
contains `edits`, an array of typed `move_vertex` edits naming unique existing
stable vertices of the same stroke. Replay installs every target before final
geometry validation. Original construction receipts, identities and signed
sweeps remain authoritative. Existing edit and transform operations keep their
semantics; entered resize quantities and reflected arc sweeps are not replaced
by solver coordinates. Empty or all-no-op public batches preserve the original
dialect; meaningless persisted batches are rejected.

Constraint command envelope 11 retains independent Boolean measured, rigid-wall,
exterior and supplemental completion modes. Its `measured_stroke_edits` lane
names a stroke, optional authored edit/quantity or rigid transform, and final
vertex edits. The authored edit and transform are mutually exclusive. Geometry
and eligible measured consumers are independently reconstructed before final
validation; raw area payloads do not supply this authority. Previously stale,
ambiguous or authored consumers remain subject to explicit source review.

Native format 35 and extraction version 33 apply across all retained revisions,
including Undo and deletion. Already-satisfied relation-only stroke bindings
also require this floor, even when the stroke itself remains v1. No SQLite
columns change. Downgraded markers reject; unsupported stroke payloads remain
preserved under read-only protection rather than being interpreted as boundaries.

## Curved survey source provenance (v34)

Survey call reports use version 2 when any call is a circular arc. The typed
`measurement_boundary.extensions.survey_source` wrapper then uses version 2,
including after a later correction back to straight calls. Current and original
reports retain entered text, exact quantity receipts and their own report/input
versions. The common identified boundary model stores analytical signed sweeps;
no new geometry representation or SQLite columns are introduced.

Native format 34 and extraction version 32 protect this source contract across
all retained revisions, including Undo, deleted owners and entity-only imports.
A v1 wrapper carrying v2 or future report/input metadata cannot lower the floor.
Future or malformed typed discriminators preserve their JSON with read-only
protection; collisions on unrelated entity types remain opaque. Ordinary v1
straight-call projects retain their previous reader floor.

Document admission checks source structure and versions, rather than treating
archival calls as live geometry. Ordinary drawing edits and transformations may
intentionally differ from the recorded calls. Opening those calls or correcting
a boundary requires sealed reconstruction from entered expressions and matching
receipts. Derived report vertices and diagnostics are recalculated. Loading a
project therefore does not certify the semantic accuracy of its archival survey
receipts. Future payloads remain preserved without being consumed.

The report contains `provenance`, ordered `legs`, local `vertices`,
`closure_tolerance_m`, `diagnostics` and `input_provenance`. V2 legs add signed
`sweep_radians`; `distance_m` remains the chord distance. The v2 entered-input
record retains `legs_text`, `source_text`, `closure_tolerance_expression`,
`default_unit` (`m` or `ft`), ordered `distances` and ordered `curves`. Each
distance receipt records `leg_id`, `line_number`, `original_expression` and
`exact_metres` with integral `numerator`/`denominator`. Each curve receipt uses
`version: 1`, `leg_id`, `line_number`, `construction_kind` and
`original_expression`. `chord_angle` retains normalized `sweep_radians`;
`chord_height` retains signed `exact_metres`; `chord_arc_length` retains measured
arc `exact_metres` plus Boolean `clockwise`. Chord and arc-length receipts are
distinct. Positive sweep/height is counter-clockwise, and negative is clockwise.

The source wrapper retains `report`, optional `original_report`,
`added_closing_segment`, `adjusted_final_endpoint` and `endpoint_adjustment_m`
(`null` or an object with `east`/`north` metre offsets). The first correction
also retains `original_closure` with those closure fields. `placement` records
its current anchor and called-north orientation. Derived diagnostic totals and
vertices are never accepted as replacement input; a reconstructed report is
required before generating or correcting a boundary.

## Grouped measured regions (v33)

A `measurement_boundary` can retain the union of at least two adjacent detected
measured faces. Its `extensions.measurement_linework_sources` remains the exact
outer boundary lineage. The additional `extensions.measurement_linework_group`
record has exactly `version: 1` and `members`, an array of at least two original
face lineage arrays. Each member uses the existing outer-style representation:
an ordered array of edges, each containing source uses with `owner_id`,
`segment_id`, `parameter_start`, `parameter_end` and `reversed`.

Member evidence retains internal seams cancelled from the combined outer
outline. A missing, hidden or changed seam source can therefore invalidate the
group even when its outer outline appears unchanged. The group does not
tessellate arcs, infer classifications or automatically deduct holes. The
single-loop combination primitive rejects disjoint selections, point branches,
nested overlapping outlines and selections that leave holes or multiple loops.

Any appearance of this reserved marker on a `measurement_boundary` requires
native format 33 and extraction version 31, including malformed, future or
missing-outer payloads and Undo/Redo or deleted-owner history. Qualification
does not depend on the current boundary model version or member validity.
Recomputing a digest cannot lower the reader floor. No SQLite columns, raw asset
encoding or existing command dialects change.

Unsupported top-level group schemas preserve their complete JSON and make the
document read-only. An understood version-one group with invalid or stale
member evidence retains its bytes while source checks withhold dependent
operations. A vendor key collision on an unrelated entity type retains the
previous format floor and does not acquire group semantics. Existing projects
without grouped measured regions keep their earlier native/extraction floors.

## Finished-room ceiling rule (v32)

ANSI-oriented policy version two uses calculation profile
`vertex-ansi-z765-2021-v2`. Sloped observations retain the gross
`room_floor_area_m2` as a geometry-binding check, and require Boolean
`complete_room_observed: true`. The half-height comparison uses current net
physical room geometry after the union of its real exclusions. Its numerator
must be observed for that same complete physical room outside those exclusions.
Scalar-only derivation, zero candidate area, missing confirmation, stale
bindings and excessive high-area observations cannot qualify a contribution.

A V2 sloped room may contain exclusion boundaries, including open-to-below and
low-height regions. It cannot treat measured child partitions as voids or
independently apply the room threshold to an arbitrary ownership remainder.
Flat floor → complete sloped room → low-height exclusion nesting remains valid.

Setup offers an explicit rule selector. Existing V1 projects retain their gross
room interpretation, and opening a dialog never migrates them. Choosing V2
preserves observations but withholds unconfirmed sloped totals until Edit facts
records the complete-room confirmation. Save/Cancel and Undo/Redo use ordinary
document commands. Details, schedules and PDF identify the selected rule; the
ceiling trace exposes the unrounded denominator and numerator.

Policy V2 or complete-room confirmation anywhere in retained history requires
native format 32 and extraction version 30, including undone/deleted entities.
Older readers refuse these semantics. Recomputing a digest cannot lower that
floor. V1-only evidence retains its existing format-21/extraction-19 floor.
This is a sourced Vertex rule interpretation, not final ANSI certification.

## Manual dimension presentation in recovery

Define First dimension orientation and omission use the existing version-two
`area_dimension` presentation, including `visible` and `rotation_radians`.
Unfinished and historical sessions with this presentation use boundary recovery
checkpoint schema 3 / replay 1. The checkpoint scans the entire retained action
timeline, including Redo-only placements, and canonically replays presentation.
See [dimension presentation checkpoints](boundary-recovery.md#dimension-presentation-checkpoints).

The native container supports v1–33. Recovery-bearing files retain the existing
container floor; older readers encounter an unknown nested checkpoint and
preserve the whole ledger opaquely without granting editable state. Completed
dimension presentation and JSON/assets recovery extraction already have their
required representation. New native/extraction version numbers are unnecessary
for this presentation extension.

## Exact chord length and heading (v31)

A standalone construction receipt with `version: 2` stores `chord_input` as
exact `length` quantity and `heading` angle records. The three chord-based arc
kinds require exactly one definition: this input or the legacy `chord_end`.
The endpoint is derived analytically from the retained local start, length and
heading. Original expressions, normalized expressions and entered units remain
part of the authority. Unused legacy receipts retain their unversioned encoding.

Boundary construction schema 4 / replay 1 permits typed receipts and retains the
schema-3 frame representation. Measurement linework schema 4 / replay 4 retains
the ordered schema-3 transform/edit representation. Promotion preserves existing
operations and their order. Transforms do not rewrite original typed inputs.
Recovery checkpoint schema 2 / replay 1 permits typed receipts, including closed
chains and undone actions; legacy checkpoint schema 1 remains unchanged. Older
known dialects reject typed input, and unknown positive versions remain opaque.

Native format 31 and logical extraction version 29 are required for typed
receipts, including retained Undo/Redo history, archived boundary source proofs,
replacement construction in redraw edits, nested reconstruction commands and
original wall input. Wall receipt validation is independent of an unknown future
context dialect; a known context additionally proves the original local frame.
It is not compared with an edited current wall baseline. No SQL or raw asset-row
representation changes accompany this version. Known malformed input rejects
atomically; retained expression strings participate in resource limits.

## Reviewed measured-area source replacement (v30)

Boundary geometry edit envelope v6 adds required `replacement_linework_sources`
to the complete v5 envelope. It is a nonempty ordered array of edge source-use
arrays, with exact `owner_id`, `segment_id`, `parameter_start`, `parameter_end`
and `reversed` fields. It is permitted only for redefinition of an existing
measurement boundary derived from measured strokes. Wall-source replacement
cannot share this authority.

Admission proves that the replacement geometry and complete lineage identify
an actual current graph face in the owner's property/building/floor/layer and
active design phase. Presentation visibility does not change the source graph.
A face already assigned to another current area cannot be assigned twice.
The retained name, appraisal facts, classifications, factors and deductions
remain authoritative; deductions must fit the chosen face. Changed child
identities require explicit reference mappings or reviewed removals.

The redefinition archives its previous construction or topology and the v6
operation. The final lineage must agree with the latest retained source review
proof. Future source edits can make the reviewed area stale again; they cannot
rewrite that proof implicitly. Each affected owner is reviewed separately.

Native format 30 and extraction version 28 are required whenever this proof
exists, including Undo history, constraint-command boundary edits and imported
identified-boundary derivation archives. Prior edit dialects retain their
historical wire representation and format floors.
Measured strokes may also participate in design-phase registries. This new
membership requires native format 30/extraction 28 even without an area review,
including retained history. Generic vendor `model` properties do not qualify.

## Measurement linework model v1

`measurement_linework` is required 2D geometry, separate from architectural
walls and calculated measurement areas. Each entity represents one connected
stroke, open or closed. Its `properties` contain the usual `property_id`,
`building_id`, `floor_id` and `layer_id`, plus a typed `model`. All four IDs must
resolve to the same drawing context. `required` is true so an older application
that does not recognize the entity preserves the project read-only.

The model has `version: 1`, `replay_version: 1`, `stroke_id` (the entity ID),
`anchor: [x, y]` in metres, `closed`, ordered `segments`, and opaque `extensions`.
Each segment holds `segment_id`, `start_vertex_id`, `end_vertex_id` and `receipt`.
Receipts use the existing exact construction-input codec; original quantities
and angles remain retained. Geometry is reconstructed with individual receipt
replay, not the closed-boundary replay engine. There is no parallel writable
geometry cache. Consecutive edges share exact endpoint coordinates and vertex
identities. A reused vertex identity must retain its exact point. Closed strokes
end at their anchor and reuse the first vertex identity. Crossings and retracing
are allowed; they do not themselves establish valid area polygons.

Linework contributes no GLA or architectural wall volume. Classification and
calculation require separate derived area entities. Unknown positive model or
replay versions preserve the original model without guessing its geometry and
make the document read-only. Invalid known data rejects atomically.

Independent version-one strokes do not increase the SQLite envelope floor:
the required entity mechanism protects earlier readers. Canvas authoring,
dimensions, point jumping, analytical output and derived face detection use this
model. These implemented adapters do not establish certified Draw First parity.

## Measured-stroke geometry derivations (v29)

The recognized `(version: 3, replay_version: 3)` pair retains the original
`anchor`, `segments`, exact construction receipts and opaque `extensions`. It
requires an ordered `operations` array and forbids the version-two `transforms`
field. Each operation is exactly one of:

- `{ "type": "transform", "transform": <version-one rigid transform> }`.
- `{ "type": "edit", "edit": <stable geometry-edit intent>,
  "authored_length": <exact quantity or null> }`.

The edit intent uses the strict boundary geometry-edit codec, restricted to
`move_vertex` and `resize_segment` with the actual stroke identity. Irrelevant
fields, unknown operations, invalid values and redundant persisted edits reject.
An exact authored length uses the construction-receipt quantity codec and must
equal the resize intent's target length. Vertex edits cannot carry that quantity.

Replay reconstructs the original analytical stroke, then applies operations in
order. Moving a stable vertex updates every occurrence of that identity. Resizing
retains the chosen start or end point, chord direction and signed curve sweep;
connected mode translates all other vertices together. Original inputs are not
rewritten to impersonate the changed geometry. Subsequent rigid transforms act on
the edited world geometry. Precision loss, degeneracy and nonfinite results reject
atomically. Open, crossing and retraced strokes remain valid linework.

An effective API no-op retains the exact existing dialect. A real edit promotes
version one or two to version three, migrating prior rigid transforms into the
ordered operations without changing their order. Any recognized version-three
stroke anywhere in retained history requires native format 29 and extraction 27.
An old envelope cannot admit these semantics by lowering its version marker.
Other positive schema/replay pairs retain the existing opaque read-only policy.

## Rigid measured-line frames and live area sources (v28)

The recognized `(version: 2, replay_version: 2)` pair adds a required `transforms`
array. Every ordered operation has exactly `version: 1`, `pivot: [x,y]`,
`rotation_radians`, `flip_horizontal`, `flip_vertical` and `offset: [x,y]`.
All coordinates and angles are finite. The local anchor and original ordered
receipts stay unchanged; world geometry is obtained by replaying those receipts
then applying the operations in order. Effective identity operations preserve the
original version-one encoding. Scaling is not part of this rigid dialect.
Reflections reverse arc sweep. Operations that lose measurement precision or
collapse an edge are rejected. Other positive schema/replay pairs remain opaque.

Defined measurement boundaries retain `extensions.measurement_linework_sources`,
an array in boundary-edge order. Each edge has one or more source-use records:
`owner_id`, `segment_id`, finite `parameter_start` and `parameter_end` within
`0 <= start < end <= 1`, and boolean `reversed`. Qualification recomputes the
complete source layer's analytical graph and compares geometry and lineage.
Deleted, unsupported, context-mismatched or semantically hidden sources withhold
the dependent measurement and appraisal totals. Presentation visibility does not
change physical totals. Parent deductions require current sources too.

Canvas movement and rotation refresh previously current areas when their source
edge identities identify one unambiguous face with unchanged edge count. Boundary
IDs, edge IDs and classification facts are retained. Source and area edits occupy
one revision. A changed or ambiguous topology requires explicit area review;
the old boundary cannot continue contributing a qualified total.

Any retained recognized version-two stroke or identified measurement boundary
with linework lineage requires native format 28 and extraction version 26,
including deleted and undone history. Published older files containing
version-one strokes and lineage can still open without being rewritten; their
next save upgrades the envelope. This legacy-read exception does not permit a
version-two stroke inside a falsely lowered envelope. Original backup and
history-preservation rules continue to apply.

## Connected curved-wall rigid intent (v27)

Constraint-command envelope 10 retains the ordinary, boundary, wall, physical,
exterior-source and supplemental lanes. It adds explicit boolean
`source_completion`, `supplemental_source_completion` and
`supplemental_asset_reference_completion`. Every lane remains an array, including
empty lanes. False flags cannot carry their corresponding changes. Envelope 10
never carries corner-edit authority. Its retained discriminator requires native
format 27 and extraction 25 even when its selected wall lane is empty.

Selected curved-wall proof 4 has exactly `version`, `wall_id`, `baseline`,
`length_entry` and `rigid_transform`. The transform contains exactly `version: 1`,
`pivot`, `rotation_radians`, `flip_horizontal`, `flip_vertical` and `offset`.
Replaying the original analytical segment must reproduce the submitted baseline
exactly. The original construction input and earlier operations remain intact;
one rigid operation is appended and any existing exact length receipt is rebased.
Missing or substituted receipts, malformed transforms and mismatched baselines
are rejected. Proofs 1 through 3 retain their old wire and replay contracts.

Connected owners continue through existing endpoint solving. Only independently
verified selected rigid wall IDs receive the endpoint-coordinate exchange
exception; dependent owners retain ordinary reversal guards. Persisted hard
relations, analytical crossing topology, live exterior-source correspondence
and hosted-object validity still apply to the complete transaction.

Supplemental assets can use the same compact references as envelope 9, resolved
only against independently validated full assets from the result revision.
Envelope 10 requires the newer floor whether or not compact references are
present. Existing resource budgets and legacy envelopes 1 through 9 are unchanged.

## Compact compound asset history (v26)

Constraint-command envelope 9 retains the version-7 entity and exterior-source
lanes, with compact `supplemental_asset_changes`. An upsert has exactly `kind`
and `asset`; its asset reference has exactly `id`, `media_type`, `sha256`,
`byte_size` and `metadata_sha256`. The metadata hash covers the UTF-8 bytes of
canonical `metadata.dump()`. It binds metadata independently of the binary
content hash. An erase has exactly `kind` and `asset_id`. References never embed
binary bytes or metadata. The explicit envelope remains version 9 even with an
empty supplemental list; it cannot acquire version-8 corner-edit authority.

New compound measured-wall operations use this representation for changed
assets. Exact unchanged upserts and erases of absent assets need no supplement.
Legacy envelopes 1 through 8 keep their original wire and replay semantics,
including inline assets in envelope 7 and independent corner intent in 8.

Full asset payloads remain in the immutable result revision's `revision_assets`
rows. Native loading validates those rows before resolving envelope-9 references
against that same revision. A resolver is mandatory for an upsert. Missing
assets, mismatched IDs, media types, sizes, binary hashes or metadata hashes,
duplicate references, unknown fields and malformed records are refused. Existing
deterministic history replay compares the complete resulting entity and asset
state; a reference does not authorize an unrelated change.

Any retained envelope-9 marker requires native format 26 and extraction version
24, including Undo and deleted history and recovery archives. Both SQLite
markers and the logical digest retain that floor; recomputing a digest cannot
authorize downgrading it. Extraction publishes full assets separately and keeps
references in revision proof JSON. It remains an export, not a JSON importer.

The existing budgets are unchanged: 256 MiB per asset,
512 MiB aggregate persisted assets across retained history,
1 MiB per persisted JSON column and 64 MiB aggregate persisted JSON.
Compact history removes duplicated hexadecimal asset bytes from the bounded
proof; it does not remove document, archive or metadata admission limits.

## SVG component colors (v25)

An SVG symbol may retain optional `svg_palette` with exactly `version: 1`,
`profile: "white-outline-2"`, `outline_color` and `surface_color` in #RRGGBB.
Absence inherits the exact source appearance. An explicit default-valued palette
still records authored intent. Unknown profiles, fields, nulls, malformed colors
and palettes on procedural definitions are rejected.

Any explicit palette emits annotation state 7. Native format 25 and extraction
version 23 qualify typed annotation state 7 throughout retained Undo/deleted
history, including preserved version-7 records with no palette. Older files
keep their previous requirements. Both SQLite markers and the logical digest
retain the reader floor; a recomputed digest cannot authorize a downgrade.
Recovery archives preserve the same intent. Extraction is not a project importer.

The palette changes only a derived render copy. Pinned SVG bytes, artwork hashes,
definitions, geometry, transformations and opaque owner metadata remain intact.
The profile maps black outline strokes and primary white/light-gray surfaces,
retaining gradient shading, opacity and protected glass, recess and dark details.
The XML parser rejects unsupported active content, resources, roles and profiles.
Unsupported saved artwork is diagnosed and blocks incorrect output until reset
or explicitly updated. Canvas, sheet and PDF use the same palette-aware renderer.

## Saved-view drawing appearance (v24)

The typed `sheet_view_model` can retain optional `presentation.appearance` in
its coordinated views. A view is identified by its owning graph entity and
local view ID. Appearance has `visible`, optional `style`, and canonical
`objects`. Each object entry has a unique `object_id`, optional `style` and
optional `visible`. Null style inherits; null object visibility inherits.
The appearance property itself is omitted for complete inheritance and cannot
be null. Styles retain `outline_color`, `fill_color`, `fill_pattern`,
`line_width_mm` and `hatch_scale`; they contain no analytical geometry.

Absent appearance emits sheet/view model version 6. Explicit appearance emits
version 7 and requires native format 24 and extraction version 22, including
when the meaningful record exists only in Undo or deleted history. Explicit
values equal to current defaults still represent authored intent. Both SQLite
markers and the logical digest retain this minimum reader version; recomputing
a digest cannot authorize a lower format marker. Older files retain their
previous native format requirement. A preserved raw model-version-7 payload
also retains this reader requirement even if it has no appearance entries;
normal typed serialization emits version 6 for that case. Extraction remains an export, not a JSON
project importer.

Document admission validates object references even for unrestricted views.
Source filters may temporarily exclude existing targets without discarding their
appearance; deleting a target removes dependent entries in the same edit.
Global style is the fallback, followed by view style and then object style.
Local visibility overrides global appearance only, never organization or source
filters. Whole-view hiding suppresses the complete rendered scene. These
presentation changes preserve measured geometry and appraisal quantities.

## Explicit automatic-angle removal during redraw (v23)

Boundary redefinition intent v5 opts into `allow_automatic_angle_removal: true`.
It contains every v4 redefinition field, including `fresh_topology` as an explicit
boolean, `replacement_wall_source_ids`, child mappings and removed-reference
IDs. The fresh flag may be false when the segment count changes. Other edit
kinds cannot carry this policy; the removal list must be nonempty.

Live application independently requires at least one listed affected automatic
angle dimension. Unrelated or unknown IDs, automatic edge lengths, area
dimensions and unused permission are refused. Automatic edge measurements
regenerate normally; kept angle targets require explicit incident edge/corner
mappings. Nothing is removed merely because an old corner disappears.

The normal redraw finish archives `desktop_operation` v3: the exact v2 target,
map, removal list and replacement-segment hash, plus the true policy flag.
The typed command must match that full archive. Removing, changing or
downgrading the flag cannot authorize another finish or survive archive replay.
Undo restores the original annotation exactly; Redo reapplies the same decision.

Any retained v5 intent requires native format 23 and extraction format 21,
including undone/deleted revisions or imported geometry-derivation evidence.
Both SQLite format markers and the logical digest retain that reader floor.
Extraction preserves the complete versioned intent and history; it remains
an extraction format, not a JSON project importer. Existing v1-v4 intent
encodings and their original removal rules remain unchanged. Older files
without this policy retain their previous required format.

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
exterior-corner edits, v23 explicit automatic-angle removal during redraw,
v24 saved-view drawing appearance, v25 SVG component palettes, v26 compact
compound asset references and v27 connected curved-wall rigid proofs, plus v4 through v27 archives through
recovery-aware APIs.
Under-versioned semantic data and versions above 27 reject. Legacy-only history
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

Application edits cross the workspace boundary as typed JSON command envelopes.
Ordinary edits use version 1; later envelopes retain the specialized geometry,
source-completion and asset-reference intent described below.
`sketch::command_to_json` and `sketch::command_from_json` preserve
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

Hosted openings always retain `wall_id` as their physical host. Without an
explicit `layer_id`, their drawing context and hierarchy parent inherit the
wall. An explicit valid layer on that same property/building/floor may provide
a separate drawing layer; if it differs from the wall's layer, that layer is
the opening's hierarchy parent and visibility context. The physical floor,
level, station and world elevation still come from the wall. Missing,
malformed, unresolved or different-floor layer references remain diagnostics;
they never establish another physical host or shift the opening.

The desktop source receipt additionally records `layer_reviewed` and a
`layer_mapping` array. Each entry contains `source_layer`, destination
`floor_id` and `layer_id`, `created_layer`, and `editable_item_count`. These
fields are provenance; editable entities use their ordinary floor/layer
properties and annotation children use `placement.layer_id`. New destination
layers, geometry, annotations and retained source are committed together.
The mapper's intermediate annotation entity carries
`extensions.dxf_annotation_layers`, an object from reconstructed child IDs to
effective CAD layer names, so labels and dimensions can be routed before
annotation merging. Native wall/opening candidates also retain their INSERT
layer in `extensions.dxf_source`. This opaque metadata does not change the
native container version or historical command interpretation.

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
`format_version` are equal and range from `1` through `60`, according to the
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

Deep local Windows folders and Unicode filenames use qualified absolute paths
at native file-operation boundaries and SQLite's locking `win32-longpath` VFS.
Public project paths, source provenance, receipt paths and backup names retain
their ordinary spelling. The same destination identity, expected fingerprint,
reparse-point, staging, backup and publication checks apply. This path handling
does not change the project format or expand the durable-filesystem boundary.

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

Legacy envelope 7 keeps its original one-MiB encoded proof ceiling and inline
hexadecimal asset bytes. New changed-asset source completion uses envelope 9
and native format 26, described above, to avoid duplicating those bytes in JSON.

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
V1 denominator uses gross room geometry and remains explicitly provisional.
V2 uses the countable finished-room rule described above. Final publisher
standard validation remains pending for both versions.

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

## Measured-stroke constraints and saved dimensions (native 35–36)

Native 35 protects retained measured replay/schema 5, command envelope 11 and
constraint bindings to measured strokes. These forms retain original entered
quantities, stable edge and vertex IDs, simultaneous vertex corrections and
explicit rigid transformations. The command completes eligible source-area
consequences and source-owned annotation offsets before final validation.

Native 36 additionally admits existing dimension models 1 and 2 whose
`target.entity_id` names a `measurement_linework` owner. Length targets use its
stable segment ID and resolve physical analytical length. Angle targets name
two distinct segments and their actual shared stable vertex, resolving outgoing
endpoint tangents. Open, terminal and revisited vertices remain legitimate;
closed strokes still cannot supply an area dimension. Unsupported owner models
remain opaque/read-only rather than acquiring inferred area semantics.

Ordinary vertex or length edits retain placed text coordinates and presentation.
Explicit measured rigid transforms reconstruct attached text coordinates once
with the complete pivot, reflection, rotation and translation. Mixed typed
boundary movement verifies the supplemental stroke against the same source
transform before reconstructing dimension positions; overlapping dimension
replacements refuse. Ordinary desktop mixed-object movement proves each stroke
payload by replay, places its dimensions from source, then validates the whole
ordinary transaction, including selected constraints and anchors.

Both native markers declare 36 whenever any retained revision has a dimension
targeting a measured stroke, including a genuine v1 stroke, entity-only import,
undone creation or deleted dimension. Logical JSON/assets extraction advertises
34 for this reader floor (native 35 maps to extraction 33). No SQLite table or
dimension schema change is required. Lowering both markers cannot make retained
new semantics readable by an older format. Original files remain preserved
through the normal save/migration workflow.

## Physical wall splits and full-span dimensions (native37)

Command envelope12 is a strict `apply_boundary_constraint_changes` envelope
containing only version, kind, expected_revision, message and wall_split. Its
version-one intent names the original wall, fresh second wall and coincident
seam relation, a strict interior fraction, and fresh analytical identities for
each affected source-current measured owner. It admits no ordinary entity or
asset mutation lanes. Replay independently reconstructs every consequence from
the retained source before comparison with saved state. Historical reuse of
entity or analytical identities rejects.

Each wall's version-one `extensions.wall_split_archive` retains directed source
and partition baselines, source identity, fraction, piece role and original exact
length entry. Its historical geometry is independently validated and retained
unchanged by subsequent ordinary edits. Signed arcs retain their construction
archive through the existing analytical reconstruction path. Unsupported future
archives remain opaque/read-only.

Fixed physical arc-length constraints use relation version4 with ordered pairs
of opposite endpoints for contiguous directed curved pieces. One equation sums
their physical lengths at their saved signed sweeps against the original exact
quantity. It does not impose independent piece locks or new equal-radius rules.
Dimension version3 stores `target.segment_ids` instead of `target.segment_id`
for an ordered contiguous chain. Its value is the sum of analytical member
lengths, retaining text placement, presentation and opaque metadata.

Reader37 applies to split commands, split archives, version4 arc-total relations
and version3 dimensions anywhere in retained history, including entity-only
imports, undone creation and deleted objects. Logical extraction advertises
exchange35. Both native markers and the package validator agree; existing older
histories retain their previous minimum floor. No SQLite table change is needed.
