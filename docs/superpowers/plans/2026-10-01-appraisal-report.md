# Appraisal report presentation

Provide a real report workflow: a property summary, inspectable per-boundary
gross/deductions/net/exact-factor/adjusted/rounding trace, source facts and IDs,
qualification issues, and a complete paginated PDF. Derive everything from one
immutable document revision and the semantic design phase. Workspace visibility
filters never change appraisal quantities. Unqualified values are diagnostic;
property totals stay withheld. Stale or malformed geometry has no current trace.

Reuse the calculation engine. Add an optional measurement trace to boundary
status without changing eligibility, void aggregation or the exact-unity factor
rule. A separate A3 appraisal sheet preset places a large coordinated plan and
summary, preserving existing architectural sheets. The complete text audit
paginates independently of the bounded sheet schedule placement.

Core worker owns appraisal_document header/source/checks. Sheet worker owns
sheet layout dialog/header/checks. Root owns the report dialog/PDF, MainWindow
integration, build registration, documentation, verification, Git and delivery.
Independent advisor challenges provenance, failure states and output integrity.

Verify linked garage and void deductions, overlapping-deduction marginal amounts,
non-unity factors, missing declarations, stale sources, semantic phase exclusion,
presentation hiding, display units/precision, long names/escaping, pagination,
actual report controls/navigation/refresh, stale and read-only behavior, atomic
file preservation, preset cancellation/graph retention, save/reopen and output.
Inspect the dialog and exported pages. This checkpoint does not establish Apex
compatibility, standard certification or full production release qualification.

Previous goal turn made progress: a19a12b was pushed, the matching wall-measurement
build installed and the Desktop shortcut read back. The full goal remains active.
