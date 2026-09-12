# Project organization

This package replaces the desktop's fixed Building 1 / Floor 1 / Default layer
presentation with the actual semantic hierarchy. It is part of the production
plan's document and authoring work, not a separate release gate.

## Contract

- Property, building, floor and layer entities retain their existing stable IDs.
  Parentage comes from typed `property_id`, `building_id`, `floor_id` and
  `layer_id` references. Names are presentation data, never identity.
- The navigator must display every object once, including columns, beams,
  stairs, railings and roofs. Hosted openings inherit their wall's drawing
  context.
  An unassigned object remains visible; the application must not invent a
  relationship to the first building or floor.
- A derived organization index resolves each object's containment and reports
  contradictory links. It does not mutate an imported document to make the
  hierarchy convenient. Unknown optional entities and metadata remain intact.
- Both workspaces use an explicit active floor/layer context for newly drawn
  boundaries and building objects. Selecting or switching a context is a view
  operation, not an edit to stored geometry. A stale or missing context blocks
  creation with an actionable message.
- Creating a building, floor with a default layer, or layer is one atomic,
  revision-checked command. Renaming preserves unknown fields. Moving an
  object's organization must preserve its geometry and maintain hosted
  relationships; it must not silently reparent unrelated objects.
- Floor and layer visibility are view filters. Calculation membership is
  determined by semantic rules, never by whether an object is visible.

## Coordinate compatibility

Existing v1 wall/slab `elevation_m` and building-object position vectors are
absolute world coordinates. A floor's `elevation_m` is currently metadata.
Resolving containment must not add that value to existing geometry, including
after save/reopen, undo/redo or layer reassignment.

The production level dependency graph requires explicit, versioned bindings
for floor-relative objects and previewable propagation. That later operation
must distinguish an object's organizational membership from its geometric
attachment. It cannot be introduced by changing the meaning of existing v1
coordinates.

## Implementation and verification sequence

1. Add and test the derived organization index, including multiple properties,
   buildings, floors and layers, host inheritance, inconsistent references and
   unassigned imported objects.
2. Connect the actual hierarchy and every supported object to the navigator;
   verify selection in both workspaces and preservation through save/reopen.
3. Add active drawing context and atomic organization commands. Verify two
   floors and two buildings, context deletion/undo, exact geometry retention,
   stale-command rejection and unknown metadata preservation.
4. Connect view filters to plan and model presentation, keeping calculation
   totals independent. Verify context labels, keyboard navigation and scaled
   Windows captures before recording the checkpoint.

The derived index and the navigator/authoring integration pass the integrated
Debug and Release suites. `project_organization_tests` covers malformed and
contradictory optional references, unresolved hosts, deterministic ordering and
unchanged geometry. `desktop_smoke` exercises two buildings/floors, actual
navigator parentage, rename history, missing active layers, stale caller
revisions, mismatched source/active slab creation, and save/reopen.

The Commands palette offers Add building, Add floor, Add drawing layer and
Rename selected property/building/floor/layer. Floor creation includes a
default layer in the same document command. A single valid layer activates
automatically; a document or container with multiple choices requires an
explicit layer selection. Imported entities keep their stored relationships.
New object candidates with supplied relationships must identify a valid layer
and consistent parent references; they cannot borrow a default from another
floor. A slab derived from a selected boundary requires the same active and
source context. Context authoring and rename pass the revision captured before
validation to the document command.

The active building/floor/layer path is also displayed in a wrapping label
below the selector, so a narrow panel cannot hide the authoring destination.
Keyboard selection uses the same context resolution as pointer selection;
deferred selection from a closed document cannot affect its replacement.

Current drawing-layer selection controls authoring destination only. All floors
remain visible and object coordinates remain absolute. View filtering, object
reassignment, the level dependency graph, floor-dependent geometry, phases,
alternatives and coordinated production views remain required.
