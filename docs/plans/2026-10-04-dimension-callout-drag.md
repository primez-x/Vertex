# Drag saved measurement callouts on the canvas

Make the visible dimension selection frame usable for repositioning its callout.
The actual native RED gesture reproduces the current failure: saved dimension IDs
enter the geometry-move path, which rejects them as unsupported objects.

Use the shared selection preparation for both preview and commit. A dimension-only
move updates presentation coordinates, retains its analytical target and value,
and deliberately changes automatic placement to manual. Preserve style and unknown
metadata through the existing dimension codec. Regenerate both callout text and
guide lines when the dimension changes, even if its source geometry stays fixed.

When the source owner also moves, its existing typed movement owns the attached
dimension. It shifts once and keeps placement provenance. Suppress redundant
selected-callout supplements after dependency expansion; do not bypass typed
dimension protections. Validate source/view freshness before presentation-only
commit, and reject malformed or unsupported dimension metadata atomically.

The initial implementation worker owned main_window.cpp exclusively. Root owns the existing
measured-dimension desktop fixture, docs, integration, Git and every native build,
test, installation and runtime. All native writers freeze during native operations.

After initial ownership return, root owns main_window.cpp and both fixtures. A
bounded canvas worker repairs exact presentation-only preview admission and the
selected-label hit outside a same-ID guide frame. Exact admission tracks validity
independently of geometry count; synchronous and deferred completions validate the
selected IDs identically. Scene changes and cancellation invalidate released work.

For a callout selected with its complete physical perimeter, promote its measured
owner to the existing typed translation only when its complete geometry closure
is already moving. Do not introduce unselected walls or deductions. Partial
physical-wall plus callout movement remains a distinct typed-command gap; do not
silently drop selected manual, angle or area presentation changes.

Verify real press/drag/release gestures for manual length, angle and area callouts,
automatic stroke and closed-area dimensions, and mixed source/callout selections.
Check live preview, one revision, unchanged values/source geometry, metadata/style,
Cancel, zero-distance clicks, exact Undo/Redo and editable save/reopen. Review the
rendered previews. Run the affected interaction/annotation checks and install the
verified offline build. Keep the full production/compatibility goal active.
