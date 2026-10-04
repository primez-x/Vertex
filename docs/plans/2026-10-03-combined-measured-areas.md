# Combine detected measured regions

Vertex must support the Apex Draw First workflow of assigning one area to
several adjoining detected regions. The official Draw First tutorial's pages
8–9 show four regions becoming one classified polygon with the shared dividers
removed: https://apexwin.com/support/ApexSketchv7/ApexSketchv7-DrawFirst.pdf.

The review dialog provides Combine selected and Separate selected actions,
an explicit shared classification, and one preview and Apply operation. Existing
combined definitions remain whole. Cancellation and invalid proposals change
nothing. Per-face preview rows resolve their actual resulting owner without
counting a combined result more than once.

The analytical graph cancels only opposite uses of the same derived edge,
requires edge-connected membership and one complete simple exterior, and keeps
curves exact. Disjoint regions, point-only contacts, overlapping nested gross
outlines and enclosed holes require an explicit different area decision. This
does not remove broader geometry requirements from the production goal.

Saved provenance records every original member's source intervals, including
cancelled interior seams. Source validation matches members uniquely and
injectively. Geometry, exterior lineage and complete group metadata refresh
together. Missing, hidden, changed or ambiguous dependencies withhold totals.
Single-face replacement cannot implicitly change group membership. Clipboard
operations must include and remap all sources.

Pasted measured graphs start beside existing geometry with one grid interval
of separation. The copy remains selected for placement. A new plain source-
derived owner must remain editable through its copied source lines; placement
must not invent a locked transformation proof. Previously saved proof metadata
remains preserved. Ordinary clipboard content keeps its existing placement.

Native format 33 and extraction 31 carry the group contract through retained
Undo/Redo history. Unknown group schema bytes are preserved with read-only
handling. Vendor fields on unrelated owners do not claim these semantics.

Ownership: graph implementation; source provenance; desktop review and copying;
storage/exchange versions are separate exclusive delegate areas. Root owns the
definition adapter, document admission, integration, native execution and Git.

Verification: observed missing behavior first; analytical straight/curved and
invalid unions; member-source validity, staleness and budgets; native review
preview/Cancel/Apply, exact reuse, Details GLA, Undo/Redo, save/reopen and visual
captures; retained-history format downgrade refusal; focused package checks.
These checks support this feature only. Final ANSI normative validation, full
Apex parity and production acceptance remain separate requirements.
