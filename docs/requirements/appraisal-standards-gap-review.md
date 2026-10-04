# Appraisal standards gap review

Reviewed 2026-10-02 against the current calculation engine and official guidance.
This remains part of the original production scope. The Details panel makes
existing calculations accessible; it does not certify a measurement standard.

The engine retains `vertex-residential-declared-v1` and
`vertex-light-commercial-declared-v1`, and adds an opt-in
`vertex-ansi-z765-2021-v1` profile based on public guidance. It validates explicitly declared facts,
classifies qualifying above/below-grade areas, applies geometric deductions and
records calculation provenance. A qualified report means those implemented
rules have sufficient inputs. It does not mean ANSI approval.

The 2026-10-03 flat-height correction uses the declared acquisition increment
before the seven-foot eligibility comparison, while preserving the recorded
height. Facts previews the rounded height; Details and PDF distinguish it from
the observation. This follows the concrete height examples in Fannie Mae's
[September 2023 ANSI Answers, page 6](https://singlefamily.fanniemae.com/media/36856/display).
It does not establish final publisher standard qualification.

The 2026-10-03 V2 correction applies the half-height threshold to current
countable finished room geometry after real exclusions, with an explicit
complete-room observation. It preserves V1 results and requires an explicit
rule change and reconfirmation. This interpretation is supported by the
[September 2025 Fannie guidance](https://singlefamily.fanniemae.com/media/30266/display)
and the publisher's [public February 2020 draft, sections 3.4 and 3.6](https://www.homeinnovation.com/documents/national_standards/ansi_z765/ANSI%20Z765%20-%20DRAFT%2020200207%20-%20no%20cover%20art.pdf).
Neither is a substitute for review against the final publisher standard.
Measured-child ownership partitions require separate whole-room membership and
height evidence; V2 refuses ambiguous sloped partitions instead of inventing it.

Fannie Mae requires ANSI Z765-2021 for relevant single-family appraisal
measurements, calculations and reporting. Apartment/multifamily buildings need
different treatment, including interior-perimeter measurement of apartment
units. Software output must conform to the applicable standard. See
[Fannie Mae's Improvements policy](https://selling-guide.fanniemae.com/sel/b4-1.3-05/improvements-section-appraisal-report).

The published measuring guidance also distinguishes acquisition precision,
sketch dimension precision and final area rounding. It specifies stair and
open-to-below treatment and ceiling-height rules. See
[Fannie Mae's standardized measuring guidelines](https://singlefamily.fanniemae.com/media/30266/display).

## Implemented rules and remaining production evidence

The ANSI-oriented implementation records inspection/method limitations,
whole-level grade, comparable finish, year-round suitability, dwelling identity,
flat minimum height, whole-room sloped proportions with actual low-height
exclusion geometry, and descending stair source floors. Room observations bind
exact boundary/deduction geometry. Nested room partitions prevent double
counting; an independent low-height union avoids identifier-order dependence.
ADUs and detached-other areas stay separate from primary GLA. Details and the
measurement summary expose source facts, category reasons and canonical whole
square-foot/tenth-foot figures with supplementary metric diagnostics.

These are implemented Vertex rules, not full normative validation. The final
publisher standard remains unverified, including final confirmation of the sloped-room interpretation,
ceiling obstructions, under-stair/stair-finish exceptions and exact prescribed
declarations. The legacy V1 gross-room sloped denominator remains explicitly
provisional. No complete legacy/UAD 3.6 form, room-count mapping, or lender
certification is implied by the measurement summary. The
[current UAD 3.6 supplement](https://singlefamily.fanniemae.com/media/document/pdf/fannie-mae-selling-guide-supplement-uniform-appraisal-dataset-uad-36-policy)
and legacy Selling Guide differ in ADU reporting; that distinction must be
handled by an explicit reporting contract rather than a universal ANSI default.

| Gap | Required behavior and evidence |
| --- | --- |
| Versioned standard profile | An explicitly identified, source-reviewed ANSI Z765-2021 profile separate from Vertex's current declared-facts policy. Store the profile version and basis in the project and output. |
| Measurement and reporting precision | Separate entered/retained precision from required sketch dimension presentation and final aggregate area rounding. Golden output cases must cover rounding boundaries without rounding every segment before calculation. |
| Ceiling evidence | Record height eligibility and low-height exclusions, including sloped-ceiling proportions. A single generic eligibility declaration does not verify the numeric threshold or its geometric exclusion. |
| Stairs, openings and circulation | Verify stairs on the applicable floor, open-to-below deductions and access through unfinished areas. Include multiple floors and overlapping exclusions in independent expected-result fixtures. |
| Dwelling identity and grade | Verify attached/detached spaces, ADUs, detached structures, noncontinuous access and partially below-grade levels. Mere building grouping cannot infer their eligibility. |
| Inspection/basis statements | Retain required statements for plans and applicable inspection limitations, including explicit unknown facts. Do not treat plan-derived dimensions as observed field measurements. |
| Output consistency | The Details totals, canvas measurements, sheets and appraisal PDF must use the same authoritative geometry while applying the relevant presentation rules. Test both units and save/reopen. |
| Qualification evidence | Complete a requirement-to-rule/output checklist with sourced expected results and independent review. Keep the current ANSI status unverified until that evidence exists. |

No release gate is waived by adding the panel. Light-commercial rules are a
separate profile scope; the residential ANSI standard is not a generic
commercial certification.
