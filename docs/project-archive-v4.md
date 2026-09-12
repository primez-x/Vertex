# Recovery-bearing project archives

The recovery-aware store API is separate from document-only `save` and `load`.
`save_archive` accepts a `ProjectArchiveSnapshot` containing a document snapshot,
a nonempty recovery ledger and an archive role. `load_archive` takes the role
expected by the caller and returns a supported archive, ledger interpretation
and a hash of the locked source bytes. It does not return an editable Document.
Unfamiliar records or a role mismatch yield an opaque result containing the
original ledger and source hash, without a reusable document snapshot. The
complete original archive remains on disk for future exact-byte handling. This is an
internal persistence API; core workspace restoration now consumes the validated
decoded aggregate. Desktop has guarded v4 open/save and workspace-command
coverage; the reusable save queue, autosave scheduler, and startup recovery
selection are implemented. Retention cleanup and final restore qualification
remain open.

Version 4 keeps the five existing document tables and adds one STRICT table:

```sql
CREATE TABLE project_recovery_records(
    record_id TEXT PRIMARY KEY,
    record_kind TEXT NOT NULL,
    envelope_json TEXT NOT NULL
) STRICT;
```

Both the SQLite user-version marker and `metadata.format_version` are `4`.
The metadata key set remains `format_version`, `document_id`, `head_revision`,
`saved_revision` and `logical_digest`. A v4 file with no recovery rows is invalid.
The document-only routes continue accepting only versions 1–3, and cannot
overwrite a v4 destination even with its correct current hash.

Unlike versions 1–3, v4 preserves the captured optional saved revision. Metadata
stores it as unsigned decimal text or the literal `null`. The logical manifest
uses the corresponding JSON integer or null. Writing an archive does not
acknowledge a save in the live workspace; recovery-copy explicit-save anchors
remain independently validated against retained document history.

The logical manifest contains the existing document fields using format `4`,
plus `recovery_records`: an array sorted by record ID. Each entry contains
`record_id`, `record_kind` and `envelope`, where the last field is the parsed JSON
value. Canonical JSON participates in the SHA-256 digest, including unknown
records and extension values. Whitespace and object-key order are not part of
this logical digest; the separately returned file hash covers exact bytes.
Duplicate JSON object keys in recovery rows are rejected, rather than discarded
by a permissive parser.

Aggregate save uses the same locked staging, validation, fingerprint comparison,
verified backup and atomic publication path as document-only saves. It validates
the complete archive before publication. It cannot rewrite an opaque ledger,
erase unknown destination records, or implicitly change an existing archive's
role. A legacy destination can be upgraded with its expected hash and a retained
backup; promotion and independent-copy operations remain separate work.

The desktop now supplies live save acknowledgement, long-lived workspace
ownership, startup selection for unsaved unbound recovery copies, and
ownership-verified Save As cleanup/rebasing for its generated recovery copy.
Exact-byte copying of opaque files and final restore authorization remain
separate qualification work.
