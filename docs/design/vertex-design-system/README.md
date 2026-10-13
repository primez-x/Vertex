Vertex is a free, open-source Windows tool for measuring, appraising and designing homes. Its interface is a quiet, neutral frame around one precise drawing: the chrome stays grayscale, and color is spent only on the sketch itself, on selection, and on status. If a color appears in the chrome, it should mean something.

## Principles

- **The drawing comes first.** Chrome is `surface-0`/`surface-1` gray with `ink` text. Hue belongs to the canvas (`draw-*`, `area-*`), the single `accent`, and status (`success`, `warning`, `danger`).
- **Numbers are exact and easy to read.** Every measurement is set in the mono family (`figure`, `dimension`, `coordinate`) so digits line up and a 1 never looks like an l.
- **Dense, not cramped.** Desktop density on a 4px grid: 28px controls (`control-md`), 13px body text, 8px gaps (`space-2`). Expert users live in this app for hours.
- **Borders, not shadows.** Docked panels are flat rounded containers outlined in `border`. Only things floating over the canvas get `shadow-float` or `shadow-menu`.

## Content fundamentals

- **Voice:** plain, precise and calm. Talk like a careful appraiser, not like marketing copy. Address the user as "you", sparingly. Never "we", never exclamation marks, never emoji.
- **Casing:** sentence case everywhere: menus, buttons, titles, column headers ("Area calculations", "Close polygon"). Abbreviations stay upper case: GLA, ANSI, PDF, MLS.
- **Buttons are verbs:** "Close polygon", "Add area", "Export sketch". Not "OK" when a specific verb exists.
- **Units:**
  - Imperial lengths are written feet-inches with straight primes and a space: `24' 6"`. Use decimal feet only when the user picks decimal mode: `24.50 ft`.
  - Areas: `1,842.5 sf` (lowercase "sf", thousands separator, one decimal). Metric: `171.2 m²`.
  - Angles: `45°`, no space.
  - Never mix systems in one view.
- **Appraisal terms are used exactly:**
  - GLA (gross living area) means above-grade finished space only, per ANSI Z765.
  - Below-grade space is "basement", never GLA. Garages, porches and decks are "non-GLA".
  - Label area rows with their type, then a name: "GLA · First floor".
- **Errors say what happened and what to do:** "Polygon is open — 2' 3" gap at the north wall. Close it to compute area." Lead with the fact, end with the fix.
- **Hints use present tense and name keys in plain text:** "Hold Shift to constrain to 45°", "Press Enter to accept the length".

## Color

Two themes, `dark` (primary) and `light`. Every text color below meets 4.5:1 on the surfaces its usage note lists, in both themes.

- **Surfaces, back to front:**
  - `canvas`: the drawing only.
  - `surface-0`: app frame, toolbars, status bar.
  - `surface-1`: docked panels and dialogs.
  - `surface-2`: inset controls such as fields and table rows.
  - `surface-3`: hover and pressed states.
- **Text:**
  - `ink` for content.
  - `ink-muted` for labels, units and headers.
  - `ink-faint` only for placeholders, disabled text and shortcut hints, and only on surface-0 to surface-2.
- **Accent:** `accent` is the only chrome hue. It is used for:
  - the primary button
  - the active tool
  - selection (`draw-selection`)
  - focus (`focus-ring`)

  Text on an accent fill uses `on-accent`, which is dark ink in the dark theme. Selected rows use `accent-subtle` under `ink`. Use at most one primary button per view.
- **Status:** `success` (teal), `warning` (orange), `danger` (red). Each always comes with an icon or a word ("Closed", "Open", "Invalid"); never color alone. Success is teal so it is never separated from danger by red vs. green alone.
- **Drawing:**
  - Lines:
    - `draw-wall` for exterior walls and perimeters (`stroke-wall`).
    - `draw-wall-interior` for interior walls (`stroke-hairline`).
    - `draw-dimension` for all dimensioning.
    - `draw-guide`, dashed, for temporary guides.
    - `draw-snap` for snap glyphs. Snap glyphs are shapes: a square for endpoint, a triangle for midpoint, an X for intersection.
  - Grid: `grid-minor` every foot, `grid-major` every 10 ft.
