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

The transport records preserve 2D LINE endpoints, ARC center/radius and
counterclockwise start/end angles, LWPOLYLINE vertices with signed bulges and
closure, plain TEXT insertion point/height/rotation/string, linear DIMENSION
extension and text points, and one-loop solid polygon HATCH boundaries. Layers
and R2013 `$INSUNITS` values 0 through 20 are retained without unit conversion.
BLOCK definitions contain lines, arcs, open/closed bulged polylines, and plain
text; INSERT records retain the block name, insertion point, independent X/Y
scale, rotation, and layer. Block names are unique and every insert must name
a definition in the same file.
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
ENTITIES ordinal, entity type, and a stable code. POLYLINE, MTEXT, BLOCK/INSERT,
CIRCLE, and every other unimplemented type are reported as `unsupported_entity`.
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
normalizes negative zero, emits LF, and groups entities as lines, arcs,
polylines, then labels while retaining each vector's order. Repeated export and
export/import/export are byte-stable within this subset. Export rejects invalid
records or strings instead of emitting injected group codes.

`dxf_exchange_tests` covers this round trip, CRLF input, bulges, wrapped arcs,
text whitespace, unsupported-feature diagnostics, malformed input, and both
input/output resource limits. This is synthetic codec evidence; external CAD
application interoperability, desktop import/export, transactional project
mapping, provenance, and retained-source handling remain separate integration
work.

## Native project mapping

`sketch/dxf_project_exchange.hpp` adds the first transactional-project mapping
layer on top of the transport codec. `export_project_dxf` reads one immutable
`DocumentSnapshot` and maps identified or legacy boundaries, wall baselines,
slab footprints/holes, native labels, symbols, and resolvable boundary
dimensions into the shared `DxfDrawing` model. SI metres are declared with
`$INSUNITS = 6`; layer names are resolved through the native layer graph when
available. Curved boundary edges stay analytical bulges, while positive
single arcs on wall baselines stay ARC records. Wall envelopes and slab
thickness/elevation/kind data have no DXF representation and are emitted as
fidelity diagnostics. The function never mutates the source document or opens
a path.

Only segment-length dimensions map to DXF's bounded linear `DIMENSION` record.
Angle and area dimensions remain native-only and produce an explicit
`dimension_semantics_not_representable` diagnostic; they are never flattened
into a misleading linear measurement.

`import_project_dxf` parses the bounded drawing and returns unparented editable
boundary candidates plus one typed annotation entity for labels. Lines, arcs,
polylines, solid hatch loops, and block INSERT geometry are reconstructed with
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

This mapping is a deterministic native-project slice, not Apex native-file
compatibility or full CAD fidelity. The desktop transaction adapter, active
floor/layer assignment, original-source asset retention, and clean-machine
interoperability evidence remain production-gate work.

## Native project mapping for IFC

`sketch/ifc_project_exchange.hpp` adds a bounded IFC4 STEP mapper on top of the
native document model. Export emits an IFC4 envelope with deterministic owner,
unit, placement, polyline, wall-axis, slab-footprint, and optional swept-solid
records. Linear analytical boundaries remain polylines; a closed slab with an
explicit thickness becomes an `IFCEXTRUDEDAREASOLID`. Curves, slab holes,
wall thickness/profile data, unsupported architectural entities, and spatial
relationships are diagnosed instead of silently flattened.

Import accepts the same IFC4 STEP subset and walks product representation
references to reconstruct editable boundary candidates for walls, slabs,
roofs, spaces, openings, and proxies. Candidate entities retain the IFC record
ID/type in `extensions.ifc_source`; placement translations and extrusion depth
are preserved when representable. Rotated placements, opening host links,
property sets, materials, and `IFCREL*` relationships remain explicit fidelity
diagnostics, and `source_retention_required` tells the caller to retain the
original bytes. The mapper is in-memory and does not claim IFC worker
isolation, Reference View conformance, or external-application certification.
