# Autosave scheduling policy

`WorkspaceAutosaveScheduler` is the owner-thread policy used above the save
queue. It observes the workspace's independent edited, checkpoint, and
autosaved checkpoint generations without owning the mutable workspace. A
pointer-only checkpoint can therefore be recovered even though it did not
advance the explicit-save edited generation.

The default policy waits for two seconds of quiet after a change and forces a
flush at 30 seconds from the first dirty checkpoint. `capture()` seals the
generation pair exactly once while a publication is in flight. A successful
stale publication advances only its own autosaved watermark; newer checkpoint
state stays dirty and is scheduled again. A failed publication remains dirty
and retryable. Generation regressions, mismatched completions, and invalid
intervals fail closed.

The scheduler deliberately does not start threads, serialize archives, mutate
saved markers, or choose a recovery path. The Windows desktop owner combines
it with `ProjectWorkspace`, `WorkspaceSaveQueue`, and
`WorkspaceSaveCoordinator`: all editable projects receive a sibling recovery
archive after the quiet interval, with v4 recovery metadata and a guarded
destination hash. Legacy documents are rebased into a detached workspace for
capture; an explicit save advances the recovery watermark only after the
publication barrier acknowledges the saved state. Recovery discovery/opening
and unfinished live boundary input remain separate work. A successful Save As
re-destines the session's recovery archive beside the new project, and removes
the previous copy only after its recovery role, archive ID, document ID, owner
token, and last trusted fingerprint still match. Changed, malformed, or
foreign files are retained and reported as a cleanup issue.
