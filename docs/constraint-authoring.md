# Persistent constraint authoring

This is the implementation contract for connecting the existing planar solver
to document editing. The solver already produces independently checked point
previews. The [versioned wall constraint codec](constraint-entity-format.md) and
document integrity checks are implemented. A command service and interactive
preview/Apply dialog connect them to both workspaces for straight walls and
identified straight measurement boundaries.

## Semantic authority

Geometry stays in its existing wall or boundary entity. A constraint stores
relationships to that geometry, never a second editable copy of its points.
Solver requests derive coordinates from one immutable document snapshot.

A point reference needs a stable owner ID and a stable semantic endpoint ID.
Straight walls have named baseline `start` and `end` roles. Boundary segment
array indexes are not stable identities, so boundary bindings name the retained
`segment_id` and `vertex_id` plus the endpoint role. Coincident coordinates
alone do not establish a relationship.
While a wall has attached constraints, a reverse operation must either remap
all endpoint bindings atomically or reject. Swapping coordinate fields cannot
silently redefine the endpoint identities.

Constraint records use a versioned codec, stable constraint ID, relation kind,
typed owner references, endpoint roles, and any exact entered quantity. They
are first-class `constraint` entities, not serialized solver DTOs. Version 1
wall bindings name the owner and baseline endpoint role. Version 2 boundary
bindings additionally name the segment and vertex identities. Its sorted,
unique top-level `entity_ids` list supplies typed ownership and structural
deletion protection and must match every nested endpoint owner.
Unknown optional fields must survive an unrelated edit. Unknown versions or
relations must not be silently solved as a known relation. Preserve unsupported
constraint data but make the document read-only until its lock semantics can
be evaluated; an optional flag is not permission to ignore a geometric lock.
The document's current reference validator sees top-level references only; the constraint
codec and document command validation must also cover nested endpoint bindings
and prevent deletion of referenced geometry without an explicit compound edit.

The supported relations in the existing adapter are horizontal, vertical,
coincident, fixed length, parallel, perpendicular and fixed anchor. Arc
constraints and the level dependency graph remain separate required engine
work; a line constraint must never approximate or flatten a curved wall.

## Preview and commit

1. Capture document identity and revision, selected geometry, current persistent
   relations, and the user's requested dimension, anchor and propagation mode.
2. Resolve the connected constraint component from explicit relationships.
   The selection and every referenced endpoint must be valid in that snapshot.
3. Derive a solver request without modifying the document. Include the selected
   anchor and branch/topology invariants. If connected movement is disabled,
   keep the other affected geometry fixed; reject an impossible result instead
   of breaking relationships or silently switching propagation modes.
4. Independently validate solver residuals, anchors and intended orientation,
   then validate every changed semantic object and hosted opening against the
   proposed geometry. A geometrically valid line solve is insufficient proof
   that its walls, openings and area boundaries remain valid.
5. Show old and proposed geometry, changed dimensions, affected objects and
   diagnostics. Cancellation makes no document/history/saved-state change.
6. On explicit Apply, verify the captured document identity and revision again.
   Commit geometry and relationship changes together as one reversible command.
   A stale preview must be recomputed; it cannot be applied to the latest head.

The preview boundary must not trust mutable caller-provided solver coordinates
as authority. An accepted preview is tied to its source snapshot and request;
the first implementation recomputes and validates the candidate from normalized
intent on Apply. Raw `ConstraintPreview` points are not a commit API.
Recomputation must also match the normalized intent and candidate digest shown
in the preview. A different result requires a refreshed preview, not automatic
application of geometry the user did not see.
Changing the open document invalidates every pending preview even if the new
document happens to have the same revision number.

## Existing edits and invalidation

Enforcement belongs inside the document state transition boundary. A pure,
solver-independent semantic validator checks known relation codecs, nested
references, analytical hard residuals and lightweight host/opening validity
after structural validation. It must cover create, generic apply, restore and
every historical state eligible for undo/redo. CLI and import paths must use
the same protection; a configurable or missing policy cannot permit bypass.
The validator is compiled into `sketch_document` as a separate source, with no
runtime callback registration. Well-formed unsupported semantics are distinct
from corrupt known data: unsupported locks preserve the project read-only;
malformed known bindings or violated known relations reject the state. Until
safe partial history navigation exists, unsupported lock semantics anywhere
in retained history make the restored document read-only, avoiding an undo
that enters an unsupported state and traps redo.
The higher command service owns connected-component resolution, solver calls,
propagation previews and candidate generation. The document layer must not
link PlaneGCS or OCCT merely to enforce a persisted hard relation.
Constraint-owner walls and every opening hosted by them share a pure semantic
validity predicate with architectural solid construction. It checks finite,
distinct endpoints, positive wall dimensions, opening bounds and overlaps;
OCCT alone constructs solids after those checks. Shared predicates prevent
the document and solid builder from accepting different semantic states.

