# Define First dimension placement

Define First now lets a pending dimension use H for horizontal text, V for
vertical text, and a stationary Space tap to omit the label while retaining the
measured side. Space with a pointer gesture still pans. Text entry, modifiers,
auto-repeat, focus loss, stale source contexts and navigation cannot accidentally
place or omit a dimension. These controls implement the documented label step in
the official Apex v7 Define First tutorial; the entire tutorial and physical
keyboard parity remain unqualified.

The existing dimension presentation model is authoritative. Placement records
all six presentation fields, including visibility and rotation, in semantic
Undo/Redo, closed chains and checkpoint schema 3. Unstyled checkpoints retain
schema 1, and typed chord checkpoints retain schema 2 unless presentation is
also present. Aggregate recovery recognizes all three, including Redo-only
presentation. Future checkpoints retain the whole enclosing ledger opaquely.
Native and extraction container versions are unchanged.

Reopening an undone vertical or omitted placement restores a visible vertical
pending label. Redo restores its recorded visibility and orientation. A pending
orientation choice that has never been placed remains a transient UI choice.
Committed labels, measurement geometry, area calculations and exports use the
same document state.

## Evidence

The Release build passed. Eleven affected CTest suites passed in the integrated
run: desktop workspace recovery, boundary canvas, boundary workflow, boundary
dimensions, authoring session, authoring recovery, recovery resource limits,
workspace history, recovery ledger, boundary commit and project exchange.
Project storage passed separately after correcting its new test fixture,
giving twelve passing affected suites across the two runs.
The final terminal records and consolidated results are under
`artifacts/df-dimensions-20261003`; the original failing logs are preserved.

Behavioral RED evidence covers unsupported checkpoint presentation, stationary
Space omission and aggregate recovery refusal. Root inspected fresh horizontal,
vertical and omitted-label captures. Native storage checks exercise real archive
restoration, exact raw records, Redo-only presentation, discard/Undo recovery and
opaque future input without modifying source bytes. Extraction covers ordinary
and recovery-copy roles. Historical/retired input is also covered by the
workspace history record checks.

The storage fixture originally assumed row order, then confused a discarded
lifecycle input with a retired slot. It now compares records by identifier and
undoes the restored discard to recover its exact active checkpoint. Additional
fixture build corrections fixed SQLite helper usage, its restore-library link
and lifecycle comparison; no application admission guard was weakened.

An independent read-only review checked geometry and presentation persistence,
canonical replay, future-version handling, interaction guards and fresh glyph
captures. Installed runtime observation and delivery metadata are recorded
separately; development-host results do not qualify a clean machine or a
network-denied installation.

## Remaining scope

The left Details tab already exposes GLA, area dimensions, deductions and the
ANSI Z765-2021 evidence profile. Final normative validation remains open; these
are not ANSI-approved results. Full Apex parity and compatibility certification
remain open. User checklist U385–U388 covers these label controls; all human
results remain Not tested.

Fresh draft captures also expose overly precise derived metric label formatting
and an overview that does not include unfinished geometry. A previous installed
runtime observation found a path-sensitive OCCT/WIC image-export failure using
a long temporary root; the identical executable passed with a shorter root.
The exact staging-path cause is not yet proven. These gaps remain visible rather
than being claimed fixed by this dimension-presentation checkpoint.
