# Persistent constraint authoring

This is the implementation contract for connecting the existing planar solver
to document editing. The solver already produces independently checked point
previews. The [versioned wall constraint codec](constraint-entity-format.md) and
document integrity checks are implemented. A command service and interactive
preview/Apply dialog connect them to both workspaces for analytical walls and
identified measurement and room boundaries, and receipt-backed measured strokes.

Measured strokes use the same segment/vertex binding vocabulary. Their open
terminal endpoint is selectable, and a revisited stable vertex is one solver
point. Crossings and retracing do not acquire a closed-area winding invariant.
Fixed length measures the endpoint chord; Fixed physical arc length measures
the analytical curve. Selected typed edits retain entered quantities. Connected
solver moves append one simultaneous vertex batch per affected stroke; rigid
reflections retain an exact transform so signed sweep changes correctly.

Envelope 11 reconstructs affected current source-derived areas in the core
before preview sealing and final relation validation, using the persisted
active phase. Automatic rebuilding requires previously current, unambiguous,
unauthored consumers. Original observations, stable boundary identities and
group lineage survive. Ambiguous, authored or already-stale consumers retain
their evidence and require explicit source review. Apply and history restore
the complete transaction together.

Schema/replay 5, command envelope 11, native format 35 and extraction version 33
protect these semantics across retained history. Source-owned annotation
offsets follow rigid rotation/reflection once; translation leaves offsets
unchanged. Ordinary supplemental metadata and assets compose independently
of exterior completion. Overlapping raw measured geometry or owned placement
changes are refused. An unchanged measured resize can close the dialog without
adding an undo step; changing an input invalidates that preview.

## Semantic authority

Geometry stays in its existing wall or boundary entity. A constraint stores
relationships to that geometry, never a second editable copy of its points.
Solver requests derive coordinates from one immutable document snapshot.

A point reference needs a stable owner ID and a stable semantic endpoint ID.
Walls have named baseline `start` and `end` roles. Boundary segment
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
coincident, endpoint distance, parallel, perpendicular, fixed anchor, physical
curve length and tangent. Endpoint distance and direction relationships retain their
point/chord meanings. Curve length applies to opposite endpoints of one genuine
curved wall, measured segment or identified edge and preserves its signed sweep.
Version-5 tangent bindings identify the actual incident child and contact
endpoint. They require coincident contacts and opposing outward analytical
tangents, including curved walls and identified measured edges. General
planar-constraint-to-level propagation remains required work. The existing
explicit level graph/editor is a separate implemented slice; point solving
never flattens a curved wall.

Explicit reconstruction of an existing curved wall is a separate authoring
intent. Version-6 wall proof retains its entered angle, signed arc length or
height and exact chord endpoints. The selected new sweep is fixed before
tangent relations are constructed; related owners retain their own sweeps.
Both selected endpoints remain exact while compatible connected geometry solves.
The proposed wall, connected changes and preserved source are shown before
Apply. The document independently replays the selected construction and its
source-owned metadata, then completes qualified measurement consumers in one
event. Outer envelope 23, native format 75 and extraction version 73 retain this
authority through history. Incompatible locks, hosted openings, ambiguous
source consumers or physical rooms requiring explicit repair refuse the edit.

Room correspondence can accompany a valid proposed curve through a separate
reviewed completion. The report uses the detached curve geometry, while the
final intent retains the original document's complete source fence. Admission
first independently replays the curve, then rederives all explicit room,
reference and relationship decisions on that result. Neither preparation
commits an intermediate state. Envelope 24, native format 76 and extraction
version 74 protect the composed event; one Undo restores curve and rooms.

The same detached review also accepts ordinary physical-wall endpoint and
length edits with their admitted connected/source consequences. Envelope 25,
native format 77 and extraction version 75 retain those completions separately
from explicit curve reconstruction. The ordinary child keeps its existing
admission and source-bound-room guard; the review cannot add raw geometry or
asset authority. Wall editing routes use the verified original proposal before
any commit and require room review for affected retained consumers. Cancellation
leaves the original wall and rooms unchanged. Each affected resolved drawing
context and physical plane receives its own explicit review.

