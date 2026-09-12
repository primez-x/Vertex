# Implementation status

## Current checkpoint (September 2026)

The Windows desktop now uses the immutable save queue and owner-thread
autosave scheduler for explicit saves, close/project transitions, and recovery
copies. Recovery archives carry stable v4 metadata and guarded destination
hashes; legacy and untitled edits are rebased into detached workspace captures
so they receive the same local recovery behavior. Recovered constraint previews
and boundary commits use sealed `ProjectWorkspace` adapters, preserving undo,
redo, and save acknowledgement. Typed relationship validation and portable
package staging are present as bounded, separately testable slices.

These additions are still development checkpoints. Apex native import/export,
device and appraisal adapters, broader recovery restore qualification, installer
and source-kit qualification, and production acceptance evidence remain open in
the requirements ledger.

The current checkpoint also includes read-only recovery-copy discovery with
source-path and hash matching, a fail-closed offline-independence policy wired
at native startup, deterministic source-kit and portable-package manifests,
immutable reference-asset calibration and transform records, typed schedule
records, and a versioned vertical level/floor-to-floor graph with strict Document
admission plus a Windows Levels editor for revision-checked creation, elevation,
link, freeze, disconnect, and per-floor level-binding operations. Floor bindings
name the graph entity and level explicitly and are validated against the graph
before entering history. Newly created walls, slabs, and architectural objects
on a bound floor opt into a versioned level placement record; plan, elevation,
section, and native 3D projections resolve that record from the same immutable
snapshot without rewriting source coordinates. Raster reference
images can now be imported into the project Asset store, displayed as a shared
canvas underlay, calibrated in metres per source pixel, and transformed through
typed undoable edits. PDF page import now retains the original PDF beside
a validated local PNG preview, so PDF tracing can start from an offline project
asset. Multiple retained underlays, PDF page selection, known-distance
calibration UI, and the explicit **Trace selected reference** boundary workflow
are integrated through the typed Document history. Automated edge extraction,
editable PDF geometry, native Apex compatibility, clean-machine packaging, and
end-to-end production evidence are still open.

The same canvas path now supports validated persisted `reference_grid` entities.
The Windows Reference grids editor creates, edits, removes, and toggles rotated
model-space grids with independent spacing/extents and deterministic major-line
classification through revision-checked Document commands. Both measurement
and architectural canvases render the same line list and axis labels after
save/reopen, while grids remain presentation aids and cannot affect measurement
geometry. Site/terrain coordination and full cross-view production
qualification remain open.

The next terrain slice now supports a validated, versioned local `terrain_surface` TIN.
Selected straight convex boundaries can be authored into a centroid-fan surface with one
elevation quantity per vertex. Deterministic deduplicated plan edges, interpolated contours,
elevation/section projections, native OCCT faces, visibility, save/reopen, and command-palette
authoring use the same semantic model. This is a bounded surface primitive for residential and
light-commercial work; site/terrain coordination, georeferencing, survey-device exchange,
native Apex compatibility, and production qualification remain open.

Horizontal architectural assemblies now carry an explicit `element_kind` on
the shared `slab` entity.  The Windows authoring path can create generic slabs,
floors, ceilings, and foundations from a closed boundary; the kind is validated
at the Document boundary, decoded by the native solid reader, preserved through
save/reopen, shown in the Inspector, and included in material schedule rows.
All four kinds use the same validated boundary, holes, thickness, elevation,
level placement, material, and quantity geometry.  This closes the bounded
semantic floor-system slice while the full production acceptance statement,
materials/assemblies, and Apex compatibility certification remain open.

The architectural object slice now accepts a true flat roof through the
existing sloped-panel schema: zero rise and zero pitch produce a thickened
planar solid, persist through the entity codec, and project through plan,
elevation, section, and the editor. Gable roofs continue to require a positive
rise and pitch. The primary command strip is constrained to a single compact
20 px logical row with 14 px icons; units, paper size, and view remain compact inline
selectors, with secondary commands in the More menu. Selecting a sloped,
flat, or gable roof also exposes contextual run (gable length), span, rise,
overhang, thickness, and derived pitch
fields in the Inspector; edits use the same validated dialog parser and one
undoable document command while preserving untouched quantity receipts.
The same slice now includes a straight railing object: a validated semantic
record produces one OCCT compound with a top rail, endpoint posts, and bounded
interior post spacing.  Railing create/edit/transform, plan/elevation/section
projection, native 3D rendering, quantity receipts, and save/reopen paths use
the shared document command and codec.  Curved or baluster profiles, stair
hosting, coordinated propagation, and production compatibility certification remain
open requirements.
Interactive plan canvases now include a model-space overview map with viewport
framing and click-to-recenter navigation. Workspace profiles persist map
visibility and the proportions of the navigator, tool rail, canvas, inspector,
and architectural canvas/3D split; asynchronous navigation qualification and
the complete acceptance statement remain open.

Precision drawing tools now show a compact, transient cursor readout directly
beside the pointer while a drawing tool is active. It reports the effective
snapped X/Y coordinates and, during an anchored boundary draft, the live
delta length and angle. The readout is deliberately absent in selection mode
and is excluded from fitted/exported scenes, so it does not consume workspace
area or become document output. Physical-input, DPI, and full accessibility
qualification remain open.

Drawing-sheet lifecycle is now integrated at the desktop boundary: the sheet
settings workflow can add a validated page with coordinated viewports, remove a
page only when graph references remain valid, and select the page used by draft
PDF/SVG/print output. Page creation/removal is ordinary undoable Document
history, and output fingerprints bind the selected sheet. Subject/area metadata
from the standalone multipage contract, richer layout editing, and production
output qualification remain open.

