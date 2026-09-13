# Production plan: offline measurement and architectural application

## Product promise

Build one independent, production-grade Windows application for residential and
light-commercial measurement, architectural design, and remodeling. ApexSketch
v7 capability is the minimum baseline. The requested modern interface,
precision improvements, recovery model, expanded architectural authoring,
editable 3D, coordinated drawings, schedules, sheets, and assisted tools are
part of that same production deliverable.

There is one production acceptance gate. Internal builds are implementation and
verification checkpoints; they are not reduced releases, substitutes for Apex
parity, or permission to defer an agreed capability to an unspecified later
product.

The application has two workspaces over one project:

- **Measurement:** fast Draw First and Define First sketching, keyboard
  precision, appraisal boundaries, classifications, calculations, field input,
  and reporting.
- **Architectural:** full residential and light-commercial authoring with
  walls, openings, assemblies, levels, roofs, stairs, remodel alternatives,
  editable 3D, linked plans/elevations/sections, schedules, and sheets.

Both workspaces must work independently where appropriate and must preserve
explicit relationships when they share geometry. A measurement boundary, a room
boundary, and an architectural wall are different semantic objects.

## Non-negotiable ownership and offline behavior

Drawing, editing, calculating, saving, recovering, printing, and exporting must
work without an account, activation server, subscription, or internet
connection. Installation must work offline as well: the installer bundles the
runtime, fonts, help, libraries, resources, and any optional local assistance
assets required by the selected installation.

Deliver the original application source privately, with reproducible Windows
build instructions, pinned dependency sources and notices, a software bill of
materials, test fixtures, and a documented project format. The source and
project format must remain practical to open-source or commercially distribute
later. Third-party licenses remain in force and are documented; no Apex code,
artwork, or proprietary implementation is copied.

The initial target is Windows 11 x64, with imperial and metric units and an
English interface. Residential and light-commercial workflows are in scope.
Cloud collaboration, other operating systems, structural engineering analysis,
and MEP engineering are outside this initial product definition.

## System design

The current implementation baseline is C++20, Qt 6 Widgets, Open CASCADE
Technology for solid geometry and visualization, PlaneGCS/Eigen behind an
application-owned planar constraint adapter, SQLite for project storage,
IfcOpenShell and ezdxf in isolated local workers, Qt PDF/Print Support for
document output, PROJ with bundled resources, and CMake/Ninja/MSVC for Windows
builds. These are qualified dependencies, not assumed approvals. Exact
revisions, transitive packages, build options, and distribution obligations
remain pending in Package 2.

The application-specific document model, commands, measurement rules,
architectural behavior, UI, adapters, and project format remain ours. PlaneGCS
is the selected planar solver; Ceres is not a substitute. A dependency that
cannot meet the licensing, offline, or isolation requirements fails
qualification before it becomes part of the shipped baseline.

The document is the sole semantic authority. Stable IDs and typed links own
property/site, buildings, storeys, levels, layers, walls, openings, assemblies,
rooms, appraisal boundaries, materials, constraints, calculations, views,
annotations, schedules, sheets, phases, and alternatives. BRep geometry,
meshes, projections, and calculation caches are derived values and can never
become independent sources of truth.

All edits enter through a serialized typed command boundary. Preview commands
use immutable snapshots; accepted compound edits commit atomically and produce
reversible history. Entered measurements and units are preserved independently
of derived values. Analytical lines and arcs, explicit tolerances, and display
rounding rules prevent pixels or rounded labels from changing measurement truth.

Saving serializes an immutable document snapshot at revision `R` into a new
standalone `.bldproj` database, validates the revision and asset manifest, and
replaces the destination while preserving the previous file. Edited,
autosaved, and saved are separate states. Named revisions, recovery records,
external-change checks, copy-based migrations, portable packages, and unknown
data preservation are required.

Every output records an `OutputFingerprint` containing the document revision and
hashes of relevant profiles, assets, fonts, views, and processing components.
Stale or incomplete authoritative output is blocked or clearly marked. Preview,
PDF, SVG, and printing use one vector scene; raster imagery and shaded 3D are
explicitly identified raster content.

Untrusted import work runs in a Windows AppContainer without network
capabilities, with Job Object limits, brokered inputs, controlled temporary
storage, and fixed DLL/Python search paths. PROJ uses bundled resources and
cannot download data. Optional assisted tracing, extraction, label placement,
and natural-language commands have an offline path, show inferred content as
unverified, and commit only through normal accepted commands. Disabling
assistance must never disable deterministic drawing or editing.

## Production capability baseline

The machine-readable ledger in [requirements/apex-parity.json](requirements/apex-parity.json)
is the authoritative checklist. It covers the documented Apex 7 workflow and
the requirements that must be qualified against the installed editions, modules,
projects, devices, and integrations:

