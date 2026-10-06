# PincSketch 4.3 compared with Vertex

Reviewed 2026-10-05 against the supplied embedded HTML and current Vertex source.
The [full inventory](pincsketch-4.3-feature-inventory.md) lists 134 practical
operations, all 22 area-type entries, 80 named symbols and 30 built-in labels.
Source presence is not runtime qualification. PincSketch's own QA report leaves
4.3 mouse-level rendering unverified. Its 4.2 title/project format and 4.3 package
labels are recorded rather than silently reconciled.

The installed Desktop checkpoint is `vertex-20261005-pinc-import-footer`; its
shortcut points to the verified executable. It includes the Pinc import command
and independent name/calculation callouts. Bounded synthetic-fixture desktop
qualification and all six installed source/reopen samples pass, recorded in the
[current import verification](../verification/pinc-desktop-import-2026-10-05.md).
These are bounded checks, not complete user workflow certification. Reviewed same-ID room
repair and all 22 named drawing choices are implemented, while automatic room
correspondence, multi-room dispositions, physical-room dimensions and complete
appraisal-preset fact-review shortcuts remain open. This does not establish full
parity.

The following dated checkpoint notes preserve the evidence and limits recorded
at each step. Current source status below supersedes earlier statements about
whether Pinc import exists; it does not change their historical test results.

The [Pinc importer foundation](../verification/pinc-import-foundation-2026-10-04.md)
now parses known modern and legacy projects and transports validated image
pixels through a separate worker protocol. Four focused native checks pass,
including the existing reference importer. This is an internal checkpoint:
native document admission, fidelity review, import UI and installed import
qualification remain open, and the Desktop build does not yet import `.pinc`.

The subsequent [native geometry admission checkpoint](../verification/pinc-native-geometry-admission-2026-10-04.md)
converts detached straight/curved sources into editable native measurement
strokes on reviewed page layers. Its focused native check passes, including
shared-wall intervals, legacy attached-room correspondence and typed arc
editing with Undo/Redo. Area ownership and classification publication,
presentation, fidelity review, the import command and installed qualification
remain open. This checkpoint does not change the installed Desktop build.

The [independent-callout checkpoint](../verification/independent-area-callouts-2026-10-05.md)
adds separate live area-name/calculation presentation, independent placement,
size, color, alignment, rotation and visibility, plus reusable-text alignment.
All 11 affected native checks pass, including actual native reopening and
rotation/reflection. It implements this portion of PINC-010; tentative-curve
shortcuts and direct reusable-label access remain open.

## Current source update (2026-10-05)

The current source offers `Commands > Import PincSketch project`, exercised with
synthetic fixtures through a separate protected worker. Cancellation and failed
import leave the active project unchanged; a successful import opens as an
independent, unsaved native project. The original source is preserved exactly,
and native Save As blocks the `.pinc` extension. The desktop importer, page ownership,
measurement conversion, labels, styles and output were exercised against a
synthetic fixture; the full [verification record](../verification/pinc-desktop-import-2026-10-05.md)
lists the covered cases and limits.

That fixture uses source-derived generic measurement areas, not ANSI facts, and
retains authored names, styles, alignment, size and pinned-SVG transforms. It
checks endpoint-coincident joints, an 8 ft to 10 ft connected edit producing a
72 sq ft slanted face, live PDF output, reopen and Undo, ordered page-canvas
focus, a screen-only previous-page ghost and a two-page PDF. Subsequently authored
native symbols/labels preserve page ownership; long rotated text, distant-callout fitting, and a
pre-publication known-artwork-fidelity note also passed against the fixture.
Visual wall association in the import does not establish semantic physical-wall
hosting.

The affected native CTest run reported 20 passes and one worker-check skip due
to its harness. Independent real-worker and desktop-GUI checks passed; evidence
is retained under
`artifacts/import-worker-independent/release-e48bd4c5695d476fb280fd45ecc4340e`.
This is bounded synthetic-fixture evidence, not certification against all 134
paired operations. Original historical Pinc project files were not supplied.
All 80 named symbol kinds have stable crosswalk rows, but complete artwork,
default-size and hosting-quality comparisons remain open. The installed Desktop
checkpoint now includes this implementation; the full paired-operation review
and clean-machine qualification remain open.

## Matching capabilities and actual differences

