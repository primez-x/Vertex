# Recovery discovery

`discover_recovery_copies(source_project, recovery_directory)` performs read-only,
nonrecursive discovery of regular files named `recovery-*.bldproj`. The source is
optional. A missing recovery directory produces an empty result. Directory access
errors or more than 4096 entries produce a diagnostic and no partial candidates.

Candidates are ordered by path and loaded using `ProjectStore::load_archive` with
`ArchiveRole::recovery_copy`. A supported candidate exposes its file hash and the
validated `RecoveryCopyRecord`: archive/document IDs, source path/hash, saved
revision and generation counters. Unsupported and corrupt candidates remain in
the list with `loadable == false` and a reason. Opaque metadata is not guessed.
Multiple supported files with one archive ID remain separate, and all are marked
as duplicate and not loadable. A loadable archive can still contain a read-only
document; this API grants neither editable state nor ownership.

Source matching requires both an absolute lexical path match and an exact source
SHA-256 match. Windows path comparison is ordinal and case-insensitive. Other
platforms compare case-sensitively. Metadata paths are never opened, canonicalized,
or otherwise resolved; relative paths and parent traversal do not match. Missing
provenance, unavailable source, differing paths and differing hashes are separate
results. A mismatch does not prevent inspection of a supported recovery archive.

Input paths with parent traversal, symlinks or Windows reparse points in any
component are rejected. Candidate directories and linked files are excluded.
Discovery does not create, save, rename, delete, promote, or overwrite any file.
The source hash and candidate metadata are point-in-time observations: filesystem
checks do not protect against a hostile concurrent path replacement, and no
identity handle is retained after loading. Consumers must revalidate file hashes
and obtain normal ownership before any later open or restore action. Hard links
are ordinary regular files and are not rejected; no write authority follows from
a match. Archive validation uses existing ProjectStore resource limits; the entry
cap is not an aggregate byte or execution-time budget.

`recovery_discovery_tests` checks valid provenance, mismatch states, role-opaque and
corrupt archives, duplicate IDs, traversal rejection, filtering, missing paths,
and unchanged source/recovery hashes. A symlink exclusion check is exercised when
the environment permits creating symlinks. It does not simulate concurrent path
replacement or establish interactive recovery UI behavior.
