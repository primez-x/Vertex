# Building-object appearance

This closes a concrete portion of APX-ANNO-003 within the accepted production
plan. The full requirement also includes output views and SVG component
appearance; those remain visible gaps until implemented and verified.

## Intended behavior

Selecting a wall or hosted opening exposes an appearance editor through its
existing quick properties. Applicable physical plan objects use the same
editor. Outline color, fill and hatch, paper line weight, visibility and reset
are presentation changes. Wall thickness, opening dimensions, model geometry,
measurement annotations and appraisal facts and totals remain unchanged.

Hidden objects remain selectable in the layer navigator so visibility can be
restored. Screen and printable plan output use the same effective appearance.
Reset removes only the selected object's override; unchanged Apply and Cancel
do not create history. Undo/Redo and save/reopen retain accepted changes.

Existing area appearance controls and their behavior remain supported. Reuse
their guarded, source-preserving editing path rather than duplicating record
mutation. Preserve opaque metadata, unrelated overrides and protected owners.
Refuse duplicate providers, read-only projects and stale modal context.

## Ownership and execution

- The implementation worker owns `src/desktop/main_window.cpp` and the focused
  native object-appearance fixture.
- Root owns CMake registration, documentation, source-kit generation,
  integration, builds, verification, Git and delivery artifacts.
- A read-only scout investigates SVG palette semantics for the remaining
  component gap. Its findings do not establish implementation coverage.

All writers freeze before builds. Native checks run offscreen with hidden
surfaces, isolated settings and noninteractive error handling. Do not launch
or close the user's application.

## Verification and delivery

Exercise the actual appearance dialog for a wall and hosted opening, verify
effective canvas and exported PDF presentation, and compare geometry and
calculation facts before/after. Include history, reopen, reset, visibility
recovery, unchanged Apply, Cancel, protected/opaque source retention and invalid
editing conditions. Recheck the existing area and wall-measurement workflows
because the editor and projection paths are shared. Inspect a real rendered
capture. Run required requirements/source-kit checks and `git diff --check`.

Commit only reviewed scope, push and verify the remote ref. A focused passing
check does not certify the full production release or ANSI/Apex compatibility.
