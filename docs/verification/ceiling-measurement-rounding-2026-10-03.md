# Flat-ceiling measurement rounding — 2026-10-03

## Reproduced behavior

Vertex previously compared the recorded SI ceiling height directly with seven
feet. An observation of 6.96 ft therefore became nonstandard finished area,
although the public [Fannie Mae height examples](https://singlefamily.fanniemae.com/media/36856/display)
apply measurement rounding first. Behavioral regressions failed in the core
classifier, actual document contributions and Details. The native Facts test
also reproduced the absence of a rounded-height preview.

## Implementation

One core helper rounds a finite nonnegative observation to the declared inch
or tenth-foot increment. It rejects unknown increments and heights whose
increment count cannot be represented. Flat classification uses that rounded
value; missing precision stays unqualified. Original `minimum_height_m` values
are never rewritten. Existing policy tokens and project schema remain intact.

Facts previews the height used for classification. Details and PDF share the
same formatter and explicitly distinguish recorded and rounded height. Whole
inches are displayed as feet/inches; tenth feet retain one decimal place. Plan
dimension and final aggregate-area rounding remain separate. The manual
checklist's U332 includes concrete observations and a precision-sensitive case.

## Verification

All six affected Release checks passed: calculations, appraisal document,
Details, ANSI desktop authoring, appraisal report, and appraisal desktop
workflows. The ANSI desktop check was rerun after its capture-only correction
and passed again. Actual Facts authoring, precision changes, atomic history,
save/reopen and reopened PDF text exercise recorded and rounded observations.
Core controls cover the source examples, precision-sensitive observations,
represented cutoff neighbors and invalid or unrepresentable inputs.

Independent review found a stale report expectation for inch formatting and a
Facts screenshot that did not expose the new controls. Both were corrected.
Root inspected the regenerated native capture and actual exported report page:
the 6.96 ft observation and 7.0 ft classification height are visible together.
Root also reviewed the integrated sources, raw preservation, output agreement
and remaining precision limitation. The requirement contract has no errors;
source-kit coverage passed with 1,261 explicit files. Local process receipts and
captures are in `artifacts/ceiling-measurement-rounding-20261003/`.

## Qualification boundary

This source-backed flat-height correction does not certify ANSI compliance,
resolve sloped-room interpretation, implement a complete appraisal form, or
complete Apex parity/compatibility. The full production gate remains open.
The helper rounds computed binary floating increment counts. Qualification of
all decimal midpoint observations beyond the tested seven-foot cutoff neighbors
remains open; conversion can move an intended decimal midpoint slightly off a
count tie. The retained observation is not replaced to hide that limitation.
