# Pinc sketch output checkpoint

This checkpoint implements PINC-005 and PINC-006 from the supplied-tool
comparison. It does not certify full Pinc or Apex parity or production readiness.

## User access

- Tools > Export sketch PDF: save visible committed plan content as a cropped
  vector PDF for report insertion. Hidden layers remain hidden. Pending drawing,
  tracing images, the grid, selection and canvas controls do not appear.
- Tools > Sketch composition guide: show or hide the exact export crop on the
  canvas. It follows geometry and annotation changes, not the viewport edges.
- Regular selected-sheet, drawing-set, print and appraisal report outputs retain
  their existing routes. Use those for a configured sheet and print scale.

The new crop has 2 mm padding around measured painted extents. It is composition
output, not a claim of architectural scale or measurement-standard compliance.
Empty, nonfinite, unsupported or oversized content is refused with a diagnostic.
PDF pages exceeding the ordinary 200-inch limit are refused rather than resized.

## Implementation and verification

The shared renderer records vector geometry and text at fixed output scale.
A forwarding paint engine measures transformed fill/stroke outlines, clipping,
image/SVG primitives and text; the finished command stream is measured again.
The optional guide maps those extents into the current canvas view and is never
included in the recording. Fingerprints identify visible projected content and
the explicit crop settings. PDF and sidecar preflight protects existing output
from common refusal cases; these two destination files are not a single atomic
filesystem transaction.

Focused renderer cases cover navigation isolation, references and transients,
rotated labels, text-only content, thin/cosmetic/model strokes, curved geometry,
large world-coordinate translation, invalid inputs, restored clipping, and
decorated SVG equivalence. The offscreen harness loads the same bundled Inter
font as Vertex; an earlier synthetic missing-font failure is retained in local
diagnostic logs. A clip-restore defect discovered by the tests was fixed.

Actual desktop export cases reopen and render finalized PDFs, verify selectable
text and vector output, page extents/padding, units, tracing-image exclusion,
fingerprints, navigation invariance, refusal safeguards and save/reopen.
The tracing asset in this output test is authored directly through the document
API; this is not qualification of the sandboxed external-image importer.

The Release build finished successfully (`final1-build.json`). All five focused
checks passed (`final1.json`/`final1.log`): sketch content output, actual desktop
PDF export, boundary canvas, SVG symbols and wall dimensions. Root inspected the
final canvas-guide and rendered-PDF captures: the crop contains the room, area
dimension, sofa, floor annotation and rotated text without interaction overlays.
Captures and diagnostic logs remain under
`artifacts/pinc-sketch-output-20261004` locally. Package and installed-runtime
results are recorded after their commands finish.

## Remaining work

PINC-004 previous-floor ghost references and PINC-007 through PINC-013 remain
required work. Physical-wall classification, complete area preset mapping, every
symbol counterpart and paired user scenarios also remain open. Developer-host
checks do not establish clean-machine offline installation, hardware support,
physical printing, user-observed resolution or the unified production gate.
