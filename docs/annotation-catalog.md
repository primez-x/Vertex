# Annotation and symbol foundation

`annotation_catalog.hpp` defines presentation-only label instances, editable styles,
visibility overrides for areas/objects/output views, and symbol placement records.
Records contain no analytical geometry or calculation classifications. Editing
them cannot implicitly reclassify an area. Target IDs are opaque references; the
caller must resolve their existence and apply output-view visibility policy.

The twelve label templates can be filtered by case-sensitive content/ID substring
and exact category. Instances copy content and keep a provenance template ID;
later instance edits do not alter the library. Empty content is allowed. Styles
support font family, physical text height/stroke width, RGB colors, bold/italic,
and none/solid/hatch fills. Font availability and hatch rendering are not checked.

The deterministic catalog contains **216 parametric footprint entries: 24 named
families times nine width/depth combinations** (80%, 100%, 120% of each family’s
nominal dimensions). IDs encode family and dimension indices. These are schematic
fixtures sharing six vector motifs, not 216 distinct polished production assets.
Each has a category, family, physical dimensions in metres, centre anchor, scale
limits, and nonempty local preview strokes. Placement transforms subtract the
anchor, uniformly scale, rotate counterclockwise in radians, then translate in
metres. Instance style and visibility remain separately editable. Invisible
instances retain geometry; callers decide whether to render them.

Version-1 JSON roundtrips instance content, style, placement, visibility, and
overrides. Decoding rejects malformed fields, unsupported versions, duplicate
instance IDs/override targets, unknown symbols, invalid scales/colors, nonfinite
coordinates, and excessive collection sizes. IDs are limited to 256 bytes and
label content to 65,536 bytes. Decode is atomic and returns a new state. The
catalog is supplied separately; instance JSON does not serialize custom catalog
definitions. Unknown JSON fields are ignored and are not retained; this is not
an opaque forward-compatible document envelope. The
`sketch.annotation_entity` Document codec wraps this state in a strict typed
entity, uses the deterministic catalog, and is included in the new-project
scaffold. Visible labels and symbol previews are projected into the shared
desktop vector canvas and persisted output. The desktop Annotations command
creates template-backed labels and catalog-backed symbols through normal
Document history; navigator rows and canvas hit testing expose their stable
child IDs, deletion is undoable, and the inspector edits label text or either
annotation kind's position, rotation, scale, and visibility through typed
history.

Stored stroke/fill colors, fill patterns, paper-independent text height,
bold/italic emphasis, and symbol stroke width now flow into the same renderer
for interactive views and fitted sheet/export scenes.

This is a bounded semantic and authoring slice for APX-ANNO-001, APX-ANNO-003
and APX-SYM-001, **not completed parity**. Visibility/override inspectors,
polished assets, and production visual QA remain open.
Headless tests check every catalog preview, instance edits and JSON roundtrip,
typed Document admission, save/reopen, filtering, placement mathematics,
deterministic IDs, and malformed-data rejection.
