# Button

Button triggers a command; four variants, two sizes.

- **Provide:** a verb label in sentence case ("Close polygon", "Export sketch"), optional `icon`, `onClick`.
- `primary`: accent fill with `on-accent` text. At most one per view or dialog: the action that finishes the task.
- `secondary` (default): `surface-2` with `border-control`. Everything else.
- `ghost`: no chrome until hover (`surface-3`). For panel headers and dense rows.
- `danger`: outlined in `danger` for destructive commands; confirm before deleting sketch data.
- `size="sm"` (24px) inside inspector sections and tables; `md` (28px) elsewhere.
- An icon-only button must have `aria-label`. Prefer `ToolButton` for tool selection.
- Disabled buttons drop to `ink-faint` text and `border`; never just lower opacity.
