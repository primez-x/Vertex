# Saved-view drawing appearance

This checkpoint implements independent plan/elevation/section drawing appearance
within APX-ANNO-003. It does not certify full Apex parity or production acceptance.

## User workflow

Choose a saved architectural view, open **Architectural view settings**, then
**Drawing appearance…**. Set outline/fill, None/Solid/Hatch, paper line weight,
hatch scale and visibility for the whole view or a physical source object.
Reset changes only the selected scope. Hidden views remain selectable for recovery.
The manual checklist has individual tasks U345–U348 for these actions.

Global drawing appearance is inherited, followed by whole-view style and then
the explicit object style in that view. Local visibility can recover global
presentation hiding but never bypasses layers, phases or source filters.
Hosted doors/windows follow their resolved host wall visibility. Whole-view
hiding also suppresses annotations, reference images, grids and overlays.
The shared derived scene feeds canvas and sheet/PDF rendering. Its identity is
the graph entity and local view ID together; duplicate local IDs are supported.

Appearance is optional sheet model 7 intent, requiring native format 24 and
extraction version 22 throughout retained history. Absent appearance remains
model 6. Typed admission validates target references. Filtering preserves
dormant entries; deletion repairs them atomically. Raw owner policy and opaque
metadata survive targeted edits; stale/read-only/no-op guards protect history.

## Verification

The baseline checks reproduced missing model/editor support and a canvas move
committing without a configured exact proposal. The corrected canvas rejects
that unavailable proposal while preserving asynchronous completion and the
providerless translation path. Escape and late completion coverage confirms
that the next gesture remains usable.

The independent review identified three scene defects. Resolved host masking
now suppresses inherited hosted openings in elevations/sections. Built-in
section labels use the same canonical graph/view as geometry. Hidden
presentation dimensions no longer produce derived errors that block another
visible sheet; analytical source validation remains unconditional. The actual
solid-view fixtures cover those corrections, including visible output refusal
for an unresolved dimension placement.

Root review also reproduced legacy clipboard failure on absent overlays.
The remapper now preserves model 1/2 wire absence rather than requiring a
later field. Native paste checks cover model 1/2 and model 7, remapped source
and appearance references, exact opaque strings and one-step Undo.

The Release application and affected targets built. Ten distinct suites passed:

- `output_view_appearance_desktop_tests`
- `object_appearance_desktop_tests`
- `connected_wall_canvas_tests`
- `wall_dimension_desktop_tests`
- `appraisal_details_panel_tests`
- `sheet_layout_dialog_tests`
- `sheet_view_model_tests`
- `sheet_view_entity_codec_tests`
- `project_store_tests`
- `project_exchange_tests`

The five affected desktop suites were repeated after the final clipboard
change. The canvas and four core suites passed against their unchanged final
implementations. The output-view fixture exercises actual controls, separate
saved views of a real wall, door and window, native save/reopen, shared sheet
PDF, complete visibility recovery, duplicate owner identities, deletion,
Undo/Redo, source preservation and guarded refusals. Core fixtures cover strict
shape/value validation, dormant styles, all-history reader qualification,
recomputed-digest downgrade refusal and recovery-bearing archive roundtrips.

Native fixtures use offscreen windows, isolated settings and noninteractive
error handling. Root inspected actual dialog, canvas and PDF captures under
`artifacts/output-view-appearance-20261003`. Real PNG source/preview assets
exercise retained reference rendering. External raster decoding is not claimed:
the mutable development directory is correctly refused by the import sandbox,
and no sandbox restriction was relaxed. Historical failures remain retained.

## Remaining scope

SVG instance palette overrides, broad physical-family visual qualification,
constraint previews of locally recovered sources, and full production printing
remain open. Final ANSI normative qualification and Apex compatibility are
separate open requirements. The existing **Details** tab exposes GLA, category
and floor totals, selected-boundary measurements and reasons, Setup and Full
report without claiming ANSI approval. This is technical evidence, not user
acceptance or a complete production release.
