# Exterior source repair and connected curved length editing

An exterior measurement can now explicitly reconnect to replacement perimeter
walls without becoming a second appraisal area. **Replace source walls...**
is available in Tools and the selected measurement's context menu. Ordinary
Refresh continues to require the retained wall identities.

The detached review shows the old and proposed analytical exterior, quantities,
source differences and elevation. The chooser displays a wall's length; amber
highlights the chosen baseline. Cancel, stale context and invalid shells leave
the drawing unchanged. One accepted command changes geometry and source
provenance together, preserving name, facts, factors, appearance and deductions.

Equivalent geometry preserves stable edge/corner/dimension identities only
after a unique cyclic/direction match. Changed geometry with the same edge count
refuses instead of guessing correspondence. Changed edge counts use the existing
explicit dimension/constraint reference planner. Automatic dimensions regenerate;
manual references require mapping or an allowed removal decision.

Curved boundary length editing now offers the related-object choice. The default
updates objects connected through saved relationships. Disabling it refuses edits
that would leave invalid connected geometry. Curvature reconstruction keeps both
endpoints fixed; physical length editing retains signed sweep.

## Verification

Release builds succeeded. Native Qt interaction checks used the offscreen
platform and actual actions, dialogs and controls; no human app was launched.

- Source-repair desktop fixtures cover Imperial/Metric and straight/curved shells,
  qualified appraisal facts, linked garage deduction, appearance, retained area
  identity, analytical area, dimension IDs, Cancel, unit/selection staleness,
  open-shell refusal, one-command Undo/Redo and save/reopen.
- A split-source fixture opens the real nested reference planner. Unresolved
  references cannot apply. Cancel is detached; explicit manual edge mapping
  commits with source replacement and survives Undo/Redo.
- A plain identified owner without original drawing receipts is repaired without
  fabricating them. Single transformed cloning and grouped clipboard paste remap
  archived topology and reviewed source IDs; history and reopening retain them.
- Core fixtures cover plain/authored straight/curved owners, strict v3 codec,
  actual hierarchy/phase/elevation refusals, demolition eligibility, retained
  deduction containment, generic source forgery and final-envelope reconciliation.
  Later source deletion leaves saved geometry/history readable and appraisal
  quantities unqualified. Fork, Undo/Redo, native reopen and exchange are checked.
- Storage fixtures reject recomputed-digest under-versioning in current, undone
  and imported states. New semantics require native 16 and exchange 14; existing
  version markers stay unchanged for projects that do not use them.
- Connected curved-edge native fixtures cover both unit systems, disabled
  related movement refusal, checked movement, preview, Cancel, one-command
  Apply, Undo/Redo and reopening.
- Full wall measurement, desktop exterior measurement, boundary workflow/editor, document,
  boundary integrity, project organization, boundary arc editing, storage and
  exchange checks passed. Adaptive grid checks continue to verify common unit
  intervals, actual painted-line/snap agreement and preserved exact geometry.

Actual baseline failures were recorded for the absent repair action, unsupported
v3 intent and disabled related-object choice. New fixtures initially exposed
their own optional access, qualification-factor, owner-ID and Windows open-file
cleanup issues; these were corrected without weakening production validation.

## Limits

Original source v1 has no archived elevation. With all old walls deleted and no
explicit owner elevation, the review reports that the original plane is
unavailable. Replacement walls still require one coherent effective plane and
the original resolved hierarchy/phase. Equal-count changed geometry needs its
original shell restored before this source-only repair; rich explicit mapping
for that case remains a gap. These checks do not certify native Apex compatibility,
device/application integrations, physical printing or the full production release.
Manual tasks U320 and U321 retain **Not tested** for the user's own verification.
