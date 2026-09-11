# Local interchange capability profiles

`InterchangeProfile` is a portable declaration and readiness validator for
COMP-IO-001/002/003 and IO-IFC-001, IO-DXF-001, IO-PDF-001. It does not implement
an IFC/DXF/PDF parser, exporter, Qt module integration, or PROJ operation.
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
local files and verifies their integrity; this API does not open files.

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
monitor; measured worker failure recovery. Unit tests exercise only the portable
declaration and fail-closed readiness decision, using synthetic attestations.
