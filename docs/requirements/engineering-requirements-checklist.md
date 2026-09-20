# Vertex manual acceptance checklist

This is the complete 130-item map from the project requirements ledger. These are requirements, not 130 independently proven usable features. Some cover installation, source delivery, licensing, performance, and compatibility rather than a button in the application.

All manual results start untested. Previous internal verification labels are intentionally not carried over as user acceptance. A requirement passes only when all of its stated behavior works. If a command is missing, inaccessible, or unclear, record that as a failure or blocked result.

Use copies of projects. For each item record Pass, Fail, Blocked, or Not tested, plus steps and observations. Keep an item unchecked until you accept it. Refer to the stable requirement ID when reporting an issue; one issue may affect several IDs.

Build/version tested: ____________________  Date: ____________________

## Capability map

- Offline: 3 requirements
- Ownership: 2 requirements
- Scope: 2 requirements
- Document Model: 7 requirements
- Precision: 2 requirements
- Architecture: 21 requirements
- Licensing: 3 requirements
- Components: 5 requirements
- Geometry: 4 requirements
- Constraints: 6 requirements
- Apex Parity: 23 requirements
- Calculations: 4 requirements
- Workspace: 6 requirements
- Field Input: 3 requirements
- Specialized Modules: 2 requirements
- Integrations: 2 requirements
- Compatibility: 5 requirements
- Accessibility: 1 requirements
- Assisted Workflows: 5 requirements
- Recovery: 5 requirements
- Interchange: 3 requirements
- Output: 3 requirements
- Security: 3 requirements
- Quality: 8 requirements
- Packaging: 2 requirements

## Test checklist

### Offline

- [ ] **001 — CORE-OFF-001**: Drawing, editing, calculating, saving, recovering, printing, and exporting work with networking disabled.

  **Expected result / acceptance:** On a clean Windows machine with network adapters disabled, complete the full authoring and output workflow without a network error or account prompt.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **002 — CORE-OFF-002**: The application has no account, activation server, subscription, or online entitlement dependency.

  **Expected result / acceptance:** Static dependency review and a network-denied run show no entitlement check, sign-in requirement, or remote service needed for the required workflows.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **003 — CORE-OFF-003**: Installation is offline-capable and bundles required runtime components, fonts, help, libraries, resources, and optional local-assistance assets.

  **Expected result / acceptance:** Install from the offline installer and portable package on a clean machine with networking disabled, then open bundled help and complete a project workflow.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Ownership

- [ ] **004 — CORE-OWN-001**: Deliver the original application source privately with reproducible Windows build instructions, fixtures, and build configuration.

  **Expected result / acceptance:** The delivered source archive builds the shipped artifacts on a documented Windows environment from a clean checkout without a hosted build service.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **005 — CORE-OWN-002**: Provide a documented, versioned .bldproj project format whose geometry, classifications, assets, metadata, history, and migrations are inspectable locally.

  **Expected result / acceptance:** Format documentation and the CLI describe schema versions, required fields, asset references, migration behavior, and preservation rules; a fixture round-trips through the documented format.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Scope

- [ ] **006 — CORE-SCOPE-001**: The production application targets Windows 11 x64, supports imperial and metric units, and serves residential and light-commercial projects.

  **Expected result / acceptance:** Supported OS, unit profiles, and representative residential and light-commercial fixtures are listed and pass the final acceptance workflow.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **007 — CORE-SCOPE-002**: Measurement and Architectural are separate workspaces over one shared project and authoritative document model.

  **Expected result / acceptance:** A project created in Measurement can be opened in Architectural, and edits, calculations, links, and saved revisions remain consistent in both workspaces.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Document Model

- [ ] **008 — CORE-DOC-001**: One semantic document model is authoritative for property/site, buildings, storeys, levels, layers, objects, boundaries, annotations, schedules, sheets, phases, and alternatives.

  **Expected result / acceptance:** Plans, calculations, projections, schedules, exports, and printing consume the same document revision and stable entity IDs.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **009 — CORE-DOC-002**: Typed links distinguish architectural walls, room boundaries, and appraisal measurement boundaries, with explicit ownership and no unconstrained bidirectional cycles.

  **Expected result / acceptance:** Independent, wall-derived, and measurement-driven relationships can be created, frozen, disconnected, undone, and rejected when cyclic or ambiguous.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **010 — CORE-DOC-003**: Save serializes an immutable snapshot at revision R into a validated standalone project while preserving the prior destination file.

  **Expected result / acceptance:** Interleaved edits during save produce a file containing exactly the reported saved revision; a failed replacement leaves the previous valid file recoverable.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **011 — CORE-DOC-004**: Every authoritative output carries an OutputFingerprint for document revision, profiles, assets, fonts, views, and processing components.

  **Expected result / acceptance:** Changing a fingerprint input either regenerates output or produces a visible stale-output block; matching inputs reproduce the same output identity.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **012 — CORE-DOC-005**: All edits use typed serialized commands over immutable snapshots; accepted compound operations commit atomically and are reversible.

  **Expected result / acceptance:** A compound edit either commits as one revision with one undo entry or leaves the previous revision unchanged; stale worker results cannot overwrite newer edits.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Precision