The Windows handoff inputs now include explicit source-kit and portable-package
allowlists under `packaging/`. The source-kit allowlist covers tracked source,
build metadata, documentation, fixtures, and licenses; the portable allowlist
covers the bundled font and required notices. The current Release runtime has
a fresh 37-binary dependency inventory, a deterministic 422-file source-kit
manifest, and a 469-file offline bundle whose generated verifier passes. These
artifacts remain qualification inputs: clean-machine installation, offline
launch/use, complete notices, and production acceptance are still open.

The relationship foundation now has a detached, deterministic geometry proposal
engine. It resolves explicit `follows` and `derived_from` links target-first,
supports rigid rotation/translation propagation through dependency chains,
requires agreement among multi-wall drivers, and reports missing, invalid,
deformed, reflected, or manually conflicting geometry without mutating either
snapshot. Stable segment and vertex identity migration, revision-checked
Document commit wiring, construction-receipt transform proofs, dependent
dimension-anchor updates, and undo/redo coverage are now implemented through
a revision-bound preview/apply adapter. The Windows editor now exposes a
visible canvas preview with editable rigid-move inputs and a confirmation step;
accepted changes use the workspace's ordinary undo/redo history. Production
cross-view qualification remains open.

The next foundation slice adds immutable room-versus-measurement relationship
validation, existing/demolished/proposed phase state with mutually exclusive
remodel alternatives, deterministic geometry-operation helpers, and a
performance-report harness that computes declared percentile thresholds without
claiming hardware qualification. Room relationship, model phase, and assembly
records are now admitted as typed Document entities: embedded models are decoded
at the boundary, referenced entities are checked atomically, and role mismatches
are rejected without advancing history. The desktop now exposes the model-phase
selector and alternative manager; active-phase filtering reaches the plan,
architectural views, native 3D visibility, schedules, and sheet output through
the shared visible-ID path. Room-relationship and assembly authoring, proposed
geometry registration, Apex compatibility, and production acceptance evidence
remain open.

The current foundation also has a strict typed annotation Document entity and
semantic annotation/style records with a 216-entry parametric symbol catalog,
coordinated view/sheet records with
section presentation settings and independent page scales, and a fail-closed
import-worker policy covering sandbox attestations, hostile inputs, resource
limits, and offline PROJ declarations. These APIs are tested in isolation, and
visible persisted labels/symbol previews now use the shared desktop vector
renderer. The desktop Annotations command creates template-backed labels and
catalog-backed symbols through typed Document history; stable child IDs are
available in the navigator/canvas for selection, undoable deletion, and
inspector edits to text, position, rotation, scale, and visibility. The
inspector also edits font family, paper text height, stroke/fill colors, bold,
and italic flags for labels and symbols through the same typed history command.
The Windows broker boundary now creates an AppContainer worker with an
ACL-protected per-job directory, explicit inherited-handle allowlist,
controlled offline environment, suspended launch, Job Object limits, and
post-launch token/network attestation. Its watchdog rejects timeouts and
partial output without an unsandboxed fallback. The live fixture is skipped
when the host test process is already inside a parent Job Object; clean-machine
runtime isolation, PROJ context wiring, output lifecycle, and production
integration evidence remain open.

Room/boundary relationship semantics are now surfaced in the Windows desktop:
the More menu and command palette open a typed editor that lists live room
boundaries, appraisal measurement boundaries, and architectural walls, applies
validated independent/follows/derived-from declarations through Document
history, and synchronizes newly created references without silently inventing
relationships. Geometry propagation, controlled retargeting, and full
cross-view acceptance remain open. A selected closed boundary can now be copied
into a distinct named `room_boundary` entity from the command palette; its
segments, area, classification, and source-independent history are persisted.

The selection command surface now includes bounded local JSON copy, cut, paste,
and delete operations. Wall selections carry their hosted openings; pasted
graphs receive fresh entity, boundary-edge, vertex, and annotation-child IDs,
and supported ownership links are remapped before a single Document command is
admitted. Cut/delete remove owned children atomically and preserve undo/redo.
Multi-select, broader relationship-aware edit policies, and production
compatibility evidence remain open.

Identified receipt-free boundaries now support guarded vertex insertion from the
desktop command surface. The operation validates a strict interior fraction,
rebuilds the boundary with fresh IDs, migrates typed references, and commits
the replacement as one undoable command without leaving duplicate source
geometry. Legacy anonymous and receipt-bound boundaries are rejected until an
explicit identity or derivation policy is available; wall/object insertion,
constraint reporting, and production qualification remain open.

The explicit geometry-operation path now connects point jumping, exact automatic
closure, and bay-window completion to the Windows desktop. Automatic closure
and bay completion publish named, identified measurement-boundary commands that
undo and redo as one document revision; point jumping moves the precision
pointer without dirtying the project and is available from the More menu and
command palette. The operation fixtures cover expected topology and exact
history behavior. Physical Apex key mapping, native Apex project fixtures, and
production qualification remain open.

The same desktop path now supports **Redefine boundary…** for identified
receipt-free boundaries. A redraw with the original edge count replaces the
analytical segments in place, preserves stable segment and vertex references,
updates optional classification metadata, and records one exact undoable
Document command. Invalid topology and edge-count changes fail closed; broad
reopen/restore fixtures and receipt-bound derivation policy remain open.

