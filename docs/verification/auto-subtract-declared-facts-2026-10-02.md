# Auto-Subtract and current appraisal declarations

The original Auto-Subtract TYPE resolver preferred a retained manually assigned
category over declared appraisal facts. Appraisal calculations instead use the
declarations. Two boundaries initially categorized Above-grade finished could
therefore remain equal TYPEs after the contained child was declared Garage,
preventing a valid deduction from its Dwelling parent.

## Baseline evidence

- `boundary_commit_tests` failed the declared Garage/Dwelling TYPE assertion with
  equal retained categories before the resolver was changed.
- The actual native fact editor and parent chooser reproduced the missing
  compatible parent. A first modal-callback assertion was caught by the UI error
  handler and reported an unrelated final-state assertion; the fixture was
  corrected to assert chooser presence outside the callback. Root then observed
  the specific missing-parent failure before implementation.

## Implemented behavior and verification

Present appraisal facts now precede saved manual categories and role strings.
Malformed declarations refuse rather than falling back. Explicit pending TYPEs
and genuinely undeclared legacy boundaries retain their prior behavior. Known
non-dwelling use does not gain new dwelling-only grade/eligibility requirements;
supplied malformed tokens still fail the existing declaration contract.

Root observed passing `boundary_commit_tests` and `appraisal_document_tests`.
Regression cases cover different facts with equal categories, equal facts with
different categories, ten invalid declaration cases, role/pending compatibility,
irrelevant garage eligibility fields and exact geometry/history preservation.
The native Auto-Subtract subset and full appraisal desktop workflow pass: actual
fact editor and chooser controls admit the declared Garage/Dwelling relationship,
produce 75 ft² dwelling and 25 ft² garage, preserve source records apart from the
parent link, and retain facts and totals across Undo/Redo and native reopen.
Equal declared TYPEs reject despite different saved categories. The full appraisal
report desktop check also passes. This is technical verification, not a claim of
user-observed resolution or a measurement-standard compliance certification.

Evidence is retained under `artifacts/auto-subtract-facts-20261002`. Checklist
U100 includes changing declared facts after initial classification and the equal
current-TYPE refusal. This bounded correction does not establish complete Apex
compatibility or full production acceptance.
