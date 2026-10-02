# Appraisal properties usability

Improve the existing selected-area workflow without changing the project schema,
calculation rules, geometric model, or source metadata representation.

## Outcome and ownership

- MainWindow worker: place Edit appraisal facts immediately below Classification,
  expose the same contextual action in the area menu, and make secondary appraisal
  totals expandable while retaining a concise status and primary totals. Keep the
  complete qualification reasons and selected-area trace in Details; Classification
  already presents the selected category. Group geometry actions in a compact row.
- MainWindow worker: replace the raw JSON attributes field with a Name/Value Details
  editor. Validate empty/duplicate names and existing UTF-8 byte limits before Save;
  preserve Cancel, no-op, stale-selection/revision guards and undo/reopen behavior.
- Native UI test worker: exercise actual controls, visible entry points, metadata
  validation and persistence without bypassing editability safeguards.
- Root: reconcile obsolete smoke assertions, review integration, update help and
  user testing instructions, build and verify, commit/push and install a fresh bundle.

## Verification

Observe the missing structured entry point fail before source changes. Run focused
native UI regressions offscreen, then the affected appraisal and smoke checks.
Inspect real UI captures and check that facts, geometry and custom presentation are
unchanged by custom metadata edits. Verify requirement/source-kit contracts and
Git diffs, then verify remote commit, installed executable hash and Desktop shortcut.
This delivery is progress toward the full production goal, not Apex certification.
