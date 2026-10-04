# Versioned sloped-room appraisal calculation

The V2 ANSI-oriented rule compares observed seven-foot-high area with the
countable finished area of one complete physical room after the geometric union
of actual exclusions. V1 retains its original gross-room interpretation. Setup
offers an explicit rule change; a new complete-room confirmation is required
before V2 sloped totals qualify. Scalar-only classification, zero candidates,
missing or excessive observations, stale geometry and measured/stair partitions
anywhere under an excluded wrapper cannot qualify that room.

Details and PDF share the unrounded ceiling numerator, denominator and share.
Schedules and area traces retain the selected profile ID/version. Current,
undone and deleted V2 policy or room confirmation requires native reader 32 and
logical extraction 30; V1-only evidence retains its earlier reader floor.
Portable project packaging accepts format 32 and refuses unknown format 33.

This follows the public-guidance interpretation described in the
[standards gap review](../requirements/appraisal-standards-gap-review.md).
Final publisher-standard validation, ceiling exceptions and full UAD reporting
remain unresolved. No ANSI approval or production acceptance is claimed.

## Verification observed

The initial regression build succeeded and both core suites failed before the
implementation. After integration, Release build succeeded and nine suites
passed: calculations, appraisal document, appraisal report desktop, appraisal
desktop workflow, Details panel, ANSI authoring, document digest, project storage
and project exchange. Python portable project-package checks reproduced the
format-32 refusal before the adapter update and passed all 14 cases afterward.

The independent advisor found an exclusion wrapper could conceal a measured
grandchild. A new document regression reproduced that gap, and validation now
checks the entire deduction tree. The final Release build and three affected
suites (appraisal document, ANSI authoring and appraisal desktop workflow) passed
after correction. One intermediate workflow rerun failed copying a fixture into
an already populated capture directory; that evidence was preserved and the
fresh-directory rerun passed without weakening the fixture or product guards.

Coverage includes 100 gross / 40 excluded / 35 high yielding 60 counted, the
exact half threshold, other floor openings, overlapping/reordered exclusions,
flat floor → sloped room → low exclusion, direct and wrapped measured/stair
partitions, V1 preservation, Cancel, atomic Undo/Redo, explicit reconfirmation,
native save/reopen, and reader-floor downgrade refusal with recomputed digests.
The advisor approved the scoped integrated implementation after inspecting
source and terminal verification evidence.

Logs, failed iterations and native light/dark captures are retained under
`artifacts/ansi-sloped-v2-20261003`. Root inspected refreshed Setup and Details
captures. The isolated label capture documents the light calculation trace;
dark legibility is assessed in the actual dark panel, which supplies its
background. Long source identifiers still need presentation refinement before
full production acceptance.

Installed package/executable observations are recorded separately in the local
delivery record. These checks do not prove clean-machine installation,
network-denied operation, power-loss recovery, full Apex compatibility, final
ANSI qualification or completion of the full production goal. User checklist
U393–U394 remains Not tested by the user.
