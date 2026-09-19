# Offline performance evidence summary

`scripts/performance_benchmark.py` validates caller-supplied workload metadata
and timing samples for OPS-PERF-001/002/003. It does not launch the application,
measure hardware, authenticate measurements, or certify the performance gate.
Every report has `audit_status: "incomplete"`, including reports within all targets.

Run with Python 3 and no third-party dependencies:

```powershell
python scripts/performance_benchmark.py --input measurements.json --output report.json
python -m unittest discover -s tests -p test_performance_benchmark.py
```

Input has exactly the following fields. This illustrative payload is synthetic;
replace the label, counts and samples with recorded observations before using it
as evidence. Counts must be nonnegative integers; zero explicitly means none.
Describe the measured machine's CPU, GPU, memory and OS in the hardware label.
A label records the caller's assertion; the tool cannot establish agreement on it.

```json
{
  "workload": {
    "id": "illustrative-drawing",
    "entities": 50000,
    "objects": 0,
    "triangles": 0,
    "sheets": 0,
    "project_bytes": 0
  },
  "reference_hardware": "SYNTHETIC EXAMPLE - replace with recorded CPU/GPU/RAM/OS",
  "samples_ms": {
    "navigation": [15.2, 16.1, 16.7],
    "input": [30, 40, 50],
    "edit": [150, 200, 250],
    "open": [4200, 4500, 5000],
    "save": [4100, 4300, 4900]
  }
}
```

All five arrays are required, with 1 to 1,000,000 finite nonnegative numbers per
array, in milliseconds. Boolean values, numeric strings, duplicate JSON keys,
unknown fields, missing fields and inputs above 64 MiB are rejected. Labels must
contain non-whitespace text and be at most 4096 characters. Workloads are
reported separately; run once per recorded workload to avoid pooling unrelated
distributions. Retain raw inputs with the instrumented run's provenance.

The deterministic p95 is the nearest-rank element at `ceil(0.95 * n)` in the
sorted sample array (one-based). A singleton's p95 is that sample. The fixed,
inclusive targets are navigation 16.7 ms, input feedback 50 ms, ordinary edits
250 ms, and open/save 5000 ms each. Input/edit/open/save p95 is this harness's
summary convention; it does not prove a bound on every operation. The report
includes sample counts, thresholds, per-metric comparisons and unresolved gates.
There is no minimum statistical adequacy claim.

Exit 0 means valid samples fall within these p95 targets; exit 1 means a valid
report contains a threshold failure; exit 2 means invalid input or an I/O error.
Reports are emitted for exits 0 and 1. These codes never mean production gate
completion. Input and output must be different paths.

The application-side cancellation boundary is implemented by
`WorkspaceRegenerationQueue` (`include/sketch/workspace_regeneration_queue.hpp`)
and its owner-thread completion contract.  Its focused fixture proves that a
cancelled derived result is discarded and cannot replace the valid source
revision; see `docs/workspace-regeneration-queue.md`.

The desktop also records bounded process-local timing samples through
`PerformanceTelemetry`. `property-studio.exe --smoke` accepts
`--smoke-performance-output <path>` and writes a version-1 JSON report after
the capture completes. The report contains the workload counts visible to the
run, nearest-rank p95 values, sample and drop counts, and explicit
`audit_status: "incomplete"` and `reference_hardware` fields. The desktop and
installed-runtime smoke runners retain and schema-check this report alongside
their screenshots and project artifacts. A missing sample or an in-process
threshold result remains incomplete; it never turns a developer-machine run
into production qualification.

Desktop navigation and input samples run from the earliest pending interaction
through completed `PlanCanvas` QPainter work. Coalesced events retain that
earliest start; committed edit samples include command duration and completed
paint. These are process-local paint completion measurements, not compositor
presentation, display latency, or native 3D frame timings. Explicit telemetry
reset and new/open project transitions partition runs and discard pending paint
measurements. Report workload counts describe final state, not every sample.
Sheets are counted from decoded sheet models; unknown persisted `project_bytes`
and unmeasured `triangles` are JSON `null`, not zero.

Validate and hash a desktop report with:

```powershell
python scripts/performance_report.py measurement-performance.json
```

### Repeatable interactive diagnostic capture