Automatic area detection now walks analytical wall endpoint graphs as planar
half-edges, returns deterministic simple bounded faces, and creates all detected
room boundaries in one undoable Document command. Open stubs are ignored and
duplicate or malformed geometry fails closed. Explicit nested-hole semantics,
face-level calculation fixtures, and production Apex output comparison remain
open.

The Architectural workspace now also exposes a compact **Assembly catalog**
that persists the typed `sketch.assemblies.v1` model in Document history. Users
can add/remove reusable types and placed instances, rename types, maintain the
local material catalog, declare typed properties/material slots/quantities, and
edit explicit per-instance overrides. Invalid references and dimension changes
are rejected atomically and every accepted change is undoable and survives
save/reopen. Geometric placement, quantity takeoff integration, nested
assemblies, and publication qualification remain open.

The coordinated sheet/view graph now has a versioned `sheet_view_model` entity
codec at the Document boundary. Document creation, command admission and
ProjectStore load reuse SheetViewModel validation, and a save/reopen regression
proves the typed graph survives the portable project format. This closes a
semantic persistence foundation; desktop sheet editing, projection, layout,
printing and output certification remain open.

Recovery validation now checks lifecycle ordering against the complete Document
history projection and replays the global navigation stacks/operation registry.
It rejects malformed IDs, sequences, targets, payload presence and revision
coverage. A separate archival-input validator checks canonical owners, direct
backward references, payload substitution, finite pointer overrides and retired
finish provenance. A mixed-workflow and corruption suite passes in Debug and
Release. Historical source validation now checks the retained revision digest
and its original drawing context; workspace validation checks activation,
archived and active bindings and rejects sources from future revisions. This
does not make a historically valid draft current for finishing. The combined
slot validator now reconciles active/retired input, counter floors, permitted
unrecorded edits, revision provenance and reachable redo. Each finish is also
reconstructed and compared with its actual document delta. Historical prefixes
retain their original editability restrictions, including abandoned unsupported
history; a review regression covers restrictions before and after a finish.
Aggregate typed-state admission now runs before combined recovery validation.
It charges shared inputs once, distinct copies separately, and bounds retained
document data and estimated validation work. Raw preflight rejects oversized
borrowed fields and incompatible action payloads before copying them; bounded
JSON counting avoids a full serialized temporary. Allocation-sensitive tests
cover oversized escaped JSON and classification/option payloads.
The version-one workspace-history codec now preserves global navigation,
immutable input references, explicit pointer clearing, independent counters and
opaque extensions. Unknown positive outer or nested versions preserve the whole
bounded envelope. Encoding and decoding share wire admission checks; aggregate
work rejection precedes owner replay. The recovery-ledger validator now checks
record uniqueness, required history, archive role, document anchors and exact
cross-record epoch/content/checkpoint agreement. Unknown kinds or versions and
role mismatches preserve the complete opaque ledger. Combined borrowed document,
wrapper and payload budgets precede copies and replay. See
`workspace-history-format.md` and `recovery-ledger.md` for the value contracts.
Internal SQLite v4 archive save/load now preserves the complete ledger and
optional saved marker. It shares the locked atomic-publication/backup path,
validates an aggregate digest, rejects duplicate recovery JSON keys and bounds
wire data before DOM construction and hashing. Opaque results expose no reusable
document snapshot. Desktop open/save now uses guarded v4 restoration and save
acknowledgement; a serialized immutable-snapshot save queue now provides FIFO
background publication, ordered barriers, exception capture, and explicit
draining/cancellation semantics. An owner-thread autosave scheduler now enforces
two-second quiet debounce and a 30-second maximum interval while preserving
stale/failure retry semantics. Desktop queue/scheduler wiring is now integrated
for explicit saves and timed recovery copies. Save As now removes only an
unchanged, role- and owner-matched recovery copy, rebases the next copy beside
the new destination, and reports a cleanup issue while retaining any changed
or foreign file. Restart restoration remains open.

Workspace navigation now orders document commands, session activation and
discard and finish in one in-memory history. Undo/redo restores exact archived drafts,
including stale source bindings; pointer-only changes preserve redo and semantic
draft changes clear it. Baseline command identities remain separate from
Document snapshot targets, so abandoned physical redo cannot bypass global
ordering. Seven focused workspace/history tests pass in Debug and Release.

Lifecycle inputs share immutable owners across operations. A source review
identified duplicate copies across discard and undo activation; the failing
regression now passes with shared ownership and exact pointer overrides,
including explicit pointer absence. Previously used session namespaces cannot
be submitted as new activations to bypass restoration. Finish publishes one
Document revision; undo retains non-finalizable retired input, and redo restores
the original geometry and dimension IDs. Multiple retired inputs survive branches
and activation navigation. Persisted event replay validation and aggregate resource
admission are implemented as core APIs; the recovery-aware SQLite v4 archive
routes now persist and reopen the ledger.
The final extension regression also passes in both builds: integer and floating
extension representations are compared canonically, so archival sharing cannot
substitute `1` for `1.0`. The five affected tests were rerun after this fix.

Aggregate workspace capture carries document, active draft, document and
lifecycle histories, navigation, identity, epoch, content generations and the
actual resource policy. Current document/draft values are detached; archival
inputs are shared immutable values. Captures survive edits and owner destruction.
Capture grants no save acknowledgement or filesystem ownership authority.
Persisted counters are restored by the core v4 archive route. Desktop
workspace-command routing, background I/O, save watermarks, startup selection of
unsaved unbound recovery copies, and ownership-verified Save As cleanup/rebasing
are integrated; broader restart qualification remains open.

