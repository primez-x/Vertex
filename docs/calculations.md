# Area calculation contract

The calculation service consumes analytical line/arc boundaries, stable area,
building and floor IDs, explicit classifications, an exact rational factor, and
a named/versioned profile. It does not infer living-area eligibility. Profiles
in this development build are custom policies, without a standards-compliance
claim.

Base area and boundary perimeter come from the precision engine. Area booleans
use the same analytical profile builder as architectural solids. A check rejects
profiles whose derived face area disagrees with the analytical result beyond
max(0.000001 square metre, 0.00000001 relative). A shared local origin keeps large
survey offsets out of these booleans. Unresolved topology fails the calculation.

Deductions must be contained in the base boundary, within that tolerance; they
may touch the perimeter. Overlapping deductions remove their union once. The
trace sorts deduction IDs and records both requested and newly removed area for
each. Net area is base minus this union, then multiplied by the exact rational
factor converted for numerical evaluation. Original geometry and entered
quantities are unchanged.

Aggregation rejects duplicate IDs and positive overlap between remaining areas
on the same building/floor. Regions on different floors remain independent.
Classification, building and living totals sum unrounded factored areas and
round once afterward. The profile explicitly selects which classifications
contribute to each total.

Display supports square metres, square feet and acres, with 0-6 decimal places.
The conversions use 0.09290304 square metre per square foot and 4046.8564224
square metres per acre. The rounding rule is nearest, with exact binary ties
away from zero. Each display result includes its unrounded value, rounded value,
rounding delta and locale-independent decimal text. Display precision never
changes a boundary or its stored measurement.

## Appraisal category report

`ClassificationRule::appraisal_category` explicitly selects an
`AppraisalAreaCategory`. Its default is `none`, so existing two-boolean rules
retain their previous building/living behavior without silently becoming GLA.
Classification strings and floor names are never interpreted heuristically.
`appraisal_category_name` returns stable lowercase persistence tokens;
`parse_appraisal_category` returns `std::nullopt` for unrecognized tokens.
Invalid enum values are rejected during profile validation.

`builtin_appraisal_profile()` returns application policy `vertex-appraisal`, version 1,
with square-foot display and two decimal places. It maps these exact
classification IDs to their corresponding categories: `above_grade_finished`,
`above_grade_unfinished`, `below_grade_finished`, `below_grade_unfinished`,
`garage`, `carport`, `porch`, `patio`, `deck`, and `other_non_living`.
All ten contribute to the legacy building total; only above-grade finished
contributes to the legacy living total. This policy makes no measurement-standard
or appraisal compliance claim.

`calculate_appraisal_areas(areas, profile)` returns `AppraisalCalculationReport`.
Its `calculation` member is the complete existing `CalculationReport`, including
site results and the original geometry/deduction trace. Its `property`,
`by_building[building_id]`, and `by_floor[{building_id, floor_id}]` members contain
`AppraisalTotals`. Each has a `by_category` map of all ten named categories to
`AppraisalAreaBucket { total, area_ids }`. `gla()` references only the
`above_grade_finished` bucket; finished and unfinished basements always remain
separate from GLA. Source area IDs are sorted and appear in their explicit bucket
even when a zero factor or full deduction makes their contribution zero.

Only building-scope areas with a category other than `none` contribute to these
buckets. Building-scope areas mapped to `none` still establish a floor/building
entry with zero buckets; site-only floors and buildings do not. The property
represents all buildings in the supplied collection, so callers must select
that collection at their property boundary. Identical floor IDs in distinct
buildings remain independent. No property identity is inferred.

Totals sum unrounded factored square-metre results using extended-precision
accumulators and produce each `AreaTotal` display only after summation. They do
not sum rounded per-area or per-floor displays. As in the existing calculation
engine, geometry and areas use floating-point analytical calculations rather
than arbitrary-precision exact arithmetic; exact-until-display means no early
display rounding. The existing overlap, geometry, deduction, factor, duplicate
ID, and profile validation failures propagate without producing partial reports.

Appraisal tests cover multiple above-grade floors, separate basement and ancillary
categories, factors and deductions, floor/building/property totals, classification
changes, provenance, site exclusion, legacy rules, persistence tokens, invalid
categories, empty reports, and rounding after aggregation.

The Windows area inspector exposes Measurement and Appraisal as explicit
workflows. Appraisal selects the built-in profile, restricts the classification
list to its ten categories, and refreshes GLA, above/below-grade unfinished and
finished buckets, garage, carport, porch, patio, deck, other non-living,
selected-floor and property square-foot totals after each accepted document
change. The contribution row names the source area IDs for the selected area's
category. Measurement classification and appraisal category are stored
separately on each closed area, so switching workflows restores the saved
profile and the correct per-area meanings in one undoable command. Workflow,
category and profile data persist in the project and participate in ordinary
undo/save/reopen behavior; the UI never infers an appraisal category from a
floor name or boundary label.

The desktop inspector now provides a local deduction editor. It lists valid
closed boundaries on the active floor, stages additions and removals without
mutating the document, and applies a revision-fenced `deduction_ids` array only
after containment, uniqueness, same-floor, and non-nested checks pass. The
calculation result lists each deduction with its requested and marginally
applied amounts. Measurement deductions and unclassified appraisal voids are
excluded from top-level aggregation. The editor is deterministic and offline;
documented Apex same-type Auto-Subtract behavior still needs compatibility
fixtures and certification. In Appraisal, an explicitly categorized deduction
remains a first-class contribution: an internal garage is subtracted from its
enclosing finished area while its own square footage appears once in the garage
bucket.

`calculation_tests` checks curved area/perimeter, reversed winding, large
translations, overlapping/full/outside deductions, invalid geometry, factors,
classification totals, same-floor overlap, unit conversion, and aggregate
rounding. This service is only part of the required calculation system. A
document-bound output fingerprint, profile editor, measurement-standard
fixtures and complete Apex behavior certification remain required work.