The existing `desktop_performance_tests` executable has an opt-in capture mode:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
desktop_performance_tests --capture drawing.bldproj new-drawing-capture.json
```

Run it separately for each project created by `performance-workload`, with the
same Qt/OCCT runtime paths used by the desktop tests. The output path must not
exist. The runner loads the supplied project through ProjectStore, creates an
in-memory window and collects exactly 100 samples each for navigation, input,
and edits. It alternates small zoom operations, sends pointer-move events, and
alternates redo/undo of a temporary boundary. Every operation waits for the
application's own telemetry to observe completed painting before the next
operation begins. Missing completion fails after a 30-second polling deadline;
synchronous handlers themselves cannot be interrupted by this deadline.

The runner does not save the source project. It checks exact entity and asset
byte equality against the initially loaded snapshot after undoing the probe.
History and revision numbers change in memory; this is content restoration,
not identical snapshot/history preservation. Output records the source file
hash, original authoring digest, final entity digest, before/after revisions,
and the unmodified application report under `desktop_report`. An unsuccessful
capture can leave an empty output file; only exit 0 yields a complete capture.

This is an offscreen 1200x800 plan-canvas diagnostic, including when the input
is an architectural or sheet project. It does not measure native 3D rendering,
sheet page/image presentation, compositor/display latency, or interactive
input hardware. Pointer events are programmatically delivered, not physical.
Probe redo/undo characterizes that specific command, not all ordinary edits.
Zero open/save samples and unknown triangle/project-byte values remain in the
application report. No percentile is reconstructed from percentiles, and no
missing measurement or qualification label is filled in. This wrapper is not
a version-1 qualification desktop report and cannot complete qualification.

The remaining capture integration requires real desktop open/save repetitions
(new/open currently resets the telemetry), workload identity and measured
triangle binding, a recorded long-regeneration cancel/discard observation,
and operator-approved reference hardware. Existing queue fixtures establish
unit-level cancellation behavior, not that runtime observation.

This validator checks the bounded report's schema, metric consistency, and
incomplete audit boundary. It does not authenticate measurements. Its desktop
report format is distinct from the caller-supplied input format above.

The `performance-workload` CLI creates representative project fixtures and
prints a separate JSON storage/integrity report to stdout:

```powershell
performance-workload drawing drawing.bldproj "CPU/GPU/RAM/OS label" "source revision"
performance-workload architecture architecture.bldproj "CPU/GPU/RAM/OS label" "source revision"
performance-workload sheets sheets.bldproj "CPU/GPU/RAM/OS label" "source revision"
```

Both the destination and its appended `.roundtrip.bldproj` path must be new.
The fixtures contain 50,000 drawing entities, 10,000 architectural objects, or
20 sheets with 12,500,000 bytes per reference asset. The report records actual
persisted size, decoded sheet counts, tessellated triangle counts and meshing
parameters, asset hashes, and semantic digests. It measures two synchronous
ProjectStore saves and opens and compares authoring content and workload facts
after both reopens. Semantic content is deterministic; document identity is
fresh. Hardware and revision labels are caller assertions. The CLI does not
measure GUI interactions, sheet image decoding, or rendered output, and its
storage report is not input to `performance_report.py`.

Native geometry preparation builds worker-owned topology from captured
snapshots. Superseding requests discard stale candidates, including requests
at equal document revisions; owner-thread completion retains the live scene
on worker failure. Cancellation polls bracket objects and join input members;
an individual wall/roof join builder is not interruptible. This implementation
does not establish a bounded cancellation latency or native 3D frame target.

Remaining evidence includes agreed reference hardware, observed runs of all
three prescribed representative workloads (50,000 entities; 10,000 objects/one
million triangles; 20 sheets/250 MB), statistically adequate interactive
measurements, long-regeneration cancellation preserving the valid revision,
and production open/save revision, snapshot, asset and manifest integrity.
Fixture generation and focused implementation checks do not complete these
acceptance gates. All reports retain `audit_status: "incomplete"`.

[`performance_qualification.py`](performance-qualification.md) validates those
inputs as one hash-bound, fail-closed evidence manifest. It requires the three
representative workload profiles, agreed-hardware metadata, adequate samples,
cancellation preservation, and project/package integrity records. A complete
manifest remains reviewable evidence and does not certify the hardware or the
unified production gate.

## Development-host storage diagnostic, 2026-09-19

The Release workload runner exposed a real save-path regression on the current
development host (Ryzen 9 7950X3D, RTX 5090, 64 GiB RAM, Windows build 26100).
At source revision `9730ad6`, the 50,000-entity drawing fixture opened in
1,984-2,103 ms but saved in 6,604-6,707 ms, beyond the five-second target.

ProjectStore was rebuilding the complete logical manifest four times and was
copy-validating the caller snapshot before validating the exact staged bytes.
The save path now retains the requested and decoded digests already used by
write/read verification, then performs its full structural validation once on
the decoded staging snapshot before publication. No digest, SQLite integrity,
asset hash, publication-handle, CAS, or recovery-ledger check was removed.

The same 50,000-entity fixture then opened in 1,981-2,206 ms and saved in
3,606-3,669 ms. The 20-sheet fixture with 250,000,000 placed asset bytes opened
in 627-652 ms and saved in 1,211-1,248 ms. Both fixtures preserved their
authoring-source digest, decoded workload facts, file hash, and asset bytes
through two save/open cycles. A 10,000-object architectural run produced
5,000,000 measured OCCT triangles and remained well below the storage target.

These are development-host diagnostics that demonstrate the defect and its
correction. They do not designate agreed reference hardware, measure compositor
presentation or interactive percentiles, establish statistical adequacy, or
complete OPS-PERF-001/002/003.
