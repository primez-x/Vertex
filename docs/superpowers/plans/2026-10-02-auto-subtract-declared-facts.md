# Auto-Subtract follows declared appraisal facts

Outcome: compatible parent selection uses the same current declared use and
grade/finish facts as appraisal calculations. A legacy manual category cannot
override newer declarations. Preserve explicit pending drawing TYPEs and truly
undeclared legacy behavior. Invalid or incomplete declarations must remain
unresolved rather than inheriting stale authority.

Core worker owns area_subtraction.cpp and boundary_commit_tests.cpp. Native
acceptance worker owns appraisal_desktop_workflow_tests.cpp. Root owns docs,
integration, builds, Git and delivery. The separate canvas writer keeps its
existing exclusive PlanCanvas paths. No source model or geometry change is
required for the category precedence correction.

First reproduce with equal saved categories but declared Dwelling/Garage facts:
the 10 by 10 ft parent must admit its contained 5 by 5 ft garage child. Conversely,
differing saved categories must not allow subtraction when current declared TYPEs
are equal. Test incomplete/malformed declarations and pending TYPE behavior.

Run failing core and real chooser regressions before the fix. Verify 75 ft²
dwelling and 25 ft² garage totals, unchanged boundaries, undo/redo and native
reopening. Run affected appraisal checks and repository contracts. Integrate with
the selection-readability changes for one scoped commit/push and installed build.
The full production acceptance gate remains open.
