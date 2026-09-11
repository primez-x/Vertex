# Workspace history record, version 1

This describes the internal `workspace_history` record codec, now used by the
recovery-aware SQLite v4 save/load routes. Core workspace restoration consumes
this record after aggregate revalidation, and the guarded desktop save path uses
the acknowledgement boundary. Live desktop asynchronous saving, autosave and
restart recovery remain separate integration work; reusable queue and scheduler
contracts are documented independently.

The JSON envelope has exactly these fields: `version`, `replay_version`,
`document_history`, `events`, `navigation`, `retired`, `workspace_epoch`,
`edited_generation`, `checkpoint_generation`, and `extensions`. Both versions
are currently `1`. Counters are unsigned 64-bit integers; booleans, negative
values and floating-point substitutes are rejected. Counter ordering is not
inferred from numeric magnitudes or event count. `extensions` is an object
whose number representations and nested values are preserved.

Active input is supplied separately, as the archive's `boundary_active` record.
It is required when history ends with an active session. Historical inputs and
retired views belong to this history record rather than separate competing rows.

`document_history` contains `baseline` and `events`. The baseline has
`document_id`, `baseline_revision`, `source_digest`, `undo_stack` and `redo_stack`.
Its stacks contain retained Document revision numbers. Each Document event has
`event_id`, `sequence`, `kind`, `before_revision` and `after_revision`, with kind
`edit`, `undo` or `redo`. The complete projection must match retained Document
history after the baseline.

Each lifecycle event has `event_id`, `sequence`, `kind`, `before_revision`,
`after_revision`, `target`, `session` and `input`. Unused optional payloads are
explicitly null. Kinds are `document_edit`, `boundary_activate`,
`boundary_discard`, `boundary_finish`, `undo`, `redo` and `clear_redo`.

A target is `{ "kind": "baseline", "id": revision }` or
`{ "kind": "event", "id": event_id }`. Baseline IDs identify original commands,
not the physical snapshot revisions restored by a later navigation operation.

Activation session metadata has `source`, `identity_namespace`, `mode`,
`initial_counters`, `revised_from_namespace` and `revised_from_finish_event_id`.
Source fields match `boundary_active`: document identity, revision, authoring
digest and complete property/building/floor/layer context. Modes are `draw_first`
and `define_first`. Initial counters use the checkpoint's four next-ID fields.
The two revision-provenance fields are both null or both populated.

An input has `owner_event_id`, `value`, `pointer_override`, `status` and
`finish_event_id`. The first owning event stores the full versioned
`boundary_active` envelope in `value`, and its owner ID equals its own event ID.
Subsequent references set `value` to null and point directly to that earlier full
owner. Forward references and reference chains are invalid. Decoding reconnects
references to the same immutable input object.

`pointer_override` has `present` and `point`. A false `present` requires null
`point` and inherits the owner's pointer. A true `present` uses either a finite
`[x,y]` or null to explicitly clear the pointer. Status is `active` or `retired`;
retired input names its original finish event, while active input has a null
finish ID. Retired views never authorize finishing or silently become active.

`navigation` contains `undo_stack`, `redo_stack` and `operations`. Both stacks
contain typed targets. Operations are objects with `event_id` and `kind`, where
kind is `document_edit`, `boundary_activate`, `boundary_discard` or
`boundary_finish`. Duplicate operation IDs are rejected.

`retired` is an array of `{ "identity_namespace": namespace, "input": reference }`
objects. Namespaces are unique and each input is a reference to a prior full
owner. Slot replay must reproduce the complete retired set and active input.

Known objects reject missing or extra fields. Unknown positive outer versions,
or unknown positive versions in an embedded input/checkpoint, retain the entire
bounded history envelope opaquely. They do not yield partially editable history.
Wire limits run before copying or decoding; aggregate action/work limits precede
canonical owner replay, followed by complete document, lifecycle, source,
finish-delta and slot validation. No codec result grants filesystem ownership
or permission to overwrite another project's file.

Encoding also applies the decoder's combined document/input/wire admission
before returning an envelope. This includes schema keys and JSON values that
typed-state estimates alone do not count. A record that fits typed admission
may therefore be rejected during encoding; it must not be emitted only to fail
the same limits on reopening. These conservative engineering budgets are not
memory-use or timing guarantees.
