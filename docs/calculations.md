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

The Appraisal workflow exposes **Tools > Area display** with decimal places from
0 through 6. The project stores this in `calculation_profile.decimal_places`;
missing settings retain two places. The local display revision advances on a
real saved change and participates in Undo/Redo and save/reopen. It does not
change the built-in appraisal policy's ID, version or eligibility rules.
Workspace Imperial/Metric units remain authoritative for display.

`appraisal_display_profile()` resolves only this precision and the caller's
unit. Hidden legacy profile IDs, units and classification metadata cannot alter
or block appraisal eligibility; display edits preserve them verbatim. The
inspector, the Appraisal schedules available in the 2D workspace, and the sheet
renderer use the same `display_area()` rounding. Schedule quantities remain
unrounded square metres; each generated area cell carries separate derived
precision metadata. Invalid decimal settings withhold numeric totals and
explain the problem. They are never silently clamped or repaired.

Qualified Appraisal plan labels also use this report: a meaningful area name
appears above its net value and unit, or a generic area shows the value alone.
Changing a deduction refreshes both its own contribution and its parent's net
label. Presentation visibility does not change the calculation. Void/site
outlines and unqualified properties receive no automatic building-area value.
Numbers are derived again after history navigation and reopen, never persisted
as separate measurement truth. Vertex previews temporarily suppress these
numbers rather than retain stale net values.

## Appraisal category report

`ClassificationRule::appraisal_category` explicitly selects an
`AppraisalAreaCategory`. Its default is `none`, so existing two-boolean rules
retain their previous building/living behavior without silently becoming GLA.
Classification strings and floor names are never interpreted heuristically.
`appraisal_category_name` returns stable lowercase persistence tokens;
`parse_appraisal_category` returns `std::nullopt` for unrecognized tokens.
Invalid enum values are rejected during profile validation.

`builtin_appraisal_profile()` returns application policy `vertex-appraisal`, version 1,
with square-foot display and two decimal places. Its compatibility categories
remain `above_grade_finished`, `above_grade_unfinished`,
`below_grade_finished`, `below_grade_unfinished`, `garage`, `carport`, `porch`,
`patio`, `deck`, and `other_non_living`. The declared-facts workflow adds
`above_grade_nonstandard_finished`, `below_grade_nonstandard_finished`,
`noncontinuous_finished`, `commercial_occupiable`, `commercial_common`, and
`commercial_service`. All sixteen contribute to the general measured building
total; only above-grade finished contributes to the legacy living-total alias.
This policy makes no measurement-standard or appraisal compliance claim.

`calculate_appraisal_areas(areas, profile)` returns `AppraisalCalculationReport`.
Its `calculation` member is the complete existing `CalculationReport`, including
site results and the original geometry/deduction trace. Its `property`,
`by_building[building_id]`, and `by_floor[{building_id, floor_id}]` members contain
`AppraisalTotals`. Each has a `by_category` map of all named categories to
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
workflows. Appraisal selects the built-in profile and refreshes above-grade finished area, above/below-grade unfinished and
finished buckets, garage, carport, porch, patio, deck, other non-living,
selected-floor and property square-foot totals after each accepted document
change. The contribution row names the source area IDs for the selected area's
category. Measurement classification and appraisal category are stored
separately on each closed area, so switching workflows restores the saved
profile and the correct per-area meanings in one undoable command. Workflow,
category and profile data persist in the project and participate in ordinary
undo/save/reopen behavior; the UI never infers an appraisal category from a
floor name or boundary label. Category-only projects retain their manual arithmetic,
visibly labeled **Unqualified**. The legacy `gla()` API and `appraisalGlaTotal`
widget name are compatibility identifiers, not standard-compliance claims.

**Edit appraisal facts...** declares an application policy independently of manual
categories. Property `appraisal_policy` accepts only `version: 1`, `policy_kind`
(`residential_declared` or `light_commercial_declared`), `property_kind`, and
`measurement_basis`. Floor `appraisal_facts.grade` declares above/below/unknown;
any partly below level must be declared below. Boundary `appraisal_facts` stores
`finish`, `access`, `ceiling_eligibility`, `area_use`, and `boundary_role` tokens.
Missing declarations remain unqualified; malformed types, unknown fields/tokens,
and unsupported versions are rejected. No facts are inferred from names,
elevations, or manual categories. The revision-fenced desktop API commits all
three scopes atomically with undo/redo and ordinary project persistence.