The v24r full integrated checkpoint passes 74/74 tests in Debug (94.81 seconds)
and Release (20.37 seconds). All 227
recorded source inputs remained unchanged across those builds/tests, and 144
executable hashes are recorded. Recovery checks cover
all 18 action kinds, both modes, phases, construction, receipt encoding,
commits, exact checkpoint/history behavior, source bindings, active/recovery-copy
records, historical baseline fences, sealed workspace document publication and
active-checkpoint publication, global activation/discard navigation and
pointer/semantic generation separation. The checkpoint also covers desktop
v4 open/save, ledger preservation, failed-open isolation and fail-closed direct
edits to restored recovery documents. Entity authoring, organization/building
object commands, undo/redo, lifecycle-only navigation, recovered constraint
previews, and recovered boundary commits now route through the workspace.
The source and executable manifest is `artifacts/reviews/integrated-v24r-provenance.json`.
The earlier v24d checkpoint also passed eighteen offscreen boundary
canvas/input/workflow checks at DPR 1, 1.5 and 2 across both builds.
These are local incremental builds and regression checks, not reproducible-build
or production performance certification.

The workspace publication API passed independent source review. It has private
candidate documents, instance/epoch/full-source checks, detached previews and
no mutable Document escape. Core archive restoration now validates and detaches
the decoded aggregate before installing it in a fresh workspace. The guarded
desktop open path now uses this API. Internal aggregate v4 persistence, the
save acknowledgement boundary, startup selection of unsaved unbound recovery
copies, and ownership-verified Save As cleanup/rebasing are implemented and
used by the guarded desktop paths; broader restart recovery and production
qualification remain open.
Explicit Revise Input is implemented in memory: canonical replay regenerates
IDs, preserves local history/counter floors and opaque extensions, and records
the original retired namespace and finish event on a fresh activation bound to
the current document. It leaves the retired input intact. Focused replay and
workspace finish suites pass in Debug and Release after this change. The v24k
full-suite checkpoint includes Revise Input, combined slot validation and
aggregate resource admission. The later v24l checkpoint adds the workspace-history
wire codec, malformed-record checks and encoder/decoder admission regressions.
The v24m checkpoint additionally covers aggregate ledger validation in both
drawing modes, including cross-record corruption, opaque preservation and
cumulative resource limits. Independent source review found no actionable
issues in the internal ledger validator; disk persistence was outside that scope.
The in-memory finish implementation passed independent source review. Subsequent
test-only additions verify mixed finish/edit navigation across the baseline and
validate the Document history projection after navigation; the finish suite was
rerun in both configurations after those additions.

Save acknowledgement binds a sealed publication to workspace identity, owner
correlation, role, destination, epoch, both content generations and the
authoring source digest before accepting a storage receipt. The current desktop
save path uses that gate synchronously and marks its shared Document saved only
after acknowledgement. `SaveReceipt` contains only revision, file hash and
backup path, so it cannot establish those workspace bindings on its own. The
future queue must preserve stale successful files without clearing current
dirty state; owner metadata remains correlation data, never proof of a
filesystem reservation.

Compact persistent history replaces the former deep semantic snapshots. The
unchanged old-engine corpus (13 fixtures, 66 positions, all eighteen actions)
passes against both implementations with exact branches, geometry bits and
checkpoint bytes. Structural checks cover shared-copy isolation, concurrent
independent copies, linear chunk retention, allocation-free geometry navigation,
and iterative teardown of 100,000 actions. Isolated 256/512/1,024/2,048-edge
measurements show approximately linear growth. At 1,024 edges, Release private
memory after construction fell from 925.9 MB to 52.7 MB and the measured
construction/codec/navigation cycle fell from 6,125 ms to 107 ms. See the
[measurement report](boundary-recovery-performance.md) for workload limits.
The shared resource-admission policy now bounds live authoring, encoding,
decoding, typed restoration, and estimator replay with the same limits. Focused
tests cover rejection rollback, redo branches, extension depth, and arithmetic
overflow. The estimator uses the compact representation. Return construction
and retired recovery metadata have allocation-fault regressions; the earlier
v24c Debug restoration failure is repaired. The latest isolated measurements
with admission enabled pass through 2,048 edges; Release total cycle time is
380 ms at that size. These engineering limits are not a minimum-hardware SLA.

The v4 typed-key Windows ownership broker repair passes configured unit and
real subprocess tests in Debug and Release. Tests cover same-key contention,
release/retry, path/file namespace independence, partial bundle rollback,
actual abandonment after helper termination, allocation-free shutdown signaling,
preallocated rollback bookkeeping, retryable cleanup and unexpected owner exit.
Independent review found a queued-acquisition shutdown race and loss of
abandonment observations during registry allocation failure. Both now have
focused passing regressions; the shutdown test failed against the previous
implementation. Restart recovery, recovery discovery, and production
qualification remain required; see [durable recovery](boundary-recovery.md).

The desktop now consumes the broker through `ProjectOwnershipSession`. Open and
Save As reserve a normalized project path; existing files also reserve their
volume/file identity. A cooperating second open is loaded read-only, while the
current owner rechecks identity and SHA-256 content before publication and
refreshes the identity after atomic replacement. External edits become an
explicit read-only/save-as boundary. Focused ownership and document regressions
pass in Debug, and the Windows cross-process fixture verifies mutex contention,
case-insensitive paths, and retry after owner exit. Clean-machine process
lifetime, restart recovery, hostile writer races, and production qualification
remain open.

