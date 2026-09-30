# Hosted CAD exchange checkpoint

Implemented native IFC4 door/window fills and curved wall/void/fill meshes from
the shared architectural kernel. DXF R2013 now carries validated editable hosted
graphs alongside exact analytical plan geometry. Desktop imports remap identities,
assign the active floor/layer, validate physical assemblies, and commit atomically.

Verification on the Windows development host:

- Release application and focused exchange/desktop targets built.
- DXF transport/project, IFC project, shared hosted geometry and reference-import
  checks passed. Rotation desktop regression passed.
- Independent AppContainer harness passed all four cases, including DXF/IFC
  two-host door/window workflows, unrelated target objects, undo/reimport,
  save/reopen after releasing project ownership, and subsequent profile editing.
  Evidence: `artifacts/import-worker-independent/release-d78e0b53f2c94c028dd2be3f6203a319/result.json`.
- IfcOpenShell 0.8.3.post2 parsed eight straight/curved door/window fixtures with
  no schema errors and generated every represented product shape. ezdxf 1.4.3
  audited two native hosted DXF fixtures without errors.
  Evidence: `artifacts/usability-review/20260929-cad-hosted/external-parser-report.json`.

The checks found and corrected STEP real-number formatting, strict reference
candidate fields, missing native DLL closure in the immutable fixture runtime,
per-host admission filtering, and legacy dimension canonicalization. Native
metadata activation rejects unsupported blocks, geometry drift, incomplete host
relationships and contradictory fill metadata. Retained DXF source evidence
stays flat across repeated exchanges.

This is implementation evidence, not production certification. DXF manufactured
profile geometry, oblique-host IFC standardized operation-frame semantics, full
Reference View conformance, external CAD/application qualification, and clean
Windows offline installation/workflow qualification remain open requirements.

## Manufactured plan and local-frame follow-up

The subsequent implementation closes the two listed source gaps: DXF and desktop
plans now use native manufactured mid-height sections; IFC fills use proper
opening-local frames with verified parent composition. Curved OverallWidth is
the local-X opening envelope, while native dimensions keep arc stations. Exact
source parameters are used only after independent void/host agreement to avoid
triangulation changes caused by reconstructed floating-point roundoff.

Exact pointer previews run off the UI thread with one running job and one latest
proposal. Canvas serial, document identity and revision fences reject stale
results. Refresh invalidates immutable preview captures even when a shared
Document replaces its head at the same revision. Committed projection keys
include all sibling opening cuts. Pending feedback stays neutral; final release
still requires the normal native command admission.

Focused Release checks passed for architecture, opening assemblies, DXF/IFC
projects, straight/curved jamb resizing, deferred preview cancellation/output
isolation, wall/opening placement, and repeated rotation. Rotation covers saved
frames, 15-degree snapping, Shift fine adjustment, undo/redo and reopen. Captures
include `rotation-live-90.png`, showing readable physical dimensions and angle.
The first test capture lacked application font initialization; the harness now
loads the same bundled Inter font as the application before visual verification.

Independent AppContainer execution passed four cases, including two-host DXF/IFC
import/edit/save/reopen. Evidence:
`artifacts/import-worker-independent/release-ddeaa531a316486b90be028bcd4a52b4/result.json`.
IfcOpenShell 0.8.3.post2 and ezdxf 1.4.3 parsed/audited 31 captured files with zero
reported schema/audit errors; every represented IFC product produced a world
shape. Evidence:
`artifacts/usability-review/20260929-hosted-plan-frames/external-parser-report.json`.

This remains development-host evidence. Full Reference View conformance,
external CAD/application semantic qualification, production IfcOpenShell/ezdxf
worker integration and dependency closure, clean Windows offline qualification,
and representative interaction/close-time latency remain required. The native
kernel cannot interrupt an individual solid operation; cancellation discards its
result and shutdown joins it. No broader performance certification is claimed.
