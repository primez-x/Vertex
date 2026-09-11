# Serialized workspace save queue

`WorkspaceSaveQueue` is the storage boundary for background publication. The
owner thread captures a detached `ProjectWorkspaceSnapshot` and a sealed
`SavePublicationTicket`, then enqueues a save operation that owns only those
immutable values. A worker executes operations strictly in FIFO order and
returns `SaveReceipt` values, tickets, and any exception as detached completion
records. The worker never calls the GUI, touches a mutable `ProjectWorkspace`,
or changes a saved marker.

`enqueue_barrier()` places an ordered completion after all prior work. Explicit
Save, close, and shutdown paths can use this barrier to wait for earlier
publications without introducing a second write path. `take_completed()` is a
nonblocking owner-thread drain; the owner passes a matching ticket and receipt
to `WorkspaceSaveCoordinator` to decide whether the current workspace may be
acknowledged. A stale completion remains a valid file fact and is never allowed
to acknowledge newer state.

The queue captures operation exceptions and continues with later jobs. Normal
destruction drains queued work; callers that are abandoning a project may call
`shutdown(false)` to discard jobs that have not started. Filesystem ownership,
destination identity, archive serialization, autosave policy, and restart
selection remain responsibilities of the desktop coordinator and are not
implicitly granted by this class.

Focused coverage is in `tests/workspace_save_queue_tests.cpp` and verifies FIFO
ordering, owner-thread acknowledgement, worker-thread execution, failure
isolation, and non-draining shutdown.
