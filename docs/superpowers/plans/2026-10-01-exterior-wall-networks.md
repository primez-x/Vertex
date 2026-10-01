# Exterior measurements from partitioned wall layouts

Outcome: selecting any connected straight wall can identify the unique exterior
perimeter even when the layout contains partitions. The calculation uses the
exterior faces of perimeter walls, preserves their source provenance, and does
not count interior walls as additional building area. Review displays the
included perimeter, excluded walls, and proposed area before creation.

Core ownership: wall_measurement.hpp/.cpp and wall_measurement_tests.cpp.
Desktop ownership: main_window.cpp and wall_measurement_desktop_tests.cpp.
Root owns integration, documentation, review, build, packaging, and Git.

Recognize a planar straight wall network using analytical contact geometry and
face traversal. Support unsplit and split T-junctions, chords, crossing interior
partitions, nested interior loops, and concave exterior outlines. Preserve the
existing strict exterior derivation and version-one source schema. Return only
whole perimeter wall identities; partial-source exterior contributions require
an explicit diagnostic until a separately versioned format supports them.

Accept disconnected interior loops or lines only when strictly contained by the
unique exterior outline. Reject disconnected outside or incomparable exterior
outlines, duplicate/overlapping walls, non-simple exterior boundaries, curves,
and unsupported precision. Do
not pick an arbitrary largest cycle. Single-wall discovery isolates placement,
phase, and elevation; multiple-wall selections remain explicit candidate sets.
Refresh recomputes only the stored perimeter sources, avoiding unexpected
changes to a previously accepted calculation.

Verification: known metric and square-foot areas, thickness offsets, shuffled
and reversed walls, exact source preservation, excluded partitions, ambiguous
layouts, cancellation, repeated creation, stale refresh, and save/reopen.
Independent review challenges calculation correctness and source isolation.
Verify the actual packaged mouse workflow and expose remaining qualifications
without treating this checkpoint as full Apex parity.
