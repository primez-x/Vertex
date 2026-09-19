# Performance evidence qualification contract

Run the standard-library validator against an explicitly selected manifest:

~~~powershell
python scripts/performance_qualification.py path/to/qualification.json
~~~

The command emits deterministic JSON to stdout. Exit 0 means the submitted
contract is complete; exit 1 means a missing, inconsistent, or failing contract
claim; exit 2 means unreadable, unsafe, oversized, or malformed input. Argument
errors use argparse's exit 2 and stderr. A result always retains
`audit_status: "incomplete"` and `production_qualified: false`, even when
`contract_passed` is true. It advances evidence readiness for
OPS-PERF-001/002/003; it cannot certify hardware or the unified release gate.

## Version 1 manifest

The root is a JSON object with these required fields:

| Field | Contract |
| --- | --- |
| schema | `vertex-performance-qualification` |
| schema_version | Integer `1` |
| run_id, operator, source_revision | Nonblank strings identifying this run, its observer, and its build source |
| timestamp | ISO-8601 date/time with seconds and explicit timezone, such as `2026-09-19T12:00:00Z` |
| hardware | One object with nonblank `id`, `cpu`, `gpu`, `memory`, `os_build`, and Boolean `agreed: true` |
| build_artifact | File binding for the measured application build |
| offline_package_manifest | File binding for an offline package manifest, which must parse as a JSON object |
| workloads | Exactly three objects, one each for `drawing`, `architecture`, and `sheets` |
| cancellation | One cancellation observation object described below |

A file binding is `{"path": "relative/file", "sha256": "<64 lowercase hex digits>"}`.
All files must be inside the manifest directory. Paths use forward slashes;
absolute, drive/UNC, alternate-stream, empty, dot, parent, reserved Windows,
and trailing-dot/space components are rejected. Symlinks and Windows reparse
points (including junctions) in any component of the manifest or evidence
path are rejected, even when they point inside the evidence directory.

Every JSON input is limited to 4 MiB. Build and project files are limited to
2 GiB each and hashed in bounded chunks. Empty files, nonregular files,
duplicate JSON keys at any depth, nonfinite numbers, and invalid UTF-8 fail.
Every reported digest covers exactly the bytes read. Metadata is checked
before and after reading to reject observed replacement or modification.
Project and round-trip bindings must refer to six distinct file identities;
case aliases and hard links cannot reuse one file for separate evidence roles.
Use a quiescent evidence directory: portable standard-library path checks
are not a race-proof sandbox against an attacker concurrently replacing
ancestor directories. The validator does not modify files or follow links
specified inside the offline package manifest.

## Workloads and reports

Each workload object has these fields:

~~~json
{
  "profile": "drawing",
  "desktop_report": {"path": "drawing-desktop.json", "sha256": "<sha256>"},
  "storage_report": {"path": "drawing-storage.json", "sha256": "<sha256>"},
  "project": {"path": "drawing.bldproj", "sha256": "<sha256>"},
  "roundtrip": {"path": "drawing.roundtrip.bldproj", "sha256": "<sha256>"},
  "document_id": "<document identity from storage report>",
  "revision": 1,
  "integrity": {
    "semantic": true,
    "snapshot": true,
    "assets": true,
    "manifest_validation": true
  }
}
~~~

Hash placeholders in this illustration must be replaced with actual SHA-256
digests. Supply equivalent entries for architecture and sheets.

Desktop JSON must satisfy the existing version-1
`scripts/performance_report.py` schema. Its hardware label must exactly equal
`hardware.id`. Desktop and storage workload IDs must equal
`vertex-representative-<profile>-v1`. Their nonnegative integer entity, object,
triangle, sheet, and persisted project-byte counts must match exactly.
Unknown desktop triangles or project bytes cannot establish a complete
contract. The desktop recorder may therefore require an independently
collected, accurately populated report for a qualification run; this
validator does not fill missing measurements or alter raw reports.

| Profile | Minimum storage workload facts |
| --- | --- |
| drawing | 50,000 entities and 50,000 drawing_entities |
| architecture | 10,000 objects and 1,000,000 triangles |
| sheets | 20 sheets and 250,000,000 placed_asset_bytes |

| Desktop metric | Minimum retained samples | Maximum p95, ms |
| --- | --- | --- |
| navigation | 100 | 16.7 |
| input | 100 | 50 |
| edit | 100 | 250 |
| open | 20 | 5000 |
| save | 20 | 5000 |

All desktop metrics must have zero dropped samples. These minimums are an
explicit readiness policy, not a statistical confidence guarantee. Desktop
p95 values are validated for schema, flags, and thresholds; aggregate desktop
reports do not contain the individual samples needed to recompute p95.

Storage JSON uses the shape emitted by `performance-workload`:

- Integer `schema_version: 1`, `audit_status: "incomplete"`, and
  `generator: "vertex-representative-workload-v1"`.
- `provenance.reference_hardware` equals hardware.id and
  `provenance.source_revision` equals the manifest source revision.
  `labels_verified` remains false because these labels are assertions.
- `workload` contains the matching counts, profile minimums, a valid
  `semantic_sha256`, an `assets` list with unique nonblank IDs, byte counts,
  and SHA-256 hashes, and `asset_bytes` equal to the sum of that list.
  `placed_asset_bytes` cannot exceed that inventoried asset content.
- `integrity.document_id` and integer `revision` match the workload entry;
  `authoring_source_sha256`, `project_sha256`, and `roundtrip_sha256` are
  valid hashes. Project hashes match the bound files, and project_bytes
  matches the actual initial project file size.
  `snapshot_and_assets_equal_after_both_reopens` must be true.
- `samples_ms.open` and `samples_ms.save` each contain 2–4096 finite,
  nonnegative durations. Their nearest-rank p95 must be at most 5000 ms.
  These supporting storage observations do not substitute for the 20
  desktop open/save observations.

The explicit workload `integrity.manifest_validation: true` records the
operator's claim that project-manifest validation was exercised. The current
storage runner does not emit a separate manifest-validation result. The
validator checks this claim's presence, but does not rerun ProjectStore or
independently verify semantic, snapshot, or asset equality from SQLite bytes.
It checks the offline package manifest's bytes and JSON object shape, not
package completeness or the manifest's internal artifact inventory.

## Cancellation and output boundary

The cancellation object requires nonblank `request` and `observation`
descriptions, finite nonnegative `latency_ms`, nonnegative integer
`document_revision_before` and `document_revision_after` values that are
equal, and Boolean `stale_result_discarded: true` and
`valid_revision_preserved: true`. No cancellation latency target is invented:
latency is recorded, while the preservation and stale-result claims are
required. This is a caller observation, not independently witnessed runtime
proof.

Output includes the qualification manifest's own hash, every referenced
file's actual byte count and hash, workload counts, desktop sample counts,
ordered errors, and an explicit qualification boundary. Identical inputs
produce identical JSON; the tool adds no current timestamp or host metadata.
Caller claims, hardware, operator, agreement, report authenticity, and
measurement accuracy are not independently established. A complete
manifest is reviewable evidence, not production approval.
