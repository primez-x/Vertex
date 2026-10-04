# Readable appraisal traces and unfinished drawing output

The Details panel now presents each value below its label and puts geometry,
dimensions and calculation results before declarations. Full raw boundary,
floor, deduction and ceiling-source IDs and the geometry fingerprint live in
an expandable read-only text view. It wraps long tokens without inserting
characters into the source text, supports normal selection/copy, starts
collapsed and clears when property context is lost. The calculation model,
profile versions and PDF trace values are unchanged.

Appraisal PDF export now refuses an active Measured lines session, including an
anchor with no accepted side. Refusal preserves the existing destination file,
document revision, history, retained sides and current pen. Explicit Cancel or
Finish ends the session and allows export. This closes the same unfinished
authoring admission gap already guarded for boundaries and walls under
IO-OUTPUT-003; it does not certify every output path.

## Observed verification

The native report regression failed before the guard at the anchor-only export
assertion. It passed after the one-condition correction and covers accepted
sides, destination-byte preservation, unchanged snapshots/history, continuation
from the retained pen and restoration after Cancel/Finish.

The initial general text-layout probe passed but a native sloped-room capture
still showed clipped IDs and fingerprints. That indirect probe was insufficient.
The revised regression first failed because the separate provenance view did
not exist, then passed with exact selectable IDs, disclosure and context-clear
checks. Native captures show a legible numeric trace at normal panel width.
The focused Release build and Details, ANSI authoring and appraisal report
desktop suites passed. Light/dark captures and failed iterations are retained
under `artifacts/appraisal-readable-details-20261003`.

Installed observations, when collected, are recorded separately in the local
delivery record. These checks do not establish user-observed resolution, final
ANSI normative qualification, clean-machine/network-denied installation, full
Apex parity or production acceptance. The full production goal remains active.
