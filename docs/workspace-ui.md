# Workspace UI

Property Studio uses a Windows desktop shell designed around the canvas rather
than a legacy menu layout. The top toolbar keeps file, history, workspace, and
command actions visible; secondary authoring and presentation commands live in
the **More** menu so the primary row remains readable at 1366 px. The branded
header identifies the active project and shows its unsaved state beside the
project name.

Offline operation is a product guarantee and is not repeated as a persistent
banner or badge in the drawing workspace.

The main area is a four-part workspace: a project navigator, a tool rail, the
shared Measurement/Architectural tabs, and a contextual Inspector. Panels use
the same spacing, focus rings, rounded surfaces, and light/dark/high-contrast
palette. Measurement and architectural canvases share the document but have a
light precision surface by default, with a dark canvas following the dark
workspace theme. Grid lines are deliberately low contrast so geometry and
dimensions remain the visual focus.

The toolbar glyphs are bundled inline SVG paths rendered by Qt's SVG module.
They contain no downloaded assets or runtime web dependency. Dialogs copy the
resolved workspace palette and stylesheet so shortcut, keypad, sheet, and
reference editors keep the same visual language as the main window.

The desktop smoke matrix captures the current Release presentation at
1366×768 and 1920×1080, at 100% and 150% scale. The captures are evidence for
layout review, not a claim that every Windows theme, font substitution, display
driver, or accessibility device has been qualified. See
`artifacts/desktop-smoke/release/` and `artifacts/field-ui/` after running
`scripts/test-desktop.ps1`.