| Workflow | Vertex implementation/access | Comparison and required action |
| --- | --- | --- |
| New/Open/Save/Save As; editable local projects | Main action bar; `main_window.cpp` file workflows; `ProjectStore` | Present. Vertex also preserves typed history, asset integrity, migrations and recovery. Current source and installed Desktop checkpoint include `.pinc` import through a separate command. |
| Undo/Redo, selection, copy/paste/delete | Unified canvas and action bar; clipboard/dependency helpers | Present. Mixed-source movement remains under active remediation; ordinary selection is not proof every compound group works. |
| Exterior/interior drawing; preview; grid/point/edge/alignment snapping | Wall drawing mode; `plan_canvas.cpp`, `constraint_authoring.cpp` | Present. Retain Vertex's physical thickness and unified surface rather than adopting separate Select/Draw/Pan modes. |
| Closed-space detection; shared edges; classification | Tools > Detect closed areas; wall-closure review; `measurement_area_definition` | Present with a more interrupted classification workflow. Imported generic measurement areas retain Pinc-authored names but do not create ANSI facts. Add the spatial palette workflow below. Classification retention after changed geometry needs direct comparison cases. |
| Feet/inches, decimal lengths, arithmetic, directions, rise/run, exact angle | Drawing input; `submitDrawingInput`, quantity parser and wall/boundary properties | Present. Pinc's one-operation arithmetic is not a reason to remove Vertex's retained exact quantities. Keyboard fluidity must still be qualified on real input. |
| Auto-close, point-of-beginning guidance, cancel/finish | Canvas/drawing input; `autoCloseDrawing`, drawing-session commands | Present. Keyboard J/along-wall offset/walk differs and remains a gap below. |
| Curves and editing dimensions/nodes | Source-derived arc authoring and inverse editing; wall/boundary quick properties and handles | Present and broader analytical constructions. Pinc wheel-bowing a tentative wall is a distinct shortcut; add it without making zoom ambiguous. |
| Panning, pointer zoom, fit | Unified drag, middle/right drag, wheel, status controls | Present. Preserve the user's requested Vertex gesture rules. Pinc has no explicit application right-click action in the inspected handlers. |
| Double-click edit, moving labels/dimensions, rotation/mirroring/nudges | Quick properties; selection transforms; typed dimension placement | Area quick properties now offer **Separate name and value**, with independent placement, size, color, alignment, rotation and visibility. Existing combined callouts remain until adoption. Pinc's symbol double-click rotation conflicts with the user's requested quick-properties behavior; retain quick properties with accessible rotation controls. |
| Fence rectangle and connected layout grouping | Ctrl marquee/multiselect; container organization; group transforms/copy | Selection and grouping exist. Batch style controls and some mixed transforms remain incomplete. Pinc's bbox/centroid fence rules are not exact containment semantics. |
| Area fill/color/opacity/hatches, label position, wall/dimension styles | Area appearance, text/dimension properties; shared scene renderer | Independent name/calculation callouts and reusable-text alignment are implemented and natively checked. Pinc import preserves fixture-authored styles, alignment and size; exterior-only/shared-edge automatic styling, batch presentation and broader parity remain open. |
| Symbol categories/search/click and drag placement; real dimensions/rotation/mirror | Left Library; pinned SVG catalog; hosted opening authoring | Present. The crosswalk now links all 80 Pinc names to candidates or remaining variant gaps. Names/counts do not prove artwork, default-size, placement or hosting parity; continue the per-entry comparison without padding counts with aliases. |
| Door/opening hosting, width, hinge and swing | Walls & openings; hosted opening properties | Present. Preserve actual hosted geometry rather than treating doors/windows solely as decoration. Reattachment and every door variant need direct workflow comparison. |
| Reusable labels, custom text, user-saved templates and search | Text library command; `text_library_dialog.cpp`, local template store | Present. Pinc saves reusable text, not searches; do not invent a saved-search gap. Compare its inline label retrieval against Vertex's dialog workflow. |
| Multiple pages, duplicate/rename/delete, navigation | Sheet/document organization and page controls; Layers > Floor reference | Present with a different property/building/floor/sheet model. A floor is not a page. The import fixture preserves ordered page-canvas focus and a screen-only previous-page ghost; Vertex's separate live floor link remains world-aligned rather than assuming printed-page order. |
| Image tracing, size/opacity | Reference import, calibration and reference properties; Layers > Floor reference | PDF/image calibration remains available. The new previous-floor reference provides visibility, opacity and XY alignment, with source updates and save/reopen; it is excluded from measurements and output. |
| GLA, categories, gross/net/deductions/perimeter and print report | Details > Setup/Edit facts/Full report; appraisal document core | Present and based on recorded grade/finish/access/ceiling/identity evidence. Pinc sums category flags. Never adopt an area preset as automatic proof of ANSI eligibility. |
| Live GLA while drawing | New compact `appraisalGlaShortcut` in status bar; same current Details report | Implemented in this change; native verification recorded separately. Click opens Details. Unqualified totals are withheld; unconfigured projects do not get a fabricated zero placeholder. |
| Short geometric calculation arithmetic | Details and printed audit use `derive_area_arithmetic` through one shared formatter | Added rectangle/strip multiplication, triangle base/height and signed chord/arc derivation, reconciled to current gross. Native known-answer, concave/major-arc, reversed-winding and fractional ANSI-rounding checks pass. |
| Portrait report guide and appraisal-ready cropped PDF | Tools > Sketch composition guide; Tools > Export sketch PDF; regular sheet/report output retained | Added an optional guide showing the actual content crop and a dedicated vector PDF with 2 mm padding. Navigation, grid, tracing images and interaction overlays are excluded. This crop is not a certified architectural scale. Pinc's useful crop workflow is retained without manufacturing a raster screenshot. |
| Stroke widths independent of screen zoom | Shared scene/output renderer and symbol palette | Present architecture; qualify actual symbols at varied zoom/output scales. Source inventory alone cannot certify output. |
| Legacy Pinc version-2 opening | `Commands > Import PincSketch project`; separate protected worker | Implemented in current source and installed checkpoint; exercised against a synthetic fixture, with cancellation, independent unsaved-project admission and exact source preservation. Original historical projects and full paired-operation qualification remain unavailable. Native Save As blocks `.pinc`. This does not replace Apex compatibility. |

