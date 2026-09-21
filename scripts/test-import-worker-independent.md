# Independent Windows import-worker fixture capture

Run from PowerShell 7 on the development host after building the test targets:

```powershell
./scripts/test-import-worker-independent.ps1 -Configuration Release
./scripts/test-import-worker-independent.ps1 -Configuration Debug
./scripts/test-import-worker-independent.ps1 -Configuration Release -CaptureSelfTest
```

The runner uses local `Win32_Process.Create` with the current user's token and
`SW_HIDE`. It creates a hidden PowerShell child; test processes use
`CreateNoWindow`, and Qt uses the offscreen platform. No elevation, password,
scheduled task, security-policy change, or persistent WMI subscription is used.
The private `-Child` and `-CaptureDirectory` options are internal launch plumbing.

Each fixture has a 60-second deadline. Each stdout/stderr stream is copied as raw
bytes through an 8 KiB buffer into a file capped at 1 MiB. Overflow terminates the
test process tree and records `stdout_limit_exceeded` or `stderr_limit_exceeded`
in `capture_failure`; capture I/O errors receive a distinct `*_capture_failed`
code. Truncated output cannot count as a passing fixture. The parent has a
150-second deadline and
retains the independent process handle to terminate its process tree on failure.
All four fixtures run even when an earlier fixture fails, so their evidence remains
separate. PowerShell, fixture/probe/codec hashes, before/after binary timestamps
and sizes, process identity, Job Object membership, exit codes, timeout flags,
stdout/stderr hashes, and confirmed host exit are recorded beneath a new
`artifacts/import-worker-independent/<configuration>-<uuid>/` directory.
`launcher.json` records the launch; `result.json` records the fixtures.

Exit zero requires a job-free host, four successful fixtures, stable binary
provenance, and confirmed process termination. Exit 77 from a fixture remains a
skip and causes runner failure. A missing result, launch failure, changed binary,
timeout, or nonzero fixture exit also causes failure. Keep the output directory
with any report; these are local development artifacts, not signed evidence.

`-CaptureSelfTest` runs hidden synthetic output producers instead of the worker
and assistance tests. It checks stdout overflow, stderr overflow, both streams
exactly at the limit, empty output, and an output-file creation failure.
Overflow and capture-failure producers wait after writing; the checks require a
nonzero killed-process exit, confirmed termination, exact capped file lengths,
the correct capture-failure code, and a nullable hash when no regular output
file exists. Self-test captures are marked `capture_self_test: true` and are not
worker qualification evidence.

On the investigated host, current-user S4U task registration was denied. A
limited interactive-token scheduled task ran inside a Job Object without
breakaway permission, leaving the worker fixture skipped. Those exploratory
tasks were deleted and absence was verified. The WMI launch ran outside every
Job Object, exposing an actual worker fixture failure in both configurations.
The broker report identified native launch error 203: its filtered environment
omitted `LOCALAPPDATA`. The broker now supplies a value rooted in its private job
directory and preserves `CreateProcessW`'s native error before cleanup. It also
allocates the full variable-size `TokenAppContainerSid` result when attesting
the expected identity. Fixture corrections grant the launching user read/execute
permission without write access, use the probe's actual arguments, reuse a
protected module directory for all cases, and treat pipe closure as input EOF.

The corrected job-free Debug and Release captures pass echo, live deadline
termination, oversized-output rejection, and malformed-image launch-error
preservation. The runner creates a fixture-local read-only runtime containing the
bundled worker and its fixed Qt dependencies, grants the worker AppContainer
read/execute access, and runs the desktop DXF and IFC entry points from that
runtime. Those workflows verify atomic insertion, broker-derived isolation
receipts, source retention, undo/redo, save/reopen behavior, and unchanged
projects after malformed input. Assistance continues to pass while logging trusted-reference
fallback. The raw worker JSON records observed controls separately from test
success: network denial is a check for absent network-capability SIDs, and
parent-exit termination is a check of the kill-on-close job flag. These are not
active network or parent-crash experiments.

The broker now constructs a minimal explicit environment from OS system paths,
the verified module roots, broker-owned temporary paths, and fixed PROJ settings.
It does not inherit arbitrary caller variables. A sentinel-presence probe checks
non-inheritance without reading or printing its value. A fixture-local import
slot injection forces job assignment failure; the test retains the exact
suspended-process handle and verifies that the broker terminates it directly
before returning. No injection hook is exposed by the production broker.

The runner captures the fixture's broker reports; it does not grant installation
ACLs or qualify an installed application. `assistance_workflow_tests` includes
synthetic attestation and trusted fixtures, so its pass does not establish live
AppContainer acceptance. Even a worker fixture pass would remain development-host
evidence; production network-denial, parent-exit, module-planting, clean-machine,
and installed PDF/raster import acceptance remain separate requirements.
