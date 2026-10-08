# Vertex canvas interaction contract

This contract covers the Measurement plan, Architectural plan, and native 3D
view. Camera navigation never changes document geometry. The pointer location at
press time owns the gesture until release, so crossing another object during a
drag cannot change the operation.

## Mouse and wheel behavior

Measurement and Architectural plan views use one pointer surface. There is no
Select, Draw First, or Define First pointer-mode toggle in the canvas toolbar.
Define First remains an explicit advanced command for workflows that require
classification and manual dimension placement before the outline is complete.

| Gesture | Measurement plan | Architectural plan | 3D |
|---|---|---|---|
| Left click on object | Select the top visible hit | Select the top visible hit | Select the visible object |
| Left click on empty canvas | Clear a retained selection first; otherwise start/place a point for the Draw choice (Wall by default, Area, or Measured lines); returning to the original node closes an eligible chain | Clear a retained selection first; otherwise start/place a wall endpoint in a horizontal plan; returning to the original endpoint closes the chain | Clear selection |
| Left drag from empty canvas | Pan without placing a node | Pan without placing a wall point | No model edit |
| Left drag from selected object | Move the selected object or compatible selected group in one undoable transaction | Move the selected object or compatible selected group in one undoable transaction | Reserved for an explicitly armed Move command |
| Left drag from unselected object | Pan; click first if the object should move | Pan; click first if the object should move | Select only |
| Ctrl + left click | Toggle the hit object in the selection | Toggle the hit object in the selection | Toggle the visible object in the shared selection |
| Ctrl + left drag | Add a directional marquee result to the selection | Add a directional marquee result to the selection | Add a directional marquee result to the shared selection |
| Alt + left click | Cycle overlapping selectable objects at the pointer | Cycle overlapping selectable objects at the pointer | Cycle distinct visible objects under the pointer |
| Alt + left drag | Pan without selecting or editing an object | Pan without selecting or editing an object | Pan without changing the shared selection |
| Left-to-right Ctrl marquee | Add fully enclosed objects | Add fully enclosed objects | Add fully enclosed selectable presentations |
| Right-to-left Ctrl marquee | Add crossing or enclosed objects | Add crossing or enclosed objects | Add crossing or enclosed selectable presentations |
| Middle drag | Pan from any hit location without changing geometry or a draft | Pan from any hit location without changing geometry | Pan |
| Space + left drag | Pan from any hit location without changing geometry or a draft | Pan from any hit location without changing geometry | Pan |
| Right click | Cancel a pending drawing or new placement while retaining committed geometry; otherwise open relevant object/canvas actions | Cancel a pending new placement; otherwise open relevant object/canvas actions | Open object or view actions |
| Right drag | Pan without changing geometry, cancelling the draft, or opening a menu | Pan without changing geometry or opening a menu | Orbit |
| Wheel | Zoom about the pointer without changing the draft | Zoom about the pointer | Zoom |
| Double click on object | Preserve an existing selected group member, otherwise select the target, then open contextual properties once | Same | Preserve a selected group member, otherwise select the target, then open contextual properties |
| Double click on empty canvas | No special action | No special action | No special action |

Ctrl is the selection modifier in plan and 3D views. Selection marquees do not replace an existing
selection. A left-to-right marquee uses enclosure rules; a right-to-left marquee
uses crossing rules. The 3D marquee highlights the visible members of the shared
selection and retains selected plan-only objects. Escape, capture or focus loss,
or a changed source, camera or view extent abandons the marquee without applying
its result.

In plan views, vertical wheel and touchpad input zooms about the pointer.
Horizontal-only touchpad packets or tilt-wheel input pan horizontally through
the same navigation path as canvas dragging. Pixel input retains its logical
travel; horizontal detents use a small logical step. A diagonal packet with
vertical travel follows the existing zoom rule. These gestures preserve drafts
and selection, and retire captured edit previews through navigation authority.

Alt-click in an idle plan cycles the distinct eligible objects at the pointer,
including labels, furniture, room interiors and reference images. The ordinary
pick comes first when the current selection is outside that list. The active
selection filter applies. An empty Alt-click places no drawing point. Ctrl has
priority when both modifiers are held. A pending drawing or new placement must
be finished or cancelled before overlap selection starts. The press retains its
displayed source and camera generation; navigation away and back cannot revive
it. Alt-drag follows canvas pan and never moves a selected object.

In 3D, Alt-click cycles the distinct semantic objects returned by the active
visible-scene pick, skipping repeated faces or material presentations of the
same object. Choosing a target replaces the group with that one object; empty
Alt-click and Alt-drag preserve the group. Rapid Alt clicks each advance once,
while Ctrl+Alt retains the normal Ctrl policy. Alt input cancels an armed Move
before choosing a target or panning.

