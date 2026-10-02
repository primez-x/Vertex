# Local reusable text library

Vertex uses `text-library.json` in Qt's Windows `AppConfigLocation` for reusable
text. The file is separate from `.bldproj` projects and can be backed up as an
ordinary file. It is created by the first successful Save, not merely by opening
the library. No account or service is involved.

Version 1 has exactly `version` and `entries` at the root. Each entry has exactly
`id`, `name`, `category`, `content` and `style`. IDs are unique stable tokens of
at most 256 bytes; built-in template IDs are reserved. The editor generates
`user-text-<UUID>` IDs. Names and categories contain readable single-line UTF-8
text of at most 256 bytes. Content is readable UTF-8 of at most 65,536 bytes,
with line breaks and tabs allowed. Limits are 1,000 entries and 4 MiB per file.

`style` contains `font_family`, `text_height_metres`, `stroke_width_metres`,
`stroke_color`, `fill_color`, `fill_pattern`, `bold` and `italic`, following the
annotation style contract in [project format](project-format.md). Text height
is a positive model-space length, bounded to 100 m; stroke width is nonnegative,
bounded to 1 m. The editor preserves existing fill and stroke-width values while
editing its exposed font, height, color and emphasis controls. Empty or generic
sans-serif canvas fonts use the bundled workspace font; unavailable named fonts
use Qt's local fallback. No fonts are fetched from the internet.

Example:

```json
{
  "version": 1,
  "entries": [{
    "id": "user-text-example",
    "name": "Site note",
    "category": "notes",
    "content": "Verify dimensions\nOn site",
    "style": {
      "font_family": "sans-serif",
      "text_height_metres": 0.08,
      "stroke_width_metres": 0,
      "stroke_color": "#202733",
      "fill_color": "#ffffff",
      "fill_pattern": "none",
      "bold": false,
      "italic": false
    }
  }]
}
```

Insertion copies text and style into an ordinary project `LabelInstance`,
with a new instance ID, retained template ID, active layer and chosen position.
The library's Insert workflow uses annotation state v5 `model_plan` anchors:
positions are world XY, rendered through each horizontal plan's frame. Dragging
in a rotated/reflected plan converts the pointer delta back to world XY.
Rotation remains relative to the displayed view; these plan labels stay out of
elevation and section overlays. Existing view-overlay labels remain unchanged.
Library changes cannot alter placed instances. Project loading, editing and
output require no library file. New insertion and instance editing retain
unrelated annotation records, pinned symbol definitions and opaque metadata.

Saves validate the complete candidate, acquire a cooperating writer lock and
use `QSaveFile` atomic replacement with direct-write fallback disabled. Exact
loaded bytes are compared before and immediately before commit; stale, locked,
read-only, malformed, oversize or unsupported files are refused and preserved.
An unrelated non-cooperating writer can still race the final comparison and
rename; the lock coordinates Vertex writers rather than arbitrary editors.
The editor reports failures inline without discarding its draft. Close and
reopen the library to reload an external change. Unknown fields and future
versions are refused rather than silently rewritten.
