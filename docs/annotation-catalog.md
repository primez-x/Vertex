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

The current deterministic catalog contains **468 parametric footprint entries:
52 named families times nine width/depth combinations** (80%, 100%, 120% of each
family’s nominal dimensions). IDs encode family and dimension indices. The
families cover plumbing, furniture, storage, fixtures, appliances, accessibility,
lighting, doors/windows, structural/site, and light-commercial equipment. These
entries are a working schematic foundation, not the final production artwork.
The production release criterion expands this catalog across plumbing, furniture,
fixtures, appliances, accessibility, lighting, doors/windows, structural/site,
and light-commercial equipment, while retaining at least 200 validated entries.
The production qualification validator enforces this requirement in both the
residential and light-commercial runs through mandatory `symbol_library`,
`symbol_resize`, and `symbol_output` observations.
Each entry must expose physical width/depth metadata, a centre anchor, scale
limits, and nonempty local preview strokes contained within its declared
footprint. Symbol family/category search is case-insensitive while category
selection remains exact. Placement subtracts the anchor,
uniformly scales, rotates counterclockwise in radians, then translates in metres;
the final acceptance fixtures must prove representative symbols can be resized
and remain legible at print and export scales. Instance style and visibility stay
separately editable. Invisible instances retain geometry; callers decide whether
to render them.

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
Document history; a category selector and case-insensitive family/category search keep the
catalog usable at its full size. Navigator rows and canvas hit testing expose
stable child IDs, deletion is undoable, and the inspector edits label text or
either annotation kind's position, rotation, scale, and visibility through
typed history.

Stored stroke/fill colors, fill patterns, paper-independent text height,
bold/italic emphasis, and symbol stroke width now flow into the same renderer
for interactive views and fitted sheet/export scenes.

This is a bounded semantic and authoring slice for APX-ANNO-001, APX-ANNO-003
and APX-SYM-001, **not completed parity**. Polished assets, visibility/override
inspectors, and production visual QA remain open.
Headless tests check every catalog preview, instance edits and JSON roundtrip,
typed Document admission, save/reopen, category/query filtering, placement mathematics,
deterministic IDs, physical-footprint bounds, independent resize behavior, and
malformed-data rejection.
