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

The visible desktop placement library contains the **320 supplied SVG symbols**
across 25 source categories. The underlying compatibility catalog also retains
**809 legacy parametric footprint entries across 393 named families** so existing
projects can reopen without losing stored symbol IDs. Those procedural
compatibility definitions are not offered for new placement. The original 52
families retain nine width/depth presets (80%, 100%, 120% of each nominal dimension); the expanded
families ship at their standard physical footprint and remain continuously
size-adjustable after placement. IDs encode family and dimension indices. The
families cover plumbing, furniture, storage, fixtures, appliances, accessibility,
lighting, electrical, mechanical, doors/windows, structure, circulation, site,
office, medical, recreation, safety, and light-commercial equipment.

The SVG library is preserved byte-for-byte under
`assets/symbols/architectural_v2/`, together with its original indexes and README.
`SOURCE.md` records provenance, the absence of a separate archive license, and
the project owner's direction to include these first-party assets under Vertex's
GPL-3.0-or-later license. No unidentified third-party rights are inferred. Run
`python scripts/generate_architectural_svg_catalog.py --check` to validate all
320 indexed paths and the checked-in deterministic C++ metadata. Run without
`--check` to regenerate after an intentional source update. The core catalog
does not parse files or depend on a working directory at runtime.

SVG IDs and families use `svg-v2-<source-category>-<source-id>`, for example
`svg-v2-01_bathroom-basin-oval`. Category qualification distinguishes duplicate
source IDs (radiator, skylight, pergola, column-round, column-square). All legacy
IDs, dimensions, previews and order remain intact in the compatibility catalog;
the SVG entries append in category/ID order. This retains catalog revision 1 and
existing saved-project compatibility. New-placement browsing exposes only the
supplied SVG set. Names are searchable case-insensitively along with IDs, families
and categories.

`SymbolDefinition::svg_asset` supplies an asset-root-relative path, native
`view_box`, `footprint_view_box`, and `dimensions_are_nominal`. In 208 assets the
SVG description supplies nominal millimetres, converted to metres. Their
footprint bounds exclude the surrounding artwork padding. The remaining 112
assets have no physical-size claim: their editable default footprint has a
one-metre longest side and follows the viewBox aspect ratio. The flag is false
for those defaults; it must not be presented as a measured or certified size.

The Windows desktop embeds every SVG in its Qt resource bundle and presents the
320 SVG entries as the complete new-placement library, with source names and
cleaned category labels. Library
thumbnails and placed components use the original SVG document. The canvas maps
the declared footprint bounds—not the padded viewBox—to the physical
width/depth centred on the placement anchor, retains the artwork padding, and
accounts for the SVG Y-down axis before applying model-space rotation. The same
retained SVG renderer feeds interactive views, fitted sheets, PDF, SVG and image
output. Save/reopen stores the stable catalog ID and placement; it reloads the
bundled artwork without an external path or archive dependency.
Legacy strokes remain unchanged for old symbols. New SVG entries reuse an exact
name-matched legacy family as a scaled fallback when available, otherwise a
footprint rectangle. These fallback/DXF strokes are **not SVG tessellation** and
do not reproduce the detailed supplied artwork. Offline manifests include the
human name and SVG metadata where present; project instances continue storing
stable catalog IDs, not filesystem paths.
Named appliance, storage, plumbing, furniture, and commercial families retain
distinct plan motifs (for example burners, drum/controls, shelves, fixtures,
and counter layouts) so the catalog cannot satisfy its count with duplicate
generic rectangles alone. The test matrix also checks representative toilets,
beds, furniture, and commercial entries at multiple scales and rotations using
the exact placement transform.
The production release criterion keeps at least 300 distinct production-quality
SVG components visible across plumbing, furniture, fixtures, appliances,
accessibility, lighting, doors/windows, structural/site, and light-commercial
equipment. Rescaled, skewed, relabeled, and hidden compatibility definitions do
not inflate that visible artwork count.
The production qualification contract requires this coverage in both the
residential and light-commercial runs through mandatory `symbol_library`,
`symbol_resize`, and `symbol_output` observations. Those observations still
require human review of artwork and visual fidelity; the validator checks the
declared evidence bindings and does not turn hashes or status text into an
automatic visual certification.
Each entry must expose physical width/depth metadata, a centre anchor, scale
limits, and nonempty local preview strokes contained within its declared
footprint. Symbol family/category search and category selection are
case-insensitive. Placement subtracts the anchor,
uniformly scales, rotates counterclockwise in radians, then translates in metres;
the final acceptance fixtures must prove representative symbols can be resized
and remain legible at print and export scales. Instance style and visibility stay
separately editable. Invisible instances retain geometry; callers decide whether
to render them.

Authoring accepts either a full variant ID (for example, `toilet-w1-d3`) or a
case-insensitive family alias (for example, `toilet`, `double-bed`, `sofa`, or
`checkout-counter`). Family aliases resolve to the deterministic `-w2-d2`
nominal footprint and the explicit variant ID is stored in the project.

The offline `vertex-cli symbols` command emits a deterministic version-1
catalog manifest containing catalog revision 1, entry dimensions, anchors, scale limits, vector
previews, family summaries, and category counts. It accepts an optional query
and category (`vertex-cli symbols toilet` or `vertex-cli symbols "" commercial`)
so release reviewers and downstream tooling can inspect the shipped library
without opening a project or contacting a service.
The complete unfiltered manifest can be checked independently with
`python scripts/validate_symbol_catalog.py <manifest.json>`; that validator
enforces the compatibility catalog's 300-family and 600-entry structural
thresholds, every required category, representative toilet/bed/sofa/commercial
families, deterministic counts/order, and preview
strokes contained within each declared physical footprint.

Version-1 JSON roundtrips instance content, style, placement, visibility, and
overrides. Decoding rejects malformed fields, unsupported versions, duplicate
instance IDs/override targets, unknown symbols, invalid scales/colors, nonfinite
coordinates, and excessive collection sizes. IDs are limited to 256 bytes and
label content to 65,536 bytes. Annotation state records the built-in symbol
catalog revision; legacy states without that field upgrade to revision 1, while
an unsupported revision fails closed until an explicit migration is provided.
Decode is atomic and returns a new state. The catalog is supplied separately;
instance JSON does not serialize custom catalog definitions. Unknown JSON fields
are ignored and are not retained; this is not an opaque forward-compatible
document envelope. The
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
and APX-SYM-001, **not completed parity**. The supplied detailed assets are now
the primary desktop library; full human artwork review, visibility/override
inspectors, and production print qualification remain open.
Headless tests check every catalog preview, instance edits and JSON roundtrip,
typed Document admission, save/reopen, category/query filtering, placement mathematics,
deterministic IDs, physical-footprint bounds, independent resize behavior, and
malformed-data rejection. `symbol_svg_desktop` additionally verifies the supplied
three-seat sofa's searchable name, detailed thumbnail, retained SVG payload,
interior canvas detail, exact physical footprint and identical save/reopen render.
