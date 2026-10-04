# Measured bay returns and phase-aware Auto-Subtract

The measured-line B command now uses the existing analytical bay-return
construction and ordinary receipt/history path. It requires two joined straight
edges, the current pen at their end, current source/context and an idle canvas.
Wall and boundary behavior remains available. Earlier receipts and identities
are preserved.

Auto-Subtract additions now require the source, target and already-linked tools
to belong to the current semantic design phase. Unregistered and current proposed
areas remain eligible, including pending authoring sources not yet in the entity
map. Hidden layers do not change calculation eligibility. Existing invalid links
remain removable.

Historical Finish validation is separate from new edit admission. Validation-only
APIs reconstruct legacy targets and the complete Finish map, including exact JSON
number representations. They return no apply-capable preview or command. Two
real pre-change archives preserve inactive/demolished-target Finish histories;
fresh Revise and Finish operations using those targets are refused atomically.

Root-owned native evidence is in `artifacts/apex-bay-phase-20261004/`:

- `red-check-0.log`, `red-check-2.log` and `bay-red.log`: intended core phase,
  chooser filtering and real B dispatch failures observed before production edits.
- `capture-old.json`: pre-change ordinary archive creation through public APIs,
  exit 0. Original fixture hashes and provenance are documented beside the files.
- `green-build.json`: Release build, exit 0.
- `green-check.log`: seven affected suites passed; the new history regression
  initially expected live Revise to admit an inactive target. That expectation
  contradicted correct strict new-edit admission and was corrected.
- `final-build.json`, `history-final-check.json`: corrected regression rebuilt
  and passed, exit 0. Eight unique affected suites pass in total: boundary commit,
  bay-return desktop, appraisal desktop workflow, area definition, nested measured
  areas, workspace history, historical phase subtraction and archive restoration.

The golden checks cover load, exact history-envelope re-encoding, save/reopen,
Undo/Redo, target field and representation tampering, unrelated changes, and
refusal of fresh edits without mutation. Desktop checks cover joined/curved/stale
bay strokes, active gestures, phase eligibility, existing-link removal, Cancel,
Undo/Redo and layer-eye independence. Manual checklist items U400–U402 describe
the user workflows.

The read-only integrated advisor reviewed source, diff, fixture hashes and native
results and approved this scoped change with no unresolved required findings.
Root reviewed the final interfaces and evidence and ran `git diff --check`.

This is an internal checkpoint. These checks do not establish Apex project
compatibility, normative ANSI certification, clean-machine installation or
completion of the full production scope.
