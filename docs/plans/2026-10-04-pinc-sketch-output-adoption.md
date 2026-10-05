# Pinc sketch output adoption

Outcome: add a tightly cropped vector Sketch PDF for report insertion (PINC-006)
and an optional canvas composition guide showing that exact crop (PINC-005).
Retain selected-sheet, drawing-set, print and appraisal-audit output. This is an
additional production requirement; it does not complete the Pinc or Apex audit.

The export uses the current visible committed canvas projection at a fixed
output scale, independent of navigation zoom. It includes actual rendered
symbols, curved geometry, strokes, dimensions and text. References, grids,
selection, previews, ghost references and composition guides are excluded.
The renderer's actual vector primitives, shaped text and transformed stroke
outlines determine the crop through a forwarding bounds collector; anchors and
Qt's QPicture bounding rectangle are insufficient. QPicture retains the command
stream for vector replay, while the separate collector measures its extents.
The guide is presentation state and conveys composition, not a certified scale.

Ownership:

- Renderer worker: PlanCanvas header/implementation and focused renderer tests.
- Read-only scout: floor-reference coordinates, persistence and visibility seams.
- Root: export controller/helper, public API, UI wiring, CMake, user documentation,
  all native builds/tests/runtime, integration review and Git delivery.

Verification: actual PDF reopen/render/text and page dimensions, inspection of
vector commands, changed navigation zoom, rotated labels, curved/stroked shapes,
bundled SVGs, exclusion of references and transient state, empty/invalid scene
refusal, destination/sidecar preflight, save/reopen and unchanged document history.
Inspect the captured canvas and PDF. Freeze every native writer before building.

PINC-004 discovery: floor geometry uses retained world coordinates; active layer
does not isolate a floor. A durable ghost link therefore needs explicit destination
and source floor IDs, visibility and opacity. It must resolve current source
geometry separately from ordinary selection/calculations/output. Existing image
references print and cannot be repurposed without an explicit nonprinting seam.
Implementation and qualification of that link remain required.
