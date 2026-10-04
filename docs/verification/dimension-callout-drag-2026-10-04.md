# Saved measurement callout dragging

Saved length, angle and area dimensions now enter the shared presentation-move
preparation instead of unsupported geometry movement. The dimension codec retains
the analytical owner, stable segment/vertex references, styling and unknown
metadata. An explicit callout-only move becomes manual placement; its measured
geometry and value remain unchanged. Exact candidate projection regenerates both
the label and its dimension guide when only presentation coordinates change.

Canvas admission now records proposal validity independently of geometry count.
Labels-only and reference-only proposals use the same selected-ID validation for
synchronous and deferred completions. Unavailable proposals refuse; cancellation,
scene replacement and stale serials cannot revive released edits. A filtered hit
on a selected label begins object movement even when its same-ID guide frame is
elsewhere. Empty canvas outside the selection retains the existing pan behavior.
Selected measurement guides and labels no longer display generic width/depth
badges for their presentation bounds; those are not additional source dimensions.

Selected source owners move attached dimensions once through their existing typed
movement. For selected complete physical perimeters plus their saved callout, the
measured owner is promoted only if its complete geometry closure is already
moving. The promotion cannot add an unselected wall, deduction or measured line.
The independent review corrected an isolated owner-closure calculation to include
the already moving graph when resolving locks to jointly selected geometry. All
eligible owners are evaluated together, avoiding an order-dependent refusal when
more than one perimeter is selected. New geometry outside the original movement
set still refuses the complete operation.

Wall-derived grouped translations use the existing rigid TransformBoundaries
lane with zero rotation. That lane verifies every physical source and reconciles
the measured outline canonically. The legacy TranslateBoundaries replay remains
unchanged for ordinary measured owners; it does not provide this exterior source
reconciliation. No project format or command dialect changed.

## Verification record

The first native RED gesture reproduced the unsupported-object error. Green1
exposed geometry-count-dependent canvas admission. Green2 confirmed the saved
angle label lay outside its guide frame. Green3 exercised the corrected length,
angle and area gestures, then exposed test setup using a canvas-only snap override
that MainWindow legitimately restored during selection refresh. The fixture now
turns Snap off through its actual toolbar control. Earlier failures remain in the
local evidence directory; they are not passing evidence.

Green5 passed the saved-callout and rotation/admission suites. The extended
two-perimeter fixture in Green6 then exposed the stale exterior left by legacy
translation. The new fixture retains a parallel relation between both measured
owners and exercises both selection orders, requiring current sources and one
placement offset with unchanged automatic provenance for both callouts.

The existing rotation fixture also guessed a pin position that collision avoidance
had relocated. It now targets selectionRotationHandlePosition, the actual painted
control, and retains its strict preview/committed image comparison. Reference
angle composition converts model radians to degrees once. The failing branch was
the label gesture, not reference rounding; no raster tolerance was weakened.

The initial Release build and all 11 affected native suites passed in Green8
(58.98 seconds). Actual gestures cover five callout creation/placement kinds, direct
source groups, physical-source groups and two constrained perimeters in both
selection orders. Checks retain exact measured values, styling, unrelated
metadata, one-revision Apply, Cancel, no-op clicks, Undo/Redo and editable reopen.
Canvas fixtures cover labels-only/reference-only exact results before and after
release, in-callback completion, null rejection and duplicate completion.

Final root review found that an exception from an exact preview provider could
be treated as an accepted empty geometry result for a reference-only selection.
An exception after marking the request pending could also leave it waiting.
The new native RED fixture reproduced this. Failed providers now return through
the unavailable-proposal path and release pending ownership, while already
completed or invalidated callbacks retain their existing serial guards. Fixtures
cover labels and references, both exception timings, unchanged preview and no
commit. The final Green10 Release build and all 11 affected suites passed.
The first installation is retained as evidence; the corrected r2 bundle is the
delivered build. No user application was closed or replaced while open.

Root visually inspected final angle and automatic-length callout captures and
the two-perimeter group view, confirming the removal of generic callout-size
badges. Focused native results, reviewed capture hashes and offline delivery
records are retained under `artifacts/dimension-callout-drag-20261004`.
Manual tasks U429 and U430 remain Not tested by the user.

## Remaining acceptance gaps

Moving a selected callout together with only part of its physical source perimeter
still requires explicit presentation authority composed with typed connected-wall
movement. The existing core refuses raw dimension supplements atomically; this
change does not silently drop selected annotation movement or move unselected
geometry. That mixed partial-perimeter workflow remains in scope.

These scoped changes do not prove full Apex workflow/file compatibility, complete
annotation/output parity, normative ANSI validation, clean-machine or
network-denied qualification, or the unified production gate.
