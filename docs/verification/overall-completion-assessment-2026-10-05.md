# Vertex completion assessment — October 5, 2026

The planning estimate is **about 53% delivery maturity**, with a scored range
of **51.7–54.8%**. This supersedes the unsupported earlier 80–85% estimate.
It measures implementation and verification maturity against the full accepted
scope. It does **not** measure the fraction of engineering hours finished,
predict an ETA, certify Apex parity, or establish production readiness.

The full Windows residential/light-commercial measurement and architectural/3D
scope remains binding. The source is public under GPL-3.0-or-later, following
the owner's later instruction. No requested architectural enhancements have
been removed from the denominator.

## Method and limitations

Three independent, read-only subagents reconstructed scope, assessed 2D and
appraisal evidence, and assessed architecture and delivery evidence. Root
reconciled their disjoint assignments against the authoritative
[130-row ledger](../requirements/apex-parity.json) and
[accepted plan](../production-plan.md). They inspected source, recorded native
checks, installed-runtime evidence and explicit gaps. They did not launch
applications or count tests as completed features.

Each ledger requirement has equal weight and a maximum of four maturity
points. The points are an explicitly chosen planning rubric, not empirically
measured fractions of effort or user value:

| Stage | Evidence credited |
|---:|---|
| 0 | No usable implementation evidence |
| 1 | Source, discovery or partial implementation checkpoint |
| 2 | Focused behavior evidence for implemented portions |
| 3 | Meaningful integrated installed-workflow evidence for implemented portions |
| 4 | Full acceptance of the requirement, including required external qualification |

A stage 2 or 3 broad requirement can still have missing features. In
particular, successful installed wall/opening/slab samples do not prove roofs,
stairs, 3D editing, coordinated issue sets or complete architectural workflows.
Native Apex discovery scaffolding earns stage 1, while actual native import and
export compatibility remains unresolved. There are **no stage 4 rows** in this
assessment. This is not a statement that no useful functionality exists.

The range reflects ambiguity between focused and installed evidence for 16
rows. It is **not a statistical confidence interval**. Equal row weights make
the arithmetic reproducible but do not make differently sized requirements
equally expensive. Unexamined behavior, stale evidence and future defects can
change the assessment. A defensible remaining-work or schedule percentage
requires separately estimated and tracked effort.

## Results

| Disjoint domain | Rows | Points / maximum | Maturity |
|---|---:|---:|---:|
| Offline foundation, ownership and shared document | 16 | 41 / 64 | 64.1% |
| Architecture, analytical geometry and constraints | 31 | 65 / 124 | 52.4% |
| Measurement, drawing, calculations, annotation and tracing | 27 | 55–65 / 108 | 50.9–60.2% |
| Workspace, field input, specialist modules and integrations | 13 | 27–31 / 52 | 51.9–59.6% |
| Apex native compatibility plus IFC/DXF/PDF exchange | 8 | 11 / 32 | 34.4% |
| Recovery and security | 8 | 16–18 / 32 | 50.0–56.3% |
| Output, accessibility, performance and quality | 12 | 22 / 48 | 45.8% |
| Optional assisted workflows | 5 | 10 / 20 | 50.0% |
| Components, licensing and installation delivery | 10 | 22 / 40 | 55.0% |
| **Total** | **130** | **269–285 / 520** | **51.7–54.8%** |

The midpoint is 277 / 520 = 53.27%, rounded to about 53%. None of these domain
percentages is a claim of certified functional parity.

### Exact row scoring

Every ledger ID is assigned exactly once. All rows score **2**, except these
explicit overrides:

**Score 3 (16 rows):** CORE-OFF-003; CORE-OWN-002; CORE-SCOPE-001;
CORE-DOC-001, 003, 004, 005, 006, 007; ARCH-MOD-001, 002, 003;
APX-TRACE-001; UX-WORK-001; COMP-GEO-001; OPS-PACK-001.

**Score 1 (7 rows):** APX-NATIVE-AX5-001; APX-NATIVE-AX7-001;
APX-NATIVE-LEGACY-001; APX-COMPAT-001, 002; OPS-PERF-001; OPS-QA-004.

**Score 2–3 (16 rows):** APX-WF-001; APX-KEY-002; APX-CURVE-001;
APX-EDIT-005; APX-AREA-001, 003; APX-ANNO-001; APX-SYM-001;
APX-DOC-001, 003; APX-UI-001, 002; UX-WORK-002, 003;
SEC-WORKER-001, 002.

The other 91 rows score 2. The lower endpoint is therefore
16×3 + 7×1 + 107×2 = 269; raising the 16 uncertain rows adds 16 points.
The accompanying [row assessment](overall-completion-assessment-2026-10-05.json)
records each ID, requirement, domain and score for arithmetic inspection.

### Pinc additions

