# Production and accessibility evidence contract

`scripts/production_qualification.py` validates explicitly supplied evidence for
OPS-QA-002, OPS-QA-003, and OPS-QA-005. It does not launch the application, generate
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
`audit_status` always remains `incomplete` and `qualification_passed` always
remains false. `real_evidence_complete` means caller declarations and file hashes
are complete; a reviewer must establish that the evidence actually records the
specified execution and assertions. Synthetic contract tests cannot do this.

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
| `observations` | Object with all required checks below |

Accessibility runs additionally require integer `dpi_percent` and nonempty
`device` describing actual pen/touch hardware, drivers, and assistive technology.
Record actual display scaling; resizing a screenshot is not a DPI run. Pen/touch
evidence must originate from physical input. Synthetic mouse injection is not
real pen/touch qualification.

A file reference is `{"path":"relative/file.ext","sha256":"64 hexadecimal digits"}`.
Every reference must exist beneath the explicitly selected evidence root and
match its declared SHA-256. Absolute paths, traversal, drive-relative paths,
alternate data streams, and links resolving outside the root are rejected. The
tool reads only referenced files and does not modify them. Keep evidence stable
during validation; this is not a hostile-filesystem sandbox or signed attestation.

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
`semantic`, `calculation`, `output`, and `fidelity`.

Light-commercial OPS-QA-003 additionally requires `multiple_levels`, `assemblies`,
`structural_objects`, `schedules`, `quantities`, `sheets`, `coordinated_views`,
`semantic`, `calculation`, `output`, and `fidelity`.

At each DPI scale OPS-QA-005 additionally requires `real_pen`, `real_touch`,
`keyboard_navigation`, `focus`, `accessible_properties`, `light_theme`,
`dark_theme`, `high_contrast`, and `dpi_layout`. Capture the entire core workflow
under these conditions. Evidence must show usable keyboard navigation and focus,
assistive access to properties, readable contrast, and no clipped or inaccessible
controls. Record failures even when a mouse workaround exists.

Production captures must retain independently specified semantic/calculation
expectations, project state before and after save/reopen and recovery, and actual
print/export artifacts. Recovery is an observed interruption/restore workflow,
not simply saving a project twice. A complete document must coordinate editable
model, views, quantities, schedules, alternatives/revisions, and sheets as
applicable to its requirement.

`docs/production-qualification.pending.json` is an intentionally incomplete
starting manifest. It contains no manufactured project files or observations.