Plan pen input follows the button reported by Windows and the pen driver: the
tip uses left-click behavior, a right-mapped barrel uses cancel/context or pan,
and a middle-mapped barrel pans. The initiating button and device retain
ownership until release. Additional buttons, another pen, a mouse or palm touch
cannot finish that drag. Focus/capture loss and a disconnected device retire it.
Synthetic mouse events cannot duplicate a pen or touch action. One-finger input
uses the plan pointer contract; a second finger cancels that edit and starts
pan/pinch navigation until all fingers lift.

Native 3D pen packets use the same pointer controls as the mouse: the tip
selects or operates an armed Move/manipulator, a right-mapped barrel orbits on
drag and opens context actions on a stationary release, and a middle-mapped
barrel pans. The initiating pen and button own the gesture. Losing that button,
the device, focus or capture cancels the edit; other buttons cannot commit it.
Live pen input accepts palm contacts without starting a second action. After
release, a short duplicate-mouse guard distinguishes pen-generated input from
independent mouse input. Native touch retains the actual device and contact IDs:
one finger uses selection or an armed Move/manipulator; a second finger restores
the object before pan/pinch navigation starts. Navigation remains exclusive
until every contact lifts. Own camera updates retain the original source,
selection and display proof; they cannot recapture a changed document. Focus,
capture, source/display changes, Escape or device cancellation retire the
gesture. Pressure and tilt do not alter authored dimensions.

Vertical wheel input zooms at the pointer in both plans and 3D. Angular wheel
deltas take priority when a driver reports both forms; pixel-only touchpad
input is supported without applying display scaling twice. Native 3D retains
fractional movements until they produce a native pixel, only within the same
source, camera, display, device and pointer anchor. Ending a scroll, retiring an
interaction or changing that context discards the remainder. Horizontal-only
scrolling does not create vertical zoom.

## Explicit 3D Move

`Move object` is available from the 3D context menu for a selected transformable
object. It arms one plain-left drag and commits one document transaction on
release. Ctrl starts additive selection and cancels an armed Move; Alt cancels
it before overlap selection or pan. Middle drag
and Space + left drag pan; right drag orbits. Navigation never translates an
object. Move and single-object manipulator controls are unavailable for a group,
including a mixed group with only one member visible in 3D.

Escape, focus loss, capture loss, hiding the view, changing the camera, or
starting another navigation gesture cancels Move and restores the unmodified
presentation. A later mouse release cannot commit a cancelled move.

## State and feedback rules

- Only one gesture owns the pointer at a time. Another button cannot steal or complete it.
- The active drawing layer is selected in the project hierarchy and marked in blue.
- Every new symbol or label stores its owning layer ID and is listed beneath that layer.
- Selected objects receive a blue contrast outline. A compact `Selected` or `N selected` badge reports selection without opening Properties.
- Dragging a movable selection shows a transient preview; release creates one undoable command. An unsupported mixed group is rejected as a unit.
- After the first drawing click, the pending edge follows the pointer with its live length.
- Each click places the next measured-boundary node. Clicking the highlighted first node closes a valid outline.
- Component placement is a distinct pending state. Escape or the drawing context menu cancels it.
- A pending furniture or equipment item follows the pointer as translucent artwork using its chosen physical footprint. Its click or drop commits at that same location. Placement retains the displayed source, layer and horizontal plan axes; a changed source or context cancels it. The preview never participates in selection, snapping, overview extents or output.
- Wall previews follow the effective snapped pointer after the first endpoint.
- Architectural wall clicks retain the displayed horizontal plan's source and axes. Connected endpoints snap to authored baselines on the active floor, including in shifted or rotated named plans. Cropped endpoints outside the view are not snap targets. Elevation and section views refuse new wall points with an explanation.
- A committed wall continues the chain in the same architectural plan. A changed source, layer or view refuses the next point; cancellation keeps already committed walls. Area and loose measured-line construction remain available in the Measurement workspace.
- Door, Window and Doorway actions and hosted library items use the current Measurement or horizontal Architectural plan. Placement retains its displayed source and axes; rotated or shifted plans convert the pointer once into model coordinates and project the preview back. Only visible walls on the active layer can host an opening. Cropped portions outside the plan cannot accept it. Changing the source, workspace, layer or plan cancels that placement; Escape or stationary right-click cancels without adding an object.
- An opening preview follows its physical wall, with the same width, height, sill and style used at commit. The completed wall, sibling openings, manufactured assemblies and affected joins must admit the proposed cut before one undoable command changes the document. Recovered projects use the same authoritative model in placement, properties and output.
- Right click is evaluated on stationary release. Crossing the drag threshold prevents the menu.
- An unmodified object double click opens the same contextual editor as right click → Properties. Ctrl-double-click only performs the first selection toggle. Active drawing and placement consume the second press so they cannot add a duplicate point or place two components.
- In 3D, right-clicking or double-clicking a selected group member preserves the whole selection. Object-specific actions apply only to a single object; Copy and Delete use the shared selection. A changed source or selection invalidates retained menu actions, and a pending plan placement prevents unrelated 3D object actions.
- Pan, zoom, fit, and overview navigation refresh the effective cursor position used by measurements and contextual UI.
- Selection and move previews are screen-only and never appear in print or export.
- Plan picking and Ctrl marquee use the rendered curve paths, custom stroke outlines and effective paper, model or cosmetic line weights. Round caps and joins retain a small selection tolerance. Filled regions preserve their actual holes, and explicit opening spans remain selectable without painting an imaginary closed pane. Furniture remains selectable within its physical footprint. The spatial picking envelope retains these width rules at every zoom and DPI.

