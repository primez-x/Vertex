# Model phases and remodeling alternatives

`ModelPhases` is a validated immutable semantic value supporting the model portion
of ARCH-MOD-010 and ARCH-MOD-011. The desktop now exposes a persisted design-phase
selector and alternative manager. Selecting an alternative is a typed, revision-
fenced, undoable Document command; the active phase filters the shared plan,
architectural projections, native 3D visible IDs, schedules, and sheet output.
The JSON remains a standalone exchange value nested under a normal Document entity.
Full production certification still requires imported Apex projects and the
remaining architectural authoring workflows to be exercised through the single
production gate.

Construction requires an explicit registry of participating model entity IDs, a
shared baseline, and alternatives. The registry must match the caller's intended
scope; this module does not independently inspect a document or geometry. Every
registered entity belongs to the baseline or exactly one alternative's proposals.
Baseline objects are existing. An alternative may mark baseline objects demolished
and introduce proposed objects. Demolished objects remain present with that phase
so downstream consumers can apply their own explicit phase filters. Proposed
objects from other alternatives are absent. Replacement geometry needs a distinct
entity ID from the demolished baseline object. This bounded model permits only one
alternative family; it does not support stacked alternatives or sequential projects.

After phase setup, desktop authoring assigns newly created geometry to the active
alternative's proposals, or to the shared baseline when baseline is selected.
Geometry and registry membership are one undoable command, including pasted and
compound geometry and hosted openings; opening host references remain unchanged.
Building and floor organization records are not newly enrolled by geometry authoring.
Editing existing unregistered geometry does not silently enroll or reclassify it.

The optional active selection names exactly one alternative. `nullopt` selects the
unmodified baseline. `with_active` returns a new value and leaves its source intact;
callers can retain those values for an undo adapter. `with_alternative` appends a
validated alternative while preserving existing alternatives and active selection.
`with_updated_alternative` replaces an existing exact alternative ID through the
same canonical membership validation. Unknown IDs refuse. The document adapter's
`model_phase_alternative_update_command` changes only its name and demolished
baseline members, preserving proposed members, identity, the active selection,
other alternatives, and unrelated entity properties and extensions.
`state` resolves an explicit
selection and `active_state` resolves the saved active selection. Both return
independent maps ordered by entity ID.

`compare(left, right)` always takes explicit selections, even when one is the
baseline. It records both selections and reports only differing entities, ordered
by entity ID. Each side is an optional phase: absence is distinct from demolition.
Comparing does not switch the active alternative or change the shared baseline.

The Windows design-phase manager exposes this comparison directly. The compact
**Compare phases** panel lets the user choose the baseline or any named
alternative on each side and lists every membership difference with its left and
right phase. Comparison is read-only; applying a phase remains a separate,
undoable command, so reviewing options cannot change the active model.

The manager opens without creating a phase record or an Undo entry. The first
**Create alternative** action publishes the registry and named alternative in one
command. Choose an existing alternative to edit its name and demolition choices,
then **Save changes**. Editing an alternative does not select it; **Apply phase**
controls the displayed phase separately. Saving unchanged values or applying the
already active phase adds no history. An unfinished drawing or placement must be
finished or cancelled before opening the manager or changing phases.

Demolition choices have a baseline floor-plan preview drawn from the actual
captured document. Selecting a list row highlights its geometry and switches the
preview to that object's floor. Clicking displayed geometry focuses its row.
Checked objects and doors/windows hosted on a checked wall appear with red dashed
outlines. This preview does not change the active phase or document geometry.
The form retains its original document, history, workspace/recovery, selection,
view, layer and unit authority; a changed source or context refuses publication
until the editor is reopened.

The shared phase visibility set also removes hosted openings when their host
wall is demolished or absent from the selected alternative. Plans, native model
preparation, schedules and sheet consumers receive that set. Ordinary layer
visibility remains a separate presentation choice. Opening membership, original
wall geometry and baseline data are retained for the other alternatives.

