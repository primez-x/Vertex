# NumberField

NumberField takes a measured value with a unit suffix, set in mono and right-aligned.

- **Provide:** `label`, `unit` (ft, in, sf, m, °), `defaultValue` or `value` + `onChange`; `error` text when invalid.
- Accept feet-inches entry (`24' 6"`, `24 6`, `24.5`) and normalize on blur to the user's unit mode; never silently round below 1/8".
- Errors name the problem and the fix: "Must be greater than 0". The border and message turn `danger`, with a warning icon.
- Use `size="sm"` in inspector rows. Focus shows a 1px accent ring around the box.
