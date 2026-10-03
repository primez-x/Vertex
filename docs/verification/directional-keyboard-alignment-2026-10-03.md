# Directional drawing alignment

Date: 2026-10-03. This is a bounded Windows development checkpoint;
production replacement acceptance remains open.

## Behavior

The focused idle Wall or Measurement canvas accepts Ctrl+Arrow to propose the
next visible structural endpoint coordinate along that axis. Repeated presses
advance through those coordinates. Ctrl+Shift+Arrow instead proposes an actual
line or analytical arc intersection with the horizontal or vertical ray.
For a collinear overlap, the next finite structural endpoint is used.

The construction start remains the retained drawing endpoint, while the
perpendicular cursor coordinate is preserved. A screen/model round trip near
the retained endpoint normalizes only the searched axis. The proposal is
ephemeral, shows its length and an Enter cue, and uses ordinary exact drawing
when accepted. A missing target leaves the cursor and proposal unchanged.
Define First retains its pending manual dimension before another side can be
proposed. Tools > Directional alignment and Commands expose the same actions.

The official behavioral reference is the [ApexSketch v7 Draw First
guide](https://apexwin.com/support/ApexSketchv7/ApexSketchv7-DrawFirst.pdf).
This implements the documented directional alignment behavior; it does not
certify the complete Apex keyboard preset or disconnected point jumping.

Only visible structural geometry is used. Wall baselines are distinct from
painted wall faces. Symbols, annotations, reference images, grid lines and the
unaccepted rubber band do not become alignment targets. Changed document or
scene context invalidates a proposal before it can publish geometry.

## Regression findings

The initial native fixture reproduced the absent Ctrl+Right/Enter behavior.
Independent review then identified two numerical errors. Long origin-to-arc
rays inflated the contact tolerance and could merge crossings or admit an
outside-circle contact. Arc resolution now clips the directed ray to the
analytical arc bounds before calculating intersections. A distant origin could
also make distinct candidate distances compare equal on MSVC; candidate
coordinates must determine nearest ordering directly.

A separate shallow-arc regression reproduced cancellation in the shared
radius-squared intersection calculation. The directional resolver now uses a
local chord-frame calculation that preserves the two actual crossings instead
of reporting a false midpoint tangent. Exact represented half turns use
canonical bounds and coefficients; this preserves ordinary semicircle tangency
without admitting an outside contact or merging nearby crossings. This checkpoint does not change the
shared geometry engine's other intersection consumers.

Visible walls on another floor originally lacked directional targets because
their ordinary mouse snap candidates are intentionally excluded. The retained
scene now supplies separate analytical directional baselines after visibility
admission, preserving the active floor's ordinary mouse snapping policy.

Native Define First continuation exposed a mouse screen/model round trip
landing just before the accepted endpoint. Nearby-axis normalization prevents
the same node from being proposed again. Review required preserving the other
cursor coordinate even with a larger recovered authoring tolerance.

Two earlier failures were fixture defects: selecting an entity not supplied to
the canvas, and iterating a reference obtained from a destroyed temporary
document snapshot. Both fixtures were corrected without weakening production
admission checks.

A diagnostic initially referenced a private canvas method and failed to
compile; it now counts selected retained entities through the public scene.
The separated-cursor and closure fixtures explicitly choose the Wall action
and verify their initial pen rather than assuming a particular interaction
state.

## Verification

The final native Release build succeeded. Seven affected executables exited 0:

- directional_alignment_canvas_tests
- witness_alignment_desktop_tests
- bay_return_desktop_tests
- drawing_measurement_desktop_tests
- boundary_authoring_session_tests
- wall_chain_connection_tests
- boundary_workflow_tests

The new fixture covers direct coordinate ordering at distant origins, actual
line/arc contacts, shallow and major arcs, signed sweeps, endpoint contacts,
tangencies and adjacent representable inside/outside ordinates, repeated
proposals, visible other-floor walls, hidden-floor exclusion, recovered 0.01 m
authoring tolerance, separated cursor/construction start, Define First phases,
first-side acceptance, source-change refusal, actual wall closure, Undo/Redo
and native save/reopen. Input guards preserve text and navigation behavior.

Root reviewed the integrated source, requirement boundaries and actual light
and dark captures. Independent review found no remaining actionable source
finding after the corrections. Twelve directional native screenshots are
retained. Runtime inspection checked 113 component binaries with zero unresolved
imports. `git diff --check` passed. Local ignored build logs, actual exit receipts
and captures are under `artifacts/directional-alignment-20261003/`; installation
and Git delivery receipts are separate.

## Scope and remaining qualification

The practical manual checklist adds U366 and retains all user result fields as
Not tested. GEO-BASE-004 and APX-KEY-004 remain in progress. Disconnected open
measurement linework, full physical keyboard/device qualification, native Apex
compatibility, ANSI normative validation and overall production acceptance are
unfinished. Offscreen native fixtures do not establish user-observed resolution
or physical device and print qualification. Same-revision foreign-head
replacement and directional checkpoint-failure branches have source guards but
lack dedicated new branch-specific cases. Other consumers of the shared
shallow-arc intersection routine retain their existing numerical qualification
boundary; this directional correction does not validate those consumers.
