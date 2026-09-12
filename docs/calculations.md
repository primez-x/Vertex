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

The desktop inspector now provides a local deduction editor. It lists valid
closed boundaries on the active floor, stages additions and removals without
mutating the document, and applies a revision-fenced `deduction_ids` array only
after containment, uniqueness, same-floor, and non-nested checks pass. The
calculation result lists each deduction with its requested and marginally
applied amounts. Referenced boundaries are excluded from top-level aggregation
so the same geometry is not reported as both a standalone area and a hole.
The editor is deterministic and offline; documented Apex same-type Auto-Subtract
behavior still needs compatibility fixtures and certification.

`calculation_tests` checks curved area/perimeter, reversed winding, large
translations, overlapping/full/outside deductions, invalid geometry, factors,
classification totals, same-floor overlap, unit conversion, and aggregate
rounding. This service is only part of the required calculation system. A
document-bound output fingerprint, profile editor, measurement-standard
fixtures and complete Apex behavior certification remain required work.
