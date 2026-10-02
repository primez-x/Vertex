# Visible appraisal details

Outcome: a third left-panel Details tab exposes property-level GLA and other
area totals, the building/floor/area breakdown, deductions and linked canvas
boundaries without requiring a selected area or opening its inspector. Missing
facts and invalid sources remain actionable rather than producing guessed totals.

Baseline: appraisal values exist inside the selected-area inspector and a separate
report dialog. The selected-area inspector hides them without a suitable selection.
The current rules are Vertex's versioned declared-facts policy, not a verified
ANSI measurement-standard profile. The new panel must accurately identify that
basis while making existing calculations useful and accessible.

Ownership: component worker owns AppraisalDetailsPanel and its focused fixtures;
root owns MainWindow integration after the drawing worker returns ownership,
CMake, documentation, verification, Git and installation. Preserve all concurrent
changes and unrelated temp.txt. Freeze all writers before builds.

- [x] Create a compact theme-aware panel using the authoritative appraisal report,
  prominent GLA, grouped other totals and a building/floor/area breakdown.
- [x] Show inspectable gross/net/deductions, qualification and actual policy; link
  rows to canvas selection and facts, settings and full-report actions.
- [x] Add Details alongside Layers and Library. Refresh from current documents,
  unit changes and semantic phase changes, independently of presentation filters.
- [x] Verify no-selection visibility, new/undeclared and qualified projects,
  deductions, both units, multiple floors, stale sources, action callbacks and
  real integrated panel navigation. Inspect compact light/dark rendering.
- [x] Update user checklist/help, rerun affected checks and prepare the verified
  source for Git and offline delivery.

ANSI standards qualification remains explicit work under the complete production
goal. Exposing an existing policy must not silently rename it ANSI-approved.
The source-backed remaining standards gaps are documented separately.

Delivery evidence: the final commit/push and installed executable are recorded
under artifacts/mixed-wall-measurement-20261002/delivery.json. The source plan
checklist records the implemented and locally verified checkpoint; it is not
production acceptance or a user testing result.