- [ ] **013 — CORE-DOC-006**: Entered quantities and units remain distinct from derived values, with analytical geometry, explicit tolerances, and display rounding that never changes measurement truth.

  **Expected result / acceptance:** Fixtures retain entered dimensions and calculate within the documented tolerance after display-format changes, save/reopen, and unit conversion.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **014 — CORE-DOC-007**: BRep geometry, meshes, projections, render pixels, and calculation caches are derived data and cannot become independent document authorities.

  **Expected result / acceptance:** Rebuilding or deleting derived caches leaves semantic geometry intact and all consumers regenerate from the document snapshot.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Document Model

- [ ] **015 — CORE-DOC-008**: Migrations are copy-based and reversible, preserve originals and unknown data, and block unsafe editing or authoritative output for unsupported required features.

  **Expected result / acceptance:** Each schema migration preserves the source fixture, reports unsupported content, and can be validated or rolled back without data loss.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **016 — CORE-DOC-009**: Provide a documented JSON-and-assets exchange representation plus local inspect, validate, extract, and migrate CLI operations.

  **Expected result / acceptance:** The CLI validates a good fixture, rejects malformed or incomplete data with actionable diagnostics, extracts assets, and produces a migration report.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Architecture

- [ ] **017 — CORE-DOC-010**: Apex exchange, CAD exchange, devices, georeferencing, and appraisal integrations attach through replaceable versioned adapters.

  **Expected result / acceptance:** A disabled or failed adapter does not prevent project open, deterministic editing, calculation, save, print, or export; manifests report protocol and capability versions.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Licensing

- [ ] **018 — COMP-LIC-001**: Qualify exact source revisions, build options, transitive dependencies, and distribution obligations for every shipped component before release.

  **Expected result / acceptance:** A signed-off dependency inventory includes source hashes, SPDX licenses, notices, corresponding source or relink instructions, and an SBOM for each installer and portable package.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **019 — COMP-LIC-002**: Original application code remains clearly separated from third-party code, with contribution provenance suitable for later private sale or optional open source release.

  **Expected result / acceptance:** Repository provenance, contributor rights, notices, and third-party source locations are complete and reviewable without relying on a hosted service.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **020 — COMP-LIC-003**: The baseline excludes GPL/AGPL application dependencies, noncommercial restrictions, and mandatory paid runtime SDKs; applicable LGPL components meet their obligations.

  **Expected result / acceptance:** Automated and manual dependency review finds no prohibited baseline dependency and verifies the selected dynamic-linking and relink path.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Components

- [ ] **021 — COMP-GEO-001**: Use a qualified Open CASCADE Technology layer for solid geometry, tessellation, and visualization with the original application model above it.

  **Expected result / acceptance:** Pinned OCCT builds render and regenerate representative residential and light-commercial solids, sections, and meshes without making render data authoritative.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **022 — COMP-GEO-002**: Use PlaneGCS/Eigen behind an application-owned planar constraint adapter; Ceres is not the planar solver substitute.

  **Expected result / acceptance:** The adapter reports degrees of freedom, redundancy, and conflict diagnostics for the application fixtures and contains no Ceres dependency for planar solving.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **023 — COMP-IO-001**: Use isolated local workers for reviewed IFC and DXF parsing/export components, with adapter manifests and no dependency on a hosted service.

  **Expected result / acceptance:** Worker versions, formats, capabilities, resources, and failure behavior are recorded; worker failure leaves the core project usable.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **024 — COMP-IO-002**: Use reviewed Qt PDF and Print Support modules for vector output and printing, with an explicit module allowlist.

  **Expected result / acceptance:** The exact Qt module set builds offline, is license-audited, and produces the same vector scene used by preview and supported output formats.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **025 — COMP-IO-003**: Use PROJ with bundled local resources and networking disabled for georeferencing.

  **Expected result / acceptance:** Required coordinate operations complete from bundled resources while a network monitor confirms no download or remote callback.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Geometry

- [ ] **026 — GEO-BASE-001**: Represent lines, arcs, holes, winding, tangency, and topology as analytical geometry rather than screen strokes.

  **Expected result / acceptance:** Independent analytical and high-precision fixtures agree within the documented area and coordinate tolerances after save/reopen.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **027 — GEO-BASE-002**: Construct and edit true curves using chord length, arc length, arc height, and angle inputs.

  **Expected result / acceptance:** Each curve input mode produces the expected analytical arc, preserves its defining parameters, and edits without converting to a faceted stroke.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **028 — GEO-BASE-003**: Support rise-and-run entry, relative turns, explicit angles, and unit-aware direction input.

  **Expected result / acceptance:** Fixtures entered in imperial and metric units reproduce expected endpoints and headings with documented tolerance.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **029 — GEO-BASE-004**: Provide alignment, snapping, point jumping, automatic closure, and bay-window completion while retaining exact geometry.

  **Expected result / acceptance:** Keyboard and pointer fixtures show each behavior, expose the resulting constraint or closure, and reopen without drift.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Constraints