The earlier v23f desktop checkpoint adds
native Draw First and Define First sessions,
a precision line/arc input form, transient draft previews and committed dimension
labels shared by both workspaces and draft output. Schema-v2 point receipts
preserve coordinate-defined endpoints directly; storage remains v3. The v23f
integrated pass has 45/45 tests passing in Debug and Release, with all 160
recorded source inputs unchanged across the builds and tests. Native Qt event
tests cover both drawing modes, dimensions, draft history, classification and
precision dialogs, context cancellation, save/reopen and visibility. The 44-case
actual-reader matrix preserves schema-v2 data read-only in the retained v22
reader, including deleted and undone history. PDF captures confirm committed
dimensions with clean output strokes and no editing grid or selection highlight.
The v23d independent review found a cursor/click snapping mismatch. The v23f
correction uses one effective point for previews, cursor status and placement,
including stationary snap toggles and active-workspace ownership. New off-grid
event tests check saved point receipts and manual dimensions in both canvases.
All three focused desktop tests pass at DPR 1, 1.5 and 2 in both configurations
(18 checks). Independent review approved the frozen v23f correction with no
findings; this remains an internal checkpoint. Unfinished drafts
are currently kept in memory and are explicitly reported as unsaved; durable
draft recovery and receipt-preserving geometry edits remain required.

The full production goal is active. The repository is an internal development
checkpoint and does not yet satisfy the production replacement contract.
All 130 requirements in `requirements/apex-parity.json` remain mandatory.

## Current construction work

- The shared boundary authoring session, durable construction codec and atomic
  commit service are integrated into the core libraries. Both drawing modes
  share eight analytical construction forms; saved receipts replay against
  canonical geometry. Format v3 protects qualified construction records
  throughout retained history while preserving unrelated vendor metadata.
  Current v22 evidence passes all 42 integrated tests in Debug and Release,
  all 12 guarded native geometry/forms cases across DPR 1, 1.5 and 2, and 18
  actual retained-reader checks. Save/undo/delete/reopen and failed/successful
  v2-to-v3 replacement tests preserve the original file and required format.
  Before/after hashes match for all 155 recorded source inputs; these remain
  local incremental builds, not reproducible-build certification. Independent
  review approved the frozen v22 persistence/commit integration with no high
  or medium findings. Its two low verification follow-ups (a stricter reader
  rejection check and explicit abandoned-receipt history coverage) were closed
  by the subsequent v23 work. Desktop session wiring is now integrated
  as described above. Persisted unfinished work and typed receipt-preserving
  geometry edits remain required.
- The private repository, Windows compiler scripts and pinned dependency
  bootstrap are established. Qt 6.8.3 is installed locally. OCCT 8.0.1,
  Eigen 5.0.1 and Boost 1.92.0 have compiled and installed from the pinned
  vcpkg registry. The distribution license audit remains open.
- Analytical line/circular-arc geometry and exact imperial/metric input are
  implemented. Focused tests cover intersections, winding, transformed
  boundaries, fractional input, measured arc length, starting tangents, invalid
  input and numeric overflow. This is
  part of the precision engine, not complete keyboard/workflow parity.
- The first architectural solid operations construct straight/curved walls,
  hosted openings and slabs with holes using OCCT. Tests check expected
  volumes and reject invalid openings. The building-object core also constructs
  columns, arbitrary-axis beams, stair flights/landings, sloped panels and
  ridge-clipped gable roofs. Debug/Release tests check volumes, frames and
  non-overlapping ridge geometry. A strict semantic codec now connects all six
  forms to the native viewer; malformed forms block framebuffer export, and
  hidden views track document changes. All six forms now have a parameter
  editor connected to revision-checked commands, undo/redo and save/reopen.
  Exact top-down projections also feed the shared plan/PDF renderer; failed
  projections block output. Hidden editor captures cover both display scales.
  Review added exact segment-set/circle-topology checks and exact quantity
  entry records. The expanded six-form desktop workflow passes its Debug and
  Release create/edit/undo/save/reopen checks. Exact quantity receipts are tested
  through the dialog and document history for all six forms. Independent exact
  projection oracles also cover stairs, both roof forms and sloped beams.
  All 35 integrated tests pass in both configurations at v18. The bounded
  independent authoring/organization recheck approved the repaired package.
  Schedules, remaining forms, joins,
  materials and coordinated documentation are still required.
- The PlaneGCS point-constraint adapter builds as a replaceable solver DLL.
  Constraint candidates are independently checked before returning a preview.
  A versioned codec now persists seven straight-wall endpoint relations and
  exact length receipts. Solver-free document checks prevent raw edits from
  bypassing hard relations, validate hosted openings, preserve unsupported
  historical locks read-only, and enforce explicit endpoint remapping during
  reversal. Shared wall semantics feed the solid builder too. Debug/Release
  tests and independent CLI fixtures pass. The targeted independent backend
  recheck approved the repaired persistence and shared wall-semantics paths.
  A source-bound command service and interactive preview/Apply dialog now support
  anchored length edits, explicit connected movement and all seven relation
  edits in both workspaces. Modified or stale previews reject before mutation.
  Debug/Release integration and normal/150-percent captures pass at v16d. Readable
  conflict diagnostics now pass focused Debug/Release service tests. Actual
  service-driven resize and undo restore the expected native framebuffer at
  DPR 1, 1.5 and 2 in both configurations. Independent service/dialog review
  found nested metadata loss and insufficient receipt validation. The repair
  preserves nested baseline/receipt data, validates exact receipt consistency,
  and passes preview/history/reopen regressions in both configurations at v18.
  The independent repair recheck remains open. Constraint bindings to boundary vertex IDs,
  curves and the level dependency graph are still required.