Wall height, thickness, sloped-top and assembly-layer edits use a profile-only completion
instead of an artificial endpoint edit. The exact authored wall upsert retains
all other fields; linked exterior measurement updates keep their existing
source completion. Room review happens on this detached candidate before any
publication. Envelope 26, native format 78 and extraction version 76 preserve
the profile and reviewed rooms as one event. Missing exterior completion,
unrelated payloads and mixed profile/geometry changes cannot borrow this lane.

When several context/plane groups are affected, the interface presents them in
sequence with floor, layer and elevation labels. Every dialog uses the exact
cumulative detached candidate, while actual source, selection, workspace and
pending-placement authority remain fenced to the original project. Cancelling
any dialog discards all decisions. Envelope 27, native format 79 and extraction
version 77 preserve up to thirty-two explicit groups as one event. The core
rederives each stage, requires complete affected-room coverage and reserves
fresh identities across history and all stages. A missing representative source
or a moved/stale plane still refuses; the interface never guesses a room context.

Detached room review also admits the existing qualified rigid physical-wall
children: direct 10, mixed measured-source 11 with rigid walls, or narrow saved
wall-callout completion 21. Genuine curved-v4/straight-v5 wall operators retain
their original core replay; ordinary connected neighbors keep their existing
proofs. Envelope 28/native 80/extraction 78 protect single reviews, and a batch
containing these children also requires native 80. This does not admit arbitrary
transform wrappers or independently moving physical-room boundaries.

Single-wall numerical offset/rotation edits retain both the exact authored
command and its admitted snapshot. Typing new values immediately retires the
previous proposal and Apply authority. Affected rooms are reviewed before the
wall is published; cancelling that review leaves the project unchanged. The
canvas wall rotation release uses the ordinary geometry-transform capture,
retaining its actual prepared command and complete candidate history. It
consumes gesture tickets before nested review while keeping source, recovery
mirror, selection, viewport and replacement-proposal fences. Acceptance applies
only the reviewed composite once. This route excludes Site, mixed selections,
copies, mirrors and scaling; those room consequences remain separate work.

Single-wall deletion uses context/elevation discovery so room review remains
available after removing a source wall, including when no walls remain. The
canonical child removes the wall and supported hosted openings, dimensions and
constraints; the review lists these original removals. Each original room and
fresh space receives an explicit disposition. Retiring a room removes its known
phase, saved-view and presentation memberships while unknown references still
refuse. One accepted event owns wall deletion and all room decisions. Exterior
appraisal observations remain historical and withhold quantities until their
physical source is repaired. Envelope 29/30, or batch 27 with context-based
intents, requires native 81/extraction 79.

A wall-only selection also supports removing several walls together with Delete
or Cut. Grouped proof 31 declares the exact original wall inventory and retains
its independently reconstructed ordinary child; single-context envelope 31 or
batch 27 requires native 82/extraction 80. Every affected original room context
and plane is reviewed against cumulative detached candidates. No intermediate
wall or room state is published. Cancel preserves the drawing and clipboard;
successful Cut writes its prepared clipboard payload after the complete event.
Mixed selections and semantic phase demolition keep their separate contracts.

Connected wall-group movement uses its intact joint envelope 17 before room
review. Shared translations, saved per-owner offsets and rigid operators retain
their actual selected source identities and independently replayed constraint
consequences. Room completion uses envelope 32, or batch 27, with native
83/extraction 81. It adds explicit room assignments after ordinary admission;
it cannot supply unrelated raw geometry or rewrite the saved operators.
Ordinary wall-only groups use the per-owner translation lane with explicit wall
offsets before worker projection; they do not borrow a rigid measured owner or
gain joint authority at release. Independently selected saved callouts carry
their own qualified placement offsets. Selected membership is compared without
requiring Ctrl-click order to match canvas presentation order; duplicate or
different membership refuses while the original primary stays authoritative.

