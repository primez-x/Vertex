# Persistent constraints for measured strokes

This delivery extends the precision editor to open, closed, crossing and
retraced measured strokes. It is an internal checkpoint within the full Vertex
production plan, not Apex parity or ANSI certification.

## Outcome

Author fixed chord length, physical arc length, horizontal, vertical,
coincident, parallel, perpendicular and fixed-anchor relationships through the
existing dialog. Connected edits retain stable identities, original receipts,
entered quantities, signed arc sweeps and unrelated metadata. Single edits,
pure-stroke movement and rigid transformations respect persisted relationships.

The solver resolves both endpoints and deduplicates revisited vertex IDs. It
does not impose closed-area winding on loose strokes. A simultaneous vertex
batch validates final geometry after all targets are installed. Typed authored
edits and rigid transforms precede any additional solved batch; reflecting an
arc must reverse its sweep through the retained transform operation.

The document reconstructs eligible source-derived areas before sealing the
preview and committing. Only previously current, unambiguous, unauthored
consumers qualify for automatic reconstruction, under the persisted active
design phase. Group members and outer lineage update together. Appraisal facts
remain recorded observations; stale or ambiguous geometry withholds totals.
Final hard-relation validation runs after reconstruction. Apply, Undo, Redo and
reopen cover the complete event.

## Ownership and compatibility

The batch worker owns linework model/replay and its focused tests. The core
worker owns constraint resolver, integrity, authoring and analysis. The desktop
worker owns dialog adapters and actual widget/action regression coverage. Root
owns document command authority, consumer reconstruction, file guards, canvas
routes, generators, integration, native execution and Git. An independent
read-only advisor challenges command/replay and source-integrity contracts.

Schema/replay v5 adds the batch operation, retaining v1-v4 input semantics and
no-op dialects. Command envelope 11 composes measured completion independently
with existing wall/exterior completion. Native format 35 and extraction version
33 protect retained v5 history, envelope 11 and relation-only stroke bindings.
Future models remain opaque/read-only; known unrelated relations still validate.
Copy remaps only the schema-owned batch targets, never vendor strings.

## Verification

Root observed the pre-change registered batch failure. Verify final-valid
endpoint swaps and translations with invalid sequential intermediates; all
relations across strokes, walls and boundaries; terminal and revisited vertices;
exact authored receipts; reflected arcs; incompatible/invalid/stale requests;
individual/grouped consumers and appraisal under active phases; one-step
history and save/reopen; strict command round trips and retained reader floors.
Exercise real dialog/context actions and inspect captured rendering. Run the
affected existing constraint, measured-line, clipboard and source-consumer
checks, then `git diff --check`. Freeze all native writers for every native
build/execution. Only observed results can support delivery claims.
