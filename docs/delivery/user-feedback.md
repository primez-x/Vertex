# Binding Vertex user feedback

The user-approved [consolidated delivery plan](plan.md) owns current execution.
The [canvas interaction contract](../design/vertex-canvas-interaction-contract.md),
[manual scenarios](../requirements/vertex-manual-testing-checklist.md), and
[19 September review](../requirements/user-feedback-2026-09-19.md) preserve
the detailed behavior and historical reports. These rows consolidate the later
accepted direction without inventing dates or copying private conversation data.
No feedback item has final user acceptance yet.

Earlier review reports remain historical evidence. Later binding direction
supersedes conflicting layout or execution cadence; it does not erase a past
observation or convert a source change into an observed resolution.

| ID | Owner | Required behavior | Observable acceptance |
|---|---|---|---|
| FB-001 | D06 | One hybrid drawing surface with Wall by default and Area / Measured lines choices | Open a plan and draw using Wall, Area and Measured lines on the same surface. Empty clicks start nodes; clicking the origin closes an eligible chain. There is no mandatory Select/Draw pointer-mode switch. |
| FB-002 | D06 | Selection and movement follow the user's pointer contract | Select an object, click outside it and observe only deselection. Drag inside the selected frame to move it once; drag elsewhere to pan. Ctrl-click toggles a hit and Ctrl-marquee adds enclosure/crossing selections with visible feedback. |
| FB-003 | D06 | Navigation, cancellation and quick properties are predictable | Right-drag pans and retains pending work. A stationary right-click cancels pending work; otherwise it opens relevant actions. Middle-drag and Space-drag pan, the wheel zooms about the pointer, and object double-click opens quick properties once. |
| FB-004 | D06 | A compact labeled header keeps the canvas dominant | Primary file commands have readable labels in one compact header. Remove decorative logo/star, offline marketing phrases and page-size clutter from the everyday canvas header. Theme selection is at the far right. |
| FB-005 | D06 | Layers, Library and Details share a useful left panel | The left panel exposes Layers, Library and Details, a drawing hierarchy with eye controls and a clear active layer, and inline add actions for buildings, floors and layers. Internal storage records do not appear as drawing objects. |
| FB-006 | D06 | Navigation and snap controls occupy the bottom-right canvas area | Grid, magnet, fit and overview-map controls are discoverable at the bottom right. The map displays content and the viewport and supports navigation; grid and snap state are clear. |
| FB-007 | D05 | At least 300 distinct usable symbols use real SVG artwork | Browse, search, place and inspect distinct furniture, plumbing, appliances, accessibility, lighting, structural/site and light-commercial symbols. Artwork is white and black with subtle gray detail, remains detailed in exports, and is not replaced by generic rectangles. Printed catalog counts do not substitute for usable artwork. |
| FB-008 | D06 | Selection handles resize and rotate persistently | Side handles resize one local dimension and corner handles scale proportionally. The frame retains its saved angle. Rotation snaps to absolute 45-degree increments; Shift permits fine angles. Live dimensions/angles, Escape, Undo/Redo and save/reopen preserve the intended result. |
| FB-009 | D04 | Adaptive grid and measurement magnets support exact drawing | Change zoom and units, draw and edit exact lengths, and use grid/length magnets without changing measurement truth. Exterior dimensions stay readable and correspond to actual exterior geometry. |
| FB-010 | D06 | Physical walls host useful doors and windows | Draw straight and curved walls, place doors/windows and edit their mechanisms, sizes and positions. Hosted components remain attached through connected edits, Undo/Redo and reopening. |
| FB-011 | D04 | Current GLA and its basis remain visible in Details | Inspect GLA without selecting an object, review its contributions and full calculation basis, then edit geometry and Undo/reopen. Missing facts or stale sources withhold unsupported totals; room classifications do not fabricate appraisal eligibility. |
| FB-012 | D05 | All 134 Pinc operations and 13 adoption requirements remain required | Execute the complete paired-operation checklist with the defined Vertex equivalents and record actual results. Preserve the original Pinc source and report unsupported or untranslated content. Source inventory, catalog size and synthetic checks do not establish full parity. |
| FB-013 | D10 | Public source is GPL-3.0-or-later and the app remains forever free | Inspect public source, license/notices, reproducible build instructions and distributed artifacts. Install and perform the agreed workflows without payment, account, activation, subscription or network access. Third-party obligations remain documented. |
| FB-014 | D01 | Appraisal compatibility has no preferred host | Discover and qualify required actual caller protocols, host/device combinations and representative projects. Record supported behavior and missing evidence for each required combination without declaring a preferred appraisal host. |

The twelve earlier review issue numbers remain available in the dated review:
drag-selection feedback; mouse closure; discoverable hierarchy additions;
remodeling alternatives; eye controls; internal-record filtering; prominent
2D/3D access; readable header commands; useful overview; clear magnet state;
visible usable library; and left-panel contextual properties with a compact
canvas tool strip. Their dated statuses have not been rewritten.

Record user observations against these FB IDs and the relevant U/PIN IDs.
Implementation, focused verification, installed verification and final
acceptance are separate in [requirements.json](requirements.json). All final
acceptance starts `not_accepted`; an issue closes only with observed evidence.
