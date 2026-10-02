# Visible appraisal measurements and ANSI-oriented rules

User outcome: the left Details tab exposes primary GLA, separate appraisal area
categories, analytical boundary dimensions, rule evidence and unresolved facts.
The calculation is inspectable without selecting a canvas object. The tab and
printable measurement summary must not describe a provisional profile as ANSI
approval or a complete UAD report.

## Implementation ownership

- Core worker: opt-in `ansi_z765_2021` v1 profile, measured ceiling facts,
  whole-level grade, access/finish/identity rules, actual geometric deductions,
  descending stairs, source-bound evidence and regression cases.
- UI worker: compact conditional Setup and area-facts controls, preserving
  undeclared values, revision guards, cancellation and one-command undo;
  canonical appraisal canvas dimension strings.
- Output worker: Details and printable summary show the same authoritative
  report, primary totals and separate ADU/detached/nonstandard areas, declarations
  and analytical lengths. ANSI-oriented figures use whole ft² and tenth feet;
  metric equivalents remain explicitly supplementary.
- Root: integrated review, native v21/extraction v19 reader floor including
  retained history, format documentation, focused verification, build, source
  delivery, commit/push and installed executable matching verified source.

## Rule evidence and limits

The official [Fannie Mae measuring guidance](https://singlefamily.fanniemae.com/media/30266/display)
provides public requirements for measurement/reporting precision, whole-level
below-grade classification, open-to-below exclusions, stairs and ceiling height.
The [UAD 3.6 policy supplement](https://singlefamily.fanniemae.com/media/document/pdf/fannie-mae-selling-guide-supplement-uniform-appraisal-dataset-uad-36-policy)
keeps ADU measurements separate from the primary dwelling. This implementation
is a measurement summary; legacy/UAD report mappings are separate requirements.

The final publisher ANSI text has not been verified. The sloped-room denominator,
ceiling obstruction/under-stair exceptions, stair finish exceptions and precise
prescribed declarations remain explicit normative gaps. Gross-room denominator
is a provisional interpretation and must appear as such. No label, test or
numeric result substitutes for full standard validation.

Room ceiling evidence binds complete boundary and deduction geometry so a
same-area reshape cannot retain apparently current measured proportions.
Below-five-foot exclusions use actual referenced geometry and an independent
union; sorted deduction identifiers must not alter eligibility. Parent/room
partitioning removes the complete room footprint from its parent then adds only
the room's net category contribution, avoiding double counting.

## Verification and delivery

Freeze all writers before compiling. Check rule thresholds, room evidence
staleness, grade conflicts, exclusions, stairs, separate ADUs, malformed facts,
both display systems, unchanged legacy rules, native history preservation and
reader downgrades. Exercise real Setup/Save/Cancel/Undo/Redo and visible
Details/HTML/PDF output. Inspect offscreen light/dark captures. Run `git diff
--check`, required contract checks and source-kit validation. Package/install
only the verified build; retain the unfinished production acceptance status.
