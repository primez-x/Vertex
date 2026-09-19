# Assistance

Vertex assistance is an optional, deterministic local feature. It is
session-scoped and starts disabled. The current engine has no model weights,
network client, account, activation check, or hosted service dependency.

The engine accepts a bounded grayscale pixel buffer instead of a file path. It
currently provides five proposal producers:

- **Tracing** finds a high-contrast dark-pixel envelope and proposes a closed
  analytical rectangle using the reference calibration. The source pixel
  bounds, calibration and confidence are retained in the proposal.
- **Edge tracing** segments the thresholded raster into deterministic connected
  components and proposes one convex analytical contour per useful component.
  Each proposal records its component bounds, pixel count, calibration, and the
  `connected-components-v1` trace mode. Small isolated raster specks are
  ignored; the contour remains provisional until reviewed.
- **Dimension extraction** recognizes explicit-unit text (`ft`, `in`, `m`,
  `cm`, and `mm`). Unqualified numbers are ignored. The original matched text,
  UTF-8 byte offset, parsed exact quantity and confidence remain visible.
  Normal PDF import extracts embedded page text inside the isolated reference
  worker. Each proposal retains the actual normalized PDF selection rectangle
  of the text line containing the match, not an estimated character position.
  A line can contain several quantities sharing that line's selection bounds.
  Text without validated selection bounds generates no dimensions. Raster-only
  PDF pages and image imports have no OCR and yield no text dimensions.
- **Label placement** turns named document anchors into note-label proposals.
- **Natural language** supports the bounded commands `label <text> at x,y`,
  `set workspace measurement|architectural`, and `draw rectangle <width> x
  <height>`.

Every result is an `AssistanceProposal` with a stable ID, producer, source
rectangle, confidence, resource declarations and a typed command preview. The
serialized status is always `unverified`; confidence is a review hint, never a
measurement certification. Resource paths are portable and are checked by the
package loader before acceptance.

The **Assistance** command is available from the More menu and command
palette. It lets the user enable the session, choose a producer (including
connected-component edge tracing), inspect the unverified list, and accept one
proposal at a time. Acceptance calls
`AssistanceSession::request_acceptance`, then dispatches through the ordinary
document command path:

- labels are added to the typed annotation entity;
- trace and rectangle proposals become identified measurement boundaries;
- a dimension proposal requires a selected target boundary, upgrades a legacy
  boundary when needed, and creates a typed segment dimension atomically;
- workspace-language commands change presentation state without changing
  geometry.

Reference-based assistance requires explicit calibration before generating any
proposal. The reference must retain two finite, distinct source points and a
finite positive known distance; their derived metres per source unit must be
finite, positive, and agree with the stored calibration. Missing, malformed,
or inconsistent calibration produces a request to calibrate the reference and
no proposal or document change. The display-only default scale remains
available for visual underlays and manual tracing; it does not authorize
automated measurements.

The reference worker uses the strict `PSIR0002` response: a 32-byte little-endian
header, exact RGBA pixel extent, UTF-8 text, then fixed 40-byte selection records
(byte offset, byte length, and four binary64 normalized coordinates). Page text
is limited to 16 KiB and 512 ordered, nonoverlapping selection runs. The broker
rejects unknown versions, trailing/truncated data, invalid UTF-8 including
incomplete sequences, NULs, oversized metadata, byte ranges splitting UTF-8
characters, and nonfinite, empty, or off-page selections. Image replies cannot
carry text metadata. PDF parsing remains subject to the worker's process,
memory, and deadline controls; unsupported or oversized text metadata fails the
import rather than being silently truncated. The reference entity persists
`source_text_version: 1`, `source_text`, and `source_text_runs`; reopened projects
revalidate these runs before assistance uses them. The source coordinates remain
relative to the original page, independent of display transforms.

All document mutations run through `Document::preview_command`, the normal
revision check, and the normal undo/redo transaction. Disabled assistance,
unaccepted proposals, missing resources, malformed payloads, read-only
documents and stale or conflicting targets fail closed without a document
change. The application does not mark an inferred value verified and does not
silently change an area classification.

The deterministic implementation is recorded in
`assets/assistance/deterministic-engine-v1.json` and is included in the
portable allowlist with the private-source notice. The source and package
manifests retain the same provenance and license boundary as the rest of the
application; no third-party model license is introduced by this engine.

`tests/assistance_contract_tests.cpp` covers the proposal envelope and strict
acceptance rules. `tests/assistance_engine_tests.cpp` covers deterministic
envelope and edge tracing, explicit-unit parsing, label placement, the language
grammar and malformed inputs. `tests/assistance_workflow_tests.cpp` covers the
Windows desktop integration, explicit acceptance, identified-boundary creation,
connected-component tracing, the calibration provenance gate, legacy-boundary
upgrade, undo and the disabled path.
`tests/reference_import_tests.cpp` covers real embedded-text PDF extraction,
distinct line positions, deterministic bounds, and malformed worker replies.
The desktop workflow covers codec extraction through persisted metadata to a
dimension proposal and project save/reopen. On source-build hosts that do not
meet the installation sandbox gate, it explicitly asserts production rejection
and seeds codec-validated test metadata for the downstream checks. Direct codec
execution and seeded fixtures do not establish AppContainer qualification or
production end-to-end import acceptance.

These deterministic fixtures establish the local runtime contract. They do not
certify contour accuracy on arbitrary architectural plans, physical pen/DISTO input,
or Apex project semantics; those remain separate qualification evidence in the
production acceptance ledger.
