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
saved markers, or choose a recovery path. The desktop owner will combine it
with `ProjectWorkspace`, `WorkspaceSaveQueue`, and `WorkspaceSaveCoordinator`
when autosave/restart UI is integrated.
