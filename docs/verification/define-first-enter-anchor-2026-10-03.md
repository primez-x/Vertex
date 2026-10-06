# Define First cursor start and measured workflow presentation

The official [Apex Define First tutorial](https://www.apexwin.com/support/ApexSketchv7/ApexSketchv7-DefineFirst.pdf),
page 7, positions the cursor and uses Enter to start the area. Vertex's native
canvas omitted that initial phase from its Enter handler. Enter now anchors an
active Define First session through the ordinary guarded point-input path.
It uses the effective snapped cursor, or the unsnapped cursor with Snap off,
and creates neither a side nor a document command. Pending dimension placement
and closure retain their existing separate Enter steps.

Successful measured-boundary startup and valid draft recovery synchronize the
actual Draw choice and cursor status to Measurement. The initial Wall default,
cancelled classification and refused starts retain their previous choice. The
same helper covers ordinary project opening and Undo/Redo recovery. This changes
workspace presentation, not the document model or project format.

## Ownership and verification

The worker owned the native drawing handler and workflow regressions. Root owned
integration, documentation, generators, native execution, Git and delivery.
Writers were frozen during native builds and checks.

Behavioral RED reproduced the absent Enter anchor. Separate RED captures/checks
exposed the Wall selector during Define First and during fresh-window recovery.
The final Release build and all four focused CTest suites passed: boundary
workflow, canvas, precision input dialog and authoring session. Root inspected
the actual anchored/completed window captures; both show Measurement correctly.
Evidence lives under `artifacts/define-first-enter-anchor-20261003`.

The native regression uses the real classification dialog, off-grid pointer and
keyboard events. Snap-on/off starts retain their exact anchor, typed quantities
and expressions, four dimensions, 2.921875 m2 area, one document commit,
Undo/Redo and unchanged save/reopen entities. Missing-pointer, stale source,
read-only, held-Space navigation and inactive-canvas inputs cannot anchor.
Fresh-window recovery retains saved geometry and changes no document command.

Two fixture errors were diagnosed from actual values: sequential double
arithmetic differed by about 1e-16 m, and a press/release helper did not model
holding Space during Enter. Only cumulative endpoint comparisons use a 1e-12 m
tolerance; anchors, original quantities, expressions and saved entities retain
strict checks. The navigation fixture now actually holds Space. No product
precision or refusal guard was weakened.

## Remaining original scope

Manual checklist U384 is a practical user workflow and remains Not tested by the
user. Pending-dimension H/V orientation and Space suppression remain concrete
Define First parity gaps. F4 already exists in the Apex-compatible preset.
Complete Draw First keyboard/pen-up behavior, original Apex project round-trips,
physical keyboard qualification, ANSI validation and the unified production
acceptance gate remain unfinished. APX-WF-002 is recorded as in progress rather
than treating this fix as complete tutorial parity. Installation/runtime/remote
evidence is recorded separately in the delivery artifact.

## October 6 source reconciliation

The October 3 H/V and Space gap statement above records the state at that time.
Current `PlanCanvas` and `MainWindow` dispatch implement pending-dimension H/V
orientation and held-Space suppression. `boundary_workflow_tests` exercises
modified/repeated-key refusal, stationary Space behavior, pan/focus/source and
read-only cases, text-field routing, persistence and unchanged history. The
delivery record retains the passing focused boundary workflow evidence. These
two implementation gaps are superseded; they are not new acceptance claims.
U384 human execution, physical keyboard qualification and original Apex
round-trips remain separate requirements.