Automatic totals require all participating building boundaries to qualify under
the declared Vertex policy. Independent site and survey boundaries are excluded
without requiring building facts; using one as a building deduction is rejected.
Otherwise the inspector withholds automatic totals and lists reasons. It displays
the selected derived category and net physical versus
factor-adjusted area separately; nonunity factors cannot qualify. Declared
open-to-below, stair-footprint and other-void boundaries have no standalone
contribution and must be explicitly linked as parent deductions. Measured
deductions such as garages or commercial service areas retain their independent
bucket once, while reducing their parent's area. Commercial occupiable, common
and service buckets are separate. Nonstandard and noncontinuous finished areas
remain separate from above-grade finished area. Qualification is only against
the application's declared-facts policy; it is not ANSI, BOMA, or lender certification.
The visible floor and building totals follow the selected area's floor and building;
the property total remains the unrounded aggregate across all buildings.

The deduction editor validates physical geometry independently of manual
appraisal categories when a declared policy is present. Linking or removing a
deduction does not assign a category or qualify incomplete facts; automatic
totals still use the declared-facts qualification checks above.

`build_appraisal_document_report` projects those same declarations and authoritative
boundary geometries for sheets and export without depending on the currently
selected object. The report is tied to a document revision and property, retains
the source boundary IDs behind every category total, and includes property,
building and floor totals. The declared-appraisal inspector uses this shared
report's qualification gate and calculated totals, so contradictory floor,
building or property references withhold automatic totals both on screen and
in sheet output. Inspecting a conflict never silently repairs the document.
Architectural room boundaries and site/survey boundaries are not
appraisal measurement areas. Design-phase visibility is semantic and therefore
participates in qualification; transient workspace visibility never changes the
totals. If any participating boundary is incomplete, incompatible, hidden by the
active design phase, or has an invalid deduction relationship, the printable
report states **Unqualified - automatic totals withheld** and emits no area
values. A qualified report can be placed on a sheet as **Appraisal area summary**
and uses the active display unit only when formatting its calculated values.

The desktop inspector now provides a local deduction editor. It lists valid
closed boundaries on the active floor, stages additions and removals without
mutating the document, and applies a revision-fenced `deduction_ids` array only
after containment, uniqueness, same-floor, and non-nested checks pass. The
calculation result lists each deduction with its requested and marginally
applied amounts. Measurement deductions and unclassified appraisal voids are
excluded from top-level aggregation. The editor is deterministic and offline;
full Apex compatibility still needs native fixtures and certification. In
Appraisal, an explicitly categorized deduction
remains a first-class contribution: an internal garage is subtracted from its
enclosing finished area while its own square footage appears once in the garage
bucket.

## Explicit Auto-Subtract workflows

For an already drawn measurement area, select it, right-click and choose
**Subtract from area…** (also available in Commands). Choose the specific parent
area on the same property, building and floor. **Apply** links the selected
subtractor through the parent's `deduction_ids`; **Remove deduction** removes
that chosen link. The original boundaries remain independently editable.
An identical valid link is a no-op. Removing a link remains possible when an
edit has made the linked area incompatible; it does not silently choose another
parent or erase links from other areas.

For Define First, choose **Define area and subtract from…** in Commands, choose
the area type and then its parent, and draw the smaller area. The drawing hint
names the chosen parent. Closing and accepting commits the new area, its manual
dimensions and the existing parent's deduction together. Canceling creates no
area or deduction. Saving an unfinished drawing preserves the explicit parent;
reopening and finishing uses the same choice. Undo/redo preserves the complete
operation and stable identities. Deleting a subtractor removes its surviving
parent links in the same undoable command.

New Auto-Subtract links reject equal area types, architectural room boundaries,
cross-floor/context links, outside geometry and nested deductions. Containment
uses analytical lines/arcs and permits touching edges; overlapping deductions
remove their union once. Types come from the active measurement classifications
or appraisal categories/declared roles, independently of display names and
report qualification. The existing explicit deduction editor retains its
separate manual workflow.

In declared Appraisal, **Open to below**, **Stair footprint** and **Other void**
are available before drawing. These store only the explicitly chosen exclusion
role, without inventing dwelling finish, access, ceiling or use declarations.
A linked nonmeasured role requires the property policy and measurement basis,
valid geometry/context, unity factor and an explicit parent link. It does not
require irrelevant dwelling facts or floor grade. Supplied malformed tokens
still reject; measured areas retain all their declaration requirements. A void
reduces its parent's physical area and has no standalone category contribution.
These behaviors are implemented Vertex policy, not standards or Apex native
compatibility certification.

`calculation_tests` checks curved area/perimeter, reversed winding, large
translations, overlapping/full/outside deductions, invalid geometry, factors,
classification totals, same-floor overlap, unit conversion, and aggregate
rounding. This service is only part of the required calculation system. A
document-bound output fingerprint, profile editor, measurement-standard
fixtures and complete Apex behavior certification remain required work.
