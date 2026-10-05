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
- [ ] Root/assigned worker: implement detached native admission with fresh page
  contexts, ordered restricted views/sheets, exact measured-line receipts,
  source-derived areas, labels, symbol mapping and visual wall associations.
  Preserve every unsupported item and its source pointer in the fidelity report.
- [ ] Root: add native import access and review UI, current-state/dirty-transition
  guards, cancellation and atomic publication. Preserve exact original source
  and image bytes; use native Save As after import.
- [ ] Root: qualify actual import, Cancel, failure, Undo/Redo, save/reopen and
  output, including underlay/ghost scope, styles, text and all symbol mappings.
  Update the practical checklist and comparison with actual evidence.

Source integration inspection also confirms that Vertex currently combines an
area's name and calculated value in one `CanvasLabel` and has no authored text
alignment field. Import fidelity therefore depends on adding separate dynamic
name/calculation callouts and text alignment, including persisted controls and
screen/output agreement. These are also tracked under PINC-010/013; static text
copies of computed totals are not an acceptable substitute.

## Independent review and limits

Read-only advisor discovery confirmed this seam and identified identity,
assignment, aggregate-budget, decoder and publication risks. Review the actual
integrated diff against these risks before delivery. Synthetic independently
authored fixtures are implementation evidence; representative historical files
and paired runtime comparison remain required qualification. Do not redistribute
the supplied HTML/installer or call a parser-only checkpoint full import parity.