- **Area fills:** translucent so walls and grid read through them.

  | Area type | Token | Counts toward GLA |
  |---|---|---|
  | Living area | `area-gla` | Yes |
  | Basement | `area-basement` | No |
  | Garage / carport | `area-garage` | No |
  | Porch / deck / patio | `area-porch` | No |
  | Other | `area-other` | No |

  Never use an area fill for chrome. In the area table, the legend chip is the only place an area color appears outside the canvas.

## Type

- IBM Plex Sans (`sans`) for the interface; IBM Plex Mono (`mono`) for every number. Both are OFL-licensed, so they can ship with the GPL app; on the web they load from Google Fonts. Fallbacks are Segoe UI and Cascadia Mono.
- Interface scale:
  - `heading` 20/26 for dialog titles.
  - `title` 15/20 for panel titles.
  - `body` 13/18 as the default.
  - `body-strong` for totals and section headers.
  - `label` 12/16 for field labels and tabs.
  - `caption` 11/14 for hints and the status bar.
- Measurement scale:
  - `figure` 13/18 for fields and tables, right-aligned.
  - `dimension` 12/14 on canvas, scaling with zoom.
  - `coordinate` 11/14 in the status bar.
- Do not use more than two weights (400 and 600, plus 500 for `label` and `dimension`). Do not set body text below 11px.

## Space, size and shape

- 4px grid:
  - `space-1` 4: icon-to-label gap, toolbar gaps.
  - `space-2` 8: control padding, gaps between fields.
  - `space-3` 12: panel padding.
  - `space-4` 16: dialog padding.
  - `space-5` / `space-6` for large layout.
- Fixed heights:
  - `control-md` 28 is the default control height; `control-sm` 24 inside dense panels and tables.
  - `tool-button` 32 square buttons inside a `toolbar` 40.
  - `status-bar` 24.
  - `icon` 16.
- **No square edges.** Every corner is rounded: `radius-sm` 4 for small marks (chips, checkboxes, grips, tooltips), `radius-md` 6 for controls (buttons, tool buttons, fields, tabs, status bar), `radius-lg` 10 for containers (toolbars, inspector sections, tables, panels, dialogs). Nested shapes use the outer radius minus the gap between them, so curves stay concentric.
- Line ends are round too: icons, dimension and extension lines, and dividers use round caps and joins.

## States

- **Hover:** quiet controls fill with `surface-3`; primary darkens with an 8% black overlay.
- **Pressed or active tool:** `accent` fill with `on-accent` icon.
- **Selected row:** `accent-subtle`.
- **Focus:** a 2px solid `focus-ring` outline, offset 1px. It shows on keyboard focus only (`:focus-visible`) and is never removed.
- **Disabled:** text and icon go `ink-faint`; borders go `border`; no opacity on the whole control.
- **Invalid field:** border and message in `danger`, with a word in the message.

## Iconography

- Line icons on a 16px box (`icon`), 1.5px stroke, round caps and joins, no fills, in `currentColor` (`ink` by default, `on-accent` when active). The bundle's `Icon` component draws the core set: select, line, wall, rectangle, polygon, arc, dimension, area, text, pan, zoom, snap, grid, layers, undo, redo, chevron, close, check, warning.
- Vertex has no third-party icon font yet. When a needed glyph is missing, draw it in the same style; do not mix in filled or two-tone icons.
- No emoji, ever, in UI or in exported reports.

## Logo

There is no Vertex logo yet. Set the name as plain text, "Vertex", in IBM Plex Sans at 600. Do not draw a mark until one is designed.

## Components

The bundle (`window.Vertex`) provides:

- `Button`, `ToolButton`, `Toolbar` (with `Toolbar.Divider`)
- `NumberField`, `Tabs`, `InspectorSection` (with `InspectorSection.Row`)
- `DimensionLabel`, `AreaTable`, `StatusBar`, `Icon`
- the `formatLength` and `formatArea` helpers

They render the desktop app's Qt widgets in HTML for specs, docs and web previews. The Qt implementation should match their metrics and tokens exactly.
