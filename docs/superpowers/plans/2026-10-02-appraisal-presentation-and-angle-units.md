# Appraisal presentation follows current declarations

Outcome: selected appraisal categories and automatic area appearance agree with
the current boundary qualification used for calculation. Declared categories are
read-only; facts remain the editing route. Unqualified declarations must not
inherit a stale saved category. Undeclared legacy classification remains editable.
Explicit appearance, label placement, geometry, facts and saved legacy metadata
remain unchanged by projection. Automatic styles continue to follow declarations.

Separately, the boundary curve-angle editor requires explicit degrees, radians or
a pi-bearing angle expression, matching Draw/Define. Bare numbers are rejected in
this UI without changing the core parser's established radians convention.

Ownership: MainWindow worker owns src/desktop/main_window.cpp. Native appraisal
test worker owns tests/appraisal_desktop_workflow_tests.cpp. Curve test worker
owns tests/boundary_editing_desktop_tests.cpp. Root owns integration, documentation,
builds, generated inventories, Git and the installed checkpoint. Writers freeze
before shared desktop builds.

Reproduce the stale saved-category/facts disagreement and bare `1` angle through
actual controls before production edits. Verify canvas defaults, appearance
dialog/reset, declared versus legacy classification, custom overrides, exact
curve receipts, no-op projection, undo/redo, native reopening and affected output.
Run affected native suites and repository contracts. Review the integrated result,
commit and push scoped paths, then deliver and verify a fresh offline installation.
The full Apex compatibility and production acceptance gates remain required.
