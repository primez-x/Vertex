# Flat-ceiling acquisition rounding

## Outcome and source

Apply the property's declared inch or tenth-foot acquisition precision before
the ANSI-oriented flat-ceiling seven-foot eligibility comparison. Retain the
recorded height in the project and show both recorded and rounded values in
Facts, Details and the printable measurement report. This corrects a concrete
rule mismatch; it does not establish complete ANSI compliance.

Fannie Mae's [September 2023 ANSI Answers, page 6](https://singlefamily.fanniemae.com/media/36856/display)
uses ceiling observations of 6.96 ft and 6.85 ft to demonstrate rounded
eligibility. Its [September 2025 measurement FAQs, page 1](https://singlefamily.fanniemae.com/media/30266/display)
retain inch or tenth-foot acquisition precision and distinguish tenth-foot
sketch dimensions from whole-square-foot final totals. Publisher-standard
validation and the sloped-room denominator remain open.

## Ownership and implementation

- Core worker owns the shared rounding function, flat classification and core
  calculation/document regressions. Unknown precision stays unqualified.
- Root owns Facts preview, shared Details/report formatting, native user-flow
  checks, documentation, source-kit generation, Git and delivery.
- All native writers freeze before any build or executable check. Tests first
  reproduce the wrong raw-height comparison; implementation follows that receipt.
- Existing policy identifiers, schema and recorded measurements remain intact.
  Sloped areas, stairs, plan lengths and final area rounding are unchanged.

## Verification

- Source examples, both acquisition precisions, a precision-sensitive height,
  missing/invalid inputs and raw observation retention.
- Actual Facts authoring, undo/redo, GLA refresh, save/reopen and report output.
- Details/report agreement and native rendering in the existing unit modes.
- Independent review of the classification change; focused affected checks,
  requirement contract, source-kit coverage and `git diff --check`.
- Scoped commit, push and remote-ref verification; production acceptance stays
  open until the full original scope and compatibility evidence are proven.
