# Drawing readouts, overview navigation and native image paths

Drawing lengths and selected sizes use millimetres or fractional inches instead
of long decimal expansions. The approximation mark identifies display rounding;
model coordinates, entered lengths and existing endpoint priorities remain exact.
The existing Snap control still governs point and length snapping; this change
does not alter measurement magnet behavior.

The overview includes accepted draft sides, the live rubber band, anchors and
wall previews. Its draft-aware bounds are used only for overview navigation,
not fitted exports or committed document content. Pointer events over the map
must not move the authoring cursor, change a pending endpoint or place a node.

Native image export captures the physical OCCT RGB framebuffer into an owned Qt
image, preserving logical row order, padding and RGB/BGR channels. Qt encodes
and atomically writes it. This removes the WIC filename boundary while keeping
the existing model-readiness guards, selection restoration, staging, footer and
fingerprint workflow. No document or project-format migration is involved.

## Investigation and corrections

Paired installed probes used the same older executable and equal final output
path lengths. TEMP/TMP at 93 characters passed all six samples; at 198 characters
light-commercial source/reopen failed. Actual OCCT staging paths were not logged.
The correction removes that filesystem codec boundary rather than shortening
the test harness path.

Formatter and missing-overview behavioral RED checks failed before the canvas
implementation. Independent integrated review found overview callbacks could
change the very draft extents used by navigation. A real MainWindow regression
then reproduced that behavior. The corrected event route isolates map gestures
from authoring callbacks, including hover and release.

The first live-wall fixture changed only a canvas projection flag; shell refresh
restored the enabled Snap state. It now operates the persistent Snap control.
The directional display fixture compared
two floating representations of the same half-millimetre length. These fixture
assumptions were diagnosed without weakening exact-geometry checks or changing
the required magnet behavior. An initial native test requested DPR 1 while the
actual Windows monitor used DPR 1.25; the harness now records that actual scale.
The initial build command also used a nonexistent target name; its failure log
is preserved and the command now uses the real `vertex` target.

## Evidence and boundaries

Build, affected checks, real captures and terminal records are retained under
`artifacts/canvas-export-20261003`. Native coverage includes padded/top-down and
bottom-up pixel storage, RGB/BGR variants, long Unicode paths, exact framebuffer
pixel equivalence, failed-encoding destination preservation and editing-control
restoration. Installed normal/long-TEMP observations and delivery metadata are
recorded separately. Human checklist U389–U391 remains Not tested.

The final Release build and five affected native desktop suites passed. The
native export-path check passed at the observed Windows DPR 1.25, including
selection restoration after failed encoding. Root inspected the five new
readout/overview captures. Independent integrated review approved the scoped
correction after the reproduced overview issue was fixed. Static inspection
found 113 component binaries and no unresolved imports. These checks do not
replace the separately recorded fresh installed long-TEMP observation.

These are improvements within the active production plan. Full Apex parity,
legacy compatibility, clean-machine and network-denied qualification, final
ANSI validation and the unified production acceptance gate remain open.