Adding persistent constraints is incomplete if existing length edits, entity
replacement, deletion, import or undo/redo can bypass them. Supported changes
must route through the same validation boundary. Unsupported operations on a
constrained component must leave it intact and explain the unsupported action.
There is no automatic suppression or deletion of a locked relation.

Unrelated metadata changes must not solve or move geometry. A constraint change
or dimension change preserves unrelated metadata, entered unit receipts and
organization. Hosted openings retain their declared placement semantics and
must remain inside the updated host. Calculation membership and floor elevation
semantics do not change merely because a constraint is added.

## Acceptance evidence

- Resize a 12-foot wall to 14 feet with either endpoint anchored. Verify the
  selected anchor exactly, previewed movement and stored exact entered quantity.
- Exercise both connected-movement choices on an explicitly connected wall
  group. Preview and Apply must agree; an incompatible fixed neighbor rejects
  without partial mutation.
- Create, edit and remove every supported persistent relation; preserve it and
  its stable references through save/reopen and undo/redo.
- Reject contradictory dimensions, malformed/unknown bindings, duplicate IDs,
  dangling owners, unsupported curves, forged candidates, stale revisions and
  previews from another document. Preserve the previous valid state.
- Demonstrate that ordinary dimension edits and referenced-entity deletion
  cannot bypass a locked relationship. An explicit relationship removal and
  dependent geometry edit must be one reversible operation.
- Verify independent topology/winding and hosted-opening checks, not only solver
  status or a repeated calculation of the implementation's expected result.
- Inspect preview, conflict and cancellation states with keyboard navigation at
  normal and 150 percent display scaling in both workspaces.

The implemented desktop dialog binds named endpoints of straight walls and
stable endpoints of identified straight measurement boundaries. Curved
boundary relations and mixed wall/boundary components reject explicitly; they
remain open production scope rather than being approximated as line geometry.

## Implemented command and receipt boundary

`preview_constraint_authoring` accepts an immutable snapshot and normalized
resize/relation intent. `ConstraintAuthoringPreview` exposes read-only candidate
entities, changed baselines and diagnostics for rendering and independent solid
validation. `apply_constraint_authoring` checks document identity, revision, the
complete source snapshot digest and the displayed candidate digest, recomputes
the intent, and commits only an identical validated result. It never treats
caller-supplied candidate coordinates as commit authority.

Wall resize receipts live in
`extensions.constraint_authoring.last_length_entry`, with a version, original
expression, entered unit, exact rational metres and the resulting baseline.
They describe the entry that produced that baseline; baseline geometry remains
authoritative. A subsequent service edit that moves a wall removes its previous
receipt unless it supplies a new explicit resize entry. Unsupported overlapping
receipt metadata rejects the operation instead of being overwritten.

Recognized v1 receipts are decoded before an edit: the original expression,
entered unit, normalized exact rational, and recorded straight baseline must
agree with each other and the current wall. Unknown members of a supported
receipt, its rational, and its baseline are retained when an explicit resize
updates recognized fields. A relation edit that would remove a receipt with
unknown members rejects instead of discarding them. Unknown members of the
wall's baseline itself are always retained. Preview, Apply, undo/redo and
save/reopen regressions cover these nested metadata rules.

The document validator indexes hosted openings once per state. Opening overlap
and full-coverage checks use ordered sweeps instead of all-pairs scans. Known
locks remain validated in mixed known/unknown history, and unknown constraint
envelopes still validate their typed wall/opening owners while preserving their
unsupported relation semantics read-only. Scale fixtures cover distributed
owners, many openings on one wall and a real retained-history save/reopen cycle.

Conflict diagnostics map typed solver results back to the requested relation,
wall name (or stable wall ID), and endpoint. Generated solver constraint IDs are
not presented as the explanation. The dialog retains a scrollable, selectable
plain-text diagnostic panel; rejected previews leave Apply disabled and preserve
the document. Focused tests cover a locked 12-foot wall resized to 14 feet and
require readable fixed-length and fixed-endpoint explanations.

Boundary solves commit through `ApplyBoundaryConstraintChanges`. The typed
transaction replays ordered semantic vertex edits before applying constraint
entity changes, preserves construction receipts as geometry-derivation proof,
and advances history once. Project format 8 stores the exact command envelope;
document restore, save/reopen, exchange, digests, recovery budgets and archive
paths validate it by deterministic replay. The dialog omits the wall-only
baseline-resize operation in boundary mode and previews current/proposed edges,
changed lengths, maximum endpoint movement, degrees of freedom and conflicts.