- Draw First and Define First, geometry from existing geometry, distance and
  direction, units, rise/run, relative turns, snapping, alignment, jumping,
  closure, bay completion, and actual curve construction.
- Selection, clipboard operations, transforms, vertex editing, cloning,
  reopening, redefining, deletion, complete undo/redo, area detection,
  aggregation, deductions, classifications, factors, totals, provenance,
  labels, dimensions, styles, fills, and a comprehensive size-adjustable symbol
  library (toilets, beds, furniture, plumbing, fixtures, appliances,
  accessibility, lighting, doors/windows, structural/site, and light-commercial
  equipment), plus calibrated PDF/raster tracing,
  multipage output, print preview, PDF/image export, and legacy/native exchange.
- Pan/zoom, independent output scale, grid, filters, overview, themes, command
  palette, shortcut presets, saved workspaces, mouse/keyboard/pen/touch input,
  DISTO, survey/metes-and-bounds, acreage, georeferencing, and appraisal
  adapters.
- Walls, hosted openings, floors/ceilings/foundations, beams/columns, roofs,
  stairs/railings, levels/grids/site, assemblies, phases, alternatives,
  editable 3D, linked views, sections, schedules, quantities, sheets, and
  remodel workflows.
- Transparent calculations, explicit geometric constraints, conflict checks,
  autosave/recovery, named revisions/comparison, portable projects, offline
  installation, accessibility, security, performance, and licensing evidence.

The ledger deliberately distinguishes **documented** requirements from
**needs_evidence** requirements. For example, the exact Standard/Pro/module
feature set, native Apex v5/v7 files, caller protocols, and device combinations
cannot be certified from public prose alone. Those rows stay open until actual
fixtures and observed behavior exist. No row may be marked complete merely
because an import exists or a PDF tracing workaround is available.

## Construction sequence

The packages below describe order of work. They do not define separate release
exits or shrink the production scope. Each package adds implementation and its
verification evidence to the one final gate.

1. **Baseline and compatibility discovery:** complete the edition/module
   ledger, acquire representative projects and outputs, identify native files,
   caller protocols, device combinations, and undocumented behavior.
   Current source findings and unresolved fixture needs are recorded in
   [Apex compatibility discovery](apex-compatibility-discovery.md).
2. **Component and license qualification:** pin source revisions, prove the
   Windows build, qualify OCCT, PlaneGCS/Eigen, Qt modules, storage, importers,
   output, PROJ resources, workers, and offline packaging.
3. **Document and recovery foundation:** implement semantic authority, typed
   links, commands, snapshots, persistence, history, revisions, migrations,
   asset manifests, and the inspect/validate/extract/migrate CLI.
4. **Measurement and architectural engines:** implement geometry, curves,
   constraints, both drawing workflows, calculations, building objects,
   assemblies, levels, relationships, phases, and alternatives.
5. **Integrated authoring workspace:** implement the canvas-centered UI,
   contextual tools, command palette, field modes, editable 3D, linked views,
   schedules, sheets, and comparison workflows.
6. **Apex parity and adapters:** complete libraries, tracing, native/legacy
   exchange, field input, survey, georeferencing, appraisal integrations, and
   device/app compatibility certification.
7. **Assisted workflows:** deliver offline tracing assistance, extraction,
   placement, and natural-language commands with visible verification and
   normal undoable acceptance.
8. **Production hardening:** finish recovery and failure injection, security,
   performance, accessibility, help, installers, source/build kit, SBOM, and
   clean-machine offline checks.
9. **Unified production acceptance:** run every requirement through its named
   required gate in [requirements/production-gates.json](requirements/production-gates.json).

## Single production acceptance gate

Production completion means every required gate passes and every requirement row
is supported by current evidence. All Apex 7 baseline capabilities,
compatibility certification, modern improvements, both workspaces, full
architectural authoring, optional assisted workflows, and quality requirements
must pass together.

There is no parity-only release, architecture-only release, reduced offline
release, “best effort” compatibility exit, or alternative path that changes a
`needs_evidence` row into an omission. Missing fixtures, unsupported required
modules, failed fidelity reports, unresolved license obligations, stale output,
or failed offline/quality checks block production acceptance.

Acceptance includes analytical geometry and calculation fixtures; constraint
conflict and topology tests; residential and light-commercial end-to-end
projects; native/import/export fidelity reports; save/reopen/recovery and
failure-injection tests; clean-machine networking-disabled installation and
workflow tests; AppContainer/import security tests; accessibility and real
pen/touch tests; performance measurements; and artifact/license verification.

The final handoff contains the private source, reproducible Windows build kit,
dependency sources/notices/SBOM, offline installer and portable package,
project-format and migration documentation, adapter contracts, offline help,
representative fixtures, and completed capability/compatibility/quality reports.
