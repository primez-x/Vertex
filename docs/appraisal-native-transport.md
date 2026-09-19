# Windows local appraisal preparation transport

`WindowsAppraisalBoundary` implements `AppraisalCallBoundary` for a trusted local
worker executable. It is an opt-in transport library, not a vendor adapter or a
desktop deployment. `AppraisalDispatcher` continues to own absolute deadlines,
correlation, exact capability selection, and exact preparation acknowledgement.

The executable must have a local absolute path. The launcher supplies no shell,
arguments, destination, credentials, document handles, commit operation, or retry.
Only stdin/stdout and a stderr NUL handle are inherited through an explicit
handle allowlist. The worker starts suspended with `CREATE_NO_WINDOW`, enters a
Job Object before it runs, and remains in that job with its descendants. Job
close kills the process tree, including on launch failures and abandonment.
The environment contains only `PATH` pointing to Windows System32, `SystemRoot`,
and `WINDIR`; ambient application configuration and environment credentials are
not forwarded. This does not restrict access afforded by the Windows token.

Launch and pipe servicing run on a private supervisor thread. API calls copy
bounded messages or exchange in-memory state; they never wait for a worker read,
response, or exit. The server uses a byte-mode, nonwaiting local named pipe.
Each supervisor iteration transfers at most 16 KiB in each direction. A full
pipe is retried in the supervisor, not the caller. `abandon()` immediately revokes
reply acceptance and signals cleanup. Destruction does not join. Cleanup is
asynchronous; `supervisor_finished()` reports handle cleanup completion, not an
OS process-exit acknowledgement. The fixture tests separately wait on process
handles to verify worker and descendant termination.
The supervisor also closes the job after the second complete reply, preserving
that reply if the worker exits immediately after its acknowledgement.

## Local protocol version 1

Each frame has a four-byte unsigned little-endian byte length followed by UTF-8
JSON. The default frame ceiling is 1 MiB; configuration permits 256 bytes through
4 MiB. Zero lengths, oversize frames, trailing bytes, unexpected replies, malformed
JSON, duplicate keys, nesting beyond 16, and unknown envelope fields fail closed.
Only two commands exist, in order, with one response per command:

```json
{"version":1,"kind":"negotiate","request_id":"unique-id"}
{"version":1,"kind":"prepare","request_id":"unique-id","payload":"mapped JSON text"}
```

Replies have exactly these five fields:

```json
{"version":1,"kind":"capabilities","request_id":"unique-id","capabilities":[],"prepared_payload":""}
```

`capabilities` contains at most 64 objects using the existing appraisal mapping
JSON schema. Other reply kinds require an empty capabilities array. `prepared`
contains the exact received payload string; every other kind requires an empty
prepared payload. Other supported kinds are `offline`, `unavailable`,
`unsupported`, `ambiguous`, `worker_failure`, and `uncertain_response`.

Transport/process failures become `worker_failure`; corrupt/incomplete frames
become `uncertain_response`. The dispatcher rejects replies at or after its
deadline and abandons the transport for every terminal outcome. Worker diagnostics
are discarded rather than executed or included in application errors.

## Authority and evidence boundary

The protocol grants no destination-write capability and publishes only data for
review. The process currently runs under the caller's Windows security token:
it is **not an OS filesystem, network, credential, or
application-automation sandbox**. Use only trusted local workers that honor the
preparation-only contract. Hostile executables require a separately verified
restricted-token/AppContainer deployment before use. No vendor compatibility,
remote commit, destination protection against a malicious worker, or shipped
integration is claimed by this library.

`windows_appraisal_boundary` uses a purpose-built fixture to test success,
malformed/duplicate/oversize/truncated replies, request mismatch, crash, missing
executable, absolute timeout, rejection of output after abandon, nonblocking
backpressure, early-abandon launch races, no console, and descendant termination.