- [ ] **030 — GEO-CON-001**: Support editable parallel, perpendicular, horizontal, vertical, coincident, and fixed-length relationships.

  **Expected result / acceptance:** Constraint fixtures report intended degrees of freedom and preserve each relation through supported edits and save/reopen.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **031 — GEO-CON-002**: Preview dimension changes with an explicit anchored endpoint and an explicit choice about moving connected geometry.

  **Expected result / acceptance:** A 12-foot-to-14-foot edit previews both supported anchoring choices, shows dependent movement, and commits only after selection.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **032 — GEO-CON-003**: Detect redundant, contradictory, underconstrained, and near-singular constraint states with understandable diagnostics; never silently relax locked measurements.

  **Expected result / acceptance:** Conflict fixtures identify the offending relations, offer only explicit user-confirmed repairs, and leave the previous valid revision intact on rejection.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **033 — GEO-CON-004**: Prevent unintended mirrored solutions, winding changes, topology changes, and unstable alternate branches during solve.

  **Expected result / acceptance:** Branch and mirrored-solution fixtures either retain the intended branch or ask for an explicit alternative selection.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **034 — GEO-CON-005**: Failed geometric edits preserve the previous valid document state and produce a reversible error result.

  **Expected result / acceptance:** Injected solver failures and impossible dimensions leave geometry, calculations, history, and saved revision unchanged.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **035 — GEO-CON-006**: Model levels and floor-to-floor relationships through an explicit vertical dependency graph.

  **Expected result / acceptance:** Changing a level elevation previews and applies expected dependent changes across plans, sections, and 3D without cycles.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Apex Parity

- [ ] **036 — APX-WF-001**: Provide a modern Draw First workflow with the documented Apex behavior and equivalent efficient completion path.

  **Expected result / acceptance:** A Draw First fixture creates, closes, edits, classifies, calculates, saves, reopens, and prints with expected geometry and command behavior.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **037 — APX-WF-002**: Provide a modern Define First workflow with the documented Apex behavior and equivalent efficient completion path.

  **Expected result / acceptance:** A Define First fixture defines an area, enters its geometry, edits it, calculates it, and preserves the expected boundary semantics through save/reopen.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **038 — APX-WF-003**: Build areas from existing geometry and preserve unfinished sketches for later completion.

  **Expected result / acceptance:** Existing walls or segments can become a measurement area without duplication, and an unfinished fixture reopens with its edit state intact.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **039 — APX-KEY-001**: Support keyboard distance-and-direction entry with predictable focus, commit, cancel, and repeat behavior.

  **Expected result / acceptance:** Recorded keyboard fixtures create and edit geometry without pointer input and reproduce the documented endpoint and heading results.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **040 — APX-KEY-002**: Accept fractional inches, feet-and-inches, metric units, and unit-aware precision input without lossy intermediate rounding.

  **Expected result / acceptance:** Equivalent imperial and metric entries create equivalent analytical geometry and preserve the original entered quantity and unit.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **041 — APX-KEY-003**: Preserve efficient keyboard semantics and provide an Apex-compatible shortcut preset alongside customizable shortcuts.

  **Expected result / acceptance:** Shortcut fixtures pass under the Apex preset, conflicts are reported, and user-customized bindings persist in a workspace.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **042 — APX-KEY-004**: Support point jumping, automatic closure, and bay-window completion as explicit geometry operations.

  **Expected result / acceptance:** Each operation is observable in command history, produces the expected topology, and is undoable.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **043 — APX-CURVE-001**: Expose curved wall and boundary construction through actual curve geometry, including chord, arc length, arc height, and angle definitions.

  **Expected result / acceptance:** Curve fixtures calculate correct area and perimeter, retain defining parameters, render smoothly, and remain editable after save/reopen.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **044 — APX-EDIT-001**: Support selection and clipboard operations for geometry, areas, annotations, and architectural objects.

  **Expected result / acceptance:** Copy, cut, paste, multi-select, and selection filtering preserve semantic links and produce undoable commands.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **045 — APX-EDIT-002**: Support rotation and flipping of supported geometry, areas, references, annotations, and architectural objects with explicit pivot behavior.

  **Expected result / acceptance:** Transform fixtures preserve analytical dimensions, typed links, and expected handedness, and can be undone exactly.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **046 — APX-EDIT-003**: Support vertex insertion and cloning for applicable boundaries, walls, and objects without breaking constraints or area topology.

  **Expected result / acceptance:** Insertion and clone fixtures preserve valid topology, report any new degrees of freedom, and reopen identically.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **047 — APX-EDIT-004**: Support reopening, redefining, and deleting areas and supported objects while retaining explicit history and relationships.

  **Expected result / acceptance:** Lifecycle fixtures cover reopen, redefine, delete, cancel, and restore with calculations and links matching the committed revision.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **048 — APX-EDIT-005**: Every committed operation, including classification, calibration, and compound edits, supports complete undo and redo.

  **Expected result / acceptance:** A mixed command sequence returns byte-equivalent semantic state after undo-all and redo-all, including derived recalculation state.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Calculations

