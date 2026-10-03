# Starting-point alignment and manual dimension placement

Date: 2026-10-03. This record covers a bounded native Windows development
checkpoint, not production replacement acceptance.

## Delivered behavior

- An active physical Wall or Measurement outline displays available X/Y guides
  to the original starting point's coordinates. X proposes a horizontal side;
  Y proposes a vertical side. Enter accepts the exact proposed endpoint.
- Proposing a side does not alter document geometry, history, or the semantic
  draft. Pointer motion, cancellation, changed source, and relevant context
  changes abandon or reject the proposal without committing stale geometry.
- A closes the actual outline. A configured Appraisal/Exterior wall closure
  creates its source-linked measurement in the same document history step.
- Define First retains manual dimensions. After an edge is accepted, clicking
  or pressing Enter places the label at the displayed pointer position. The
  closing dimension is also placed manually before final area publication.
- Bare letter commands apply to the focused idle drawing canvas. Modified
  shortcuts and text fields keep their behavior. Enter during a navigation
  gesture cannot fall through to ordinary Finish.
- Witness guides are transient canvas controls. The output rendering branch
  excludes them from printed/exported drawing content.

The behavioral reference is the official [ApexSketch v7 Define First
guide](https://www.apexwin.com/support/ApexSketchv7/ApexSketchv7-DefineFirst.pdf),
including the X/Y examples and second-Enter/manual-dimension workflow. These
examples do not establish the separate disconnected point-jump behavior.

## Verification

The initial native fixture failed because X/Y actions were absent. A later
regression failed because the second Enter did not place a pending manual
dimension. Both behaviors were implemented before their final passing runs.

The final native Release build succeeded. Seven affected executables exited 0:

- witness_alignment_desktop_tests
- geometry_operations_tests
- boundary_authoring_session_tests
- bay_return_desktop_tests
- drawing_measurement_desktop_tests
- wall_chain_connection_tests
- boundary_workflow_tests

The native fixture checks literal off-grid coordinates, exact proposal endpoint
reuse with snap enabled, both workflows, Imperial and metric display, key/menu
dispatch, cancellation, typing and gesture isolation, stale-source rejection,
read-only admission, checkpoint rollback, retained geometry tolerance,
manual-dimension persistence, atomic undo/redo, and native save/reopen. Actual
light/dark screenshots capture both witness choices for Wall and Measurement.
The populated pointer fixture measured 672 ms for 100 native moves with 301
unrelated walls; this bounded fixture is not a 60-frame-per-second certification.

Independent source review identified and resolved Enter-during-pan fallthrough,
retained-tolerance mismatch, and repeated document work on pointer motion. A
separate review approved the subsequent manual-dimension Enter change and shared
read-only/context guards against the rebuilt fixture and final seven-suite run.
Root reviewed the integrated change, requirement boundaries, and source-kit
delivery inputs. `git diff --check` passed.

Local, ignored evidence is saved under
`artifacts/witness-alignment-20261003/`: `final-checks.json`, individual final
logs, build logs, and eight screenshots in `ui/`. Packaging/install receipts
record the delivered executable hash and remote Git reference separately.

## Remaining qualification

Recovered pending dimensions with an absent pointer and direct foreign-document,
workspace, or layer Enter transitions have explicit refusal guards but lack
dedicated branch-specific native fixture cases. The checks used isolated Qt
offscreen windows; physical keyboard, touch, tablet, and user-observed resolution
are not established by them. Output exclusion was source-reviewed, not a new
physical print test.

Full disconnected point jumping, broader Apex keyboard parity, native Apex
compatibility, ANSI normative validation, and overall production acceptance
remain open. GEO-BASE-004 and APX-KEY-004 are therefore recorded as in progress.
The practical user checklist adds U365 for X/Y acceptance and updates U050 for
A/manual closing dimensions; user result fields remain Not tested. This
checkpoint does not complete the full production application goal.
