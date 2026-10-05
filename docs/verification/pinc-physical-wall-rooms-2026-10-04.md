# Source-bound physical room checkpoint

This delivers the creation and stale-state portion of the physical-wall palette
workflow in PINC-002. It does not complete source repair, all Pinc mappings or the
unified production gate. The Pinc inventory still contains 134 practical operations;
no manual checklist result is inferred from these engineering checks.

## User behavior

The searchable area-class palette detects spaces from actual physical wall
footprints as well as the existing measurement-linework route. Drop a type onto
a clear space or its row, or arm a type and click. Wall-based room creation uses
the same detector. Repeating creation reuses the current room without changing
its identity or classification. Ambiguous wall-adjacent spaces require an
explicit choice rather than silently picking a centerline polygon.

Retained rooms contain an identified outer boundary, analytical holes and exact
source evidence. Clear floor area excludes wall thickness, intrusive partitions
and isolated obstacles. Details shows outer area, individual hole deductions,
net area and source status. Canvas labels, schedules and supported DXF/IFC
footprints use the same canonical query. Outlined rooms are selectable inside
their material; holes remain empty and furniture keeps picking priority.

These are room boundaries, not measured exterior appraisal boundaries. A room
classification neither fabricates ANSI eligibility facts nor increases exterior
GLA. The desktop fixture displays a 10.24 m² clear room separately from its
declared 25.00 m² exterior GLA.

Source edits currently invalidate a retained room. Its stale geometry, numeric
labels, schedule quantities and exchange footprint are withheld with a repair
diagnostic. Independent outline edits and marker stripping are refused. Typed
same-ID repair is still required; this checkpoint does not transfer classifications
between newly detected split or merged spaces.

## Evidence

Root ran hidden Release jobs with all native writers frozen. Local evidence is
under `artifacts/physical-wall-rooms-20261004/`.

- `initial-build` and `corrected-build` failed on a missing desktop declaration
  and a JSON/string comparison. Both source defects were corrected.
- `repaired-build` exited 0. `focused` passed ten checks and exposed an outlined
  room interior picking defect in the new desktop check.
- `picking-build` exited 0 after the picking correction. The desktop check passed;
  `boundary_canvas` exposed an old fixture that called a room interior empty.
- `hole-selection-build` exited 0 after supplying a real hole in that fixture.
  `final-interaction` passed all three affected interaction checks.

The combined passing outcomes cover twelve checks: `physical_wall_spaces`,
`physical_wall_room`, `physical_wall_room_desktop`, `area_class_palette_desktop`,
`boundary_canvas`, `boundary_dimensions`, `document_schedule_adapter`,
`room_relationship_geometry_commit`, `dxf_project_exchange`,
`ifc_project_exchange`, `project_storage` and `project_exchange`.

Known answers include 10.64 m² outer clear space, 0.40 m² isolated wall deduction,
10.24 m² net clear area and a 78.4 m² nested parent. Checks cover thickness,
movement and insertion/removal invalidation, context/phase/visibility, exact
source/outer/hole tampering, wrong-owner marker admission, unknown future
descriptor read-only retention, Undo/Redo and native save/reopen. Desktop checks
use the bundled SVG sofa, actual mouse picking, Details and schedule values.

DXF checks retain outer and inner loops and withhold stale footprints. IFC checks
retain supported linear footprint loops and opaque native evidence across two
complete export/import cycles. Bounded chunk manifests validate owner count,
byte count, ordering and SHA-256 before reconstruction; tampering is refused.
Unsupported curved IFC footprint semantics remain explicitly reported.

Root visually inspected native white/dark captures in
`final-interaction-captures/physical-wall-room-white.png` and
`final-interaction-captures/physical-wall-room-dark.png`. The room label, wall
dimensions, furnished canvas and separate GLA display are visible. Details values
are checked by desktop assertions, not by those canvas captures.

One independent read-only advisor approved this bounded creation/stale checkpoint
with no remaining actionable P1/P2 findings after marker integrity, aggregate
cache limits, IFC retention and picking corrections. That review does not claim
full lifecycle delivery or production readiness.

## Format and limits

Native format 43 / extraction 41 protects the reserved source-room descriptor,
including retained deleted history. Unknown positive versions are read-only.
The canonical query bounds owners, detector contexts, geometry and aggregate
cache charge. IFC reconstruction also bounds chunks, bytes and aggregate copied
metadata. Stored claimed area is never a calculation authority.

The installed `vertex-20261004-floor-reference` application and Desktop shortcut
are unchanged. The current Release executable is in `build/windows-release/vertex.exe`;
it depends on the developer build runtime until packaged. No new installer,
clean-machine/network-denied installation, physical printer, native Apex migration,
full IFC interoperability or user-observed resolution is claimed.

Remaining work includes same-ID source repair, proven split/merge and rigid
correspondence, source-bound annotations, full 22-entry palette mapping, all 80
Pinc symbol counterparts and paired qualification of all 134 operations. Original
Apex, architectural, device, integration and recovery requirements remain binding.