- [ ] **049 — APX-AREA-001**: Detect, aggregate, and subtract areas from analytical boundaries with explicit grouping and hole behavior.

  **Expected result / acceptance:** Nested, adjacent, overlapping, and disconnected area fixtures produce independently inspectable totals and deductions.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **050 — APX-AREA-002**: Support classifications, base area, factors, net area, and explicit measurement profiles.

  **Expected result / acceptance:** Changing classification or factor produces a visible, attributed calculation revision without altering boundary geometry.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **051 — APX-AREA-003**: Report perimeter, living-area totals, building totals, and other required area aggregates with configurable rounding.

  **Expected result / acceptance:** Reference fixtures match independently calculated perimeter and totals within the documented tolerance, with rounding shown separately.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **052 — APX-AREA-004**: Make every total inspectable through boundary, deduction, classification, factor, rounding, and provenance details, including documented Auto-Subtract behavior.

  **Expected result / acceptance:** The calculation inspector identifies every contributing object and applies the same-type Auto-Subtract rule from the compatibility fixture, with no hidden deduction.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Apex Parity

- [ ] **053 — APX-ANNO-001**: Provide editable text and label libraries with per-instance content, style, placement, and visibility.

  **Expected result / acceptance:** Text library fixtures create, edit, filter, hide, and save labels without changing calculation classifications silently.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **054 — APX-ANNO-002**: Provide editable dimensions with placement, visibility, styles, references, and independent output-scale behavior.

  **Expected result / acceptance:** Dimension fixtures remain associated with semantic geometry through supported edits, print at the selected output scale, and can be undone.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **055 — APX-ANNO-003**: Support editable drawing styles, fills, and visibility controls for areas, objects, and output views.

  **Expected result / acceptance:** Style and fill changes affect presentation only, persist across save/reopen, and do not alter analytical geometry.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **056 — APX-SYM-001**: Provide a comprehensive, editable, size-adjustable symbol library for residential and light-commercial drawings, with more than 200 usable entries spanning plumbing, furniture, fixtures, appliances, accessibility, lighting, doors/windows, structural/site, and commercial equipment.

  **Expected result / acceptance:** The catalog contains at least 200 deterministic symbol entries across every required category; each entry has dimensional metadata, a validated preview, an anchor, and scale limits; each placed instance supports arbitrary rotation and visibility; persisted annotation state pins the catalog revision and any changed definitions require an explicit migration; representative residential and light-commercial symbols can be resized, placed, saved/reopened, printed, and exported without geometry or visual loss.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **057 — APX-TRACE-001**: Import PDF and raster reference images as project assets for tracing and calibrated presentation.

  **Expected result / acceptance:** PDF and common raster fixtures import offline with asset identity, page selection, and explicit fidelity/error reporting.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **058 — APX-TRACE-002**: Calibrate a reference image against a known measurement and preserve calibration provenance.

  **Expected result / acceptance:** A known 100 mm fixture measures within 0.1 mm numerically after calibration, save/reopen, and view transform changes.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **059 — APX-TRACE-003**: Trace over references and support resize, rotate, flip, intensity adjustment, and show/hide controls without changing source assets.

  **Expected result / acceptance:** Reference transforms are independently undoable, remain linked to the source asset, and preserve measured geometry.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **060 — APX-DOC-001**: Support multipage projects with subject information, area attributes, page-level presentation, and shared project data.

  **Expected result / acceptance:** A multipage fixture preserves subject and area metadata, page order, independent view settings, and linked model references after save/reopen.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **061 — APX-DOC-002**: Support local save, import, export, and declared legacy-version exchange workflows with fidelity reports.

  **Expected result / acceptance:** Each supported format has a fixture, documented subset, round-trip comparison, and explicit report for data that cannot be preserved.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **062 — APX-DOC-003**: Provide page layouts, print preview, PDF output, and image output with independent output scale.

  **Expected result / acceptance:** Layout fixtures preview and export at the selected page size and scale, and output fingerprints identify all influencing inputs.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Workspace

