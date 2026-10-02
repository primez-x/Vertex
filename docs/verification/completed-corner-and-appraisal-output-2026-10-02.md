# Completed corner precision and appraisal output

This checkpoint continues the full production plan and 2D appraisal work. It is
not production acceptance, Apex compatibility certification or ANSI approval.

## Implemented user behavior

The existing boundary geometry dialog offers **Vertex position**. It selects a
stable corner identity and accepts absolute X/Y in active or explicit units.
Untouched axes preserve their original values without a feet conversion round
trip. The original and actual validated candidate appear together with exact
coordinates, area/perimeter changes and related-object consequences. The same
typed candidate is applied as one revision-fenced history operation. Cancel
preserves source geometry. Locked or conflicting relationships refuse Apply.

Previously current physical source correspondences are checked for every
affected measured owner, including an unselected exterior joined to a free
boundary. A numeric correction cannot silently invalidate that exterior while
leaving its physical walls unchanged.

Appraisal schedule rows retain the property policy and calculation profile.
The native schedule table and printed sheet share one formatter. ANSI-oriented
rows show canonical whole square feet in either workspace, with explicitly
supplementary metric values. Legacy rows retain their own units and precision,
including mixed-policy output. The sheet status names the actual ANSI-oriented
policy, pending final validation and any provisional sloped-room denominator.
Plan dimensions continue to use the analytical, policy-aware labels.

## Review and technical evidence

The independent review found an indirect source-correspondence defect in the
first numeric guard. The guard now protects other previously current source
owners as well as the selected boundary. The native fixture proves the rejected
proposal is geometrically valid, changes that related owner and leaves its
physical walls fixed; its refusal is not a coincidental topology failure.

The review also found that a whole-page PDF text assertion could succeed from
canvas annotations alone. The persisted-sheet regression now anchors the
expected square-foot value to the GLA schedule row. It additionally opens the
actual native Schedules dialog and checks that row in both unit modes.

Test iterations corrected fixture defects rather than weakening admission:
the original writer lease must be released before testing an editable reopen,
and source-corner refusal uses a small offset from its identified coordinate
rather than a hard-coded X that could self-intersect depending on corner order.
Exact geometry and one-step Undo/Redo remain compared after reopening.

Native checks run offscreen with isolated settings and noninteractive error
handling. Actual logs, exit records, light/dark coordinate-dialog captures and
saved-sheet PDF/rendered captures are retained in
`artifacts/completed-corner-output-20261002`. The final verification and delivery
records identify the checked build, installed executable and remote commit.
User tasks U335 and U336 remain **Not tested** by the user.

The Release build passed. Six final native suites passed: boundary editing,
named plan vertex, connected wall canvas, ANSI appraisal, appraisal report and
sheet layout. The source-kit tracked check covers 1,221 files, the requirement
contract passed, and static runtime inspection found no unresolved imports
across 113 component binaries. Root inspected the light/dark dialog and actual
metric sheet captures. These checks do not prove full production acceptance.

## Remaining scope

Direct nonrigid coordinate editing of a physical wall-derived exterior requires
an inverse/coordinated source-wall command that the current core does not yet
support. The new dialog refuses a stale correspondence and directs editing to
the source walls or reviewed replacement sources. This remains an implementation
gap, not a release exclusion.

The full production gate still requires final ANSI normative validation, Apex
native-file/workflow compatibility and the existing device, output and clean
installation qualification. No narrowed completion claim is made here.
