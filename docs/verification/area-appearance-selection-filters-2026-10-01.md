# Area appearance and canvas selection categories

## User workflows

Select one closed area and open **Area attributes > Area appearance…** from its
properties. Outline/fill colors, None/Solid/Hatch, hatch scale, paper outline
width and visibility are editable. Reset followed by Apply removes only that
area's override and restores its classification preset. Geometry, declared
appraisal facts and calculated totals remain authoritative and unchanged.

The compact bottom-right selection dropdown chooses All items, Areas, Building
objects, Dimensions, Text labels, Symbols or Reference images. Both plan
canvases use it for new clicks, Ctrl-clicks, window/crossing marquees,
double-click properties and context targets. Existing selections, transform
controls and visible content remain intact. Ignored occupied content does not
become a drawing start. Dimension lines and label-only area dimensions retain
their semantic category; generated titles remain presentation rather than
independent editable text.

## Preservation and admission

Area edits use captured revision/context guards and one ordinary Document
command. Read-only or stale edits are refused. Raw owner protection, extensions,
state/record/style metadata and unrelated overrides are retained. Unchanged
Apply adds no edit, including optional-free older overrides. Editing another
control does not materialize absent width/scale fields or change the older
stroke width. Duplicate area override providers report an error.

Optional `paper_line_width_mm` and `hatch_scale` fields remain within annotation
state version 3. Validation rejects nonfinite/out-of-range values. Absence keeps
the older defaults. Explicit paper widths survive analytical architectural
plan generation and share the screen/output renderer.

## Verification

Baseline regressions failed on missing optional-field roundtrip, missing
selection filtering, and missing desktop Area appearance control. Independent
review identified real dimension-line classification and optional-free no-op
preservation gaps; both were fixed. A further pointer regression reproduced a
miss on a thick paper outline's outer edge. Picking, crossing marquees and
rendering now share the paper-width conversion, and the focused check passes.

Release builds succeeded for Vertex and the affected native/core executables.
The focused `--area-appearance-only` run passed with the actual editor, both
canvases' filter dropdown, real length/area dimension clicks, source-preserving
style commands, no-op/visibility/legacy behavior, history/reopen and PDF output.
The full `appraisal_desktop_workflow` check passed in 12.75 seconds after the
duplicate-provider refusal regression was added. Nine affected CTest checks
passed across the focused runs: symbol transforms, named-plan vertex editing,
boundary canvas, annotation catalog/codec, annotation clipboard, Appraisal
workflow, requirements schema and source-kit contract. `git diff --check` passed.
The allowlist matches all 1,118 required tracked files. Unrelated local work was
preserved.

Root inspected the actual dialog and exported page under
`artifacts/area-style-selection-20261001-verified`. The PDF contains the chosen
outline and hatch hue. The pixel check measures the fill's white-composited hue
across antialiased stripe coverage instead of assuming opaque pixel colors.
The earlier fixed-RGB assertion was a test-oracle failure, not missing output.

The rotation implementation remains verified: `symbol_transform_desktop` passed
in 14.30 seconds, covering retained frame/pin angles, common-angle snaps, Shift
fine adjustment, live degrees, repeated gestures and history/save/reopen.
`named_plan_vertex_desktop` passed in 8.40 seconds. Annotation catalog, annotation
entity codec, full boundary canvas and annotation clipboard checks also passed;
the codec check needs the native DLL directory in the process PATH for this
architecture-enabled build.

Current development executable: `build/windows-release/vertex.exe`, SHA-256
`9aab037e0e21011a2a1da0c509fa76b0d85d40c15f851e23ec58ff319bb9422b`.
Launch with `scripts/run.ps1 -Configuration Release`, which supplies local
runtime/plugin paths. The previously installed checkpoint remains separate.

These are scoped development changes. Broader linked-object selection policies,
object/output-view appearance editors, full Apex compatibility and production
visual/print qualification remain open. The full production goal remains active.