- A strict identified-boundary codec now preserves stable vertex/segment IDs,
  analytical arcs, exact joins and unknown metadata through upgrade and
  reversal. Raw document edits reject malformed identified geometry and
  identity removal; exact upgrade undo/redo and reopen preserve the original
  states. Unknown boundary versions remain read-only. The shared plan and
  calculation path uses identified canonical segments even when auxiliary
  vendor geometry is present. Focused regressions and the full v17c suites pass.
- A strict segment-length dimension codec derives measurements from stable
  boundary/edge references and preserves nested metadata. Document state and
  retained history reject dangling references and require atomic dimension
  retargeting or deletion when an edge is retired. Unknown dimension versions,
  kinds and boundary owners remain read-only without skipping other supported
  validation. The codec and document tests pass in both v18 configurations.
  Dimension rendering, styles and the authoring workflow remain required.
- Storage format v2 protects identified boundaries across the entire retained
  history, including an anonymous head reached by undo. Legacy-only v1 files
  still load. The retained v16 CLI rejects three actual v2 fixtures; the current
  reader preserves undo/redo and unknown-version state and rejects marker
  disagreement, nonconforming v1 identity history and future storage versions.
  Eight additional v18 boundary/dimension/deleted-history fixtures pass actual
  older-reader rejection and current-reader reopening without file changes.
  A same-file v1-to-v2 upgrade test verifies failure preservation, an exact
  original-file backup, reopening and undo to the original anonymous geometry.
  Full migration/recovery qualification and independent boundary integrity
  review remain open. Drawing sessions, placed dimensions, exact construction
  receipts and source-edge/constraint reference handling are still required.
- Area calculations handle analytical boundaries, deduction unions, exact
  rational factors, explicit classification rules and inspectable rounding.
  The inspector now exposes area/perimeter/factors, versioned profile rules,
  totals and rounding details, plus a contained-boundary deduction editor that
  persists `deduction_ids` and reports requested versus marginally applied
  amounts. Unknown classifications, unresolved deduction references and
  overlaps block totals. Its save/reopen and undo/redo workflow tests pass;
  Apex same-type Auto-Subtract compatibility remains open.
- Document commands, revision history and SQLite snapshots work through the
  CLI and desktop. History, schema and reference repairs passed their recheck.
  UTF-8 byte limits, Windows path aliases and staging-file publication identity
  passed independent recheck. Backup retention and reporting of simultaneous
  cleanup residues are repaired and pass the integrated Debug/Release suites.
  The independent persistence recheck approved this bounded checkpoint with no
  actionable findings. Session recovery, power-loss and filesystem qualification
  remain open.
- Both native workspace tabs use one document. Wall/opening/slab editing,
  undo, save/reopen and draft PDF have integrated test coverage. Native render,
  picking, panning and portrait-aspect checks pass at DPR 1, 1.5 and 2 in Debug
  and Release. The UI is still an internal construction build.
- The project navigator now derives actual property/building/floor/layer
  relationships and includes all objects. New geometry uses an explicit drawing
  layer. Atomic building/floor/layer creation and rename, unresolved placement
  diagnostics, multi-layer reopen selection, stale caller revisions and
  cross-context slab rejection have integrated Debug and Release coverage. Floor association
  remains organizational: world elevations are unchanged. Level dependency
  bindings remain open; transient visibility is integrated as described below.
- Floor/layer visibility now has a pure derived core and a native presentation
  mask. Focused Debug/Release tests cover hierarchy masks, hosted openings,
  sibling layers, unresolved placement and unchanged snapshots. Actual native
  framebuffer and picking tests pass at DPR 1, 1.5 and 2 in both configurations,
  including hidden invalid geometry and an initially hidden viewport. Navigator
  checkboxes, keyboard selection/Space, both plan canvases, project-switch rules
  and filtered draft PDF are integrated. A hidden floor containing a nonzero
  area leaves full-document totals unchanged. Normal/150-percent captures and
  a rendered PDF were inspected at v16d. Saved-workspace profiles now restore
  presentation preferences and panel proportions; broader acceptance remains
  open.
- Modal authoring retains document identity, revision, selection, drawing layer
  and units. Timer-based tests prove opening/slab/building/organization commands
  reject edits, selection changes, layer changes, unit changes and same-revision
  project replacements that occur while their prompts are open. Organization
  creation also forwards the retained revision to the final document command.
- Constraint validation indexes opening owners once per state and uses ordered
  opening sweeps. Tests cover 1k/2k/4k owners or openings, exact union/gap edge
  cases, future-owner validity, mixed known/unknown history and a real
  64-wall/16-revision save/reopen cycle. These bounded checks do not certify
  whole-application large-project responsiveness.
- Automated native tests run in hidden windows with process-local error
  handling and timeouts. Every CMake test executable receives the same error
  policy automatically; forced failures test that automatic linkage.
  CRT assertions are also routed to stderr and tested in Debug and Release.
  The comprehensive `desktop_workflow` test has a six-minute CTest guard
  because its single hidden process covers the full workspace, output,
  recovery, and architectural-form matrix; focused tests retain the
  60-second guard.
  The temporary probe that raised repeated CRT dialogs
  had invalid fixture metadata and an uncaught exception; its corrected run
  exits successfully, and forced exception/abort tests exit without dialogs.
- Portable JSON/assets extraction uses retained directory identity for
  reservation/publication and preserves foreign files during cleanup. Root
  substitution, protected payloads, foreign additions and destination collision
  regressions pass Debug/Release. Postpublication verification and rollback
  cover Windows' required close-to-rename interval. The precise
  concurrency boundary is documented in `project-extraction.md`; independent
  recheck approved the bounded implementation, including postpublication
  identity verification and preservation of foreign content.
