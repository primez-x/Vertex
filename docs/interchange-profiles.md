# Local interchange capability profiles

`InterchangeProfile` is a portable declaration and readiness validator for
COMP-IO-001/002/003 and IO-IFC-001, IO-DXF-001, IO-PDF-001. It does not implement
an IFC/DXF/PDF parser or exporter. The separate Windows georeferencing runtime
preflight now opens the pinned PROJ library, verifies declared local resources,
binds `proj.db`, disables networking, and validates the declared CRS and an
identity operation; the profile remains a declaration and does not replace
that runtime evidence.
The baseline profiles intentionally fail readiness until a reviewed component,
version, license review, exact module inventory, and real runtime evidence are supplied.
Adapter IDs are proposed local worker identities, not registered implementations.

| Profile | Declared target | Bounded capability declaration |
| --- | --- | --- |
| `local.ifc.worker` | IFC4 ADD2 TC1 Reference View 1.2 | Geometry, types, properties, materials, relationships, reference preservation, fidelity report |
| `local.dxf.worker` | DXF R2013 | LINE, ARC, LWPOLYLINE, POLYLINE, TEXT, MTEXT, DIMENSION, HATCH, BLOCK, INSERT, fidelity report |
| `local.qt-pdf.worker` | PDF calibrated reference and vector scene output | Calibration, provenance, traceable/image-only classification, vector scene output, fidelity report |
| `local.proj.worker` | PROJ bundled-resource coordinate operations | Local resources only; networking and remote callbacks disabled; `proj.db` mandatory |

These are target subsets, not conformance or round-trip claims. IFC editable
reconstruction requires a future adapter to prove reliability per entity and
preserve identifiable unreconstructed content. PDF declares no editable text or
geometry extraction. Existing reference-asset calibration remains a separate
contract. The PDF allowlist is exactly Qt6Core, Qt6Gui, Qt6Pdf, Qt6PrintSupport;
the real package must audit these modules and their transitive dependencies.
Other formats require an explicit reviewed module inventory. Module/resource
names are logical IDs, never filesystem paths. A broker maps them to immutable
local files and verifies their integrity; this API does not open files. The
PROJ preflight is the only current runtime resource verifier and is invoked
explicitly by the georeferencing workflow.

Unsupported entities either reject the operation or preserve an identifiable
reference with a fidelity report. Silent omission is not an available policy.
PROJ rejects unsupported operations instead of falling back to another coordinate
operation. Default IFC/DXF/PDF declarations request reference preservation.
Actual per-entity preservation, report creation, geometry conversion and native
project transaction isolation remain adapter responsibilities.

Validation binds adapter ID/version and license review ID to the supplied
attestation, requires a matching loaded module inventory, all required local
resources, offline build evidence, network denial, worker isolation, enforced
limits, and proof that worker failure preserves the project. PROJ additionally
requires disabled networking and callbacks. Baseline resource limits cap input
at 64 MiB, output at 256 MiB and execution at 30 seconds; smaller positive bounds
are accepted. Use `ImportWorkerPolicy` separately for expanded-input, memory,
process, path and launch controls. A successful profile decision does not replace
those checks, authenticate attestation evidence, or launch a worker.

JSON schema version 1 sorts unordered declaration lists and diagnostic codes.
The validator rejects duplicate list entries, invalid enum values, unknown target
or capability declarations, malformed IDs and incomplete evidence. Diagnostics
contain stable local codes without echoing untrusted input. The descriptive
manifest JSON retains caller-supplied metadata and must only be published after
review. There is no deserializer or untrusted JSON ingestion in this boundary.

Remaining production gates: reviewed pinned adapter binaries and license
artifacts; broker integration and runtime evidence tied to exact binaries and
resources; representative IFC/DXF fixtures and fidelity reports; shared vector
scene PDF/print output; bundled PROJ operation tests with an external network
monitor; and measured worker failure recovery. The local PROJ preflight test
does not prove the full import-worker sandbox or external network observation.
Profile unit tests exercise only the portable declaration and fail-closed
readiness decision, using synthetic attestations.

## Native bounded DXF codec

`sketch/dxf_exchange.hpp` supplies an independent, in-memory ASCII DXF R2013
(`AC1027`) codec. It neither opens paths nor launches processes, loads fonts,
resolves references, contacts a service, or mutates a project. It is a smaller
implemented subset than the declared `local.dxf.worker` target above; it does
not satisfy that worker's reference-preservation or runtime-attestation gates.

