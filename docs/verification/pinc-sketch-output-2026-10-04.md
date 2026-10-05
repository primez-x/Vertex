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
`artifacts/pinc-sketch-output-20261004` locally. Terminal package and
installed-runtime results are recorded below.

## Delivered internal build

Implementation commit `0b8443d074105cc852a6ad9fa8b461e82c62f500` was pushed and
the exact remote ref verified. Independent review approved this increment with
the documented paired-file limitation. The offline package
`artifacts/packages/vertex-offline-20261004-sketch-output` contains 4,076 files:
1,424 source-kit files and 2,645 runtime files, plus package control files.
Source-kit allowlist and requirement-contract checks pass.

Installation at `artifacts/installed/vertex-20261004-sketch-output` finished with
exit 0. The installed application SHA-256 is
`661d497f85310bfd7c3910d7db5d7170e9bdbc079465c4e3bc63c5d5823a0f2b`,
matching the verified build and runtime report. All six installed source/reopen
samples pass for measurement, residential architecture and light-commercial
architecture with developer dependency paths removed. These samples also import
an actual PNG through the normal installed decoder and reopen its saved asset;
they are separate from the direct-asset PDF fixture described above.

The Desktop Vertex shortcut was updated and read back to confirm this exact
installed executable. The previous installation was preserved. `delivery.json`,
installer logs, the installed runtime report and capture hashes retain local
evidence. These observations are developer-host checks, not clean-machine,
network-denied or full production qualification.

## Remaining work

PINC-004 previous-floor ghost references and PINC-007 through PINC-013 remain
required work. Physical-wall classification, complete area preset mapping, every
symbol counterpart and paired user scenarios also remain open. Developer-host
checks do not establish clean-machine offline installation, hardware support,
physical printing, user-observed resolution or the unified production gate.
