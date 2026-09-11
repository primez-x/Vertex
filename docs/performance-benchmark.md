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

Remaining evidence includes agreed reference hardware and the three prescribed
representative workloads (50,000 entities; 10,000 objects/one million triangles;
20 sheets/250 MB), actual application instrumentation, long-regeneration
cancellation preserving the valid revision, and open/save revision, snapshot,
asset and manifest integrity. The harness deliberately leaves all three
requirements unresolved regardless of supplied timing values.
