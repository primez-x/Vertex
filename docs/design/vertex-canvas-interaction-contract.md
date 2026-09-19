# Vertex canvas interaction contract

This contract covers the Measurement plan, Architectural plan, and native 3D view. Camera navigation never changes document geometry. A model edit requires an explicit drawing tool, placement state, property edit, or Move command.

## Mouse and wheel behavior

| Gesture | 2D Select | 2D drawing or placement | 3D |
|---|---|---|---|
| Left click | Select the top visible hit; empty space clears selection | Add one point or place one component | Select the visible object; empty space clears selection |
| Left drag | Marquee selection | Does not add extra points; component placement ignores marquee | Reserved for an explicitly armed Move command |
| Left-to-right marquee | Select fully enclosed objects | — | Planned |
| Right-to-left marquee | Select crossing or enclosed objects | — | Planned |
| Shift + left | Add or toggle selection | Available to the active tool only when its hint says so | Planned selection modifier |
| Ctrl + left drag | Pan | Temporarily pan and resume the tool | Pan |
| Alt + left drag | Pan alias | Temporarily pan and resume the tool | — |
| Middle drag | Pan | Temporarily pan and resume the tool | Pan |
| Right click | Open object/canvas actions | Finish a boundary; cancel pending wall or component placement | Open object/view actions |
| Right drag | No model action | No model action | Orbit |
| Wheel | Zoom about the pointer | Zoom without changing the draft | Zoom |
| Double click | One selection result only | Never inserts or places a duplicate | One selection result only |

Marquee direction is based on the release point relative to the press point. Ctrl is reserved for navigation; Shift is the selection modifier.

## Explicit 3D Move

`Move object` is available from the 3D context menu for a selected transformable object. It arms one plain-left drag and commits one document transaction on release. Ctrl-drag always pans and never translates an object.

Escape, focus loss, capture loss, hiding the view, changing the camera, or starting another navigation gesture cancels Move and restores the unmodified presentation. A later mouse release cannot commit a cancelled move.

## State and feedback rules

- Only one gesture owns the pointer at a time. Another button cannot steal it or complete it.
- The active drawing layer is selected in the project hierarchy and marked in blue.
- Selected canvas objects receive a blue contrast outline. The outline is screen-only and never appears in print or export.
- Component placement is a distinct pending state. Dragging during placement does not change selection; Escape or right click cancels it.
- Wall previews follow the effective snapped pointer after the first endpoint.
- Right click is evaluated on release. Crossing the drag threshold prevents the click action.
- Double click consumes the second press so it cannot add a duplicate point, toggle twice, or place two components.
- Pan, zoom, fit, and overview navigation refresh the effective cursor position used by measurements and contextual UI.

## Keyboard behavior

| Key | Behavior |
|---|---|
| Escape | Cancel the active gesture or explicit Move; otherwise cancel the pending tool step or clear selection |
| Enter | Finish a valid boundary; no effect for other tools |
| F | Fit drawing or model content |
| D | Open precise boundary input while drawing |
| Ctrl+Z / Ctrl+Y | Undo or redo through the current document/draft route |

## Remaining interaction work

The following items remain visible gaps rather than implied behavior:

- 3D directional marquee selection and Shift selection parity.
- A Space + left-drag pan alias.
- Pen barrel-button mapping, pinch zoom, and multi-touch navigation.
- Overlap cycling for several stacked selectable objects.
- A component ghost preview before click placement.
- Shared painted-footprint hit testing for every filled and custom-stroke entity type.

## Acceptance sequences

The focused interaction checks cover below/above drag threshold, mixed-button release, Ctrl pan without document callbacks, directional 2D marquee, stationary right click versus right drag, double-click suppression, Escape/capture cancellation, explicit Move exactly-once commit, selection visibility at zoom extremes, and exclusion of selection feedback from output.

Manual Windows validation still covers physical mouse capture, high-DPI pointer thresholds, pen hardware, touch hardware, context-menu placement across multiple monitors, and long sessions that combine drawing, navigation, save/reopen, print, and export.
