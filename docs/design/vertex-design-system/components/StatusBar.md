# StatusBar

StatusBar runs along the bottom of the window: the current prompt on the left, readouts on the right.

- **Provide:** `message` (the active tool's next step, or a result) with optional `tone`, and `items` (cursor X/Y, snap mode, scale, units).
- Prompts are imperative and short: "Click to place the next corner · Enter to finish". Results with a `tone` always carry an icon and a word.
- Readouts are mono `coordinate` style; an `active` item (e.g. snap on) shows in `accent-text`.
