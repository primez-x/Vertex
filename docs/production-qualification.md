# Production and accessibility evidence contract

`scripts/production_qualification.py` validates explicitly supplied evidence for
OPS-QA-002, OPS-QA-003, OPS-QA-004, and OPS-QA-005. It does not launch the application, generate
production fixtures, infer visual correctness, or qualify a device. No real
production or device observations are supplied by this package.

Run with Python 3.11 or later:

```powershell
python scripts/production_qualification.py capture.json --root C:/Evidence/selected-capture
python -m unittest discover -s tests -p test_production_qualification.py
```

The JSON report is deterministic for the same files and manifest, including run
order changes. Exit 0 means the contract is valid, every required observation is
declared passed, and every run declares real evidence. Exit 1 means missing,
invalid, synthetic, failed, blocked, or unrun evidence. Exit 2 means the manifest
could not be read/parsed. **Exit 0 does not mean runtime qualification passed.**
`audit_status` always remains `incomplete`; `qualification_passed` and
`production_accepted` always remain false, including parse failures.
`real_evidence_complete` means caller declarations, execution bindings and file hashes
are complete; a reviewer must establish that the evidence actually records the
specified execution and assertions. Synthetic contract tests cannot do this.
The validator rejects missing or inconsistent declarations and simple payload
reuse. It cannot identify a coherently fabricated execution record or authenticate
the operator, process, timestamps, hardware input, or artifact contents.

## Manifest

The root is an object with `schema_version: "1.0"` and a `runs` list. Duplicate
JSON keys and nonfinite numbers are rejected. At least one run is required for
each production requirement and one accessibility run at each of 100, 150, and
200 percent DPI. Every run has:

| Field | Contract |
| --- | --- |
| `id` | Unique nonempty string |
| `requirement` | `OPS-QA-002`, `OPS-QA-003`, or `OPS-QA-005` |
| `evidence_kind` | `real` or `synthetic`; synthetic cannot complete evidence |
| `operator` | Person who conducted the capture |
| `recorded_at` | ISO 8601 timestamp with timezone |
| `environment` | Actual Windows version/build, application version/build, display configuration, relevant settings, and capture conditions |
| `provenance` | Fixture origin, retention permission, setup, and reproduction instructions |
| `application` | File reference identifying the application binary tested |
| `source_project` | File reference to the input project |
| `reopened_project` | File reference to the saved/reopened project |
| `execution` | For real runs, a reference to the machine-readable execution JSON described below |
| `observations` | Object with all required checks below |

Accessibility runs additionally require integer `dpi_percent` and nonempty
`device` describing actual pen/touch hardware, drivers, and assistive technology.
Record actual display scaling; resizing a screenshot is not a DPI run. Pen/touch
evidence must originate from physical input. Synthetic mouse injection is not
real pen/touch qualification.

A file reference is `{"path":"relative/file.ext","sha256":"64 hexadecimal digits"}`.
Every reference must exist beneath the explicitly selected evidence root, be
nonempty, and match its declared SHA-256. For a `real` run, the application,
source project, and reopened project references must name three distinct files;
one file cannot be reused as proof of all three roles. The application content
cannot be copied to a project under a different filename. Source and reopened
projects may have equal hashes if saving preserves their contents. Absolute paths,
traversal, drive-relative paths, alternate data streams, and links resolving
outside the root are rejected. The tool reads only referenced files and does
not modify them. Keep evidence stable during validation; this is not a hostile
filesystem sandbox or signed attestation.

Every file reference in a `real` run additionally requires `run_id` equal to the
containing run's `id`, `role` equal to its exact role (`application`,
`source_project`, `reopened_project`, `execution`, or `observation:<check>`), a
`media_type` MIME type, and a nonempty `content_description` describing what the
file contains. These are explicit declarations, not inferred from filenames.
For example:

```json
{
  "path": "capture-001/recovery.mp4",
  "sha256": "<64 hexadecimal digits>",
  "run_id": "capture-001",
  "role": "observation:recovery",
  "media_type": "video/mp4",
  "content_description": "00:34-01:12: interruption, restart and restored object properties"
}
```

Observation evidence must be separate from the application and project files,
including renamed copies with identical content. Observation content cannot be
reused across different runs, even with a different path and updated metadata.
A capture may support several checks within one run, provided each reference
declares the appropriate role and the expected/observed text identifies the
relevant location. The same application build may be tested across runs.

## Execution record for real runs

The `execution` reference must declare `media_type: "application/json"`. Its
hashed UTF-8 JSON file must contain:

| Field | Contract |
| --- | --- |
| `schema_version` | `"1.0"` |
| `run_id`, `requirement`, `recorded_at` | Exact values from the containing run |
| `dpi_percent` | Exact integer from an accessibility run; omitted or null for production runs |
| `process` | Object with positive integer `pid`, integer `exit_code`, and `application_sha256` matching the tested binary |
| `artifacts` | Exactly `application`, `source_project`, and `reopened_project`, each mapped to its verified lowercase SHA-256 |
| `observations` | Exactly the required check names, each mapped to `evidence_sha256`, `status`, `expected`, and `observed` matching the manifest |

