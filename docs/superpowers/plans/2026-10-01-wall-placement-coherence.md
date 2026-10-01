# Wall placement coherence

Repair the reported wall drawing/snapping/dimension workflow and consolidate
door/window insertion around the existing hosted-opening model.

- Root owns `main_window.cpp`, palette regressions, documentation, native mouse
  verification, packaging and Git. Worker owns the sloped-wall regression;
  advisor reviews pending-tool lifecycle and admission. Preserve existing work.
- Ordinary clicks start/continue physical walls by default. Endpoint snaps and
  lengths must agree in preview and persisted geometry. Sloped walls must obey
  the same Snap toggle. Escape retains committed walls.
- Door/Window quick buttons and catalog activation use one preset preparation
  path and one Style/width/height/sill editor. Activation previews a host opening;
  placement cuts the wall. Furniture remains freely placed. Category names and
  instructions distinguish openings from free components.
- Verify behavioral failures before fixes, run the affected desktop checks,
  review the integrated diff and inspect the packaged app with real mouse input.
  Install a uniquely named current build and provide one current launch shortcut,
  preserving the older open application and its unsaved work.

This correction does not certify every catalog subtype. Bay windows, roof-hosted
skylights and non-hinged door operation still need their own authoring/model
qualification. Apex compatibility and the complete production gate remain open.
