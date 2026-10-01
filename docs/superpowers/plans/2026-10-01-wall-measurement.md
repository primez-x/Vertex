# Exterior wall measurements

Outcome: derive a distinct measurement boundary from a closed physical wall
loop, show its exterior area, and use the existing appraisal declaration and
calculation workflow. This closes a missing connection in the production plan;
it does not replace the full production acceptance gate.

The user selects the perimeter walls, then chooses **Measure exterior from
walls**. A single selected wall may identify its connected wall loop; branched
layouts require selecting the perimeter explicitly. The command measures wall
exterior faces, using the stored thickness of each wall and joined offset corners.
Room boundaries remain independent. Open, ambiguous, invalid, and unsupported
curved loops are refused before mutation rather than silently using centerlines.

The result stores source wall IDs and geometry, retains normal area attributes,
and can be refreshed through **Refresh exterior measurement**. Repeated creation
from the same sources reuses the boundary. Refresh preserves its identity and
declared facts. Source changes invalidate appraisal qualification until refresh;
wall openings do not reduce the exterior footprint. Appraisal eligibility is
derived only from explicit facts through the existing calculation policy.

Ownership: root owns desktop/UI, CMake, integration, documentation, build,
packaging, and Git. The core worker owns exterior geometry and stale-source
validation. A separate worker owns native user workflow regressions.

Verification: focused geometry cases (including winding, concavity, differing
thickness and invalid loops), native selection/create/refresh, qualified totals,
undo/redo, save/reopen, stale/read-only refusals, output and rendered appearance.
Use the smallest affected checks and keep original user documents/windows intact.
