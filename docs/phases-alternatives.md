# Model phases and remodeling alternatives

`ModelPhases` is a validated immutable semantic value supporting the model portion
of ARCH-MOD-010 and ARCH-MOD-011. It does not yet implement their full acceptance
criteria: view, calculation, schedule, sheet, undo-command, save-project and print
integration remain adapter work. Its JSON is a standalone exchange value, not an
automatic extension of the document persistence schema.

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

The optional active selection names exactly one alternative. `nullopt` selects the
unmodified baseline. `with_active` returns a new value and leaves its source intact;
callers can retain those values for an undo adapter. `state` resolves an explicit
selection and `active_state` resolves the saved active selection. Both return
independent maps ordered by entity ID.

`compare(left, right)` always takes explicit selections, even when one is the
baseline. It records both selections and reports only differing entities, ordered
by entity ID. Each side is an optional phase: absence is distinct from demolition.
Comparing does not switch the active alternative or change the shared baseline.

`to_json` returns a detached canonical JSON object tagged `sketch.model_phases`,
version 1. Registry IDs, baseline IDs, alternative IDs and member IDs are sorted;
the active selection is persisted as a string or null. `from_json` rejects unknown
fields, unsupported versions, incorrect field types, blank or duplicate IDs,
unknown references, missing membership, conflicting proposal ownership and unknown
active selections. Repeated demolition of a shared object in different mutually
exclusive alternatives is valid. Names are display metadata, never identity.

The focused C++20 tests exercise isolation, explicit comparison, deterministic
roundtrips, detached exports and malformed/conflicting input rejection. They do
not establish consistent rendering, downstream quantities, or printed output.