- [ ] **063 — APX-UI-001**: Provide pan, zoom, grid, independent output scale, and responsive overview navigation without rebuilding the whole interface.

  **Expected result / acceptance:** Navigation fixtures retain input responsiveness while derived geometry rendering updates asynchronously and view state persists.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **064 — APX-UI-002**: Provide visibility filters, overview map, light/dark themes, and saved workspace configurations.

  **Expected result / acceptance:** Workspace fixtures save and restore filters, theme, overview, and panel state without changing document geometry.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **065 — APX-UI-003**: Provide a searchable command palette, shortcut editing, and view tabs without duplicating primary commands in a second toolbar menu.

  **Expected result / acceptance:** Every exposed command is searchable, can be bound where supported, reports conflicts, and remains available from keyboard and pointer paths.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Field Input

- [ ] **066 — APX-INPUT-001**: Support mouse, keyboard, active pen, and touch input for precision drawing and editing.

  **Expected result / acceptance:** Recorded mouse/keyboard, real pen, and real touch fixtures complete core draw/edit operations with correct focus, pressure-independent geometry, and undo.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **067 — APX-INPUT-002**: Support DISTO measurement input through an adapter that targets an explicitly selected field and retains units and provenance.

  **Expected result / acceptance:** At least one selected DISTO model/transport fixture records a reading in the intended field without silently overwriting another measurement.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Specialized Modules

- [ ] **068 — APX-SPEC-001**: Provide survey and metes-and-bounds workflows with traverse geometry, acreage calculation, closure diagnostics, and output.

  **Expected result / acceptance:** Survey fixtures import or enter bearings/distances, report closure and acreage, preserve traverse provenance, and export the declared result.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **069 — APX-SPEC-002**: Provide the Pro georeferencing workflow with explicit coordinate reference, control points, residuals, and offline resources.

  **Expected result / acceptance:** A georeferencing fixture records control points and residuals, applies the selected transform, and reopens without a network request.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Integrations

- [ ] **070 — APX-INT-001**: Provide appraisal-software integrations through isolated, versioned adapters with explicit field mapping and failure behavior.

  **Expected result / acceptance:** Each supported appraisal application/version has an exchange fixture, field mapping, round-trip or one-way fidelity report, and offline failure test.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **071 — APX-INT-002**: Document and implement caller protocols for external integrations without coupling them to the drawing engine.

  **Expected result / acceptance:** Each caller protocol has a versioned adapter contract, capability negotiation, timeout/error semantics, and a recorded compatibility fixture.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Compatibility

- [ ] **072 — APX-NATIVE-AX5-001**: Import and preserve supported native Apex v5 projects with geometry, curves, classifications, dimensions, labels, symbols, imagery, and metadata where representable.

  **Expected result / acceptance:** Representative Apex v5 fixtures open with a field-by-field fidelity report and round-trip or preservation result for every supported data class.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **073 — APX-NATIVE-AX7-001**: Import, edit, and preserve supported native Apex v7 projects with geometry, curves, classifications, dimensions, labels, symbols, imagery, and metadata where representable.

  **Expected result / acceptance:** Representative Apex v7 fixtures open, edit, save, and export with a field-by-field fidelity report and no silent loss of required content.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **074 — APX-NATIVE-LEGACY-001**: Export declared legacy Apex-compatible versions with explicit subset, loss, and round-trip behavior.

  **Expected result / acceptance:** Each declared legacy export has a real consumer fixture, output comparison, and visible report for unsupported or transformed content.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **075 — APX-COMPAT-001**: Audit Apex Standard, Pro, and enabled modules rather than infer feature scope from purchase price or public documentation alone.

  **Expected result / acceptance:** The ledger records the installed edition/module inventory, observed behavior, settings, version, and fixture for every required capability.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **076 — APX-COMPAT-002**: Compare representative Apex projects and expected outputs across geometry, curves, classifications, dimensions, labels, symbols, imagery, metadata, calculations, and print/export.

  **Expected result / acceptance:** A compatibility report compares paired source/replacement projects and outputs, identifies any loss, and leaves no required mismatch unclassified.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Architecture

