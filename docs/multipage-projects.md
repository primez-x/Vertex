# Multipage project semantic model

`multipage_document.hpp` implements the bounded semantic portion of APX-DOC-001.
A project contains subject name/address/reference and string attributes, shared
project attributes, shared logical models, attributed areas linked to models,
and an ordered collection of presentation pages. Each page owns sheet dimensions
in millimetres, a positive scale denominator (100 means 1:100), navigation zoom
and center in metres, grid visibility, and an ordered list of model links with
independent visibility. Unlisted models have no presentation on that page.

Callers assign stable IDs. Project, model, area, and page IDs share one unique
namespace and survive reordering and JSON roundtrips without regeneration.
Links must resolve to a model declared in the same project. A model may appear
on multiple pages; duplicate links on one page are rejected. Page views contain
no copies of the model metadata, so view edits leave shared data unchanged.
Area attributes are descriptive strings, not calculated geometry or quantities.

`validate_multipage_project` rejects empty/duplicate IDs, dangling links,
nonpositive or nonfinite dimensions/scale/zoom, nonfinite centers, and invalid
metadata. Limits are 1,000 pages (at least one required), 10,000 models, 10,000
areas, 10,000 links per page, 256 attributes per object, 256-byte IDs and attribute
keys, and 16,384-byte text values. Embedded NULs are rejected. Empty model and
area lists, subject fields, and page titles are allowed for new projects.

The version-1 `sketch.multipage-project` JSON representation preserves page and
model-link order; object keys are emitted deterministically by nlohmann JSON.
Serialization validates state first; decoding validates shape, required fields,
types, limits, and references before returning a complete value. Unknown fields
and unsupported versions fail closed to prevent silent loss. Validation and
decoding report `std::invalid_argument`. These are object-count/field limits,
not a total byte or parser resource budget: callers parsing untrusted bytes must
apply their own byte limit before JSON parsing.

Tests cover a two-page text serialization/parse roundtrip, subject and area
metadata, linked models, reordered stable IDs, independent presentation edits,
invalid references, duplicate IDs/links, numeric validation, and malformed JSON.

The richer `MultipageProject` contract remains the interchange model. The
application-level `sheet_view_model` entity now supplies the corresponding
desktop page lifecycle: pages can be added, removed, and explicitly reordered
through Document history, the order survives save/reopen, cross-sheet references
are validated, and selected-page and complete ordered drawing-set output have
separate fingerprints. The drawing set exports as one multipage PDF and can be
sent through a multipage print preview, including mixed physical sheet sizes.
The property entity now
stores a `subject` object with `name`, `address`, `reference`, and bounded
string `attributes`; the Project details inspector edits that record through
the same undoable command path and save/reopen preserves it. Closed-boundary
area attributes are stored under each boundary's `area_attributes` object and
the Area attributes inspector edits them with the same bounded string-object
validation and history semantics. Physical printer behavior, rendering fidelity,
and production qualification remain open. The standalone
JSON roundtrip is therefore not by itself evidence of the full APX-DOC-001
requirement.
