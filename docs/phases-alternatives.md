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

`to_json` returns a detached canonical JSON object tagged `sketch.model_phases`,
version 1. Registry IDs, baseline IDs, alternative IDs and member IDs are sorted;
the active selection is persisted as a string or null. `from_json` rejects unknown
fields, unsupported versions, incorrect field types, blank or duplicate IDs,
unknown references, missing membership, conflicting proposal ownership and unknown
active selections. Repeated demolition of a shared object in different mutually
exclusive alternatives is valid. Names are display metadata, never identity.

The focused C++20 tests exercise isolation, explicit comparison, deterministic
roundtrips, detached exports and malformed/conflicting input rejection. The
Windows desktop smoke workflow additionally proves that baseline and proposed
geometry reach both workspace canvases and each persisted plan, elevation, and
section sheet viewport; undo/redo restores the selected state, and a saved
alternative reopens with the same rendered output and fingerprint. Imported
Apex projects, physical printer behavior, and broader production certification
remain separate acceptance gates.