- [ ] **077 — ARCH-MOD-001**: Author straight, curved, sloped, and composite walls with joins, thickness, materials, and stable semantic identity.

  **Expected result / acceptance:** Wall fixtures create and edit each type in plan, elevation, section, and 3D while preserving joins, quantities, and links.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **078 — ARCH-MOD-002**: Author hosted doors, windows, and openings with type/instance properties and wall relationships.

  **Expected result / acceptance:** Opening fixtures host, move, resize, replace, schedule, and export without losing wall geometry or handedness data.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **079 — ARCH-MOD-003**: Author floors, ceilings, foundations, and their materials, levels, boundaries, and quantities.

  **Expected result / acceptance:** Floor-system fixtures appear correctly in plans, sections, elevations, and 3D and contribute the declared schedule quantities.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **080 — ARCH-MOD-004**: Author beams and columns suitable for residential and light-commercial building documentation.

  **Expected result / acceptance:** Beam and column fixtures support creation, placement, property editing, section display, schedules, and save/reopen.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **081 — ARCH-MOD-005**: Author flat, shed, gable, and hip roof forms with slopes, joins, openings where supported, and materials.

  **Expected result / acceptance:** Roof fixtures produce correct plan, elevation, section, and editable 3D representations and retain defining parameters.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **082 — ARCH-MOD-006**: Author stairs, landings, and railings with level connections and editable dimensions.

  **Expected result / acceptance:** Stair fixtures validate supported dimensions, display in all required views, and save/reopen with connected levels intact.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **083 — ARCH-MOD-007**: Provide levels, reference grids, floor-to-floor alignment, and basic site/terrain tools.

  **Expected result / acceptance:** Level, grid, and site fixtures coordinate plans, sections, elevations, and 3D without ambiguous vertical references.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **084 — ARCH-MOD-008**: Provide reusable assemblies with separate type and instance properties, materials, and quantity behavior.

  **Expected result / acceptance:** Changing a type updates its instances according to explicit rules while instance overrides remain visible and undoable.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **085 — ARCH-MOD-009**: Model rooms and architectural boundaries separately from appraisal measurement boundaries, with explicit relationships where desired.

  **Expected result / acceptance:** A room can relate to, follow, or remain independent from a measurement boundary without silently conflating classifications or geometry.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **086 — ARCH-MOD-010**: Support existing, demolished, and proposed phases across the model, views, calculations, schedules, and sheets.

  **Expected result / acceptance:** Phase fixtures show the selected state consistently in every required view and can be changed through undoable commands.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **087 — ARCH-MOD-011**: Support mutually exclusive remodeling alternatives over a shared existing model with clear comparison state.

  **Expected result / acceptance:** Alternative fixtures switch, compare, save, and print without contaminating one another or changing the shared existing state.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **088 — ARCH-3D-001**: Provide editable 3D authoring over the same semantic objects used by measurement and 2D documentation.

  **Expected result / acceptance:** A 3D edit changes the shared object and is visible in linked plan/elevation/section views, schedules, calculations, and save/reopen.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **089 — ARCH-VIEW-001**: Provide coordinated plans, elevations, and sections that edit and display the same building objects.

  **Expected result / acceptance:** A representative wall/opening/roof edit in any view updates the other views and retains stable references.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **090 — ARCH-VIEW-002**: Sections support cut depth, line treatment, material hatching, annotations, and detail overlays.

  **Expected result / acceptance:** Section fixtures render and print the selected cut and presentation settings and preserve them in the project.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **091 — ARCH-SCH-001**: Provide door/window, room, and material schedules with the required marks, properties, dimensions, counts, areas, deductions, and net quantities.

  **Expected result / acceptance:** Schedules generated from a representative project match the semantic model and update after accepted object edits.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **092 — ARCH-SCH-002**: Editable schedule cells issue normal document commands; calculated cells are read-only and expose their source.

  **Expected result / acceptance:** Editing a permitted cell creates history and updates the model; editing a calculated cell is prevented with a source explanation.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **093 — ARCH-SHEET-001**: Provide coordinated sheets with title blocks, revisions, callouts, schedules, and independently scaled viewports.

  **Expected result / acceptance:** Sheet fixtures preserve viewport scale, linked annotations, schedules, title-block data, and revision information through print/export.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **094 — ARCH-EDIT-001**: Every supported architectural object supports creation, selection, property editing, appropriate transforms, duplication, deletion, undo/redo, and save/reopen.

  **Expected result / acceptance:** Object-family fixtures execute the complete lifecycle and compare semantic, geometric, view, schedule, and persistence state.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **095 — ARCH-REL-001**: Plans, elevations, sections, 3D, calculations, schedules, and sheets use explicit stable references to the same semantic objects.

  **Expected result / acceptance:** Deleting or changing a referenced feature produces a visible issue or controlled update rather than silently retargeting an annotation or schedule row.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **096 — ARCH-OUTPUT-001**: Support complete residential and light-commercial remodel documentation from existing conditions through alternatives and coordinated output.

  **Expected result / acceptance:** End-to-end residential and light-commercial remodel fixtures produce editable model, comparison views, schedules, and sheets from one project.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Workspace

- [ ] **097 — UX-WORK-001**: Use a canvas-centered workspace with compact tool rail, expandable navigator, contextual inspector, and cursor-adjacent measurement panel.

  **Expected result / acceptance:** Representative workflows expose relevant controls in context, keep the canvas dominant, and preserve panel layout in saved workspaces.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **098 — UX-WORK-002**: Selecting a wall, area, reference, or architectural object exposes its relevant dimensions, relationships, styling, calibration, classifications, or calculations without unrelated modal dialogs.

  **Expected result / acceptance:** Selection fixtures show contextual properties and commit edits through normal commands with clear validation errors.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **099 — UX-WORK-003**: Provide split plan/3D views, view tabs, saved workspaces, light/dark themes, and a high-contrast field mode.

  **Expected result / acceptance:** Workspace and theme fixtures restore view arrangement and remain usable at supported DPI scales without changing document data.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Accessibility

