# Analytical circles and annotation layers in DXF exchange

This closes concrete 2D import/export gaps within APX-DOC-002 and IO-DXF-001.
It does not certify native Apex files or the complete production release.

## Outcome and design

A planar DXF CIRCLE imports as one editable measurement boundary containing
two analytical semicircles. Its source center, radius, units and layer survive
transport. The original file remains a retained project asset. Ordinary block
INSERTs honor their base point, placement, rotation, uniform scale and reflection.
Nonuniform circle scaling needs an ellipse model and is reported explicitly.
Nondefault object coordinate systems, thickness and 3D coordinates remain
unsupported rather than being flattened.

The project mapper follows the existing anonymous imported-boundary contract.
The existing Upgrade boundary editing command supplies stable topology before
typed vertex/edge editing. Export may use an exact closed two-bulge polyline;
transport record identity and native semantic compatibility are distinct.

Labels and symbols exported from a native project use their assigned layer's
name. Unassigned children retain the existing Annotations/Symbols fallbacks.
Broken assigned references produce child-specific diagnostics and layer 0.
Adding a recognized circle to native wall/opening block artwork must invalidate
native metadata activation unless it matches regenerated physical plan geometry.

The transport mapping follows [Autodesk's CIRCLE group codes](https://help.autodesk.com/cloudhelp/2018/ENU/AutoCAD-DXF/files/GUID-8663262B-222C-414D-B133-4A8506A27C18.htm).

## Ownership and verification

- Source worker: DXF transport header/codec and project mapper.
- Test worker: transport and project exchange regressions.
- Adapter worker: production library normalization and its Python regressions.
- Root: native desktop workflow, docs, integration, build, Git and delivery.

1. Observe the supported-circle omission in a Release regression before editing.
2. Implement bounded parse/write/count handling, exact geometry, unit conversion,
   block transforms, native activation parity and annotation layer resolution.
3. Verify independent hand-written records, malformed/unsupported input,
   resource limits, analytical area/perimeter and uniform/reflected INSERTs.
4. Verify real desktop layer review, source-byte retention, Undo/Redo, typed
   curve editing, save/reopen and geometrically exact DXF output.
   The production library normalizer must retain circles before invoking the
   strict mapper; direct mapper coverage alone does not prove desktop import.
5. Inspect the native rendered circle, run affected import/output checks and
   requirement/source-kit contracts, then commit/push and deliver matching build.

Full external CAD qualification, general ellipses and native Apex compatibility
remain part of the original release scope and require separate evidence.
