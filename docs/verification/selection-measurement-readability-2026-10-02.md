# Selection and measurement readability checkpoint

The selected appraisal outline's frame and size badge can cover wall and exterior
measurement text. The underlying values are separate and correct; selection
controls are painted after their annotations. This change reserves the resolved
text footprints without changing stored dimension positions or numerical truth.

## Observed baseline

- The focused rendered regression failed because selection controls changed
  strong measurement-glyph pixels. The earlier fixture-prerequisite failure was
  corrected before implementation; it was not used as evidence of the defect.
- The native curved-wall fixture passes selection, zoom/pan, saved presentation,
  native round-trip and PDF text checks before the visual correction. Those
  checks prove preservation only; they do not prove unobstructed text.
- Root inspected the selected native capture. The frame crosses the 4.000 m
  baseline label while the derived exterior remains 4.140 m. Both measurements
  need to stay visible.

## Implemented behavior and verification

Rendering supplies its already-measured rotated text footprints to the selection
overlay. A winding-fill exclusion mask protects overlapping footprints as well as
separate labels. Frame strokes, connectors and control paint reserve that space.
Rotation, corner and side markers share collision-resolved placement with their
24-pixel hit targets; displaced resize markers have connectors to their original
anchors. The geometry's transform center and axis anchors stay unchanged.
Offscreen anchors can search into the visible viewport. The size badge chooses
a nearby clear viewport-contained candidate and avoids other size badges.

Root observed passing focused and full `boundary_canvas_tests` at device pixel
ratios 1 and 2. The new checks preserve strong glyph pixels, including overlapping
label footprints, rotated text and 2x zoom; drag the visible displaced rotation and
corner markers; and resize from a control whose original anchor is offscreen.
Saved positions and fitted output remain identical. Full boundary-workflow,
wall-dimension and wall-measurement desktop checks pass. The native curved fixture
preserves manual position, typography, numeric values and document state through
selection, zoom/pan, native reopen and PDF export.

The first implementation's diagnostic failure located an obscured glyph at a
corner resize marker. That prompted the shared resize-marker correction; it was
not treated as a passing checkpoint. Independent read-only review identified the
odd-even mask and handle collision issues, and confirmed both corrections.
Root inspected `desktop-final/curved-exterior-refreshed-area.png`: complete
4.000/4.240 m and 6.283/6.660 m labels now remain visible beside the selected area.

Extremely small or completely annotation-filled views may have no clear slot:
controls retain their fallback targets and the badge retains the least-overlap
available position. These bounded checks do not certify arbitrary dense-plan
packing, physical touch hardware or production performance. No annotation model,
saved placement, measurement value or output layout migration was introduced.

Logs and captures are retained under `artifacts/selection-measurement-20261002`.
User checklist task U310 covers selection readability and the rotation control.
Full production and Apex compatibility acceptance remain open.
