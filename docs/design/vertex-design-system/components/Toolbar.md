# Toolbar

Toolbar holds tool buttons and quiet actions on `surface-0`.

- **Provide:** `ToolButton`s and ghost `Button`s as children, `Toolbar.Divider` between groups; `orientation` (`horizontal` 40px tall under the menu bar, `vertical` 40px wide on the canvas edge).
- `floating` lifts it over the canvas with `shadow-float` and `radius-lg`, for contextual tools that appear near a selection.
- Order groups by workflow: select/pan, draw (wall, polygon, arc), annotate (dimension, text, area), view (grid, snap, layers).
- Every toolbar is a rounded container (`radius-lg`, `border`). Docked toolbars sit inset from the window edge by `space-1` and take no shadow.
