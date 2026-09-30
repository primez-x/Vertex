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
