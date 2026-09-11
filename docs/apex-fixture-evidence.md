# Apex fixture evidence capture slice

`scripts/apex_fixture_evidence.py` validates an explicitly selected manifest of
Apex-related fixture files and emits a capture slice for downstream compatibility
work. It is a read-only process: input files are never modified.

## Scope

This script does only one thing:

- read a manifest and its selected files
- validate schema and provenance safety
- compute per-file identity fields (`sha256`, `bytes`, `suffix_hint`,
  `bounded_header_fingerprint`)
- emit capture status fields that are intentionally separate from compatibility
  judgment

It does **not** certify Apex import compatibility.

## Required manifest shape

Top-level required keys are:

- `schema_version`
- `native_format`
- `provenance`
- `producing`
- `evidence_refs`
- `files`

### Validation rules

- `schema_version` must be exactly `1.0`.
- `native_format` must be `unknown` for this slice.
- `provenance` must contain `observed_identity` as a non-empty string.
- `producing` must contain:
  - `build` (non-empty string)
  - `modules` (list of strings)
  - `settings` (list of strings)
  - `permissions` (list of strings)
- `evidence_refs` must be a list of objects with:
  - `kind: "file"` with safe repo-relative `value` that exists, or
  - `kind: "uri"` with `http`/`https` URL.
- `files` must be a non-empty list of file entries with `path`.
- `expected_sha256` on file entries is optional; when present it must be a
  lower-case or upper-case 64-hex SHA-256 string and must match the observed
  file hash.

### Unsafe references are rejected

The script rejects:

- JSON objects with duplicate keys
- absolute paths and `..` traversal in selected file paths
- Windows drive-letter references (for example `C:\\...`)
- missing referenced files
- unsupported URI schemes
- malformed SHA-256 strings
- unsupported schema versions
- missing `provenance.observed_identity`

## Output fields

The generated JSON includes:

- `capture_complete`: capture status for the selected files
- `compatibility_passed`: always `false` in this slice
- `compatibility_reason`: explicit statement that Apex import compatibility is not
  being claimed
- `captures`: per-file records with:
  - `path` (repo-relative path)
  - `sha256`
  - `bytes`
  - `suffix_hint`
  - `bounded_header_fingerprint`
  - `expected_sha256`

`compatibility_passed` is intentionally separate from `capture_complete`; a
successful hash capture does not imply an importer compatibility claim.
