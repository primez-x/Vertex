# Touch navigation and explicit legacy identity upgrade — 2026-09-30

Two-finger input now pans and pinches the plan around a moving model anchor.
Navigation takes over from a pending one-finger edit without committing it.
The navigation sequence stays latched until all contacts lift. Changed contact
pairs rebase without jumps; Escape, cancellation, focus loss, hide, modal
dialogs, tool changes and projection replacement discard touch ownership.
Navigation retains geometry and independent output scale.

Anonymous legacy Vertex boundaries now have an explicit **Upgrade boundary
identities** action in More, command search and their context menu. It uses the
existing exact upgrade and normal document admission. One history operation adds
stable topology IDs while retaining the owner, geometry and opaque metadata.
Ambiguous or approximate joins, owned receipts, stale contexts, read-only projects
and unsupported versions retain their refusal behavior. This is not an Apex
native-file importer or a certification of legacy Apex compatibility.

## Observed evidence

- Release `vertex`, `desktop_smoke`, and the affected canvas targets compiled.
- CTests `symbol_transform_desktop`, `axis_canvas_controls`, `boundary_canvas`
  and `requirement_schema_contract` passed (4/4, 13.47 seconds).
- The added pinch regression failed against the previous canvas, then passed
  after implementation. Events use Qt's delivered touch sequence helper; checks
  cover moving anchors, pan, second-finger edit cancellation, remaining-finger
  suppression, pair replacement, output invariance, zoom ceiling, hide, Escape
  and late events after source replacement.
- Native selectors `--boundary-identity-upgrade-only`,
  `--mixed-constraint-workspace-only` and `--boundary-insertion-only` exited zero.
  Identity coverage includes the More action and command search, exact metadata,
  one command, undo/redo, save/reopen, subsequent insertion/transform, no-op and
  atomic refusal cases.
- Local captures/logs are in `artifacts/touch-navigation/release/` and
  `artifacts/touch-and-identity/release/`. The pinch/pan capture was inspected.
- Independent source review identified Escape retaining touch ownership. The
  correction and regression were added, reviewed and included in the final run.

## Gap review

Both scoped behaviors are implemented and technically verified. Physical pen,
touch-screen and precision-touchpad behavior is not observed here. Wall-group
translation, curved-owner constraint solving, Apex compatibility fixtures,
clean-machine offline installation and the full production acceptance gate
remain open. Existing requirement rows stay `in_progress`; these checks do not
establish overall production readiness.