## Required additions and improvements

These are additional production requirements from the user's supplied-tool
comparison. They remain gates at the required qualification level; an
implementation or synthetic fixture pass does not by itself satisfy a complete
paired-workflow requirement. The original unified release gate remains binding.

The initial adoption checkpoint added live GLA access, the spatial palette and arithmetic,
plus 20 independently authored SVGs (342 visible library entries total).
[Verification](../verification/mixed-selection-and-pinc-adoption-2026-10-04.md)
records the actual native evidence and limits. The palette currently uses the
active calculation profile and detected measurement-linework spaces; physical-wall
space detection and all 22 Pinc preset mappings remain unfinished. Added artwork
has been rendered and checked as bundled resources; that does not certify every
Pinc symbol's default size, hosting or editing behavior.

The subsequent [physical-wall geometry checkpoint](../verification/pinc-physical-wall-spaces-2026-10-04.md)
adds exact clear-space discovery from actual wall thickness, including partitions,
stubs, curves, obstacles and nested islands, and corrects a rotated-support graph
rejection. Seven focused native checks pass. This is the geometry foundation;
the palette's physical-room source consumer and persisted assignment workflow
remain unfinished. It does not change the installed build or establish PINC-002
completion.

The [physical-room consumer checkpoint](../verification/pinc-physical-wall-rooms-2026-10-04.md)
connects that geometry to the palette and wall-based room commands, retained
classifications, Details, canvas, schedules and supported exchange footprints.
Clear room area excludes physical wall material and holes, while exterior
appraisal GLA remains separate. Twelve relevant checks have passing outcomes.
Source changes explicitly make retained rooms stale; same-ID repair and complete
22-entry preset mapping remain gaps. This does not establish PINC-002 completion
or full lifecycle/production qualification.

The [room-repair extraction checkpoint](../verification/physical-room-repair-extraction-2026-10-04.md)
makes the same geometry and source admission callable from retained revision
state. Three lower-layer targets link without Document and 12 affected checks
pass. It changes no user command or installed build: reviewed same-ID repair
and complete room lifecycle handling remain open.

The [reviewed room-repair checkpoint](../verification/physical-room-reviewed-repair-2026-10-04.md)
adds an explicit same-ID destination review with Cancel/Apply, reference
decisions, rederived holes and retained-history validation. It also makes all
22 named drawing choices available, with an explicit addition action for older
measurement profiles. In appraisal mode those choices remain descriptive and
do not replace fact-derived eligibility. Fifteen affected checks have passing
outcomes. Automatic room correspondence, atomic multi-room dispositions,
physical-room dimensions and complete appraisal-preset fact-review shortcuts
remain gaps; this is not full PINC-002/012 or production certification.

