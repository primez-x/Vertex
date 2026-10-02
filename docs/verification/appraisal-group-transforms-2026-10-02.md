# Existing appraisal group transforms

This checkpoint implements in-place rotation and reflection of a measured area
with its deductions, physical source walls, hosted openings, dimensions and
internal relationships. The modal and canvas share one numeric command builder.
The complete detached proposal is reviewed before one atomic history operation.
Stable owner, edge and vertex identities, classifications, factors, area totals
and unrelated annotation records are retained.

The baseline native check failed with an explicit refusal to rotate or reflect
an existing area with deductions or source walls. Initial curved-source checks
then exposed a tangent intersection cancellation error. The stable calculation
uses a machine-roundoff bound, rather than a larger geometry tolerance. An
independent review also identified historical replay incompatibility; exact
legacy calculation validation is required alongside the stable live path.

Native interaction coverage exercises numerical preview/Cancel, rotation-handle
preview, Escape, release before asynchronous projection finishes, the accepted
proposal matching the committed drawing, atomic Undo and concurrent-edit
rejection. Generated solid opening edges are compared analytically one-for-one,
allowing kernel traversal order/direction to change; identified measurement edge
identities and ordering remain fixed. Test clicks use the actual painted rotation
control, including object rotation and annotation avoidance.

Rendered previews were inspected in `artifacts/appraisal-transform-20261002/ui/`.
The numeric dialog shows original gray and proposed blue geometry with boundary
dimensions, uses the current units and hides interactive transform badges. The
canvas shows the complete proposed group, its angle and unchanged net area.

Verification passed: Release desktop build, full wall-measurement desktop checks
(including 32 combinations of units, source type, rotation/reflection and property
kind), boundary canvas, connected-wall canvas, batch cloning, wall-measurement
core, boundary edits, document, digest, storage, exchange and appraisal checks.
The frozen original tangent fixture preserves exact geometry/history through
fork, Undo/Redo and native formats 16, 17 and 18; its altered outline is refused.
Escape is checked both while a released projection is pending and after its
accepted completion queues a commit. Mixed movement passed with a trusted
reference-editing fixture after asserting unavailable sandbox import rejection;
this does not certify reference import.

The root resolved the required independent review findings and reviewed the
integrated command, UI protocol, persisted proof, historical arithmetic and
rendered previews. A legacy curved outline with original numerical drift requires
an explicit reviewed stable source refresh before the new exact group transform;
compatibility validation does not silently normalize it. Physically separated
legacy offset sources require correction before stable refresh.

Build logs, screenshots, remote-ref and matching offline installation evidence
are retained under `artifacts/appraisal-transform-20261002/`. The release audit
still reports 208 evidence gaps across 130 requirements and 10 gates. These are
not a count of absent features. This checkpoint is not Apex compatibility
certification or production acceptance. The user checklist has 325 individual
tasks; U324 covers the new behavior and remains untested by the user.