## Keyboard behavior

| Key | Behavior |
|---|---|
| Escape | Cancel the active gesture or explicit Move; otherwise cancel the pending tool step or clear selection |
| Enter | Finish a valid boundary or end an architectural mouse wall chain, keeping its committed segments |
| F | Fit drawing or model content |
| D | Open precise boundary input while drawing |
| Ctrl+Z / Ctrl+Y | Undo or redo through the current document or draft route |

## Remaining interaction work

- Selected symbols use an oriented selection frame that retains their saved rotation. Corner handles scale proportionally; side handles resize local width or depth with the opposite edge anchored. Canvas dimensions show the current physical footprint.
- Rotation uses a positive counterclockwise model angle. The handle and frame retain the committed orientation and preview the resulting angle, snapping to absolute 45-degree increments (including 0°, 90°, 180° and 270°); Shift bypasses snapping for fine adjustment. The canvas callout displays the current angle while rotating. Release commits once, Escape restores the document, and editing controls never appear in output.
- Circular columns retain the selection-frame angle even though their cylinder is rotationally symmetric. Property edits, history navigation and save/reopen preserve it, so the rotation pin remains attached to the committed frame.
- Selected walls have endpoint grips in Measurement, Site and horizontal Architectural plans. Dragging one changes the physical baseline length and direction with the opposite endpoint fixed. Curves retain their signed sweep. The grip's larger hit area preserves the original press offset before snapping. The preview solves connected geometry, admits the completed physical hosts and retains the exact command for one released Undo step. Crop and depth limits remove grips for invisible endpoints; elevation and section views use Properties. Navigation, source or context changes invalidate the retained edit even if the view later returns to its earlier position.
- Selected structural beams have endpoint grips in those same horizontal views. The chosen endpoint changes X/Y while both original endpoint elevations, the opposite endpoint and the actual cross-section/up direction stay fixed. The readout uses physical 3D span, and the native beam codec rejects a degenerate axis or unstable section frame. Preview and release retain the same complete source/context authority and exact admitted command as wall endpoint edits.
- Selected independent slabs and rooms have source vertex grips on their outer and hole rings in those same horizontal views. The edit changes the outgoing start and preceding end while preserving signed sweeps, other vertices, all holes and unrelated properties. Plan-only rooms gain no inferred volume measurements; wall-derived rooms are edited through their source walls. Net area deducts holes and the displayed perimeter is the outer ring. Only source vertices supported by the retained projected geometry can offer grips; crop/depth intersections never become source vertices. Preview and release use the same captured authority and admitted command, with one Undo step.
- Direct geometry grips for additional architectural object types remain in the production scope.
- Windows hardware qualification of pen barrel mappings and touch navigation.

## Acceptance sequences

Hosted doors, windows and bare openings on straight or circular walls have two jamb
handles in a horizontal plan, including a shifted, rotated or reflected named
plan. Both jambs must be visible inside the crop and the entire opening must
survive the view's depth limit. Dragging one jamb projects the
pointer displacement along the wall and keeps the other jamb fixed. Circular
hosts use continuous arc stations, including across the angle branch seam;
grabbing within the handle's hit area does not jump the jamb. Only
width and the necessary host offset change; height, sill, wall thickness,
handing and frame dimensions remain unchanged. The live readout shows the
opening's actual width and height. Circular door-swing geometry and the wall
cut are regenerated together for interactive preview, rather than stretching
painted strokes. Invalid placements are shown as rejected proposals. A
completed drag publishes the exact captured-source command admitted by its
queued preview after validating the host, sibling openings, manufactured
assemblies and wall joins. Pending release waits for that one proposal; it
cannot substitute a newer source or width. Escape, focus loss, navigation,
display changes or a refreshed source cancels the proposal, even if the camera
later returns to its original values. These handles and previews are excluded
from printed and exported geometry. Partially clipped openings and nonhorizontal
views retain their property editor without offering misleading jamb grips.

Focused interaction checks cover click selection, object drag with one commit,
empty-canvas click drawing, plain-drag pan, Space and middle-button pan, Ctrl-click toggle, directional Ctrl marquee, mixed-button
release, stationary right click versus right drag, double-click properties and authoring suppression,
Escape and capture cancellation, explicit 3D Move exactly-once commit, selection
visibility at zoom extremes, and exclusion of transient feedback from output.

Manual Windows validation still covers physical mouse capture, high-DPI pointer
thresholds, pen and touch hardware, context-menu placement across multiple
monitors, and long sessions that combine drawing, navigation, save/reopen,
print, and export.
