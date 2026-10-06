# Workspace UI

Vertex uses a Windows desktop shell designed around the canvas rather
than a legacy menu layout. The compact top toolbar keeps file, history,
workspace, and command actions one click away in a compact labeled row. The
primary commands use bundled icons, readable text, tooltips and accessible
names. Units remain available; paper size belongs to output settings. The theme
control stands alone at the far right. Secondary
authoring and presentation commands live in the **Tools** menu so the primary
row remains readable at 1366 px. Project identity is represented by the native
window title and its unsaved marker, leaving the canvas the first visible
content below the toolbar.

Properties are contextual: double-click an object or use its context menu to
open a compact quick-properties panel. A single selection shows its human name
and canvas handles without opening a permanent inspector. A sloped, flat, or gable roof exposes
run (gable length), span, rise, overhang, thickness, and derived pitch in place, with one explicit Apply
action. A zero rise is shown as a flat panel; invalid values stay in the
editor and do not create a history entry. The fields use the same quantity
parser and atomic document command as the full building-object editor, so
exact untouched dimensions and extension metadata remain intact.

Offline operation is a product guarantee and is not repeated as a persistent
banner or badge in the drawing workspace.

The main area is the canvas beside a resizable left panel with **Layers**,
**Library**, and **Details** tabs. Layers contains the property/building/floor/
layer hierarchy, inline add action, active-layer indicator, and visibility eyes.
Placed symbols and text belong to their assigned layer. Library provides inline
category/search and placement artwork; Details exposes calculations without
requiring a selection. The 2D/3D controls retain the same project. Panels use
the same spacing, focus rings, rounded surfaces, and light/dark/high-contrast
palette. Measurement and architectural canvases share the document but have a
light precision surface by default, with a dark canvas following the dark
workspace theme. Grid lines are deliberately low contrast so geometry and
dimensions remain the visual focus.

Each interactive plan canvas includes a compact overview map in its lower
right corner. It draws the same committed geometry in model coordinates and
shows the current viewport as a dashed frame; clicking any map location
recenters the live canvas there. The **Map** control at the bottom right hides or
restores it without changing geometry, and that preference is included in
saved workspace profiles. Grid, magnet/snap, fit and map are compact status-bar
controls. Saved profiles retain applicable panel proportions and the
architectural canvas/3D split. The map is an interaction aid only and is omitted
from PDF, SVG, print, and native-image output.

The [canvas interaction contract](design/vertex-canvas-interaction-contract.md)
defines one hybrid pointer surface, with **Wall** as the default drawing kind,
plus **Area** and **Measured lines**. There are no Select/Draw First/Define First
mode toggles. Workflow commands retain the precision capabilities behind that
surface. When drawing, the canvas shows a small readout beside
the pointer with the effective snapped X/Y coordinates. Once a boundary draft
has an anchor, the same readout adds the live delta length and angle. It is an
editing aid only; the readout is omitted from
fitted/exported scenes.

The toolbar glyphs are bundled inline SVG paths rendered by Qt's SVG module.
They contain no downloaded assets or runtime web dependency. Dialogs copy the
resolved workspace palette and stylesheet so shortcut, keypad, sheet, and
reference editors keep the same visual language as the main window.

The desktop smoke matrix captures the current Release presentation at
1366×768 and 1920×1080, at 100%, 150%, and 200% scale. The captures are evidence for
layout review, not a claim that every Windows theme, font substitution, display
driver, or accessibility device has been qualified. See
`artifacts/desktop-smoke/release/` and `artifacts/field-ui/` after running
`scripts/test-desktop.ps1`.
