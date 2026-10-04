# Curved survey calls

Fill the confirmed straight-only survey authoring gap within the accepted 2D
measurement scope. This is practical curved-parcel authoring; it does not certify
native Apex module compatibility or legal-survey accuracy.

Retain existing quadrant-bearing line calls and their version-1 reports exactly.
Add explicit curve calls with chord bearing and chord length, using signed sweep,
signed height or measured arc length plus direction. Reuse analytical circular
geometry for endpoints, perimeter, acreage and self-intersection checks. Preserve
entered expressions and exact length receipts. Never tessellate curves into
measurement lines or adjust closure silently.

Use one shared core parser/rebuilder for native Calculate, report reopening and
boundary correction. Derived report vertices and totals are recalculated from
retained input. New curve reports and source envelopes have explicit versions;
retained history must require a reader that understands those semantics.
Unknown versions remain preserved with edit protection.

The native workflow must preview curves, create and correct actual curved
boundaries, keep compatible row identities and dependent dimensions, preserve
Undo/Redo and save/reopen, and explain invalid calls without mutation. An
explicit final-endpoint adjustment is unavailable for a curved final leg because
it would alter its defining geometry. A separate closing straight leg remains
an explicit option.

Ownership: core worker owns survey contracts, canonical parsing and corrections;
desktop worker owns Survey traverse and its native regression. Root owns reader
and extraction versions, build integration, documentation, all native execution,
Git and installation. All native writers freeze during builds or observation.
Read-only advice challenges geometry, closure and compatibility before release.

Observe the current native five-field Calculate failure before implementation.
Verify literal rectangle-plus-semicircle area/perimeter, both directions, major
arcs, other supported constructions, open/invalid/self-intersecting calls,
version-1 preservation, precise retained input, create/correct/Undo/Redo/reopen,
and refusal of reader downgrades including deleted and undone history. Inspect
actual rendered preview and canvas captures. Keep remaining production gates
visible after this checkpoint.