- A versioned output-fingerprint core binds document content and explicit
  profile/font/view/CRS/component/build identities. It rejects incomplete inputs
  and detects stale content without serializing private entity or asset payloads.
  Its parser repairs pass Debug/Release and independent re-review, including
  canonical ordering, signed/unsigned limits and typed UTF-8 errors. The privacy
  guarantee excludes snapshot-derived payloads; caller dependency metadata is
  public and unredacted. Preview, draft PDF, draft SVG, and print share the
  retained PlanCanvas vector renderer and visible draft stamp. PDF, SVG, and
  native 3D image exports now write adjacent fingerprint manifests that bind
  the document head, view filters/page size, linked processing roles, and
  running Windows executable digest. Print preview now writes a local
  driver-evidence receipt with printer identity, DPI and page rectangles;
  persisted sheet layout, physical printer calibration, and production output
  qualification remain open. The desktop output selector now applies Letter, Legal, Tabloid, A4, or
  A3 to draft PDF and print preview.
- New desktop projects now seed a validated `sheet_view_model` entity with a
  coordinated plan/elevation/section views, an A-101 sheet with plan,
  elevation, and section viewports, title block, and independent viewport scale;
  the entity survives normal save/reopen and participates in output-fingerprint
  view inputs. Draft PDF/SVG/print consume the persisted sheet renderer and
  select view-specific derived geometry for each persisted viewport, even when
  the active workspace tab differs, and
  sheet number/title-block plus viewport bounds/scale edits use Document
  history; persisted schedule placements now render revision-bound schedule
  rows in draft output, and placement bounds can be edited through typed
  Document history. Printer receipts now capture driver evidence; physical
  calibration and qualification remain open.
- The architectural projection engine now derives analytical plan/elevation
  hidden-line output and true solid-plane section intersections for the
  supported semantic building objects. Frames are validated before OCCT
  projection, circles remain analytic arcs, and unsupported intersection
  curves or missed section planes fail closed. Focused Debug coverage passes;
  the Architectural tab now switches between transient plan/elevation/section
  canvas views and routes its selected view through draft output; persisted
  cut-depth presentation now persists and is editable from the desktop view
  settings command; annotations/hatching, complete sheet layout, and production
  print integration remain open.
- Release import inspection resolves 37 selected application/component
  binaries on this machine. The Qt dry-run exposed default plugin expansion;
  an explicit current-component allowlist and pinned archive-member hash
  overrides for the stale Qt SPDX entries are recorded. The generated offline
  bundle passes its local verifier; clean-machine installation, complete
  notices, corresponding-source qualification, and offline runtime evidence
  remain open.
- Compatibility and interchange foundations now include a fail-closed Apex
  Standard/Pro/module evidence matrix, declared IFC/DXF/PDF/PROJ worker profiles,
  and deterministic field/DISTO adapter contracts. These artifacts validate
  supplied manifests and attestations only; native Apex parsers, real worker
  binaries, device/application exchanges, runtime isolation, and representative
  fidelity fixtures remain open production work.
- The bounded ASCII DXF R2013 codec now round-trips linear dimensions and
  one-loop solid polygon hatches in addition to lines, arcs, bulged polylines,
  plain text, block definitions, and INSERT references. Unsupported dimension
  styles, patterned or multi-loop hatches, nested blocks, and external CAD
  fidelity still produce diagnostics or remain open; native project mapping and
  production interchange evidence are not implied by this codec. A separate
  native project-mapping layer now translates immutable snapshots to that
  drawing model and reconstructs bounded DXF geometry into editable boundary
  candidates plus a typed annotation entity. It preserves analytical bulges,
  records source layer/primitive metadata, expands block INSERT geometry when
  representable, and marks unbound dimensions and unsupported transport data
  for original-byte retention. Desktop floor/layer assignment, source-asset
  retention, Apex native compatibility, external CAD fidelity, and production
  interchange qualification remain open.
- A bounded IFC4 STEP project mapper now exports deterministic owner/unit
  records, analytical linear boundaries, wall axes, slab footprints, and
  optional slab swept solids from immutable snapshots. Import walks the product
  representation graph and reconstructs editable boundary candidates with
  `extensions.ifc_source` record identities, placement translations, and
  extrusion depth where representable. Unsupported products, properties,
  materials, rotations, and relationships produce explicit source-retention
  diagnostics. IFC worker isolation, desktop transaction/source-asset wiring,
  Reference View conformance, and external interoperability fixtures remain
  open.
- Multipage project semantics, reusable assemblies, and optional offline
  assistance are implemented as standalone typed models plus a deterministic
  local engine. The engine produces unverified tracing, explicit-unit
  dimension, label-placement, and bounded natural-language proposals; the
  desktop dialog previews and accepts them through normal typed Document
  history, including identified-boundary creation and legacy-boundary upgrade.
  The proposal contract, engine tests, desktop workflow tests, and portable
  resource declaration are in place. Accuracy on arbitrary plans, physical
  input and production qualification remain open. A
  Document-derived schedule adapter now generates revision-bound door/window,
  room, and material rows with calculated-cell provenance and explicit
  diagnostics. Editable source cells dispatch through revision-checked
  Document commands, persist with the project, participate in undo/redo, and
  are exposed through the desktop Schedules dialog. Architectural projections
  now append deterministic read-only material-summary rows grouped by catalog
  identity or normalized explicit name, with source provenance, aggregate counts,
  net-volume totals when complete, and visible diagnostics when a source
  quantity is unavailable. Full sheet/save/print/export qualification remains
  open.
