# Completed corner precision and appraisal output consistency

This work continues the full production plan. It does not substitute a smaller
release gate or claim Apex or ANSI certification.

## Outcome

An appraiser can correct a completed measurement corner with exact coordinates
without redrawing the outline. The existing geometry editor previews the actual
candidate and applies its consequences in one reversible command. Coordinates
use the current workspace units and support explicit units. Invalid geometry,
conflicting relationships and stale document revisions must refuse the edit
without changing the document. Cancel leaves the original unchanged.

Review saved-sheet appraisal output against the newly implemented ANSI-oriented
Details and measurement summary. Correct any demonstrated disagreement in
units, precision, profile identity or provisional status using the authoritative
report; do not reimplement area arithmetic in the renderer.

## Ownership and sequence

- The numeric-corner worker owns the native main-window editor and its boundary
  editing regression. Root waits for returned ownership before integrating
  other main-window changes.
- A read-only scanner traces appraisal schedule and saved-sheet presentation.
  Root integrates demonstrated output corrections after the writer freezes.
- Root owns documentation, source-kit inventory, builds, native captures,
  focused checks, commit/push and updated local installation.

## Verification

Exercise the real dialog in Imperial and Metric, preview and Cancel, exact
Apply, Undo/Redo and save/reopen. Cover related/source-backed geometry through
the shared command path. Inspect native captures. Verify output corrections
using actual projected rows and PDF text/rendering where relevant. Run required
source-kit and requirement contract checks and `git diff --check`.

The previous connected-move review findings were checked against current source:
pending-release Escape and unavailable asynchronous preview are already fixed
and covered by `connected_wall_canvas_tests`. No duplicate patch is needed.

Final production acceptance, final ANSI normative validation, Apex native
compatibility and physical device/printer qualification remain open.
