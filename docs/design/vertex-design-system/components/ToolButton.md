# ToolButton

ToolButton is a square 32px icon button that selects a drawing tool.

- **Provide:** `icon`, `label` (the tool's name; becomes the tooltip and accessible name), optional `shortcut`, `active`.
- Exactly one tool is `active` at a time: accent fill with `on-accent` icon. Hover is `surface-3`.
- Always place tool buttons inside a `Toolbar`; group related tools between `Toolbar.Divider`s.
- The tooltip reads "Wall (W)". Keep shortcuts to one letter where possible.
