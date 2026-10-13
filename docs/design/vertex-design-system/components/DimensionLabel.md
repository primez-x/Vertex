# DimensionLabel

DimensionLabel is the on-canvas dimension: extension lines, dimension line, architectural ticks and the length in mono.

- **Provide:** `value` (decimal feet, formatted per `units`, or a preformatted string), `length` in px, `state`.
- `default` draws in `draw-dimension`; `selected` and `editing` switch to `draw-selection`. Editing replaces the text with an inline field: Enter accepts, Esc cancels.
- The text sits centered on the line with a `canvas` knockout so grid lines never cross it. Ticks are 45° slashes, not arrows.
- Text scales with zoom from the `dimension` style; never below 9px on screen.
