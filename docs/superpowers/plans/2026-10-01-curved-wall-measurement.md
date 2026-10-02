# Analytical exterior measurements from curved walls

Outcome: the existing exterior-wall measurement command accepts a unique closed
shell containing straight and circular walls. It measures the exterior faces at
each wall's actual thickness, retains analytical curves, and supports appraisal
qualification, source-change detection, refresh, undo and native persistence.
This closes a production requirement gap; it is not full replacement signoff.

Core worker owns wall_measurement.hpp/.cpp and wall_measurement_tests.cpp.
Desktop acceptance worker owns wall_measurement_desktop_tests.cpp. Root owns
MainWindow integration, documentation, generators, builds, Git and delivery.
An independent advisor challenges geometry and calculation integrity once the
integrated implementation is ready. Writers do not run concurrent builds.

Recognize exterior faces using analytical intersections, arc parameter splitting
and endpoint tangents. Preserve exclusions of interior partitions and connected
branches, explicit context isolation and ambiguity refusals. Do not substitute
faceted strokes or an arbitrarily selected largest cycle for the source shell.

Offset lines and circles analytically, intersect neighboring offset supports,
retain signed curvature when reversing orientation, and validate the completed
outline. Exact source currentness must include curvature, not just chord ends.
Unresolved, overlapping or unbounded geometry must produce a specific diagnostic
without editing walls, the current boundary or qualified quantities.

Creation must feed curved edges to the existing analytical boundary authoring
commands rather than rebuilding them as straight chords. Refresh must retain
boundary and annotation identities through the typed redefinition path.

Verification: failing regression first; known analytical area and perimeter;
mixed line/arc joins, per-wall thickness, reversed/shuffled sources, partition
exclusion and ambiguous-layout refusals; same-chord curvature edits invalidate
the source; actual native review/create/refresh, dimensions, one-command undo,
save/reopen and PDF. Run affected appraisal checks and repository contracts,
inspect captures, then commit, push and verify the remote ref. Preserve unrelated
desktop_smoke.cpp and temp.txt work. Keep full production acceptance open.
