# Physical wall corners in plan drawings

Outcome: two straight walls meeting at an unambiguous endpoint in the same
property/building/floor/layer/phase and elevation form a thickness-aware corner.
Their source baselines, entered lengths, opening stations and appraisal source
boundaries remain unchanged. Each wall retains a closed physical footprint for
picking/fill; a separate derived stroke path omits the internal shared cap.

Reuse the opening-aware wall footprint builder. Intersect analytical offset
faces using each wall's own thickness and endpoint orientation. Bound unstable
acute/short corners and retain capped geometry when a join cannot be constructed
safely. Curved and multi-way joints remain visible gaps for subsequent work.
The document wrapper groups geometric endpoints without inferring editing
constraints or changing persisted architectural join relationships.

Ownership: the geometry worker owns hosted_opening_geometry.hpp/.cpp and its
core regression; the document test worker owns document_wall_plan_tests.cpp.
Root owns the shared document projection, canvas/desktop integration, build
registration, rendered checks, documentation, package generation and Git.

Verification: exact unequal-thickness corners, endpoint reversal, both winding
directions, opening cuts, full cuts, context separation, ambiguity and numeric
guards. Check retained picking polygons and pixel output without an internal
seam. Recheck connected drag/cancel/history, opening editing, wall drawing and
exterior appraisal calculations. Inspect the delivered runtime.

Coordinated full-depth horizontal plans share this projection. Clipped/oblique
solid views, native DXF host-block validation, 3D solid corner construction and
multi-way/curved joints still require their own coordinated implementation.
They remain production gaps, not exemptions from the final acceptance gate.
