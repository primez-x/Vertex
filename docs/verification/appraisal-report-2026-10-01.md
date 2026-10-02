# Appraisal report and sheet checkpoint

Tools > Appraisal area report exposes property totals, areas and deductions,
qualification issues, source facts, gross/deducted/net/adjusted values, exact
factors, perimeter and display rounding. Its complete PDF paginates independently
of the bounded plan-sheet summary. The detached layout dialog can add an A3 plan
and appraisal summary; accepting commits one undoable edit and preserves the
original sheet graph.

Valid individual measurements remain diagnostic when qualification fails.
Exclusions do not become standalone totals. Stale wall-derived sources and
invalid dependencies expose no current numeric trace. Some malformed legacy
inputs are issue-only entries, identified by source ID rather than numeric rows.
The report uses semantic design-phase visibility; presentation hiding does not
change calculation inputs. Numeric provenance binds document ID, revision and
the source entity-map digest. Assets and history are not numeric inputs. The
interactive modal additionally checks document instance, full snapshot digest
and units before navigation or export.

## Evidence

- Release desktop build succeeds. Core appraisal and wall-measurement checks pass.
- Full appraisal desktop workflow checks pass, including existing precision,
  qualified/withheld summary behavior, live quantities and PDF output.
- New native report checks pass actual Tools action, tabs, trace rows, source
  navigation, Refresh, stale refusal, read-only export, floor/policy inputs,
  Imperial/Metric precision, non-unity diagnostics and missing source traces.
- Saved design alternatives change report membership and totals; baseline
  restoration returns the excluded contribution. Workspace hiding does not.
- The 20-boundary PDF preserves every source ID over 11 pages, including long
  escaped names. Empty/nonexistent/directory output failures and stale identity,
  revision or entity digest preserve existing output. The open native project
  cannot be replaced by report output despite a misleading .pdf suffix.
- Sheet layout checks pass preset view filtering, geometry, unique identities
  and numbering, registry preservation, cancellation and model round-trip.
  Actual MainWindow acceptance commits the preset once; one-step Undo/Redo,
  project save/reopen and the real two-page drawing-set PDF pass.
- Root inspected actual report controls and first, middle and final PDF pages
  on white paper. Qt PDF renders are composited over white for captures; the
  original PDF bytes are retained. Root also inspected the preset's plan and
  summary. Its small 10-foot fixture proves placement and retention, not a
  production-size property layout qualification.
- Independent review required document-state binding, complete qualification
  clearing on unavailable semantic phases, and policy/floor provenance. Those
  corrections are integrated. The runtime preset check found and fixed the
  transaction anchor for newly staged sheets. Existing sheet unit notation is
  retained. Earlier failed checks remain in the artifact directory.
- Focused adaptive-grid canvas and boundary workflow checks also pass. Inch,
  fractional-inch, half-foot and whole-foot intervals, centimetre/metre/zoom
  behavior, visible-grid/snap agreement and exact-geometry retention are covered.

Evidence and captures: `artifacts/appraisal-report-20261001`. Manual tasks
U303–U305 cover report inspection/export and the plan-sheet preset; U081/U082
cover grid/snap across units and zoom.

Release executable SHA-256:
`58504166c4b7ac20f6ede4a3703f954fd3febce998f3634d47161f84a80c1f1e`.

Physical printing, production-size report/performance qualification, user-observed
resolution, measurement-standard certification and original Apex compatibility
remain open. This checkpoint does not complete the production release goal.
