# Vertex canvas interaction contract

This contract covers the Measurement plan, Architectural plan, and native 3D
view. Camera navigation never changes document geometry. The pointer location at
press time owns the gesture until release, so crossing another object during a
drag cannot change the operation.

## Mouse and wheel behavior

| Gesture | 2D Select | 2D drawing or placement | 3D |
|---|---|---|---|
| Left click on object | Select the top visible hit | Add the point required by the active command | Select the visible object |
| Left click on empty canvas | Clear selection | Add the point or place the component required by the active command | Clear selection |
| Left drag from empty canvas | Pan | The active drawing command owns the drag; middle drag remains available for navigation | No model edit |
| Left drag from selected object | Move the selected object or compatible selected group in one undoable transaction | The active drawing command owns the drag | Reserved for an explicitly armed Move command |
| Left drag from unselected object | Select and move that object in one gesture | The active drawing command owns the drag | Select only |
| Ctrl + left click | Toggle the hit object in the selection | No drawing point is added | Reserved for later additive-selection support |
| Ctrl + left drag | Add a directional marquee result to the selection | No drawing point is added | Pan |
| Left-to-right Ctrl marquee | Add fully enclosed objects | — | Planned |
| Right-to-left Ctrl marquee | Add crossing or enclosed objects | — | Planned |
| Middle drag | Pan from any hit location | Temporarily pan without changing the draft | Pan |
| Right click | Select an unselected hit, preserve a selected group member, then open object or canvas actions | Open Finish, Undo last, Precise, or Cancel actions appropriate to the active command | Open object or view actions |
| Right drag | No model action and no context menu | No model action and no context menu | Orbit |
| Wheel | Zoom about the pointer | Zoom without changing the draft | Zoom |
| Double click | One selection result only | Never inserts or places a duplicate | One selection result only |

Ctrl is the 2D selection modifier. Selection marquees do not replace an existing
selection. A left-to-right marquee uses enclosure rules; a right-to-left marquee
uses crossing rules.

## Explicit 3D Move

`Move object` is available from the 3D context menu for a selected transformable
object. It arms one plain-left drag and commits one document transaction on
release. Ctrl-drag and middle drag navigate the view and never translate an
object.

Escape, focus loss, capture loss, hiding the view, changing the camera, or
starting another navigation gesture cancels Move and restores the unmodified
presentation. A later mouse release cannot commit a cancelled move.

## State and feedback rules

- Only one gesture owns the pointer at a time. Another button cannot steal or complete it.
- The active drawing layer is selected in the project hierarchy and marked in blue.
- Every new symbol or label stores its owning layer ID and is listed beneath that layer.
- Selected objects receive a blue contrast outline. A compact `Selected` or `N selected` badge reports selection without opening Properties.
- Dragging a movable selection shows a transient preview; release creates one undoable command. An unsupported mixed group is rejected as a unit.
- Component placement is a distinct pending state. Escape or the drawing context menu cancels it.
- Wall previews follow the effective snapped pointer after the first endpoint.
- Right click is evaluated on stationary release. Crossing the drag threshold prevents the menu.
- Double click consumes the second press so it cannot add a duplicate point, toggle twice, or place two components.
- Pan, zoom, fit, and overview navigation refresh the effective cursor position used by measurements and contextual UI.
- Selection and move previews are screen-only and never appear in print or export.

## Keyboard behavior

| Key | Behavior |
|---|---|
| Escape | Cancel the active gesture or explicit Move; otherwise cancel the pending tool step or clear selection |
| Enter | Finish a valid boundary; no effect for other tools |
| F | Fit drawing or model content |
| D | Open precise boundary input while drawing |
| Ctrl+Z / Ctrl+Y | Undo or redo through the current document or draft route |

## Remaining interaction work

- Direct vertex, endpoint, and rotation grips for post-draw editing.
- 3D directional marquee selection and additive selection parity.
- A Space + left-drag pan alias.
- Pen barrel-button mapping, pinch zoom, and multi-touch navigation.
- Overlap cycling for stacked selectable objects.
- A component ghost preview before click placement.
- Shared painted-footprint hit testing for every filled and custom-stroke entity type.

## Acceptance sequences

Focused interaction checks cover click selection, object drag with one commit,
empty-canvas drag pan, Ctrl-click toggle, directional Ctrl marquee, mixed-button
release, stationary right click versus right drag, double-click suppression,
Escape and capture cancellation, explicit 3D Move exactly-once commit, selection
visibility at zoom extremes, and exclusion of transient feedback from output.

Manual Windows validation still covers physical mouse capture, high-DPI pointer
thresholds, pen and touch hardware, context-menu placement across multiple
monitors, and long sessions that combine drawing, navigation, save/reopen,
print, and export.
