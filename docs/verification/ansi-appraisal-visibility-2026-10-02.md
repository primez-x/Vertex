# Visible ANSI-oriented appraisal measurements

The left Details tab exposes GLA, separate categories and building/floor/area
contributions without a canvas selection. ANSI-oriented Setup and facts controls
record inspection/method statements, whole-level grade, finish/access/identity,
numeric ceiling observations, real low-height exclusions and descending stairs.
Unknown values remain undeclared. Save/Cancel, revision guards, atomic Undo/Redo
and native save/reopen follow the ordinary document paths.

The opt-in profile uses canonical whole square feet and tenth-foot analytical
dimensions, with supplementary metric diagnostics. ADUs and detached-other areas
remain separate from primary GLA. Room observations bind exact boundary and
deduction geometry. Nested footprint/room/low-height partitions subtract the
complete child footprint then contribute its net area. The live GLA explicitly
marks sloped-room totals provisional; final publisher standard validation remains
unresolved. Output is a measurement summary, not a complete UAD appraisal report
or ANSI approval. See the standards gap review for remaining normative work.

## Review corrections

Independent approach/source review identified the ADU reporting convention,
deduction-order-dependent low masks, same-area evidence staleness, overly broad
stairs exemption, provisional sloped denominator and reporting limitations.
These were addressed with separate categories, independent geometric unions,
source-bound whole-room observations, stair-role/source guards and explicit notes.
The integrated review additionally found Properties and deduction authoring still
using legacy nesting restrictions. ANSI Properties now uses the same core report
as Details/PDF; the actual deduction dialog admits valid nested partitions while
rejecting cycles and invalid containment. A native fixture compares all three
surfaces. Root visually inspected light/dark views and corrected unreadable dark
dialog labels and scroll backgrounds.

## Verification

Release build passed. Thirteen focused executable suites passed: calculations,
appraisal document, native project store, project exchange, document admission,
document digest, wall measurements, ANSI authoring, Details, report HTML/PDF,
appraisal desktop workflows, wall dimensions and full wall-measurement desktop
workflows. ANSI/Details were rechecked after the final theme-only correction.
Fixtures run offscreen with isolated settings and noninteractive error handling;
no user application was launched.

Coverage includes height thresholds, actual masks and identifier-order
invariance, same-area reshapes, grade conflicts, ADUs, stair roles/source floors,
malformed/unknown declarations, nested partitions, overlaps/cycles, hidden
dependencies, canonical units/rounding, analytical curves, and preserved legacy
behavior. Native format 21 and extraction 19 preserve the new semantics even in
undone/deleted history; recomputed digests cannot downgrade their reader floor.

Build/check logs, actual exit records and native captures are retained under
`artifacts/ansi-appraisal-visibility-20261002`. The initial checks corrected test
fixture defects: a null JSON extensions value, wrong canvas object name, a call
to a private facade method and differing equivalent area-unit notation. An older
report fixture attempted an ordinary forbidden source replacement; it now
verifies refusal and uses a separate imported diagnostic snapshot. No production
admission guard was weakened to make a test pass.

The user checklist contains 335 practical tasks, including U331–U334 for these
workflows; all user results remain Not tested. Delivery metadata records the
actual commit, remote verification, package/install and executable hash. The
full production goal and parity/compatibility acceptance gate remain unfinished.
