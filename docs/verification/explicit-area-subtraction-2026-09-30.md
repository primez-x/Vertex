# Explicit area subtraction

This development checkpoint adds parent selection after drawing and before a
Define First drawing. The parent is an explicit measurement-area instance;
classification names never select it automatically. New links reject equal
area TYPEs, different contexts, outside geometry and nested deductions.
Analytical containment and the existing deduction union remain authoritative.

The new area, its manual dimensions and the chosen parent's deduction commit
together. Pending parent identity survives unfinished save/reopen, history,
retired-input validation and finish replay through active recovery v2. Ordinary
active/finished v1 encoding remains unchanged. Explicit deletion cleans links
in the same command. Removal remains available after incompatible type, geometry
or context edits and changes only the chosen link.

Declared appraisal void roles retain only the chosen role. The shared report
and desktop parser omit irrelevant missing dwelling declarations after parsing
every supplied token. Missing policy/basis/role and malformed supplied values
still fail; measured-area declarations remain required. Qualified role-only
voids reduce the parent's net without a standalone living-area contribution.

## Evidence

- Baseline desktop regression failed: selected subtractor had no Auto-Subtract
  action. Baseline role-only report failed qualification.
- Release application and affected core/native test targets built.
- Focused native `--auto-subtract-only` passed in 2.32 seconds. It exercises
  actual parent dialogs, cancellation, idempotency, stale/same-type/outside
  refusal, removal after type and floor changes, deletion cleanup, and a
  keyboard-only Define First void with four manual dimensions. Saving/reopening
  unfinished input retains the parent; finishing yields 75 square feet from a
  100-square-foot parent minus a 25-square-foot void, with exact undo/redo/reopen.
- A combined 16-check Release run passed in 36.02 seconds: calculations,
  appraisal document/desktop, boundary commit/recovery/workflow, workspace
  history/budget/archive restoration, ledger/discovery/storage, rotation,
  canvas controls and requirements schema. After relinking the remaining
  affected recovery executables, ledger/budget/discovery/archive restoration
  passed again, 4/4 in 1.91 seconds.
- Independent review found and resolved legacy v1 finish replay and cross-floor
  repair visibility gaps. An independently assembled classification-only legacy
  finished archive loads, restores and survives exact undo/redo. Review approved
  the corrected source and fixtures; root ran the checks.
- Root inspected `artifacts/auto-subtract-20260930-native-final/auto-subtract-define-target.png`:
  readable bundled-font parent name/type, square footage, identifying suffix
  and Apply/Cancel controls. The first capture lacked test font initialization;
  the settled capture uses the same bundled Inter resource as the application.

The old installed document-only CLI and the current document-only CLI both
refuse the captured recovery-bearing archive; this is not evidence of a complete
older v14 archive reader handling active v2. Source/version scanner and opaque
recovery tests cover that policy, but a frozen older archive-reader launch
remains unqualified. No project-format bump was made for the versioned embedded
recovery payload.

## Limits

These are controlled Windows Qt offscreen fixtures. An initial unconfigured
desktop test run failed at a component-property edit; the configured rotation
fixture passed. This does not establish user-observed resolution in a previously
launched executable or qualify physical mouse/pen/touch input. Native Apex
project/workflow compatibility, standards compliance, network-denied operation,
clean-machine installation and full production acceptance remain open.

Development executable SHA-256:
`515570e5d0b30124c02ad836c6bed917df545efd032e749f9839d9fa8925ad92`.
Launch with `scripts/run.ps1 -Configuration Release`; the earlier installed
checkpoint has not been replaced by this development rebuild.
