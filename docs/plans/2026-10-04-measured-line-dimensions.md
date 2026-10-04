# Saved dimensions on measured lines

Extend the existing placed-dimension workflow to supported receipt-backed
measured strokes. This fills an existing 2D precision/appraisal drawing gap
within the full production goal; it does not establish Apex parity or ANSI
certification.

Use existing dimension versions and stable owner/edge/vertex bindings. Resolve
physical segment length and outgoing tangent angle from authoritative measured
replay. Open endpoints and revisited stable vertices remain valid. Strokes do
not acquire enclosed-area semantics; even a closed stroke must become an area
before accepting an area dimension. Future models and bindings remain opaque
read-only; malformed known data and missing references refuse the transaction.

The existing creator offers supported measured sources and edge pairs. Keep
labels editable with the established presentation controls. Open-line automatic
placement chooses a visible side without claiming an exterior winding; closed
area placement retains its existing exterior rule. Canvas, printable sheets,
SVG and PDF share the same resolver and projection. Copy/clone remap associated
dimension references and retained metadata independently.

Ordinary coordinate editing updates the measurement while retaining the saved
label position. Explicit rigid stroke transformation reconstructs attached
dimension positions from source once in command11 completion. Reject overlapping
supplements; do not transform an already transformed candidate again. Full
preview, Apply, Undo/Redo and reopen cover the complete event. Native format36
and extraction34 protect new measured-dimension owner semantics across retained
history and entity-only files; no SQLite table change is intended.

Mixed typed boundary groups verify each measured stroke against their shared
rigid transform before core position reconstruction. Mixed ordinary wall/area
groups replay the original stroke under that same transform, reconstruct owned
positions from source and retain whole-transaction constraint admission. A
subset solve must not reject a valid group whose selected anchor also moves.

The core worker owns dimension resolver/header, boundary integrity and their
existing tests. The desktop regression worker owns new measured-dimension
desktop/storage fixtures. Root owns Document completion, desktop paths, format
guards, CMake, generators, native execution, integration and Git. A read-only
advisor challenges admission and transform/reader protection. All native
writers freeze throughout every build or native execution.

Observe RED with a real open measured stroke. Verify physical arc versus chord,
terminal/revisited bindings, future/malformed owners, chosen angle pairs,
manual/automatic placement, direct and connected edits, explicit transforms,
independent clipboard graphs, Cancel, atomic history, source metadata,
save/reopen, reader floors and actual canvas/printable exports. Run affected
existing dimension, boundary, measured-stroke and constraint checks. Inspect
rendered captures and finish diff/source-kit/requirement contract checks, scoped
commit, push and exact remote-ref verification. Track unverified full-release
requirements as open gaps.
