# AreaTable

AreaTable lists a sketch's areas by type with GLA and non-GLA totals.

- **Provide:** `rows` (`id`, `name`, `type`, `area` in sf, optional `perimeter` in ft), `units`, optional `selectedId` + `onSelect` to sync with the canvas selection.
- Every row shows a legend chip in its `area-*` fill plus the type in words; the chip is never the only cue.
- Figures are mono and right-aligned; totals are computed, never typed. Total GLA counts `gla` rows only, per ANSI Z765; basement, garage, porch and other roll into Non-GLA.
- Name rows by floor or use ("First floor", "Covered porch"), not "Area 1".
