# Independent live area callouts and text alignment

This implements the independent callout and text-alignment portion of PINC-010.
It does not complete tentative-curve shortcuts, direct reusable-label access,
the `.pinc` import workflow or the overall Pinc/Apex production acceptance gate.

## Implemented behavior

An existing area retains its combined name/value callout until the user chooses
**Separate name and value** in its quick properties. Name and Calculation then
have independent placement, text height, color, alignment, rotation and
visibility. Both retain the native area owner's identity; neither is copied
geometry or a new analytical area. Calculated strings come from the current
geometry and applicable calculation checks. Stale physical rooms withhold their
previous value rather than presenting it as current.

The same aligned label layout drives painting, picking, selection, placement,
floor references and output. Role-aware preview/cache keys keep two callouts
with one native owner distinct. Conflicting presentation providers withhold and
diagnose only the affected role, retain unrelated drawing content, and refuse
editing that role without mutating the document.

Annotation state 8, native reader 45 and extraction 43 preserve these meanings,
including retained Undo/deleted history. Centered legacy records retain their
existing schema until adoption. Reusable text-library version 2 preserves text
alignment. The portable project package validator recognizes native reader 45.

## Verification record

The final Windows Release build passed. All 11 focused native checks passed:
annotation catalog/entity codecs, core/desktop text library, canvas label
presentation, callout storage/desktop workflow, project storage, wall dimensions,
sketch output and boundary canvas. The final CTest run took 26.54 seconds.
The portable-package unit checks also passed all 14 cases.

The desktop fixture exercises separation as one presentation-only revision,
independent appearance/visibility/placement, live geometry changes, current and
stale physical-room values, actual save/reopen, owner-filtered named views,
closed-area rotation/reflection and duplicate-provider native reopen. The canvas
fixture covers aligned paint/pick/rotated selection, same-owner preview roles,
committed output isolation and aligned floor ghosts. Storage checks cover reader
floors through active, undone and deleted history, extraction, downgrade refusal,
exact native reopening and measured-owner offset completion.

Root inspected the actual offscreen light, dark and conflict workspace PNGs.
Name/value placement and styling remain distinct and readable; the conflict
capture retains the calculation and unrelated geometry while diagnosing the
affected name by its visible area name. These captures are in
`artifacts/area-callouts-20261004/captures/callout-settled-tests/`.

Earlier failed runs are preserved. Corrections included a Windows test-helper
macro collision, a missing SQLite version gate, a fixture that disabled the
selection it asserted, ghost testing through the wrong rendering lane, reused
PDF capture paths and a text-movement fixture that read before asynchronous
admission. The last fixture now observes selected identity, the drag threshold,
pending preview and one committed revision before asserting exact world-space
movement, Undo/Redo and native reopen. Geometry assertions were not weakened.

Build/test commands and terminal exit records are in
`artifacts/area-callouts-20261004/callout-settled-{build,tests}.{json,log}`.
These results verify the source checkpoint; installation is recorded separately.
The independent read-only advisor reviewed the integrated fixes, final native
results and captures and approved this bounded checkpoint with no remaining
required P1/P2 correction. The approval does not certify installed-app adoption
or the overall replacement release.

The user checklist adds U444-U449 as observable actions; their user outcomes
remain **Not tested**.

## Installed checkpoint

Implementation commit `03d2b52460b1360ad01cc8f9b125a6c857111bda` was committed,
pushed and checked against the remote main reference. All six bundle stages and
the bundled installer exited successfully. The bundle
`artifacts/packages/vertex-offline-20261005-area-callouts` contains 4,131 files,
including 1,479 source-kit files and 2,645 runtime files. The new installation is
`artifacts/installed/vertex-20261005-area-callouts`.

Six installed-runtime samples passed with a private test profile and a system-only
PATH. The installed executable's SHA-256 matches the Release executable:
`91ec286bed75ffc8750b817a90e048ff986bcf84732926e429204b67f5a89f7b`.
The Desktop Vertex shortcut was updated and its target read back. The preceding
room-repair installation remains preserved. No existing user process was closed.

The installed report is in
`artifacts/area-callouts-20261004/installed-runtime/run-20261005-003253-969881b1/report.json`;
the delivery record is `artifacts/area-callouts-20261004/delivery.json`.
These samples exercise installed runtime/persistence and are not a complete
installed callout interaction audit. They do not establish clean-machine,
network-denied installation/use, printer or full production qualification.

## Limits

Focused native checks do not certify the full 134-operation Pinc comparison,
legacy project fidelity, printer behavior, a clean-machine installation or
network-denied installation/use. No ANSI approval or full replacement parity
is asserted.
