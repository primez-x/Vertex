# Interior wall junctions

Outcome: straight partitions meeting the interior of another wall, crossing
walls and three-way endpoint junctions display a continuous physical outline.
Preserve complete per-wall material polygons for picking and fill, all stored
baselines, opening stations and calculation inputs. Remove internal strokes
only within context-qualified material contact; retain actual exterior faces
and exposed opening jambs. Ordinary two-wall miters retain their existing guards.

Use analytical line/polygon clipping in model coordinates, with the existing
geometry tolerance, finite-value guards and deterministic ownership of duplicate
exterior strokes. Do not subtract bounding boxes or extend a wall through an
opening. Curved junction construction remains a required production gap.

Ownership: the junction worker owns the new core clipping module and its tests;
the native join worker owns architecture.cpp and wall_join_tests.cpp. Root owns
document contact detection, canvas regression, CMake, documentation, packaging
and Git. Native joined solids must admit real straight T/X contact while
continuing to reject disconnected and vertically separated source solids.

Verification: exact T/X/three-way silhouette spans, reversed and unequal-width
walls, openings, placement separation, numeric guards, retained physical picking
and source immutability. Verify real fused solid volumes and opening voids.
Inspect screen and printable output, draw a partition with actual mouse input,
then deliver a verified self-contained checkpoint without claiming final parity.
