# Wall authoring and hosted library openings

The conventional 2D canvas defaults to physical Wall drawing. The common
left-panel Draw selector switches to Measurement for appraisal boundaries;
selection and empty-space drag panning remain on the same canvas. Click to
start, click successive connected wall endpoints, and press Esc to finish
without removing committed segments. A wall draft shows its physical thickness
and one live length callout. Committed wall lengths remain beside the walls.

Endpoint, wall-centerline, perpendicular, alignment and grid feedback use the
same resolver as committed points. Eligible geometry belongs to the active
property, building and floor and passes the existing visibility filters.
The snap toggle disables geometric snapping as well as grid snapping.

Library door and window variants create semantic hosted openings, preserving
their catalog identity. Hinged variants use the explicitly authored 760, 810
or 910 mm opening width rather than the artwork footprint including jambs;
windows default to a 0.9 m sill and 1.2 m height. A drop with no host is refused
without adding an annotation. The Doorway action creates a bare wall opening.
The Wall, Door, Window and Doorway actions use this same wall/opening model.

## Verification

- Release application and affected desktop targets built successfully.
- `boundary_canvas`, `drawing_set_output` and `coordinated_view_output`
  passed 3/3 in 31.16 seconds. The expanded `wall_opening_palette` check
  passed separately after correcting hinged-door opening width. The later
  corner-alignment and boundary checks passed 2/2 in 7.96 seconds.
- Actual Qt mouse events exercised the default idle surface, chained endpoints,
  endpoint priority, on-wall projection, preview/commit equality, wall selection
  by its body, selected-wall movement, deselect-first behavior, canvas pan and
  explicit measurement-area drawing.
- Actual library drag/drop events exercised hosted door/window creation, wall
  cuts, schedule quantities, refusal without a host, undo/redo and save/reopen.
- A reopened ordinary-plan PDF contained the generated wall length and unit.
  Existing plan/sheet output regressions passed with the shared renderer.
- Capture review removed duplicate preview lengths and separate selection
  controls on derived wall labels. Captures are saved locally under
  `artifacts/wall-workflow-20261001/`.
- Direct desktop clicks exposed a horizontal/perpendicular guide taking
  precedence over a nearby corner's second axis. A regression failed with that
  resolver; resolving both axes together makes it pass while retaining
  endpoint and on-wall priority.

This is a correction build, not full production or Apex compatibility
certification. Numeric completed-boundary vertex input, richer catalog door
operations, nominal opening dimensions for the six source door styles that
declare only artwork footprints, and persistent wall-junction relationships
remain separate gaps.
