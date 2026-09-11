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