The Curve length relation uses version-3 constraint semantics and an exact
positive length quantity. At a fixed sweep its solver target is the equivalent
chord length, but independent integrity validation measures the analytical arc.
The dialog distinguishes it from Endpoint distance, prefills the selected curved
edge's physical length, and previews an anchored change before Apply. Editing its
target changes geometry; removing it preserves geometry. Straight segments or
unrelated endpoint pairs cannot masquerade as an arc-length relationship.
Retained relations require project format 12 and exchange version 9. Direct
curved-wall resize is also implemented with physical-length input receipts
and its own reader protection, described below.

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
  dangling owners, invalid curves, forged candidates, stale revisions and
  previews from another document. Preserve the previous valid state.
- Demonstrate that ordinary dimension edits and referenced-entity deletion
  cannot bypass a locked relationship. An explicit relationship removal and
  dependent geometry edit must be one reversible operation.
- Verify independent topology/winding and hosted-opening checks, not only solver
  status or a repeated calculation of the implementation's expected result.
- Inspect preview, conflict and cancellation states with keyboard navigation at
  normal and 150 percent display scaling in both workspaces.

The desktop dialog binds named endpoints of analytical walls and stable
endpoints of identified measurement and room boundaries, including mixed
components. Previews render true arcs and show physical edge lengths; the
relationship field explicitly labels a fixed point-distance lock as Endpoint
distance. Curved walls expose **Change curve length** as the direct resize
operation, prefilled with their physical arc length. It needs no added lock.
Keep start/end fixed chooses the anchor, and Allow connected objects to move
controls related geometry. The signed sweep and chord direction remain fixed;
an incompatible lock, frozen connection or host placement rejects atomically.

Curved wall geometry proofs use explicit wall-edit version 2 inside command
envelope version 3. Historical unversioned straight proofs and envelopes 1/2
retain their old rules. Version 3 permits curved wall-only geometry transactions;
version 4 permits straight wall-only transactions. Direct physical curve-length
entries use wall-edit version 3 inside command version 5, with a mandatory exact
entry and a version-2 physical length receipt. All use the shared topology
admission during preview, apply and retained-history replay.
Original curve construction input is retained as derivation evidence when its
measured length or height no longer describes the fixed-sweep endpoint result.
Rigid curve transforms use a separate shared helper: rotation, reflection,
translation, and cloning retain the exact original construction and append an
independently replayed operation. Its version-2 archive preserves prior endpoint
and construction history, requires project format 14 and exchange version 11,
and does not weaken the fixed-signed-sweep endpoint-edit rules. Shift-adjusted
rotation changes geometry while retaining that measurement evidence. Scaling
measured curves remains unsupported and rejects atomically.
Project format 10 and exchange format 7 protect new proofs and curved-bound
relations throughout retained history, including undone states.
Straight wall-only endpoint authoring now retains an unversioned straight wall
proof in command envelope 4, requiring project format 11 and exchange format 8.
The minimum version follows the complete retained history, including undone
edits. Projects without this new proof retain their earlier minimum version.

An explicit construction operation supplies a complete, independently validated
input and baseline. It may follow a derived curve through an ordinary entity
command if it appends exactly one construction operation without rewriting the
original archive or prior operations. An endpoint-only appended operation still
requires typed wall replay. These are different recorded operations; generic
construction commands are not claimed to carry the solver's topology proof.

Connected endpoint edits use `validate_constraint_edit_topology` at both the
authoring and typed document command boundaries. It checks analytical
intersections against existing topology, requires declared stable endpoint
relationships for new contacts, and protects closed wall-cycle and boundary
winding. Saved command replay uses the same admission path; matching retained
geometry, a valid command encoding and a recomputed checksum do not bypass it.
Ordinary explicit construction and transformation remain distinct operations.

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

Recognized v1 straight receipts and v2 physical curve receipts are validated
on restoration and before an edit: the original expression,
entered unit, normalized exact rational, and recorded baseline must
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
Measured strokes expose replayed segments and both stable endpoints in the
same dialog; chord distance and analytical physical curve length are distinct.
