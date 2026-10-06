# Pinc project import implementation plan

**Goal:** Open the supplied tool's saved projects in Vertex without overwriting
the original, silently changing geometry, losing content or inventing appraisal
facts. PINC-011 remains required until the complete workflow is qualified.

**Spec:** [source inventory](../requirements/pincsketch-4.3-feature-inventory.md)
and [comparison requirements](../requirements/pincsketch-4.3-vertex-comparison.md),
including PINC-011/012/013. Original production scope remains binding.

**Architecture:** A separate bounded Pinc candidate protocol runs inside the
existing import sandbox. The parser produces detached page records; the broker
validates the response and the desktop reviews fidelity/context mappings before
publishing a validated native document. Preserve source JSON once as an immutable
asset and use source pointers for provenance; unknown content stays opaque.

**Tech stack:** Existing C++20 geometry, JSON, document and Windows import worker;
Qt pixel transport and established safe preview creation for underlays.

## Contract and constraints

- Known modern source is format `PincSketch`, version `4.2`; known legacy is
  the inspected `2.*` conversion dialect. Reject unsupported versions explicitly.
- Feet and downward Y map to metres and upward Y. Signed sagitta maps to positive
  Vertex signed chord height without an extra sign inversion. Symbol/text angles
  change sign. Curves under the original 0.001-foot threshold are straight.
- Page occurrence, collection and original identity jointly identify a source;
  imported native identities are fresh. Remap only known references. Duplicate
  identities within a collection are refused, across pages are supported.
- Do not infer floor grade, physical wall thickness, dwelling identity or ANSI
  facts from page names, stroke width or area category. Pinc openings have visual
  line associations, not authoritative physical wall cuts.
- Exact analytic source/ring correspondence must uniquely establish an area.
  Rounded-node and sampled-centroid heuristics are not identity authority.
  Retain ambiguous/unmatched assignments and explain their review requirements.
- Bound input at 64 MiB, JSON depth/nodes/string retention, page/object counts,
  aggregate graph work and decoded image bytes/pixels before reconstruction.
  Reject duplicate JSON keys. No external URL fetch or source HTML execution.
- Decode underlays only in the worker; broker validates raw pixels and encodes
  the preview. Always set `render_asset_id`. Convert top-left to center using
  decoded aspect ratio and preserve the required vertical image transform.
- Existing DXF/IFC `PSIP0001` remains strict. No generic extension of its entity
  allowlist or promotion of opaque Pinc fields into native reserved metadata.
- Display source names, text, pointers and diagnostics as plain text in review
  and error controls. Source strings must not become rich-text image or link
  markup in the desktop.
- The native save path starts unset. Native Save As cannot overwrite `.pinc`.
- Root owns CMake, broker/desktop integration, generators, native verification,
  installation and Git. Source writers freeze before every native job.

## Execution and ownership

- [x] Parser worker: create `include/sketch/pinc_project_import.hpp`,
  `src/core/pinc_project_import.cpp`, `tests/pinc_project_import_tests.cpp`.
  Implement bounded modern/v2 parsing into typed detached pages, analytical
  segments, known presentation and source pointers. Expose
  `parse_pinc_project(std::span<const std::byte>, const PincImportLimits&)`.
  This internal checkpoint does not establish user-visible import.
- [x] Root: register the library/tests in CMake and inspect the returned API.
  Independently verify feet/Y/sagitta/rotation and modern/v2 known answers;
  exercise duplicate page identities, malformed/deep/oversized input, duplicate
  keys, ambiguous assignments and aggregate budgets.
- [x] Root/assigned worker: implement a Pinc-specific protocol and worker branch
  through existing sandbox controls. Check response schemas and aggregate
  assets before construction; refuse malformed/forged worker output.
  The bounded codec/pixel transport is verified. Actual installed sandbox
  qualification remains part of the user-visible import gate below.
- [x] Root/assigned worker: implement detached native admission with fresh page
  contexts, ordered restricted views/sheets, exact measured-line receipts,
  source-derived areas, labels, symbol mapping and visual wall associations.
  Preserve every unsupported item and its source pointer in the fidelity report.
  The geometry-only substep has a separate
  [checkpoint record](../verification/pinc-native-geometry-admission-2026-10-04.md).
  It does not satisfy the complete admission or user-visible import requirement.
- [x] Root: add native import access and review UI, current-state/dirty-transition
  guards, cancellation and atomic publication. Preserve exact original source
  and image bytes; use native Save As after import.
- [ ] Root: qualify actual import, Cancel, failure, Undo/Redo, save/reopen and
  output, including underlay/ghost scope, styles, text and all symbol mappings.
  Update the practical checklist and comparison with actual evidence.

The [desktop import checkpoint](../verification/pinc-desktop-import-2026-10-05.md)
now records passing actual synthetic import, Cancel/failure, connected editing,
Undo/Redo, native persistence and output cases. The final checkbox remains open
for installed qualification, representative historical files and full symbol/
paired-workflow coverage; bounded fixture success does not replace these.

Initial source integration inspection found that Vertex combined an
area's name and calculated value in one `CanvasLabel` and lacked authored text
alignment. The subsequent installed callout checkpoint added separate dynamic
name/calculation callouts and text alignment, including persisted controls and
screen/output agreement. These are also tracked under PINC-010/013; static text
copies of computed totals are not an acceptable substitute.

## Current completion work (2026-10-05)

Independent live callouts and text alignment are now implemented, checked and
installed. Complete the remaining import path as one end-to-end feature:

- Measurement admission worker owns `pinc_measurement_admission.hpp/.cpp` and
  its focused fixture. It composes existing exact geometry admission and
  re-derived measured-area authoring into one detached command, with explicit
  classifications and no inferred appraisal facts or containment deductions.
- Presentation admission worker owns its new pure core module/fixture: source
  dimensions, independent live name/value styles, aligned text, exact size and
  transformed pinned symbol instances, page/layer context and visual source
  associations. Unknown/missing counterparts remain identified losses.
- Cancellation worker owns the import supervisor's optional cancellation
  request and its focused tests. Cancellation terminates only its task-owned
  sandbox job and returns no publishable output.
- Root owns resource symbol bindings, raster/source assets, ordered page views,
  desktop review/dirty-state/atomic publication, CMake, generators and delivery.

The desktop import creates a new unsaved native project after the ordinary
dirty-project transition guard. Review maps source pages to explicit native
floors and distinct calculation/interior layers; pages are not assumed to be
floors. Source JSON is retained once unchanged, and Save As cannot overwrite it.
Cancellation or failure preserves the active project. Review uses plain-text
source strings and lists unsupported/unresolved content before publication.
Root freezes all source/header/test/script writers before any native job.
The gate includes actual broker import, Cancel/failure, edit/Undo/Redo,
save/reopen, ordered page/ghost scope, rendered output and fidelity reporting.

## Independent review and limits

Read-only advisor discovery confirmed this seam and identified identity,
assignment, aggregate-budget, decoder and publication risks. Review the actual
integrated diff against these risks before delivery. Synthetic independently
authored fixtures are implementation evidence; representative historical files
and paired runtime comparison remain required qualification. Do not redistribute
the supplied HTML/installer or call a parser-only checkpoint full import parity.
