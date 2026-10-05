# Physical wall space classification

Outcome: complete the physical-wall portion of PINC-002. The spatial class
palette must work on rooms made from actual walls, retaining each assignment
through supported source edits. Clear interior room space and appraisal exterior
floor measurement are distinct geometries. Assigning a class never supplies
missing ANSI observations or eligibility evidence.

First implement exact discovery from the selected active wall's complete drawing
context and effective elevation plane. Analytically node original baselines;
subtract their uncut joined physical wall footprints from bounded baseline
faces. Include open partitions, intrusive wall projections and isolated walls.
Nested closed islands exclude both the enclosed space and surrounding material
from their parent's clear area, while retaining the separate inner room. Keep
analytical lines/circles and validate outer loops and holes. Hosted openings keep
virtual continuous room divisions; doorway threshold area is not silently added
to clear room area. Eye masks affect presentation, not calculation inputs.

Retain deterministic component order and full wall-source provenance, including
thickness, effective plane, baseline and active phase. A baseline face can split
into several clear spaces. Cache discovery per captured document state rather
than running Boolean operations for every pointer move. Invalid or unsupported
geometry gets a repairable diagnostic, never a substitute centerline area.

Then connect discovery to a typed, validated source consumer. Store holes in a
representation understood by authoritative calculations and output, reuse
existing assignments, and track source additions/removals as well as edits.
Preserve explicit classifications, facts and presentation through supported
updates. Withhold stale totals; topology changes require explicit reassignment.
Creation/reclassification/clearing and dependent updates must be atomic and
reversible, with save/reopen and immutable-source UI guards. The older anonymous
centerline room detector is not qualification of this workflow.

Ownership: geometry worker owns the new detector header, implementation and
focused core tests. Root owns CMake, consumers, UI, format/version contracts,
documentation, all native jobs and Git. Writers return frozen before native jobs.

Verification: independent known-answer rectangle, unequal thickness, T/X
partitions, interior stubs, isolated walls, curved room, nested islands,
disconnected spaces, open chains, effective elevation, semantic phase and hidden
layers. Source direction/order must preserve geometry and detection must not
mutate the document. Subsequently qualify actual palette pointer/drop actions,
assignment retention, source edits, Undo/Redo, save/reopen, stale-source reporting
and output. A passing detector checkpoint does not establish the UI workflow or
complete PINC-002, PINC-012 or the unified production gate.
