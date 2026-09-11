# Recovery ledger validation

`recovery_ledger.hpp` defines the aggregate validation boundary for the three
recovery record kinds. A ledger is a nonempty sequence of records containing
`record_id`, `record_kind` and a JSON envelope. IDs are unique; each known kind
occurs at most once. This internal value API is used by the separate SQLite v4
archive routes documented in `project-archive-v4.md`; core workspace restoration
revalidates and detaches the decoded aggregate before installation.

An active boundary requires workspace history. A recovery-copy record also
requires history, and its workspace epoch, edited generation and checkpoint
generation must equal the corresponding history values. Its document identity
and nullable explicit-save revision must match the associated retained document.
These checks neither mark a document saved nor grant ownership of a path.

The ordinary archive role excludes recovery-copy records; the recovery-copy
role requires one. A role mismatch yields the whole ledger as opaque, without
decoded editable state. Changing a filename cannot promote a recovery archive.

Unknown record kinds or positive outer/nested versions preserve every original
record, including row order and JSON number representation. Known malformed
version discriminators and duplicate IDs/kinds are errors. Version discovery
precedes canonical replay, so an unfamiliar aggregate is never partly decoded.

Borrowed document and envelope data are checked against cumulative resource
limits before ledger copies or canonical input replay. Wrapper metadata is
charged as well as payloads. For recognized records, raw action counts, closure
work and lifecycle-event counts are checked before invoking individual codecs.
The limits are conservative admission estimates, not memory or timing promises.

SQLite schema checks, sorted-row logical hashing and locked atomic publication
are implemented in the archive store. Exact-byte copying of unfamiliar archives,
the remaining desktop mutation paths, desktop autosave scheduling, and restart
recovery remain separate work. The reusable immutable save queue and
owner-thread autosave policy are covered independently. The document-only store
still supports formats 1–3 and refuses recovery-bearing destinations.