The [Pinc comparison](../requirements/pincsketch-4.3-vertex-comparison.md)
adds 13 adoption requirements and paired qualification for 134 inventoried
operations. They substantially overlap ledger rows and are therefore tracked
separately, rather than inflating the principal denominator with duplicated
behaviors. They remain part of final acceptance.

PINC-001–006 score 2–3 each; PINC-007, 008 and 009 score 1 each;
PINC-010 scores 2; PINC-011 scores 2–3; PINC-012 scores 1–2;
PINC-013 scores 1: **21–29 / 52 = 40.4–55.8%**. If these are nevertheless
added as 13 equal rows, the combined sensitivity result is
290–314 / 572 = 50.7–54.9%. This still does not approach 80–85%.

Pending keyboard-travel and inline-label changes receive no additional credit
until integrated checks and a new installed checkpoint qualify them. The
comparison checklist remains a user test checklist, not a record of completed
human acceptance.

## Evidence boundary and substantial gaps

The installed baseline is `vertex-20261005-pinc-import-footer`, executable
SHA-256 `52e32397e37238514e2325c16c84904c266b1c807a335998def511abc4533e09`.
Its six source/reopen samples pass with matching project, screenshot and native
image hashes. The authoritative local record is
`artifacts/pinc-import-20261005/installed-footer-runtime/run-20261005-185818-abbfc0c3/report.json`.
It explicitly records `clean_machine=false`, `network_denied=false` and
`production_qualified=false`. Source evidence for the sample scope is
`src/desktop/main.cpp::seed_smoke_document`: architecture samples author two
walls, a hosted door and a slab; the commercial sample adds a counter symbol.

The review used these material evidence and gap sources:

- Drawing/history: [typed chords](typed-chord-input-2026-10-03.md),
  [measured bays](2026-10-04-measured-bay-and-phase-subtraction.md),
  [measured-line editing](measurement-linework-editing-2026-10-03.md) and
  [mixed movement](mixed-selection-and-pinc-adoption-2026-10-04.md).
- Appraisal/rooms: [nested partitions](nested-appraisal-partitions-2026-10-04.md),
  [sloped-area facts](sloped-room-appraisal-v2-2026-10-03.md),
  [reviewed room repair](physical-room-reviewed-repair-2026-10-04.md), and
  [standards gaps](../requirements/appraisal-standards-gap-review.md).
- Symbols/text/output: [SVG palette](symbol-svg-palette-20261003.md),
  [independent callouts](independent-area-callouts-2026-10-05.md),
  [symbol coverage](../requirements/pincsketch-symbol-coverage.md),
  [floor references](pinc-floor-reference-2026-10-04.md),
  [composition output](pinc-sketch-output-2026-10-04.md), and
  [installed Pinc checkpoint](pinc-desktop-import-2026-10-05.md).
- Architecture/delivery: actual `native_model_view.cpp` manipulator and semantic
  callbacks, architecture/core and native verification sources identified by
  the ledger, [production qualification](../production-qualification.md),
  and `docs/production-qualification.pending.json` with no qualifying runs.

Remaining work includes:

1. Actual Apex v5/v7/native legacy import/export, observed edition/module
   behavior, representative original projects and outputs, real caller
   protocols and appraisal integration round trips.
2. Complete drawing and editing lifecycle, active-outline travel, mixed
   connected movement, true exterior/shared-edge styling, batch presentation,
   tentative-curve gestures and automatic room correspondence/repair.
3. Per-family symbol artwork, default sizes and hosting quality; complete
   area-preset fact shortcuts; all 134 paired Pinc operation comparisons.
4. Final normative appraisal review, ceiling/stair exceptions, full required
   reporting and external integration. Current GLA logic is not ANSI approval.
5. Complete architectural object lifecycle and coordinated downstream output,
   broader stairs/railings/roof/assembly detailing, constraints/propagation,
   3D editing across families, alternatives, schedules and sheets.
6. Diverse real-plan assistance qualification. Raster-only images/PDFs lack
   OCR; optional deterministic proposal tools do not establish that capability.
7. Clean-machine offline installation and rebuild, full networking-disabled
   workflow, complete corresponding source/license handoff, printer scale,
   actual pen/touch/DISTO, assistive technology, reference-hardware performance
   and recovery/portability under production conditions.

## Reconciliation of older progress records

At assessment, `apex-parity.json` has 72 implementation-`verified` and 58
`in_progress` rows. `artifacts/acceptance/completion-audit-current.json` instead
reports the historical 86/44 split. `acceptance-evidence.json` has only 115
of the 130 IDs, of which 114 say `in_progress` and one has no status. The audit
and evidence index have different source fingerprints and older dates.

These records are retained as historical evidence. Their counts were not used
as the maturity numerator. A ledger's implementation-`verified` status is not
production acceptance: several notes expressly leave complete workflows,
physical keyboard behavior, print calibration or external comparisons open.
The unified production gate remains unsatisfied, and the full goal stays active.
