# Live appraisal values during corner editing

Outcome: an accepted corner drag shows current net-area values for the changed
boundary and any deduction parent. The preview uses the same qualified report,
units, rounding and semantic design-phase visibility as committed calculations.
The saved document and history remain unchanged until release. Cancel restores
the original scene; invalid proposals must not show plausible new totals.

Use typed command replay to obtain an immutable constraint candidate snapshot.
Share the existing constraint command verification with Apply so the preview
cannot construct arbitrary document authority. Derive label values from the
candidate report in the existing background projection job. Re-measure the new
multiline text when placing labels, including deduction obstacles.

Ownership: the core worker owns constraint_authoring.hpp/.cpp and its regression;
root owns MainWindow integration, appraisal desktop regressions, docs, builds,
review and Git. The separate draft/view worker returns MainWindow ownership
before integration. Preserve unrelated dirty files.

Verification: the baseline actual pointer regression fails because values are
suppressed. A five-foot-square deduction with a corner moved 0.2 m on both axes
must preview 21.72 ft² and change its 100 ft² parent's net value to 78.28 ft².
Check unchanged source, cancel, matching commit and undo; retain a rendered
canvas. Check exact candidate replay, stale-source refusal and assets. This
checkpoint does not close the full production/compatibility acceptance gate.
