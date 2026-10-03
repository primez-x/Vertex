# Live measured-stroke precision constructions

The real Measured lines D dialog must author all eight supported analytical
construction methods: length/heading, rise/run, relative turn, world-coordinate
endpoint, arc chord/angle, chord/height, chord/length and start-tangent/length/sweep.
It must also support an exact starting point. Entered units and expressions,
current pen, analytical end tangent and ordinary per-edge history stay authoritative.
Invalid input remains correctable in the dialog; Cancel and stale context cannot
commit. No new geometry source, approximate curve or project-format change is
planned.

Reuse the established precision fields, parsing and receipt replay. Open strokes
must retain their own semantics, including retracing and crossing; a synthetic
closed-boundary seed must not change their admissibility or their previous tangent.
Preferences update only after actual admission and respect changes of input units.

The implementation worker owns the shared dialog, desktop entry handler and
dedicated native regression. Root owns registration, integration review, docs,
checks, source packaging, Git and installed delivery. Native writers freeze for
every build and executable check. Verify an actual missing-mode failure first,
then exercise every mode in both input systems, curved-edge relative turns,
invalid/cancel/stale dialogs, Undo/Redo, branching, save/reopen and analytical
area/output consequences. The full production and compatibility goal remains open.
