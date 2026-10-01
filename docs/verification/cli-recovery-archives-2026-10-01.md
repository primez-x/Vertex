# CLI recovery archive support

The CLI now opens supported desktop project archives, including ordinary and
recovery-copy files. Inspect reports the archive role and recovery summary.
Migration preserves the complete ledger, history and role; JSON extraction
includes the unmodified recovery envelopes at exchange version 12. Existing
document-only output stays compatible.

Opaque recovery archives expose diagnostic inspection only. Validate,
migrate and extract refuse them. Direct extraction API calls also validate
the ledger before creating a destination, so a caller cannot bypass archive
admission by constructing a public snapshot aggregate.

## Evidence

- Before the change, inspecting an actual desktop-generated ordinary v4
  fixture failed with `unsupported SQLite project user_version`.
- A direct extraction regression failed before the API admission guard:
  an unknown recovery kind could be exported. It passes with the guard.
- A migration regression exposed an incorrect reported format version for
  v4 archives. The report now matches the destination's SQLite user version.
- Release headless CLI and storage/archive/exchange targets built.
- Storage, archive, exchange, CLI, requirement schema and source allowlist
  checks passed 6/6 in 9.07 seconds. The process tests exercise actual v4/v6
  archives in both roles, complete envelope/history round trips, opaque and
  malformed inputs, and preservation of source/target bytes on refusal.

This verifies archive handling. It is not Apex compatibility or production
qualification.