The transport records preserve 2D LINE endpoints, CIRCLE center/radius, ARC center/radius and
counterclockwise start/end angles, LWPOLYLINE vertices with signed bulges and
closure, plain TEXT insertion point/height/rotation/string, linear DIMENSION
extension and text points, and one-loop solid polygon HATCH boundaries. Layers
and R2013 `$INSUNITS` values 0 through 20 are retained without unit conversion.
BLOCK definitions contain lines, arcs, circles, open/closed bulged polylines, and plain
text; INSERT records retain the block name, insertion point, independent X/Y
scale, rotation, and layer. Block names are unique and every insert must name
a definition in the same file.
The HATCH subset requires one closed polygon path without bulges. Group 92
must include the polyline bit: imports accept `2` (polyline) or `3` (external
polyline), and export canonicalizes the sole boundary to `3`. The prior
codec's value `1` declares an external non-polyline edge path and is now
diagnosed rather than interpreted as polygon vertices. Other path flags and
edge-list boundaries remain unsupported. See Autodesk's
[boundary path group codes](https://help.autodesk.com/cloudhelp/2016/ENU/AutoCAD-DXF/files/GUID-DC5215D6-E73F-4DFF-8BE9-01CA9610FAEE.htm).
Arcs require distinct angles in `[0, 360)` and a positive radius. Polylines
require at least two vertices; bulges remain analytical values and are never
tessellated. Labels use baseline/left alignment with default width and no
oblique angle, mirroring, custom style, or DXF formatting escapes. TEXT
whitespace is preserved. These group-code mappings follow the
[Autodesk DXF reference](https://images.autodesk.com/adsk/files/autocad_2013_pdf_dxf_reference_enu.pdf).

Malformed input throws `std::invalid_argument` with a stable message and returns
no partial result. Nonfinite/oversized numbers, duplicate required singleton
fields, missing coordinates, inconsistent vertex counts, wrong/missing version,
unclosed sections, missing EOF, and trailing records are rejected. The parser
accepts LF and CRLF and requires printable ASCII values; Unicode, binary DXF,
and encoded text controls are outside this subset. Coordinates, radii, angles,
and bulges have absolute numeric magnitude capped at `1e12`.

`DxfImportResult::diagnostics` identifies unsupported entities by one-based
ENTITIES ordinal, entity type, and a stable code. POLYLINE, MTEXT,
ELLIPSE and every other unimplemented type are reported as `unsupported_entity`.
Unsupported dimension types, patterned or multi-loop hatches, nested blocks,
attributes, unsupported block content, 3D coordinates, nondefault OCS,
paper-space entities, widths, and styled text omit the whole affected entity
with `unsupported_feature`. Unknown sections and header variables produce
section-level diagnostics. Raw unsupported records are **not** preserved;
callers must retain the original input if preservation is required and must
surface the diagnostics before using partial geometry. Handles, ownership IDs,
subclass markers, and polyline vertex IDs are transport metadata and are not
retained. No native Apex compatibility or full DXF fidelity is claimed.

Both import and export enforce caller-reducible hard ceilings: 16 MiB, 500,000
group-code pairs, 50,000 entities, 100,000 aggregate polyline vertices, and
255 bytes per value. Export uses locale-independent round-trip numeric precision,
normalizes negative zero, emits LF, and groups entities as lines, arcs, circles,
polylines, dimensions, hatches, labels and INSERTs while retaining each vector's order. Repeated export and
export/import/export are byte-stable within this subset. Export rejects invalid
records or strings instead of emitting injected group codes.

`dxf_exchange_tests` covers this round trip, CRLF input, bulges, wrapped arcs,
text whitespace, independent HATCH path-flag fixtures, unsupported-feature diagnostics, malformed input, and both
input/output resource limits. This is synthetic codec evidence; external CAD
application interoperability, desktop import/export, transactional project
mapping, provenance, and retained-source handling remain separate integration
work.

## Native project mapping

### Phase authoring transfer (V9, source integration; qualification incomplete)

V1-V8 phase observations remain private source evidence. Complete editable
phase transfer requires a separate V9 authoring inventory and PSIP0004 worker
contract: all baseline, demolition and inactive proposal owners must accompany
the registries, independently of the active CAD depiction. Full catalogs,
wall joins, terrain, actual reviewed hierarchy members and typed host/level
relationships participate in that closure. Bodies, catalogs, registries,
reviewed contexts and stair children require separate owner mappings.

The shared ModelPhases source codec now preserves surviving saved array order
for ordinary edits and provides complete injective owner remapping. It patches
only the roster, baseline and each alternative's demolition/proposal owner
lists; alternative identities, names and active selection remain local. It does
not supply transport authentication or destination ownership. A separate source
graph codec now captures and validates complete touched registries, raw owners,
catalogs, hosts, wall joins, terrain and context dependencies. Explicit role
inventories distinguish complete retained bodies from their active depiction.
PSIP0004 carries this inventory as bounded source evidence. Conservative ambient
source admission can reject large otherwise valid graphs, and existing catalog
helpers still refuse unsupported canonical owner references. Neither source
inventory nor wire admission authenticates CAD depiction or grants live ownership.
The mapper now emits a V9 authoring carrier whenever actual phase registries
exist. One bounded chunked raw graph carries the complete authoring inventory;
separate active CAD body blocks authenticate native geometry, hierarchy, layer
and identity placement. Empty active depiction is valid. Import rejects missing,
duplicate, orphaned or malformed carrier records and overlapping legacy source
ownership. PSIP0004 repeats the ownership checks at the worker trust boundary.

Destination mapping requires complete, injective body/catalog/registry/context
maps and separate stair-child identities. Typed owner references change while
local alternative, level, catalog and terrain names remain local. Existing
context owners must be actual destination records with equivalent mapped source
semantics; they cannot be overwritten by retained evidence. An explicit import
defaults to creating source hierarchy copies. The interactive review can instead
choose existing equivalent contexts; no existing hierarchy merge is inferred.
The pure binder receives those caller-authorized new contexts separately from
the retained source graph, validates the combined actual destination, and returns
only proposed new owners. Desktop publication combines them, legacy drawing
items and the retained original DXF in one command. The initial selection,
active layer and design-set target follow one imported object's actual ownership.
An unregistered selected object uses the first imported design set as an explicit
editing-target fallback, without enrolling that object.

The desktop authoring target now selects among existing phase registries, with
each registry retaining its saved active alternative. Enrollment, deletion,
hosted ownership, annotation filtering and automatic wall measurements evaluate
the actual registry graph. V1-V8 retain their prior contracts and are not
retrospectively treated as complete phase authoring inventories. V9 still
refuses unsupported incoming annotation/constraint dependencies and canonical
catalog references. Whole-history admission has explicit finite capacity;
otherwise valid large projects may refuse. This increment has source inspection
only. Compilation, runtime round trips, history/storage replay, usability,
capacity and independent CAD interoperability remain unqualified.

### Complete material catalogs (V8, reviewed source integration)

V8 adds complete original material/assembly catalogs to supported editable
wall/opening cohorts, including material-bearing walls without a retained room.
One bounded operation-wide table retains unused definitions, embedded instances,
raw model dialect/order/numeric forms, metadata, extensions and required flags.
Proofs reference catalog IDs; a separate exact subset identifies live catalog
owners. Observer-only catalogs cannot become live authoring by inference.

Catalog consumers and all embedded hosts share a dependency cohort. Desktop
review allocates catalog owners once, rejects conflicting context assignments,
and stages bodies, catalogs, hosts and root/layer material references together
against the actual project before the existing atomic import command. Private
source admission uses authentic source evidence; it grants no destination
hierarchy authority. PSIP0003 carries the new tables and complete entity flags;
PSIP0001/2 keep their existing schemas.

Additional V8 source integration covers catalog-bearing slabs, roofs, stairs,
railings, columns, beams, room volumes and roof joins, plus independent assembly
roots. It regenerates analytical plans from real authored objects, preserves
typed host/join relationships and distinguishes stair child identities from
body/catalog identities. Connected stairs require a real destination floor
binding with matching level graph semantics. Full phase authoring and changed
local levels remain explicit gaps. Integrated root and independent source review
approved this increment and its required corrections. It has not been compiled
or exercised. External-consumer fidelity, capacity and
production qualification remain open; no installed candidate has changed.

### Physical-room groups (2026-10-10, source implementation)

V7 source work adds retained physical-room boundaries with complete active
source walls, hosted openings, analytical voids and supported mixed appraisal
and measured-source dependencies. Shared bounded metadata carriers retain the
actual original hierarchy and referenced vertical graphs once. A result-level
proof table crosses the isolated worker boundary; no repeated full snapshot is
embedded in each entity. Phase observations are retained as evidence, rather
than silently assigned as destination authoring state.

Actual staged destination hierarchy and all applicable existing/imported walls
participate in admission. Completed V7 transfer-only markers are removed before
live publication; raw DXF assets and opaque source provenance remain. Save,
copy and later export derive from the current Document. Full phase/material/
assembly authoring exchange and external-consumer fidelity remain open.

Integrated root and independent source review approved this scope. Cumulative
admission includes repeated level/phase replay, hosted-opening/layer solid
construction, retained history and proof object-key limits. No compilation,
test, probe, launch, package or installation has run; the installed candidate
is unchanged. This section records the source contract, not accepted fidelity.

### Standalone boundary records and project unit authority (2026-10-09)

The subsequent source implementation adds a version-2 JSON envelope for closed
standalone `boundary`, `measurement_boundary` and `room_boundary` entities.
It uses the existing bounded `VERTEX_ENTITY_V1` XDATA carrier and requires the
explicit depiction `BOUNDARY_PLAN_V1`; version-1 wall/opening envelopes remain
unchanged. Ordinary analytical outer/hole polylines remain in the block for CAD
consumers. Native properties, classifications, local edge/vertex identities and
inline hole ownership can reconstruct together with a fresh entity owner.

Activation requires metre units, identity INSERT/base placement, complete valid
outer/hole geometry, an editable detached document, and exact agreement with
the entire independently regenerated block. Every hole participates in that
proof. Known construction and geometry-edit receipts remap only their owner;
their input expressions, local IDs, numeric JSON, schemas and transform history
remain exact. The isolated response validates the explicit native marker,
bounds analytical pair/segment work, and revalidates the full document. Desktop
import handles all three boundary types and assigns reviewed destinations in
the existing atomic import transaction. Legacy layer names cannot override the
chosen destination on later export.

This record transports one standalone boundary. Live wall/measured-line sources,
active deduction links and ANSI ceiling room/stair/deduction references require
their dependent graphs and are not promoted by this carrier. Generic document
reference validation alone does not cover those appraisal links. Mapper and
broker explicitly reject them; unsupported graphs, unknown active schemas,
oversized metadata and geometry mismatches retain ordinary curves with fidelity
diagnostics and original-source retention. Physical source-bound rooms retain
their existing checked-output path. These remain explicit compatibility gaps,
not a lossless whole-project DXF certification.

### Linked appraisal boundary records (2026-10-09, source implementation)

The version-3 envelope extends `BOUNDARY_PLAN_V1` with an explicit
`dependency_graph` and one identical sorted `member_ids` list for a complete
connected appraisal boundary group. Each member retains its own bounded JSON
and ordinary analytical outer/hole plan. Incoming links participate in group
discovery; a deduction cannot export separately from a referencing area. The
original version-1 wall/opening and version-2 standalone contracts remain.

Native activation requires every declared member exactly once, matching group
declarations, complete typed references, an acyclic deduction graph, editable
native geometry, and exact regenerated plans for every member. Boundary and
receipt ownership and consumer-owned deduction/ceiling references remap through
typed visitors; local topology and arbitrary source JSON remain untouched.
The isolated response revalidates group membership and all bounded geometry.
Desktop CAD-layer review must assign every linked area/deduction to the same
floor before one atomic import command can publish the group.

Recoverable identities and links are inventoried before full native admission;
malformed duplicates and inconsistent incoming declarations cannot leave other
group members eligible. Source deductions use the calculation engine's actual
inclusive containment and union semantics, permitting edge-sharing and
full-parent deductions. Segment and operation budgets precede solid operations.
Actual inline holes keep their strict topology rules. Core-only builds without
the containment engine retain deduction groups as ordinary geometry fallback.

Copied sloped-ceiling observations require reconfirmation: the active room
anchor is cleared and `complete_room_observed` becomes false. Original facts
and hashes remain source evidence; import never manufactures a new confirmation
or reporting digest. `source_confirmation_required` makes this disposition
explicit. Existing source evidence must be consistent before native activation.

Live wall/measured-line graphs, physical source-bound rooms and real
`stair_from_floor_id` references still require additional source/floor transport.
They retain ordinary geometry and original-source evidence with diagnostics.
Partial, duplicate or inconsistent groups likewise activate no native members.
This is source implementation, pending qualification;
it does not certify whole-project DXF fidelity or external consumer behavior.

### Stair footprint floor binding (2026-10-09, source implementation)

The version-4 envelope retains the version-3 group fields and adds transport
for the appraisal stair footprint's self-floor relationship. Source admission
requires `boundary_role: stair_footprint`, `ceiling.kind: stairs` and a nonempty
`stair_from_floor_id` equal to the area's owning `floor_id`. A group containing
such a member uses version 4 throughout; an otherwise standalone stair uses a
singleton group. Missing or mismatched source floors cannot be repaired by
import. This does not introduce arbitrary cross-floor stair dependencies.

The canonical source dependency graph retains the original floor reference.
Detached candidates clear the active reference and carry exactly
`vertex_dxf_stair_floor_binding: {version: 1, source_floor_id: <source floor>,
destination_floor_id: null}`. The isolated response requires detached
organization and the pending state. After desktop destination review assigns
the actual floor and layer, a typed binder sets the active reference and
`destination_floor_id` to that floor. Complete final group validation precedes
the atomic import. Floor IDs never pass through the boundary-owner identity
map, and no observation hash or reporting confirmation is refreshed. Re-export
rebuilds the source declaration from the current owning floor.

V1/V2/V3 contracts remain unchanged. Live wall/measured-line graphs and physical
source-bound rooms still require complete transport; source-only remapping
helpers do not close that exchange gap. V4 is uncompiled and runtime unverified;
external round trips and production acceptance remain open.

### Exterior wall-source groups (2026-10-09, source implementation)

Version 5 transports connected exterior measurement boundaries, their source
walls, hosted openings, deductions and ceiling dependencies as one complete
group. Every member declares identical sorted membership and an exact typed
dependency graph. Walls also declare their complete hosted-opening inventory.
Source and detached native plans must exactly match the analytical DXF drawing;
the source walls must reproduce the retained measured boundary before activation.
Missing, stale, duplicate, overlapping or unsupported dependencies prevent native
activation of the entire component and retain ordinary geometry with diagnostics.
Geometry and source/containment work use a cumulative operation allowance across
independent components; expensive failed proofs remain charged. A component
exceeding the remaining allowance falls back before source replay.

Detached members carry a pending direct-context binding. Desktop destination
review obtains the actual property/building/floor/layer context from the real
hierarchy, then binds the group atomically and rechecks measurement currentness.
Only declared context and entity identities change: captured baselines,
thickness, kernels, origin outlines, translation history and local topology
remain intact. Absolute vertical placement is retained; level, material and
assembly dependencies are not yet transported. Source phases are diagnosed and
retained as evidence rather than silently bound to destination phases. Older
source provenance and appraisal observation/report hashes are not refreshed.

V1/V2/V3/V4 contracts remain unchanged. Complete measured-line and physical-room
source graphs remain open, including mixed components that require them. V5 is
uncompiled and runtime unverified; external consumer fidelity and production
acceptance remain open.

### Measured-line source groups (2026-10-09, source implementation)

V6 adds receipt-backed measured strokes with `LINEWORK_PLAN_V1`, including
standalone open strokes, and complete source-derived area groups. The wire adds
each member's resolved source context and both retained source owners and full
applicable measured-graph inventory. Unmarked areas retain the complete active
unisolated layer graph; isolated copies retain their referenced-owner cohort.
Import does not introduce an isolation marker merely to force admission.

Raw models retain authored measurements, local topology, receipts and ordered
history. Typed fresh-owner and source-reference remapping changes ownership
without re-encoding those records. Pending direct and resolved context bindings
remain separate from reviewed actual hierarchy destinations. Complete graph
inventory, source currentness, exact analytical plans and cumulative replay/work
preflight are required before native activation. Existing destination strokes
must not silently change an imported unmarked area graph.

Original inventory is checked across components before remapping, including
recoverable rejected stroke carriers. Dependency admission avoids topology
decoding; strict proof follows cumulative work limits. Analytical fallback
shares the export replay/topology allowance. Existing destination stroke
observations participate only in the final actual-context inventory proof.

V1/V2/V3/V4/V5 contracts remain unchanged. Integrated and independent source
review approved the corrected scope; compilation/runtime and external consumer
behavior are unverified.
Physical-room and level/material/assembly graph transport remain open.

IFC now uses one actual project's linked length-unit assignment for every core
editable reconstruction, including legacy axes and swept solids. Orphan metre
declarations cannot authorize a wall, slab or opening. Recognized nonlength
assignment members do not invalidate a proved metre length unit. Missing,
ambiguous or unsupported length authority yields inert `ifc_reference` products,
including products with no Vertex metadata; original arguments, metadata and
the complete source bytes remain available. No unscaled coordinates are
published as editable native metres. The separate CAD library's proved unit
conversion policy is unchanged.
Unit form/role checks follow the IFC4 ADD2 TC1
[named unit layout](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcmeasureresource/lexical/ifcnamedunit.htm),
[named unit enumeration](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcmeasureresource/lexical/ifcunitenum.htm)
and distinct
[derived unit enumeration](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcmeasureresource/lexical/ifcderivedunitenum.htm).

These changes are source-only and uncompiled. Historical checks that require
top-level boundary polylines or an editable orphan-unit IFC axis need adjustment
when qualification resumes; they have not been changed or run. External CAD/IFC
round trips, complete graph fidelity and production acceptance remain open.

### Source fidelity corrections (2026-10-09)

Ordinary native boundaries with inline holes now export every supported hole
loop as analytical DXF curves, using the same helper as slab footprints. The
loops import as independent candidates; native outer/hole ownership remains
unrepresented and is explicitly diagnosed. Malformed hole collections report
their loss instead of silently disappearing. Export also reports lost stable
edge/vertex topology, native boundary types and area classifications. A CAD
primitive classification already reproduced by the selected output primitive
does not receive a false classification-loss report.

A DXF polyline shares one coordinate at each joined vertex. Export now requires
exact authored joins before combining edges: distinct adjacent endpoints remain
independent primitives with a connection-loss diagnostic. A merely
tolerance-close final endpoint remains open with its original coordinate;
export does not silently snap it to the first point. Full-turn or otherwise
unsupported sweeps remain explicit representability failures. Near-full turns
remain supported when their finite bulge fits the transport ceiling. Current
source-bound physical rooms use the same semantics-loss reporter and diagnose
hole association for their checked inner loops.

Native IFC mesh activation for wall, void and fill carriers now requires the
same actual linked-project metre proof as roofs/rooms/stairs. An orphan metre
declaration cannot activate those native carriers. Every actual void relation
participates in host ambiguity, including a slab or other parent outside the
editable wall subset. A proved curved legacy void now follows the same retained
door/window classification rule as a proved swept void. Manufactured assembly
and operation metadata still require the separate fill proof. The earlier
legacy swept/axis unit policy is unchanged by this bounded correction.

These are source changes only. Compilation, runtime round trips, geometry and
external consumers remain unverified. The existing upgraded-circle export
expectation at `tests/dxf_project_exchange_tests.cpp:122` requires reconciliation
with the new truthful topology/classification diagnostics when qualification
resumes; it was neither changed nor run here. Lossless native boundary-hole
reconstruction and full Reference View conformance are not established.

`sketch/dxf_project_exchange.hpp` adds the first transactional-project mapping
layer on top of the transport codec. `export_project_dxf` reads one immutable
`DocumentSnapshot` and maps identified or legacy boundaries, wall baselines,
hosted-opening jamb/threshold markers, slab footprints/holes, native labels,
symbols, and resolvable boundary dimensions into the shared `DxfDrawing` model.
SI metres are declared with
`$INSUNITS = 6`; layer names are resolved through the native layer graph when
available. Curved boundary edges stay analytical bulges, while positive
single arcs on wall baselines stay ARC records. Wall envelopes and slab
thickness/elevation/kind data have no DXF representation and are emitted as
fidelity diagnostics. Hosted openings are emitted on an `Openings` layer (or
the explicitly assigned layer) as three plan markers; the host relationship,
vertical dimensions, and opening assembly are retained only in the native
project and are reported in the fidelity diagnostics. The function never
mutates the source document or opens a path.

Only segment-length dimensions map to DXF's bounded linear `DIMENSION` record.
Angle and area dimensions remain native-only and produce an explicit
`dimension_semantics_not_representable` diagnostic; they are never flattened
into a misleading linear measurement.

`import_project_dxf` parses the bounded drawing and returns unparented editable
boundary candidates plus one typed annotation entity for labels. Lines, arcs,
circles, polylines, solid hatch loops, and block INSERT geometry are reconstructed with
stable import-local IDs and an inspectable `extensions.dxf_source` record.
Dimensions retain their extension geometry and displayed text as annotation
content, but are explicitly diagnosed as `dimension_associativity_unbound`
until a user selects a native boundary segment. The extension candidate also
retains the source dimension-line point, text anchor, rotation, and link to
the reconstructed annotation. Nonuniform block scaling of
curved geometry is diagnosed rather than flattened; malformed INSERT
references with missing block definitions are rejected before mapping. The result sets
`source_retention_required` whenever either the transport parser or project
mapper reports a limitation, so a desktop adapter can retain the original DXF
bytes alongside the editable candidates.

CIRCLE follows [Autodesk's entity contract](https://help.autodesk.com/cloudhelp/2018/ENU/AutoCAD-DXF/files/GUID-8663262B-222C-414D-B133-4A8506A27C18.htm):
center coordinates, positive radius and a default planar object coordinate
system. Nonzero thickness, nonplanar centers and nondefault normals are diagnosed
as unsupported. A native circle boundary contains two exact semicircles with
shared opposite endpoints; area and perimeter remain analytical. Uniform
INSERT scale, reflection, rotation and block base points are honored. A
nonuniform circle scale requires an ellipse and produces
`nonuniform_circle_scale`. A numerically unrepresentable circle produces
`circle_geometry_not_representable` and requires source retention. Imported
boundaries follow the existing anonymous geometry contract; **Upgrade boundary
editing** adds stable topology for typed edge/vertex edits. Native export can
represent the circle as a closed two-bulge LWPOLYLINE without changing its curve.

The desktop library normalizer also retains ASCII/binary circles. Foreign block
copies must satisfy exact XY uniformity and the original circle's default
normal, zero elevation and finite positive radius before being admitted. A
valid reflected copy is converted to an equivalent default-normal world circle.
Library tolerance or transform repair cannot make unsupported source geometry
editable; these cases retain `dxf_insert_transform_not_mapped` diagnostics.
General nonuniform transforms may instead report an unmapped ELLIPSE. Native
metadata continues through the original strict representation.

Project mapping also preflights expanded INSERT work before creating candidates.
`max_entities` caps candidate records and annotation children, including their
shared owner; `max_vertices` caps analytical geometry work (line/arc: one,
circle: two, polyline/hatch: source vertex count, label: one anchor, dimension:
four anchors). Native metadata activation reserves an additional record and
geometry unit. Checked accumulation covers direct and repeated block content;
excess work throws `dxf_project_expansion_limit_exceeded` without a partial
candidate set. Conservative accounting can refuse content that a later
unsupported-feature fallback would otherwise simplify.

Annotation export resolves each label/symbol's assigned native layer name.
Unassigned children keep Annotations/Symbols fallback layers. A missing or
invalid layer reference is reported against the child with
`layer_reference_missing` and layer 0; an unrepresentable name uses the existing
`layer_not_representable` diagnostic. Labels do not revert to a generic layer
after a reviewed import.

This mapping is a deterministic native-project slice, not Apex native-file
compatibility or full CAD fidelity. The desktop transaction adapter retains the
original source asset and commits the import atomically. The actual Import DXF
action reviews source-layer destinations before insertion; the direct
programmatic API retains its active-layer default. Clean-machine and external
consumer interoperability evidence remain production-gate work.

## Native project mapping for IFC

`sketch/ifc_project_exchange.hpp` adds a bounded IFC4 STEP mapper on top of the
native document model. Export emits an IFC4 envelope with deterministic project,
site, building, storey, containment, owner, unit, placement, polyline, typed
wall, hosted `IFCOPENINGELEMENT`, slab-footprint, and optional swept-solid
records. Linear analytical boundaries remain
polylines; a closed slab or straight hosted opening with explicit depth becomes
an `IFCEXTRUDEDAREASOLID`. Straight hosted openings also receive an
`IFCRELVOIDSELEMENT` relationship to their exported wall when both products
are representable; wall elevation plus opening sill are retained in a local
placement. Wall occurrences receive an `IFCWALLTYPE`; homogeneous materials
use `IFCMATERIAL`, and layered constructions use an ordered
`IFCMATERIALLAYERSET`. The exporter explicitly reports
`wall_layer_placement_not_exported` because the layer set does not yet include
an occurrence-relative usage axis. In core-only builds curves and opening assembly parts remain unavailable;
slab holes, unsupported architectural entities, and other spatial relationships are
diagnosed instead of silently flattened.

Import accepts the same IFC4 STEP subset and walks product representation
references to reconstruct typed straight walls, closed slabs, and rectangular
hosted openings when their geometry and dimensions are reliable; other
products remain editable boundary candidates for walls, roofs, spaces,
openings, and proxies. Exported walls, slabs, and openings carry a bounded
`Pset_VertexExchange_v1` property payload so native dimensions, slab kind,
opening kind, and other inspectable values can round-trip without pretending
they are standardized IFC semantics. Candidate entities retain the IFC record
ID/type and original arguments in `extensions.ifc_source`, while the decoded
property payload is retained in `extensions.ifc_vertex_properties`. Translation-only
placements preserve elevations and hosted openings recover host-relative sill,
offset, and width when the void relationship is unique. Rotated placements,
non-metre units, foreign compound representations, materials, and unsupported
`IFCREL*` relationships remain explicit fidelity diagnostics. Architecture-enabled
native meshes and validated opening fills are described below. Required
native semantics that cannot be represented faithfully are emitted as an
`IFCBUILDINGELEMENTPROXY` and reconstruct as an inert `ifc_reference` carrying
the complete bounded native payload. The desktop accepts that reference-only
record without assigning it to a drawing layer, retains the exact source bytes,
and preserves both through save/reopen and re-export. Desktop parsing runs in
the AppContainer import worker and publishes all mapped objects plus the source
receipt as one undoable document command. `source_retention_required` tells the
caller when those original bytes remain necessary. Reference View conformance,
external-application certification, and complete standardized semantic mapping
remain open production gates.


### Hosted graph exchange

DXF R2013 native wall/opening blocks register `VERTEX_ENTITY_V1` in the APPID
table and carry bounded version-1 JSON XDATA (16 KiB per entity; UTF-8 chunks
at most 255 bytes). Editable reconstruction requires metre units, identity
INSERT, a complete host graph, and agreement with every supported primitive.
Unsupported block contents, geometry drift, bad profiles, incomplete relationships
or transformed inserts fall back to inert plan geometry with source retention.
Desktop import remaps identities and assigns the reviewed floor/layer
destinations atomically, then validates the newly imported wall and manufactured
assembly solids. A hosted opening and its wall must share the destination floor.
Source names default to matching layers on the active floor or new layers
created in the same transaction. Geometry coordinates and elevations remain
unchanged. The receipt and adjacent fidelity report record the mapping.
Imported label/dimension children retain their effective CAD-layer names for
destination assignment. In flattened INSERTs, layer-zero children inherit the
insertion layer and explicit nonzero child layers remain distinct, following
the [Autodesk block property rules](https://help.autodesk.com/cloudhelp/2026/ENU/AutoCAD-Core/files/GUID-25E9F20C-D146-426C-8815-37DF48D2D33F.htm).
This is 2D plan exchange. Manufactured frame/panel/glazing sections and actual
leaf swing arcs use a versioned depiction marker; exact regeneration is required
before native metadata activates. Older approximate depictions remain inert.

With the architectural engine enabled, IFC4 exports native wall/void/fill meshes
and `IfcRelFillsElement` relationships for door/window products. Import verifies
the matching void/host graph, native profile and regenerated meshes, including
representation context and placement. Mesh limits and 1 mm meshing deviation
remain bounded; differently tessellated foreign products are preserved with
fidelity diagnostics instead of activating trusted native metadata. Builds without
the architectural engine report the unavailable native mesh capability.
Full Reference View and third-party application certification remain open.

Door/window fills now use explicit proper Z-up opening-local frames, including
oblique straight and circular hosts. Import composes bounded parent placements
and checks canonical handing frames against regenerated world geometry. Curved
OverallWidth follows the opening body's local-X envelope. Closed leaves without
specified handing use a user-defined operation description. Qualification must
still verify standardized operation semantics in external applications; this
subset does not claim full interoperability or Reference View conformance.
