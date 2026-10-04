# Deep Windows project paths

Project save/read/hash, guarded replacement and recovery discovery now qualify
long paths at native I/O boundaries. SQLite uses the locking `win32-longpath`
VFS. Caller-facing receipts, barriers, backup paths and source provenance keep
their ordinary spelling. The project format remains unchanged. Existing local
fixed NTFS/ReFS durability admission, stream/reparse refusal, expected hashes,
staging locks, backup verification and handle publication remain in force.

The native conversion follows Microsoft's
[extended-length path guidance](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation).
The database adapter uses SQLite's
[documented Windows long-path VFS](https://www.sqlite.org/vfs.html), retaining
locking rather than selecting a no-lock adapter. No registry changes, temporary
directory relocation or shorter test-path workaround are used.

## Investigation and implementation

The prior installed build failed saving a project before image export. With
console logging enabled, its actual failure was `CREATE_NEW` staging reservation,
Windows error 3. A deep storage regression failed because ordinary filesystem
inspection reported its existing parent absent. Ownership and discovery
regressions independently failed before their path corrections. Original
records remain under `artifacts/deep-project-paths-20261003` and the previous
`artifacts/canvas-export-20261003` failing installed report.

A shared native-path helper resolves conventional paths before qualifying them;
short Windows alias behavior is retained. File inspection, creation, read locks,
volume checks, publication, sidecar checks and cleanup use the qualified form.
Temporary cleanup caches its native path before publication so the destructor
does not acquire a new throwing path-resolution operation. Recovery scanning
compares Windows filenames as wide strings, preventing an unrelated Unicode
name from aborting discovery, and returns conventional candidate paths.

Independent review found missing destinations could acquire separate ownership
leases through ordinary and qualified spellings. A regression reproduced this
before the correction. Ownership now collapses recognized qualified DOS/UNC
spellings only for identity-key construction and path matching, preserving
ordinary lease keys and public provenance; other namespaces remain distinct.

## Verification and boundaries

The Release build and nine focused suites passed: storage, ownership, process
ownership, recovery discovery, workspace, archive restore, save coordinator,
save queue and native desktop recovery. Storage fixtures include deep ASCII
and CJK paths beyond both the legacy Windows boundary and the default SQLite
VFS's UTF-8 capacity, with native save/load/hash, relative and qualified aliases,
replacement backups and recovery archives. All four injected failure stages
preserve originals and clean unpublished staging. Existing staging, destination,
backup and competing-save tests remain in the passing storage suite.

After the ownership alias correction, the final application build and three
affected ownership/process/native-recovery checks passed. The separate final
build/check records bind that correction to delivery.
Independent integrated review approved the scoped correction after the alias
finding was reproduced, fixed and verified. Static inspection found 113
component binaries and no unresolved imports.

Build/check logs and actual exit records are retained in the artifact directory.
Fresh installed observations, executable hashes, packaging and remote revision
verification are recorded separately. User checklist U392 remains Not tested.
These focused checks do not establish machine power-loss/disk-full qualification,
final ANSI validation, Apex compatibility or full production acceptance.
