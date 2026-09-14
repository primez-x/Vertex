# Background regeneration boundary

`WorkspaceRegenerationQueue` is the owner-independent boundary for expensive
derived work such as analytical view preparation and future native-model
regeneration.  A caller captures immutable source data before enqueueing an
operation.  The operation receives a `RegenerationCancellationToken` and must
check it at bounded points; it never receives a mutable `Document` or
`ProjectWorkspace`.

Each operation returns a `RegenerationReceipt` containing the source revision
and an opaque output digest.  The queue does not publish that receipt into the
document or a widget.  The owner thread drains completions and applies a
completed receipt only when its source revision and any additional dependency
fingerprints still match the current workspace.  A cancelled operation never
publishes its receipt, including when it returns after cancellation was
requested.

The queue is FIFO and has one worker.  `cancel(sequence)` marks queued or
running work; queued work is skipped and running work is expected to exit at
its next cancellation point.  `shutdown(false)` cancels all accepted work,
retains ordered cancellation completions, and never invokes queued operations.
`shutdown(true)` lets accepted work finish.  Completion records are drained
nonblocking on the owner thread, so UI input and document mutation remain
thread-confined.

The `workspace_regeneration_queue` fixture covers active cancellation,
pending cancellation, FIFO completion, failure capture, non-draining shutdown,
and the source-revision invariant that prevents a cancelled result from
replacing valid workspace state.  This is the deterministic implementation
slice for OPS-PERF-002; reference-hardware latency and integrated production
regeneration measurements remain separate qualification evidence.
