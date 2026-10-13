# Icon

Icon draws Vertex's core line glyphs: 16px box, 1.5px stroke, round caps and joins, `currentColor`.

- **Provide:** `name` from the core set; `label` only when the icon stands alone without visible text (otherwise it is hidden from assistive tech).
- Color comes from the parent: `ink` by default, `on-accent` on accent fills, status colors in messages.
- Use 16px in toolbars and buttons, 12px inline with `caption` text. Never fill, never two-tone.
