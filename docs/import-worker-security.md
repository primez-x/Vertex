# Import worker security policy

`ImportWorkerPolicy` and `evaluate_import_worker` are a portable, fail-closed
policy and adversarial fixture for SEC-WORKER-001, SEC-WORKER-002 and SEC-PROJ-001.
They do not create a Windows process, set a token, enforce a Job Object, inspect
the filesystem, call PROJ, or prove OS-level isolation. This fixture is not a
production sandbox acceptance result.

The Windows boundary now has a native broker slice in
`run_windows_import_worker`. It validates local executable and root paths,
creates a named AppContainer profile, provisions a per-job ACL-protected
temporary directory, launches the worker suspended with explicit brokered
handles and a controlled Unicode environment, assigns a single-process
memory-limited Job Object, attests the AppContainer identity and network
capability set, and only then resumes the worker. A watchdog terminates the
whole job on deadline or output-limit failure, and partial output is discarded.
The broker has no unsandboxed fallback.

`windows_import_worker_tests` and its static `/MT` probe exercise the live
Windows path when the test process is not already inside a parent Job Object.
Some CI and desktop test harnesses place every child in such a job; those
hosts return CTest skip code 77 with an explicit message. A skipped fixture is
not runtime evidence: clean-machine AppContainer launch, network-denial,
module-planting, parent-exit, and PROJ-resource observations remain required
for the production gates.

The policy requires AppContainer, no network capabilities, brokered input
handles, a private temporary root, immutable fixed module search roots, a
single-process job with a memory ceiling and parent-exit termination, and
disabled PROJ networking and custom network callbacks. Every corresponding
adapter attestation defaults to false. A missing control rejects the work.
The broker must provide policy and attestations from trusted configuration;
neither may come from the imported document or the worker's own claims.

The Windows adapter must create the worker suspended, establish and verify the
restricted token and network denial, assign the Job Object with memory and
active-process limits and kill-on-job-close before resume, and prevent the
worker from breaking out of that assigned job. If the caller is already inside
an enclosing Job Object, the broker requests a breakaway only when that parent
explicitly permits it; otherwise it fails closed.
It must apply a watchdog that kills the whole job at the deadline independently
of worker cooperation. It must close the job on parent termination. Only
explicit brokered handles may be inherited. No direct source/project handles
or writable module roots may be exposed to the worker.

Temporary storage must have a per-job private ACL, a storage quota and bounded
cleanup. Resolve paths using handles, reject reparse points and verify final
containment and root identity without a check/use race. The portable lexical
validator deliberately accepts a conservative ASCII path subset, rejecting
absolute archive names, traversal, alternate streams, device names, ambiguous
trailing characters, and empty segments. Lexical validation cannot detect
junctions, symlinks, hard links, filesystem aliases or root overlap. The adapter
must ensure the temporary and immutable module roots are disjoint. Fixed search
must exclude current directory, user PATH and temporary directories and must
cover dynamically loaded modules as well as startup imports.

Input length, cumulative expanded bytes, expansion ratio, elapsed time and
child-process counters are checked with overflow-safe arithmetic. The broker
must measure them itself and interrupt decoding as soon as a limit would be
crossed; a post-decompression check alone does not contain a bomb. Malformed
input and crashes reject the operation. Never publish partial worker output:
validate broker-owned staged results and commit only after successful worker
completion, preserving the existing project on every failure.

For PROJ, the adapter must explicitly disable networking on each context,
install no custom network callbacks, ignore inherited network-enabling settings,
and restrict resource lookup to its verified bundled manifest. The fixture's
resource lists represent that trusted manifest, not evidence that files exist.
A missing resource emits `missing_local_proj_resource`; the adapter should map
that code to a local UI diagnostic and never attempt a download or silent
lower-accuracy coordinate transformation.

Decision JSON has schema version 1, sorted unique diagnostic codes, and no
untrusted path or payload text. A rejection requests whole-worker termination;
the actual adapter must perform and verify that action. Tests cover fail-closed
attestation, hostile paths, size/ratio overflow, deadline, child process, malformed
data, crash, PROJ offline resources and deterministic reports. Live Windows
network denial, module planting, job termination, temp cleanup and project
survival fixtures remain required at the adapter boundary. The native broker
reports those controls only after successful runtime attestation; a launch
failure or skipped host cannot be promoted to a passing production result.
