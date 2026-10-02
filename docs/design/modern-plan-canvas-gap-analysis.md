# Modern plan canvas gap analysis

The reference is a production floor-plan drawing, so the target is its visual
and semantic hierarchy rather than a screenshot-specific skin. Every visible
feature below must come from document geometry or retained presentation data
and must survive save, reopen, print, and export.

| Visible reference feature | Previous Vertex behavior | Implemented behavior |
|---|---|---|
| Fine graph-paper grid | One coarse adaptive lattice | Unit-aware Imperial/Metric intervals refine with zoom; visible subdivisions and grid snapping share the interval, with stronger major lines and a one-cell scale cue |
| Architectural line hierarchy | Bright orange walls and similarly weighted objects | Navy primary boundaries and walls, slate openings/components, lighter dimensions, and semantic draw ordering |
| Named rooms and areas | Names appeared only when users created separate text annotations | Room and meaningful area names derive from persisted `name` and `classification` fields and render as plain plan text |
| Calculated square footage on plans | Derived names lacked automatic area values | Qualified Appraisal areas append their calculated net area and workspace unit, using the report's precision and deductions; generic names can show the value alone |
| Labels remain readable around furniture | Labels stayed at a fixed center point | Derived labels evaluate alternate positions within their owning boundary to avoid placed components |
| Floor title | No plan title derived from the project hierarchy | The active floor name is anchored above the largest visible measurement area |
| Main, exterior, and accessory-area colors | Entity type alone selected a generic color | Living areas use blue, porch/patio/deck use green, garage/carport use orange, and excluded/service areas use distinct muted palettes |
| Garage and material hatching | Hatch renderer existed but ordinary areas never supplied it | Classification presentation supplies retained fills and diagonal garage/excluded-area hatching; saved overrides can replace it |
| Door swings and wall gaps | Door operation geometry was available | Door leaf/swing geometry stays above area linework in the plan hierarchy |
| Windows | Hosted windows only cut a gap in the wall | Hosted windows now add a double-line plan symbol across the validated opening |
| Recognizable furniture and fixtures | Components rendered, but sparse smoke output hid the workflow | The integrated plan fixture places beds, sofa, table, kitchen appliances, sink, toilet, and tub from the normal component catalog |
| Exterior dimensions | Analytical overlays existed but ended as plain intersecting lines | A public segment-length dimension command retains the stable boundary edge, with extension lines and endpoint ticks |
| Clean plan labels | All labels received rounded white UI pills | Plan and ordinary annotation labels render directly on the drawing unless their persisted style requests a fill |
| Stable overlap order | Snapshot iteration order determined stacking | Areas render below walls, openings, symbols, and dimensions |
| Plan-only semantics | Derived room/floor labels could leak into non-plan views | Derived plan labels are removed from elevation and section canvases |
| Presentation overrides | Saved area/object overrides were decoded but ignored | Visible area/object overrides now drive stroke, fill, hatch, and visibility on the canvas |

The integrated visual fixture is generated through the same public document
commands used by the application. It creates a residential floor with six
rooms, porch, patio, garage, wall-hosted door and window, ten placed components,
four exterior dimensions, and the normal property/building/floor/layer model.
It is a repeatable rendering check, not hand-painted test artwork.

Appraisal numerical labels use the full semantic phase, independently of layer
visibility. Linked voids and site outlines do not acquire standalone building
area labels. Net labels avoid their own deducted footprints, including hidden
deductions. Multiline text is measured as a complete text block on screen and
in output. Existing placement may omit a label when no readable position fits.
Accepted vertex previews recalculate net values from a validated candidate
snapshot, including unchanged deduction parents. Cancel restores committed
values; release commits the same geometry and values. See the
[live-preview verification](../verification/live-appraisal-preview-2026-10-01.md).
