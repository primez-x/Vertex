# Portable project extraction

`extract_project` writes a new directory containing versioned `project.json`
and content-addressed `assets/<sha256>.bin` files. It exports all retained
revisions and deduplicates identical asset bytes. It never replaces an existing
destination, including one created concurrently before publication.

Windows directory reservation uses `NtCreateFile` with `FILE_CREATE` and
`FILE_DIRECTORY_FILE`, relative to a retained parent directory handle. Creation
and handle acquisition are one operation. The parent and root handles remain
open through publication. The root handle denies deletion sharing until the
operation returns, preventing rename-and-plant substitution of that object.
The exact root object is published with native
`NtSetInformationFile(FileRenameInformationEx)`: `RootDirectory` is the retained
parent handle, `FileName` is one relative leaf, and zero flags forbid
replacement. There is no absolute-path fallback. The Win32
`SetFileInformationByHandle(FileRenameInfoEx)` relative-root form returned
`ERROR_INVALID_PARAMETER` in the qualified runtime probe, while the native form
completed the same handle-relative rename and rollback protocol.

Asset directories and payload files are also created exclusively relative to
owned handles. Payloads are flushed. Before any descendant handle closes, the
extractor captures every object file ID and type plus every payload's byte size
and SHA-256, verifies those values from the retained handles, and enumerates the
exact expected tree. Unexpected, missing, substituted, or reparse-point objects
block publication.

Windows refuses the final directory rename with its descendants open, even
when those descendants share deletion. The implementation closes descendant
handles immediately before renaming the still-locked root. It then reopens
`assets`, `project.json`, and every asset relative to the retained, renamed root
handle. It compares their file IDs, types, sizes, and hashes and enumerates the
exact published tree before returning success. A validation failure closes
those guards and renames the exact root back to its reserved staging leaf. If a
collision or external handle prevents rollback, the contaminated destination is
reported as the residual and is never recursively deleted.

**The descendant close-to-rename interval does not protect against another
process running as the same user changing, adding, or replacing descendants.**
Postpublication checks detect changes present during that validation, but they
are not a transactional compare-and-swap for an entire directory: a process
with the same filesystem permissions can race again after a particular check.
Extraction requires that other software leave its staging and destination
descendants alone for the operation. Ancestor-directory replacement, unusual
filesystem/filter-driver semantics, and power-loss durability are not certified
by the current tests.

Failure cleanup deletes only objects created by this extraction. Open objects
are deleted through their own handles. After descendant handles have closed,
cleanup reopens the owned `assets` directory relative to the retained root and
each payload relative to its retained or revalidated parent; file IDs and types
must match before deletion. It does not reopen a pathname and then infer
ownership. Unknown or substituted objects are preserved, and cleanup never
recursively deletes a pathname tree. If a foreign child, changed identity, or
external handle prevents cleanup, the original error and exact residual root
directory are reported through `ExtractionCleanupError`. Descendants moved
outside the staging root by an interfering process remain outside this cleanup
boundary.

Regression coverage includes Unicode destinations, complete history, binary
asset fidelity/deduplication, injected faults before and after publication,
root rename-and-plant denial, payload write/delete/rename denial before
publication, postpublication tamper detection with successful rollback, blocked
rollback with an exact destination residual, preservation of foreign additions,
and a racing destination creator. This is internal format extraction; it is not
Apex native-file compatibility certification.

API references: [NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile),
[FILE_RENAME_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information),
and [FILE_RENAME_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info).
