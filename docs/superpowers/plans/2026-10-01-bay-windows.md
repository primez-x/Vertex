# Physical bay windows

This implements an existing architectural capability gap in the full Vertex
production plan. It does not replace the production gate with a bay-only gate.
Roof-hosted skylights and the other recorded production gaps remain required.

The bay catalog item must create a real wall-hosted projecting assembly, with
three framed glass faces and sealed top and bottom plates. Plan, native 3D,
print, schedules and exchange derive from the same semantic parameters.
The wall cut remains the mouth of the bay; a window alone does not create an
appraisal measurement area or floor extension.

Append the bay layout to the existing window descriptor. Preserve existing v1
and v2 representations and geometry. A strict window-only, bay-only v3 adds
projection depth and front-width fraction. Projection depth is measured beyond
the selected wall face; the front fraction is relative to the full mouth width.
The existing side flag selects projection toward the wall's left or right
normal. Curved hosts require separate fitted geometry and remain an explicit gap.

Ownership:

- Geometry worker: opening assembly schema/header, common solid factory and
  geometry tests. Validate actual parts, glazing, joins, extents, both sides,
  rotated hosts, frame fit, invalid dimensions and legacy representations.
- Adapter worker: schedule adapter, IFC exchange and schedule/IFC/DXF tests.
  Preserve native source only when projected primitives or physical meshes
  agree. IFC represents the non-coplanar partition with a truthful user-defined
  descriptor; labels and geometry must both agree on native import.
- Root: desktop catalog routing, placement controls, context editor,
  reflection/reclassification, Library workflow regression, documentation,
  generators, integrated review, native UI evidence and scoped Git delivery.

Verify the missing schema/catalog behavior first, then run the smallest
meaningful geometry, schedule, IFC, DXF and desktop checks. Review the integrated
project-format and geometry boundaries independently. Draw/place/edit a bay
through the real Windows UI, save/reopen, and inspect plan and native 3D before
describing the user-facing result as verified.