- [ ] **100 — UX-ACCESS-001**: Support keyboard navigation, predictable focus, accessible properties and commands, high contrast, and layouts from 1366x768 through 4K at 100%, 150%, and 200% scaling.

  **Expected result / acceptance:** Accessibility and DPI fixtures complete core workflows without clipped controls, lost focus, inaccessible properties, or unreadable contrast.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Field Input

- [ ] **101 — UX-INPUT-001**: Provide pen/touch-specific controls and an on-screen measurement keypad while preserving keyboard precision paths.

  **Expected result / acceptance:** Real pen/touch fixtures enter and commit measurements through the keypad, preserve units/provenance, and remain undoable.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Assisted Workflows

- [ ] **102 — ASSIST-001**: Provide optional offline suggested tracing from plans or reference images.

  **Expected result / acceptance:** A local model or deterministic assistant suggests trace geometry on a fixture without network access and labels every suggestion unverified.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **103 — ASSIST-002**: Provide optional offline dimension extraction from supported plans and references.

  **Expected result / acceptance:** Extracted dimensions are visibly provisional, include source location/confidence, and require acceptance before becoming document commands.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **104 — ASSIST-003**: Provide optional assisted label placement and natural-language commands through the same command and permission model as manual edits.

  **Expected result / acceptance:** Suggestions preview as normal typed commands, identify affected entities, require acceptance, and are fully undoable.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **105 — ASSIST-004**: Assistance never silently invents measurements, changes classifications, or modifies accepted geometry; disabling it does not affect deterministic workflows.

  **Expected result / acceptance:** Assistance-off and malformed-suggestion tests complete draw/edit/calculate/save/print/export, and unaccepted suggestions leave no semantic changes.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **106 — ASSIST-005**: Distributed assistance assets and processing libraries have offline resource paths, provenance, and license records.

  **Expected result / acceptance:** The offline package contains or explicitly omits optional assets, reports missing local resources, and passes the same source/license audit as core dependencies.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Recovery

- [ ] **107 — REC-001**: Provide local autosave, recoverable history, and crash/power-loss recovery with explicit edited, autosaved, and saved states.

  **Expected result / acceptance:** Crash and power-interruption fixtures recover the last valid revision, identify the session and checksum, and never silently overwrite the saved source.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **108 — REC-002**: Provide named revisions and comparison views for semantic, geometric, calculation, and presentation changes.

  **Expected result / acceptance:** Two named revisions can be compared and restored without losing later work or changing the original revision.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **109 — REC-003**: Provide portable project packages and reusable templates containing required assets, profiles, and documentation references.

  **Expected result / acceptance:** A package copied to another offline Windows machine opens with all required assets and produces the same calculated and output fingerprints.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **110 — REC-004**: Detect external changes and enforce exclusive editing ownership, with read-only or independent-copy behavior for a second opening.

  **Expected result / acceptance:** Concurrent-open and external-change fixtures prevent silent overwrite and make the selected read-only/copy path explicit.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **111 — REC-005**: Failure-injection tests cover transaction, serialization, flush, replacement, recovery, migration, disk exhaustion, file locks, damaged assets, stale recovery, and newer schemas.

  **Expected result / acceptance:** Each injected failure has an expected recoverable result, evidence log, and no corrupted authoritative project is presented as valid.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Interchange

- [ ] **112 — IO-IFC-001**: Support the declared IFC4 ADD2 TC1 Reference View 1.2 interchange target with editable reconstruction where reliable and identifiable reference preservation otherwise.

  **Expected result / acceptance:** Representative IFC fixtures export/import with a fidelity report for geometry, types, properties, materials, relationships, and unreconstructed content.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **113 — IO-DXF-001**: Support a declared DXF R2013 subset covering lines, arcs, polylines, text, dimensions, hatches, and blocks.

  **Expected result / acceptance:** Subset fixtures round-trip supported entities and report unsupported entities without blocking native project use.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **114 — IO-PDF-001**: Treat PDF and raster underlays as calibrated reference assets, and distinguish editable extraction from simple tracing or image display.

  **Expected result / acceptance:** Import reports whether content is editable, traceable, or image-only and preserves calibration and source provenance.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Output

- [ ] **115 — IO-OUTPUT-001**: Preview, PDF, SVG, and printing consume one vector scene; raster imagery and shaded 3D are explicitly identified raster content.

  **Expected result / acceptance:** Paired output fixtures compare scene geometry, scale, text, dimensions, and raster placement across preview and exported/printed results.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **116 — IO-OUTPUT-002**: Support Letter, Legal, Tabloid, A4, A3, and architectural sheet sizes with independent scale and printer calibration verification.

  **Expected result / acceptance:** Each sheet fixture reports numeric page dimensions and a physical/driver scaling check separately from PDF geometry.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **117 — IO-OUTPUT-003**: Block authoritative output or clearly mark drafts when calculations, required references, assets, or fingerprints are stale or incomplete.

  **Expected result / acceptance:** Stale-calculation, missing-asset, broken-link, and incomplete-drawing fixtures cannot silently produce an authoritative file.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Security

