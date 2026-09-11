# Workspace accessibility and field-input contract

`WorkspaceAccessibilityProfile` records the minimum interaction surface for
both workspaces: keyboard navigation and predictable focus, accessible
properties and commands, light/dark/high-contrast themes, pen and touch
controls, an on-screen measurement keypad, and qualification layouts from
1366x768 through 4K at 100%, 150%, 200%, and 400% scaling.

The profile is a strict version-1 JSON contract. It fails closed if any required
control or theme is omitted, a layout is below the supported minimum, a scale is
outside the declared matrix, or an unknown field appears. It expresses the
product target and gives the test harness a stable matrix; it does not claim
that a physical pen, touch device, screen reader, keyboard-only run, or every
Qt style has passed. Those observations remain required for the final quality
gate.

## Desktop shortcuts and measurement keypad

Open **Shortcuts** on the workspace toolbar, or search for **Customize keyboard
shortcuts** in Commands. Each listed command accepts one key combination;
clearing it disables that command's shortcut. Save validates the entire set,
rejects duplicates and reserved text/canvas keys, then writes atomically to
`keyboard-shortcuts.json` in Qt's per-user `AppConfigLocation`. This is an offline
workspace preference shared by project windows opened afterward; it does not
alter a project archive. The editor's Cancel button leaves both active and saved
bindings intact. Invalid or unsupported saved settings activate the complete
default preset, with a diagnostic in the shortcut editor.

The file contains `version: 1` and a `bindings` object with all nine command IDs:
`new`, `open`, `save`, `save-as`, `commands`, `measurement`, `architectural`,
`annotations`, and `define-area`. Values are Qt portable shortcut strings (for
example `Ctrl+S`), or an empty string to disable a binding. The file can be copied
between local workspaces using the same schema. Existing open windows retain
their current bindings until edited or reopened.

The **Apex v7 compatible subset** preset assigns F2 to Save, F3 to Open, and F4
to Define Area, following the official [Apex v7 user-interface guide](https://apexwin.com/support/ApexSketchv7/ApexSketchv7-User-Interface.pdf)
and [Define First guide](https://www.apexwin.com/support/ApexSketchv7/ApexSketchv7-DefineFirst.pdf).
Other commands retain Property Studio defaults. This subset does not establish
full Apex keyboard or behavior parity. Canvas F, D, Enter, Escape, Undo and Redo
retain their existing meanings; navigation and ordinary text-editing keys cannot
be reassigned by this editor.

The inspector's **Measurement keypad** button (also available in Commands)
opens a local dimension-entry panel for the selected object's editable Length,
Height, or Thickness. Large on-screen digit, fraction, decimal, space, and unit
buttons compose the same exact expressions accepted by the existing inspector,
including mixed feet/inches. Unsuffixed values use the workspace's current unit.
Apply uses the existing validation and document commands, preserving undo;
invalid input remains in the panel with an error. Wall lengths continue through
the existing constraint preview before any geometry change. Cancel performs no
edit, and a changed document or selection invalidates an open keypad session.
This panel does not claim DISTO connectivity or physical touch-device qualification.

`desktop_workflow` exercises preset application, persistence, cancellation,
duplicate/reserved binding rejection, malformed settings fallback, on-screen
measurement entry, invalid quantities, and undo.
