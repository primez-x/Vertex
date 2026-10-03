# Visual revision comparison verification

Original requirement: REC-002, named revisions and comparison views for semantic, geometric, calculation and presentation changes. Baseline 218a5e1 provided native named revisions, textual comparison and non-destructive restore, with graphical comparison explicitly open.

## Observed baseline

The new dedicated native desktop regression built against the unmodified product and exited 1: `missing native revision comparison control: revisionBeforeCanvas`. It exercised the real revision QAction, naming controls, named/current selectors and Compare button. This established the missing user-visible workflow before implementation.

## Integration findings

The integrated product compiled after returning the shared hosted-opening collection to the live architectural projection. The initial new native comparison regression passed, including analytical lines/arcs, styled and derived dimensions, a same-ID raster asset change, missing-floor diagnostics, navigation, failed-result clearing, current-head refresh and live-state preservation. Root inspected its actual dialog captures.

Existing boundary canvas (including touch navigation notification), connected-wall canvas, Details, ANSI appraisal and SVG/palette checks passed. The existing visibility workflow then exposed a hidden-opening dependency bug; inspection of baseline 218a5e1 confirmed the same blanket visibility-mask erasure already existed before this extraction. Independent review also found that overlay child lookup used the wrong annotation-state nesting, missing palette-only symbol changes. Both findings require repair and affected reruns before delivery. Production acceptance remains unproven.

Runtime logs and captures are local artifacts under `artifacts/visual-revision-comparison-20261003`; user-facing checks are U359–U361. Full architectural linked-view/3D comparison, project-scale qualification and native Apex compatibility remain separate open production requirements.

## Final observed checks

All 13 affected native checks exited 0 after repair:

- Full revision comparison: exact lines/arcs, styled and unchanged dependent dimensions, same-ID retained raster pixel changes, exact pinned SVG artwork with palette-only changes and unchanged sibling appearance, context union/missing floors, live-mask independence, real Fit/pan/zoom/overview controls, failed-result clearing/recovery, fresh current head, live entity/asset/history/save-marker/selection/draft preservation and ordinary edit/Undo after closing.
- Full visibility workflow and output-view appearance: hidden opening/host/assembly dependencies and explicit saved-view re-show.
- Full boundary canvas (including touch notification), connected-wall canvas, Details, ANSI appraisal, SVG symbol, symbol palette, drawing measurement and wall measurement suites.
- Existing named-revision restore (`desktop_smoke --named-revisions-only`) and coordinated output (`--coordinated-view-output-only`).

Root inspected both final native comparison captures. Independent source review identified the annotation nesting defect, confirmed its codec/signature repair and the visibility correction, and inspected the final captures and central passing checks. Its remaining check conditions were subsequently satisfied by the wall/named/output results, which root verified.

The reference fixture first exercised import and observed the source-build sandbox gate fail closed; it then used locally generated trusted fixture pixels for historical editing/rendering. These results do not qualify isolated production import. Mouse/touch events are synthetic native Qt events, not physical-device qualification. Paint-time automatic callout avoidance remains derived layout rather than a persisted edit or independently certified pixel-change classification.

The plan, source, UI, tests and documentation agree on conventional 2D scope. No project-format migration or live-document rebinding was introduced. Full REC-002/production acceptance remains in progress for the unresolved architectural/scale/compatibility clauses above.