- The product scope and interaction qualification contracts now enumerate the
  Windows 11 x64 target, both markets, both workspaces, both unit systems,
  themes, DPI layouts, keyboard/focus/property access, pen/touch controls, and
  the measurement keypad. Production qualification manifests cover the
  residential, light-commercial, and accessibility runs but intentionally ship
  empty; physical input and integrated runtime observations remain required.
- Survey/metes-and-bounds and Pro georeferencing now have deterministic local
  contracts for quadrant-bearing traverses, closure/acreage diagnostics,
  projected-metre affine transforms, residuals, and offline resource
  declarations. They do not execute PROJ, verify resources, import Apex files,
  or replace the required specialist-module fixtures.
- The architectural transaction adapter now applies type-aware transforms to
  canonical building entities and carries hosted wall openings through
  duplicate and delete commands, preserving unrelated metadata and ordinary
  undo/redo. The Windows editor exposes a compact preview dialog for scale,
  rotation, XYZ translation, clone, and apply/cancel history behavior on
  recognized architectural objects. Architectural transaction and output descriptors cover create, select,
  property edit, transform, duplicate/delete intent and coordinated
  plan/elevation/section/3D/schedule issue requirements. Live Document history,
  projection/render/export, and complete residential/light-commercial output
  remain integration work.

## Verification boundary

Individual passing tests do not complete a broad requirement row. The
production audit requires current source fingerprints and acceptance artifact
hashes, in addition to a verified status. Run:

```powershell
python scripts/requirement_audit.py
python scripts/requirement_audit.py --release
```

The second command must fail until the complete application, external
compatibility evidence and all quality gates pass. Missing required Apex
files, edition/module behavior, caller protocols and physical device evidence
remain visible in the ledger. No workaround, limited importer or internal
build converts those requirements into completed parity.

## Next integration checks

1. Complete independent rechecks of boundary/history repairs and shared
   document-digest extraction before recording the next source checkpoint.
   The 36-test v21 Debug/Release runs cover the expanded document and constraint
   checks. The last UI capture pass covered the actual hierarchy, full context
   label and quantity-entry defaults at 1366x768, 1920x1080 and 150 percent
   scaling; the current native rendering checks pass at 100, 150 and 200 percent.
   The resumed independent constraint/visibility review found nested receipt
   metadata loss and stale cached output after direct document mutation. The
   receipt and output repairs pass full v18 tests. Every output request reads
   current geometry, including same-ID/same-revision alternate heads and
   changes after a print preview opened. The fresh source-bound repair review
   approved the bounded constraint/visibility package and test-only addendum.
   Boundary/dimension/storage review found retained-identity resurrection and
   numeric representation gaps. The repairs add a cached retained-history
   identity ledger, ordered endpoint ownership with exact typed reversal, and
   strict canonical JSON navigation equality. All 36 integrated tests pass in
   Debug (55.88 seconds) and Release (15.33 seconds), including properties,
   extensions and asset numeric representations through undo/redo/naming,
   square/lens edge swaps, abandoned upgrades, retired child identities,
   valid coordinate edits/reversal, split with atomic dimension remapping,
   forged history and dimension-only nonconforming v1 rejection. Eight retained
   v2 files open with the v21 reader and reject with the retained v16 reader;
   all fixture hashes remain unchanged. Independent repair review is pending.
   The shared document-digest extraction and map-key identity checks received
   independent approval. Follow-up boundary review closed the v1 findings and
   found a raw legacy-upgrade marker bypass. The v21 guard requires exact
   helper-equivalent upgrades, rejecting vendor-field collisions, geometry
   changes and metadata loss. The visibility fixture now upgrades first and
   attaches its opaque auxiliary metadata as a separate edit.
   Follow-up lineage work observes anonymous legacy IDs while retaining v1
   legacy-only reuse compatibility. Ordinary reuse/retyping makes a legacy ID
   ineligible for later same-ID identification, including after an abandoned
   branch; exact undo after deletion alone remains eligible. Fresh-ID rekeying
   preserves the original history. Focused Debug and Release suites pass 4/4.
   All 43 saved fixtures retain their hashes: both readers open three legacy-only
   v1 histories; only the current reader opens forty protected-format histories.
   Independent review approved this bounded lineage policy. Whole-integration
   verification of the additional policy remains pending.
   Universal nonreuse for unrelated generic IDs before any legacy/protected
   observation remains separate typed-command/format policy work.
   All 136 recorded v18 build
   inputs matched before and after the builds; 68 executables were hashed.
   These are local incremental builds, not reproducible-build qualification.
   A test-only addendum also verifies queued navigator events cannot affect a
   replacement project, and partial native masks preserve visible-wall picking,
   exclude hidden-wall picking and restore the original framebuffer/camera.
   Both checks pass Debug/Release, with native coverage at DPR 1, 1.5 and 2.
   The expanded combined native Debug test exceeded its 15-second guard at
   DPR 2. Timing isolated substantial image-oracle scan overhead; normalized
   ARGB32 row scanning preserves the original image and RGB assertions.
   Geometry and building-form checks now run in separate hidden processes,
   each retaining the same 15-second limit. The runner defaults to both
   scenarios at all three scales and rejects empty selections. Timings are
   local test observations, not application performance qualification.
2. Continue the remaining production packages in `production-plan.md`,
   including full architectural objects/constraints, Apex workflows/adapters,
   local assistance, recovery, output, packaging and certification.

No remote repository is configured. Source publication, a product release and
deployment have not been performed.