The subsequent [sketch output checkpoint](../verification/pinc-sketch-output-2026-10-04.md)
adds PINC-005/006. It measures painted vector primitives and finished text replay,
including symbol artwork, rather than cropping to object anchor positions.
The other additional requirements remain open unless their individual evidence
is recorded; neither checkpoint establishes full parity.

PINC-004 now has a live same-building floor link in the Layers tab. A linked
destination has a focused 2D measurement surface; reference visibility does not
change that surface's export scope. Saved sheets and architectural/native views
retain ordinary project visibility. Optional floor metadata preserves the exact
source, opacity and metre offsets. Source errors clear the reference and expose
a repair diagnostic. Architectural named-view tracing and printed-page ghosts
are not established by this world-XY floor workflow. See the
[floor-reference checkpoint](../verification/pinc-floor-reference-2026-10-04.md)
for actual qualification and remaining limits.

| ID | Required result | Evidence needed |
| --- | --- | --- |
| PINC-001 | Live current GLA with one-click breakdown while drawing | Qualified, withheld, units/profile, invalid source, edit/Undo/reopen and selection-free native cases; light/dark capture. |
| PINC-002 | Searchable area-class palette; drag a type onto a detected space or its row; arm and click repeatedly | Real mouse/drop tests and user checklist; assign/reclassify/clear without geometry distortion or fabricated ANSI facts. |
| PINC-003 | Compact exact area arithmetic for rectangles, orthogonal components, angled and curved boundaries | Independent known-answer figures; sum equals authoritative gross area; deductions/factors/rounding explained separately; same canvas/report result. |
| PINC-004 | Previous floor/page as an aligned ghost reference, excluded from output | Reference linkage, visibility/opacity, source revision updates, floor coordinates, save/reopen; exported output excludes ghost. |
| PINC-005 | Optional report composition guide on canvas | Guide reflects actual export crop/layout, excluded from drawing/output; no implied architectural scale. |
| PINC-006 | Tight content-only vector PDF for inserting a sketch into an appraisal report | Crop includes symbols, callouts and strokes without clipping; real PDF inspection at multiple zoom/unit/page cases; retain regular print/report routes. |
| PINC-007 | Classification/area outline style applies to true exterior edges; shared separators have a neutral thin default | Adjacent house/garage, partial overlap, curves and holes; screen/PDF agreement; user overrides preserved; geometry/calculation unchanged. |
| PINC-008 | Bulk presentation edit for a selected fence/group's walls, dimensions and text | Heterogeneous selection, retain unrelated values/metadata, one atomic revision, cancellation, Undo and reopen. |
| PINC-009 | Fast J/corner jump, typed travel, along-wall starting offset and pen-up wall walk | Physical keyboard and pointer tests; pen state/preview/cancel; separate walls versus measurement mode; exact stored distance. |
| PINC-010 | Useful tentative-curve adjustment shortcut, direct reusable-label access and independently editable area-name/calculation callouts | Modifier/gesture conflicts resolved against Vertex's unified input; separate label anchors, size, color and visibility; text alignment; preview/commit/cancel and no accidental geometry changes. |
| PINC-011 | Pinc `.pinc` import, including known version-2 conversion | Current source command and bounded synthetic desktop fixture pass are recorded in the [import verification](../verification/pinc-desktop-import-2026-10-05.md). Original historical files were unavailable; complete conversion/fidelity comparison, unsupported-content reporting, semantic physical hosting and all 134 paired operations remain unverified. Preserve original source. |
| PINC-012 | Complete semantic counterparts for all 80 Pinc named symbol kinds and 22 area-type entries | [Per-entry symbol crosswalk](pincsketch-symbol-coverage.md) now has stable rows for all 80 names, including three new window SVG entries. Complete artwork/default-size/placement/hosting comparison remains open; map classifications to explicit Vertex facts rather than preset-driven GLA certification. |
| PINC-013 | Qualify equivalent drawing/edit/layout/history/output workflows against all 134 inventory operations | Actual paired scenarios and practical checklist outcomes; differences judged by useful behavior, not copied menus or unchecked source claims. |

Remaining mixed-selection gap: a hard-connected component spanning rigid and
partial lanes needs a combined solver. The new disjoint composition must refuse
that case atomically, preserve anchors, and keep the full capability requirement
visible. All original Apex, architectural, device, compatibility and recovery
requirements remain; Pinc adoption does not lower them.

## User testing

Use the [practical comparison checklist](pincsketch-vertex-user-checklist.md).
It is a list of observable actions, not developer signoff tasks. A feature marked
as source-present here remains unverified until its applicable runtime/user
evidence exists. User outcomes start Not tested.