These changes are source implementation only; no new build or runtime
qualification has run. A demolition choice never globally deletes or rewrites a
shared baseline room.

Explicit physical-wall phase discovery evaluates a named registry and
alternative without changing its saved selection. It admits actual registry and
member identities and model types, refuses overlapping ownership of physical
walls/rooms, retains other registries' saved selections, and records the exact
evaluated phase in source lineage. Context/plane discovery can return an actual
empty destination without inventing a wall. A separate room roster identifies
active and inactive original owners deterministically.

Current room values may remain qualified when only phase bookkeeping changes.
The resolver independently discovers the current active walls, admits original
and fresh evidence, and requires identical physical inventory, selected source,
context/plane, clear boundary and holes. Only the validated `semantic_phases`
field may differ. Changed physical support remains stale. This comparison does
not rewrite the original descriptor, transfer facts or authorize a repair.

Phase correspondence has a distinct report containing the explicit destination
selection and exact old-room roster. Missing, duplicate, malformed, foreign or
unresolved roster owners refuse; bounded analytical overlap uncertainty remains
visible. Other alternatives' rooms are not silently added to the roster. Its
ordinary payload is marked as an explicit phase evaluation and cannot borrow
ordinary destructive retain/retire acceptance. These pure APIs support the
separate phase-variant command described below. The analytical APIs themselves
do not introduce a persisted command dialect.

Creating or changing an alternative with affected room planes opens a dedicated
room review. Each listed floor shows the previous outlines and actual current
clear spaces. Every previous room must be shared unchanged or preserved in the
baseline and superseded in this alternative. Each current space must share an
exact unchanged owner, become a proposed room, or remain unclassified. New rooms
require explicit name, classification, factor and a picked interior point. No
overlap guess transfers facts. Baseline dimensions, relationships and opaque
references receive individual preservation confirmations.

One accepted envelope-33 event owns the registry change and every room decision.
Cancel publishes nothing. Original baseline owners and topology remain exact;
proposed owners receive new identities and actual detected geometry/source
lineage. Save changes does not select another alternative. Returning to baseline
retrieves original records; switching an already complete design does not force
new room choices. Name-only changes have no geometric review.

Acknowledged baseline references are omitted from shared visible IDs while their
reviewed bindings still refer to inactive baseline room/edge/vertex identities
in that same alternative. Position/style edits retain this association; explicit
retargeting can remove it. Original records are preserved for baseline display.
This visibility does not itself copy constraints or companions into a proposal.
Editing/replacing an existing changed proposed room and phase-aware constraint
solving remain follow-up work.

Preparation and replay share actual plane/room coverage and dependent discovery.
The GUI retains its complete source and workspace/edit context throughout modal
review. Document admission and history restoration verify source authority and
fresh identity lifetimes. Global destination-component admission prevents
near-identical requested elevations from duplicating one actual room area.
Native reader 84/extraction 82 is required throughout retained history. All phase
membership consumers now share the same model-role admission, including terrain,
measured linework and wall/roof joins.

`to_json` returns a detached canonical JSON object tagged `sketch.model_phases`,
version 1. Registry IDs, baseline IDs, alternative IDs and member IDs are sorted;
the active selection is persisted as a string or null. `from_json` rejects unknown
fields, unsupported versions, incorrect field types, blank or duplicate IDs,
unknown references, missing membership, conflicting proposal ownership and unknown
active selections. Repeated demolition of a shared object in different mutually
exclusive alternatives is valid. Names are display metadata, never identity.

Earlier focused C++20 tests exercise isolation, explicit comparison, deterministic
roundtrips, detached exports and malformed/conflicting input rejection. The
Windows desktop smoke workflow additionally proves that baseline and proposed
geometry reach both workspace canvases and each persisted plan, elevation, and
section sheet viewport; undo/redo restores the selected state, and a saved
alternative reopens with the same rendered output and fingerprint. Imported
Apex projects, physical printer behavior, and broader production certification
remain separate acceptance gates.