A nonzero process exit is recorded as a blocker. Expected interruption/recovery
subprocess failures belong in the recovery capture; the execution record describes
the completed qualification harness/application run. Duplicate JSON keys,
nonfinite numbers, malformed records, missing checks and inconsistent hashes or
results are rejected. Generate this record from the actual capture harness and
retain its raw logs and outputs in the referenced evidence. Hand-authoring a
matching JSON record is not proof that a process ran. An independent reviewer
must examine the artifacts and provenance before any production acceptance.
Synthetic manifests do not require these extra fields and always remain incomplete.

Each observation requires nonempty `expected` and `observed` strings, a `status`
(`pass`, `fail`, `blocked`, or `not_run`), and an `evidence` file reference. Unknown
check names are rejected. Include concrete values, tolerances, comparison method,
and a location in the evidence artifact. A failure must describe the blocker;
missing or unrun steps cannot be represented as passes or silently omitted.
Hash equality is file integrity, not proof of semantic or visual fidelity.

## Required observations

All runs require `create`, `measure`, `editable_3d`, `edit`, `calculate`, `revise`,
`save_reopen`, `recovery`, `print`, and `export`.

Residential OPS-QA-002 additionally requires `architectural_authoring`, `plans`,
`elevations`, `sections`, `schedules`, `alternatives`, `revisions`, `sheets`,
`symbol_library`, `symbol_resize`, `symbol_output`, `semantic`, `calculation`,
`output`, and `fidelity`.

Light-commercial OPS-QA-003 additionally requires `multiple_levels`, `assemblies`,
`structural_objects`, `schedules`, `quantities`, `sheets`, `coordinated_views`,
`symbol_library`, `symbol_resize`, `symbol_output`, `semantic`, `calculation`,
`output`, and `fidelity`.

The `symbol_library` observation must enumerate the shipped deterministic
catalog, record its catalog revision, and prove at least 200 usable entries
across plumbing, furniture, fixtures, appliances, accessibility, lighting,
doors/windows, structural/site, and commercial equipment. `symbol_resize` must place representative toilets,
beds, furniture, and commercial symbols at multiple scales and rotations while
retaining their declared physical footprint and anchor through save/reopen.
`symbol_output` must compare those same instances in print preview, PDF/SVG, and
image output at the supported page scales. Missing categories, placeholder-only
artwork, clipped geometry, or an unverified output path is a production blocker.
The `symbol_library` evidence should include the complete unfiltered output of
`property-cli symbols` and the passing report from
`scripts/validate_symbol_catalog.py`; this structural check supplements, but
does not replace, visual artwork and output review.

At each DPI scale OPS-QA-005 additionally requires `real_pen`, `real_touch`,
`keyboard_navigation`, `focus`, `accessible_properties`, `light_theme`,
`dark_theme`, `high_contrast`, and `dpi_layout`. Capture the entire core workflow
under these conditions. Evidence must show usable keyboard navigation and focus,
assistive access to properties, readable contrast, and no clipped or inaccessible
controls. Record failures even when a mouse workaround exists.

The OPS-QA-004 run is the packaged clean-offline workflow. In addition to the
core checks, it must explicitly record `packaged_install`, `network_denied`, and
`assistance_disabled`; these observations prevent a generic desktop smoke run
from being presented as proof of the packaged offline boundary or the
assistance-independent path.

Production captures must retain independently specified semantic/calculation
expectations, project state before and after save/reopen and recovery, and actual
print/export artifacts. Recovery is an observed interruption/restore workflow,
not simply saving a project twice. A complete document must coordinate editable
model, views, quantities, schedules, alternatives/revisions, and sheets as
applicable to its requirement.

`docs/production-qualification.pending.json` is an intentionally incomplete
starting manifest. It contains no manufactured project files or observations.

The installed-runtime smoke helper is a capture aid, not a qualification
manifest. `scripts/test_installed_runtime.py` now runs a save/reopen pair for
the measurement workspace and for both residential and light-commercial
architectural profiles, recording source and reopened `.bldproj` paths, hashes,
process results, explicit assistance-disabled mode, screenshots, and native 3D
output in its report. Source and reopened captures use distinct artifact paths;
the report compares project and native 3D hashes as the stable round-trip gate
and retains screenshot hash differences as presentation-state diagnostics.
The latest neutral-state packaged run matched all project, screenshot, and
native 3D hashes for the three captured profiles.
Those files
can be retained as candidate artifacts when a qualified operator performs the
real production workflow; the report still records developer-machine limits
and cannot satisfy the real-evidence contract by itself.