- [ ] **118 — SEC-WORKER-001**: Run untrusted import workers in Windows AppContainer without network capabilities, with Job Object limits, brokered inputs, controlled temporary storage, and fixed search paths.

  **Expected result / acceptance:** A worker security fixture verifies token/container policy, network denial, resource limits, path restrictions, and controlled parent termination.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **119 — SEC-WORKER-002**: Contain malformed imports, path traversal, decompression bombs, DLL/module planting, child-process escape, timeouts, and worker crashes.

  **Expected result / acceptance:** Adversarial fixtures terminate or reject safely, leave the core project usable, and produce actionable local diagnostics without network access.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **120 — SEC-PROJ-001**: Disable PROJ networking and custom network callbacks; missing coordinate resources produce a local diagnostic instead of an attempted download.

  **Expected result / acceptance:** Network-denied georeferencing fixtures either use bundled resources or fail locally with no network request.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Quality

- [ ] **121 — OPS-PERF-001**: Target 95th-percentile navigation frame time at or below 16.7 ms on agreed reference hardware for representative workloads.

  **Expected result / acceptance:** Recorded 50,000-entity, 10,000-object/one-million-triangle, and 20-sheet/250 MB workloads meet the measured target or produce an explicit unresolved gate failure.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **122 — OPS-PERF-002**: Target input feedback at or below 50 ms and ordinary edits at or below 250 ms, with expensive regeneration in cancellable background work.

  **Expected result / acceptance:** Instrumented drawing and editing fixtures report latency percentiles and cancel a long regeneration without losing the valid revision.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **123 — OPS-PERF-003**: Target representative project open/save at or below five seconds while preserving snapshot and asset integrity.

  **Expected result / acceptance:** Instrumented representative projects report open/save time, revision identity, and manifest validation result.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **124 — OPS-QA-001**: Validate geometry, calculations, constraints, units, factors, rounding, topology, and persistence through independent analytical fixtures.

  **Expected result / acceptance:** The verification suite covers lines, arcs, holes, overlaps, winding, tangency, units, area tolerance max(1e-6 m2, 1e-8 times reference area), and constraint branches.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **125 — OPS-QA-002**: Pass an integrated residential production fixture exercising measurement, architectural authoring, editable 3D, plans, elevations, sections, schedules, alternatives, revisions, sheets, save/reopen, print, and export.

  **Expected result / acceptance:** The fixture completes the full workflow and its expected semantic, calculation, output, recovery, and fidelity assertions pass.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **126 — OPS-QA-003**: Pass an integrated light-commercial production fixture exercising multiple levels, assemblies, structural objects, schedules, quantities, sheets, and coordinated views.

  **Expected result / acceptance:** The fixture completes the full workflow and its expected semantic, calculation, output, recovery, and fidelity assertions pass.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **127 — OPS-QA-004**: Complete Create, measure, model in 3D, use optional assistance, edit, calculate, revise, save, reopen, recover, print, and export on a clean offline Windows machine.

  **Expected result / acceptance:** The packaged application completes the workflow with networking disabled, and the same workflow passes with assistance disabled.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **128 — OPS-QA-005**: Verify real pen/touch input, keyboard navigation, focus behavior, accessible properties, themes, high contrast, and supported DPI layouts.

  **Expected result / acceptance:** Recorded device and accessibility fixtures complete the core workflows at 100%, 150%, and 200% scaling without usability blockers.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

### Packaging

- [ ] **129 — OPS-PACK-001**: Deliver signed or otherwise integrity-verifiable offline installer and portable Windows packages with no hidden network prerequisite.

  **Expected result / acceptance:** Clean-machine installation, launch, repair/recovery, uninstall behavior, and package hashes are recorded with networking disabled.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

- [ ] **130 — OPS-PACK-002**: Deliver offline help, keyboard/workflow reference, source/build kit, dependency sources/notices/SBOM, format and adapter documentation, fixtures, and verification reports.

  **Expected result / acceptance:** The handoff inventory contains every required artifact, opens locally, and is sufficient for a clean Windows rebuild and review.

  **Your result:** Not tested

  **Steps, issues, screenshots, or notes:** ____________________

## Feedback log

| Issue | Requirement IDs | What you did | Expected | Actual | Severity |
|---|---|---|---|---|---|
| 1 | | | | | |
| 2 | | | | | |
| 3 | | | | | |
| 4 | | | | | |
| 5 | | | | | |
| 6 | | | | | |
| 7 | | | | | |
| 8 | | | | | |
| 9 | | | | | |
| 10 | | | | | |
| 11 | | | | | |
