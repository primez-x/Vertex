# InspectorSection

InspectorSection is a collapsible group of properties in the right-hand inspector.

- **Provide:** `title`, children (usually `InspectorSection.Row`s, each with a `label` and one or two `size="sm"` controls), optional `actions` (ghost sm buttons) and `defaultCollapsed`.
- Rows use a 96px label column in `label` style, `ink-muted`. Each section is a rounded container (`radius-lg`, `border`); stack them with a `space-1` gap.
- Order sections from most to least edited: Geometry, Area, Appearance.
