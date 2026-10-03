# Live measured-stroke precision input

The actual Measured lines D command previously offered only distance and absolute
heading, even though shared boundary controls and the engine already supported
other analytical constructions. This change connects those controls to the live
measured stroke and adds typed starting-point placement before its first edge.

## Implemented behavior

All eight construction forms are available: length/heading, rise/run, relative
turn, world-coordinate endpoint, arc chord/angle, chord/height, chord/arc length
and start-tangent/arc-length/sweep. Measured input returns an analytical receipt
through the existing replay and per-edge document command. It does not synthesize
a closed boundary or substitute a chord for the previous curved edge's tangent.
The existing physical-wall and boundary-session paths retain their presentation
and topology rules.

Invalid input remains editable with an inline reason. Cancel publishes no result
or changed preferences. Successful input remembers non-coordinate fields only
after admission; changing the input-unit basis clears those preferences. Exact
source maps, document identity, history, pen, layer, phase, workspace, units and
editability are checked before a modal result can affect the document. The dialog
uses the existing compact native controls; measured mode omits redundant headings.

## Evidence

The new native regression first failed against the old D dialog with:
`live Measured lines D input must offer all eight analytical construction methods`.
The final Release build succeeded, followed by exit-zero checks for:

- measurement_linework_precision_desktop_tests
- measurement_linework_desktop_tests
- measurement_linework_history_desktop_tests
- boundary_input_dialog_tests
- boundary_workflow_tests
- wall_measurement_desktop_tests
- measurement_linework_area_desktop_tests
- measurement_area_source_desktop_tests
- project_store_tests
- appraisal_details_panel_tests

The new registered CTest also passed with its configured offscreen Qt environment.
Logs and native captures are in `artifacts/measured-stroke-precision-20261003`.
Root inspected the new precision form and the saved semicircular area capture.
The fixture verifies the true semicircle's area against pi/2 square metres,
native save/reopen identity and PDF export through the desktop path.

Coverage includes every form in Imperial and Metric, mixed-unit starting points,
stored quantity/angle expressions, actual previous-arc tangents, Undo/Redo and
branching, retracing, impossible/corrected input, Cancel, queued submission,
unit changes away and back, replaced pen sessions, same-document/revision
entity and asset changes, read-only transitions and layer/phase/workspace changes.
Root reviewed the integrated source, command admission, test assertions and
rendered captures. A fresh independent advisor could not be dispatched because
the runtime's agent-thread limit was reached.

## Remaining qualification

Chord forms currently use endpoint X/Y; direct chord-length/bearing entry remains
open. Coordinate values are exact numerically, but their original textual
expressions are not retained by the existing coordinate receipt contract.
Nested-hole area definition, native Apex roundtrips, field-device and application
integrations, final ANSI normative validation, clean-machine offline qualification
and the unified production acceptance gate remain open. These focused results
do not certify the complete Apex replacement or mark user testing as passed.

Manual tasks U369–U375 describe the new user actions. They remain Not tested
until the user records their own outcomes. Installed delivery and remote commit
verification are recorded separately in the local delivery manifest.
