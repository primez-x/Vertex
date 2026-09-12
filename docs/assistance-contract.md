# Offline assistance

Property Studio assistance is an optional, deterministic local feature. It is
session-scoped and starts disabled. The current engine has no model weights,
network client, account, activation check, or hosted service dependency.

The engine accepts a bounded grayscale pixel buffer instead of a file path. It
currently provides four proposal producers:

- **Tracing** finds a high-contrast dark-pixel envelope and proposes a closed
  analytical rectangle using the reference calibration. The source pixel
  bounds, calibration and confidence are retained in the proposal.
- **Dimension extraction** recognizes explicit-unit text (`ft`, `in`, `m`,
  `cm`, and `mm`). Unqualified numbers are ignored. The original matched text,
  character offset, parsed exact quantity and confidence remain visible.
- **Label placement** turns named document anchors into note-label proposals.
- **Natural language** supports the bounded commands `label <text> at x,y`,
  `set workspace measurement|architectural`, and `draw rectangle <width> x
  <height>`.

Every result is an `AssistanceProposal` with a stable ID, producer, source
rectangle, confidence, resource declarations and a typed command preview. The
serialized status is always `unverified`; confidence is a review hint, never a
measurement certification. Resource paths are portable and are checked by the
package loader before acceptance.

The **Offline assistance** command is available from the More menu and command
palette. It lets the user enable the session, choose a producer, inspect the
unverified list, and accept one proposal at a time. Acceptance calls
`AssistanceSession::request_acceptance`, then dispatches through the ordinary
document command path:

- labels are added to the typed annotation entity;
- trace and rectangle proposals become identified measurement boundaries;
- a dimension proposal requires a selected target boundary, upgrades a legacy
  boundary when needed, and creates a typed segment dimension atomically;
- workspace-language commands change presentation state without changing
  geometry.

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
tracing, explicit-unit parsing, label placement, the language grammar and
malformed inputs. `tests/assistance_workflow_tests.cpp` covers the Windows
desktop integration, explicit acceptance, identified-boundary creation,
legacy-boundary upgrade, undo and the disabled path.

These deterministic fixtures establish the local runtime contract. They do not
certify accuracy on arbitrary architectural plans, physical pen/DISTO input,
or Apex project semantics; those remain separate qualification evidence in the
production acceptance ledger.
