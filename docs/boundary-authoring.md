# Precision boundary authoring

The native desktop now uses the shared session and sealed commit service for
both drawing modes. This remains an internal integration in the accepted
production plan; keyboard parity and persistent unfinished sessions are incomplete.

## Two drawing workflows

Define First chooses an area code/classification before drawing. The official
[Apex v7 Define First tutorial](https://www.apexwin.com/support/ApexSketchv7/ApexSketchv7-DefineFirst.pdf),
pages 5–9, describes F4 opening Define Area, applying the selected classification,
and Enter anchoring the cursor. Its default entry flow separately anchors the
line and places its dimension; automatic placement is an option. A closes the
area. Define First must not be implemented as solving dimensions on an existing
chain of walls.

Draw First begins with measured linework and subsequently defines/classifies
areas, as described in the official
[Apex v7 Draw First tutorial](https://apexwin.com/support/ApexSketchv7/ApexSketchv7-DrawFirst.pdf).
Both workflows retain efficient keyboard input alongside pointer input. The
modern workspace can expose them without copying the old menu arrangement or
requiring an application restart to switch methods.

Creating an area from existing geometry is a distinct required workflow. It
needs explicit source relationships and geometry ownership; it is not the
definition of the legacy Define First mode.

## Shared engine contract

One non-UI authoring session owns the semantic cursor, pen state, analytical
segments, classification and dimension-placement state. Both workflows will
share exact quantity input, distance/direction, rise/run, relative turns, curves,
closure and validation. Alignment and desktop integration remain required.
Canvas pixels remain a projection of metre
coordinates, never measurement authority.

Local correction of unfinished input must be distinguished from document
undo/redo. A committed area is one reversible command tied to the captured
document identity, revision and drawing context. Cancelling or rejecting input
must preserve the document and its calculated totals. Persisted unfinished
sketches, reopen/redefine, stable vertex/segment identities, exact entry receipts
and source-edge provenance remain part of the full production requirements.

The architecture review requires stable child identities and strict validation
before connecting the new session to the desktop. Existing v1 anonymous segment
arrays must not acquire a new coordinate meaning, silently lose unknown
metadata, or masquerade as stable references. Exact Apex keyboard and
dimension-placement parity still needs recorded acceptance fixtures; these
tutorials alone are not certification.

## Accepted identity and command direction

Identified boundaries add `boundary_model_version: 1` and inline `segment_id`,
`start_vertex_id`, and `end_vertex_id` fields to the canonical `segments` array.
The existing `start`, `end`, and `sweep_radians` values remain the only coordinate
authority. IDs are assigned independently of geometry. Repeated vertex IDs
must have consistent coordinates, and committed areas must be simple, closed
analytical cycles. No decoder may skip malformed members and calculate from
the remainder.

The codec must preserve unknown entity, property, and segment metadata. A
legacy identity upgrade is explicit and undoable, preserves coordinates, and
assigns IDs in stored topology order. It must not snap nearly joined vertices
as an undocumented side effect. Moving a vertex retains identity. Reversal
retains segment identities while reversing order, endpoint roles, and arc
sweep; split/merge explicitly retires and creates identities and updates
dependent references in the same command.

Project storage uses format v2 whenever retained history contains identified
boundaries, while continuing to read legacy v1 files. Entity versioning alone
does not prevent an older binary from stripping new identities. A sentinel in
the current drawing is insufficient because undo can restore a head without
that sentinel while identified history remains. The storage version therefore
guards the complete retained history without rewriting old revisions.
Unsupported boundary versions must preserve their data and make the affected
project read-only. Actual older-reader and migration tests are required; the
codec alone does not satisfy this compatibility gate.

`BoundaryAuthoringSession` owns stable draft identities and semantic local
undo/redo. Cursor movement is ephemeral. Define First requires classification
before anchoring and explicit dimension placement after each edge unless
automatic placement is enabled. Draw First supports unclassified linework and
pen-up relocation, then closed-cycle selection and classification. Lines and
all supported analytical arc constructors belong in this session.

Commit previews must bind the complete source snapshot, document identity,
revision, organization context, normalized actions, generated IDs and displayed
result. Apply recomputes and compares the preview before one atomic command.
Exact construction receipts key by segment ID and describe, rather than
override, the resulting geometry. Placed dimensions are separate stable
entities referring to the boundary and segment IDs. Unknown affected receipt
or reference semantics block an edit rather than being silently erased.

The durable receipt value types, strict JSON codec and deterministic replay
kernel belong below both Document and the authoring session. They depend on
precision geometry and JSON, without depending on Document, the desktop or
project organization. Document uses that kernel to validate raw commands and
restored history; the session uses the same construction implementation. A
separate commit adapter resolves organization context and binds the complete
preview above these libraries.

The persisted `boundary_authoring` envelope is confined to supported explicit
identified boundaries. Its schema version and replay version are separate;
known v1/v2 records retain the anchor and exactly one input receipt per canonical
segment ID, with a dedicated opaque `extensions` object. Replay must reproduce
the ordered topology and analytical coordinates exactly. Current classification,
style and drawing context remain editable metadata, not geometry inputs. The
preview binds their committed values without turning them into permanent locks.

Persisting this envelope on a supported identified boundary requires project
format v3 anywhere that qualified owner occurs in retained history. An
entity-local version cannot protect against a v2 writer
that ignores receipt semantics. A retained, hash-pinned actual v2 reader must
reject v3 files; the new reader must preserve them through save/reopen and
retain the v3 guard after deletion or undo. Known malformed receipt data
rejects; unsupported positive versions preserve opaque data and keep the whole
retained document read-only. The storage guard and lower receipt codec are
implemented; integrated validation and actual older-reader evidence are
tracked separately from production acceptance in `implementation-status.md`.

Owner qualification precedes decoding, format selection and provenance display.
A same-named property on an anonymous legacy boundary or unrelated entity is
opaque vendor metadata under its existing format. It must not become an input
receipt or a new read-only restriction. Legacy promotion must reject such a
collision until it is deliberately resolved. An unsupported boundary model
also keeps the property opaque; that model already makes retained history
read-only, and its apparent receipt version is never decoded. Known independent
entities still receive full validation.

Initial receipt-bound editing will preserve boundary ID/type/model version,
ordered segment/vertex identities, analytical geometry and the complete
receipt envelope. Classification, style, organization references and unrelated
metadata remain editable under their existing validators. Whole-boundary
deletion and exact undo/redo remain available. Attaching or removing receipts
on a surviving boundary, geometric edits and reversal require explicit typed
derivation semantics; rewriting original input expressions is not an acceptable
substitute. This temporary staging restriction must be replaced by the planned
move, dimension-edit, reversal, split and merge derivations before production.

Persisted unfinished work needs a versioned draft/recovery record that retains
mode, stable identities, normalized actions, local history position, anchor,
cursor, classification, and pending dimension. Finalization removes that record
and creates the completed boundary and dimensions atomically. Discarding an
already persisted draft is a document/recovery edit; cancellation of an
unpersisted session is not.

The separate geometry-to-area service must declare source-edge relationships
and follow/detach behavior. It cannot silently reclassify the source wall or
create an unexplained second coordinate authority.

## Desktop integration contract

The two workspace tabs share one authoring session; switching tabs with the
same drawing context preserves unfinished work. Workspace and drawing method
are independent choices. Define First obtains the area classification
before anchoring. Draw First accepts measured linework before its later
classification. Neither method substitutes a hard-coded classification for
that workflow. Enter follows the current phase, including placement of the
closing edge's dimension, rather than bypassing an unresolved dimension.

Both canvases receive copied analytical draft geometry, dimension labels,
anchor/pen markers and phase instructions from the session. They do not own a
second list of authoritative points. Draft corrections use local undo/redo;
committing the accepted chains produces one document command, and document
undo removes the boundary and its dimensions together. Replacing the document
or context invalidates a captured commit preview and must resolve unfinished
work explicitly through preservation, completion or confirmed discard.

Transient boundaries, wall previews, cursor lines, markers and draft labels
must be excluded from print and export. Screen rendering must still display
analytical curves and keep labels readable at different display scales.
Output equivalence tests compare a committed scene before and after adding
transient overlays; event tests distinguish draft undo from document undo.
Output also excludes the editing grid and selection highlighting. Non-wall
outline weights are presentation sizes: screen pixels for editing and a
quarter-millimetre stroke for current printed output. Wall thickness remains
model geometry. Committed dimension text comes from the referenced analytical
segment and follows the selected display units; display formatting does not
change its stored source or placement.

Cursor status, rubber-band endpoints and pending dimension labels use the same
effective canvas point as a click: quarter-metre grid snapping when enabled,
raw model coordinates when disabled. Changing snapping recomputes the last
pointer position without requiring mouse motion. Only the active workspace
updates the shared draft pointer when both canvases receive the setting change.

Pointer coordinates are already model-space positions. Connecting them must
not invent a typed distance expression, round through a decimal quantity, or
reconstruct the target through trigonometry. A direct endpoint construction
uses the schema-v2 `line_to_point` receipt. Schema v1 retains its original eight
construction kinds. Both implement replay version 1; unknown positive replay
versions remain opaque and read-only. The existing precision-entry constructors
remain available for measurements supplied as lengths and angles. Both envelope
schemas use project storage v3; an actual schema-v1 reader must preserve schema-v2
projects read-only.

## Next implementation gates

The complete unfinished-session persistence and recovery contract is documented
in [Durable boundary recovery](boundary-recovery.md). Its archive and desktop
integration are still required; current session drawing does not imply recovery
is implemented.

1. Strict codec and metadata-preserving identity upgrade/reversal, with line,
   arc, malformed input, duplicate identity and unknown-version tests.
2. Document state/transition validation through raw commands, restore,
   undo/redo, reference changes, and the older-writer compatibility guard.
3. Both mode state machines, all line/arc input forms, exact receipts,
   dimension placement, local history and sealed atomic commit.
4. Desktop keyboard/pointer integration, unfinished save/reopen and explicit
   existing-geometry area creation, followed by end-to-end output fixtures.

These are internal construction gates within the unchanged full production
scope. No gate or current helper is Apex parity certification.
