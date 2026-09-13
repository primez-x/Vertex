# Apex compatibility evidence matrix

`scripts/apex_compatibility_matrix.py` checks explicitly supplied evidence for
`APX-COMPAT-001/002` and the v5, v7, and legacy native requirements. It hashes
selected files and compares expected/observed output hashes. It does not parse
proprietary native files, run Apex, verify claimed file origins, or independently
verify behavior descriptions. **It provides no compatibility certification.**
The production ledger remains unchanged and every report has
`audit_status: "incomplete"` and `compatibility_passed: false`.

Run with Python 3.9 or later:

```powershell
python scripts/apex_compatibility_matrix.py manifest.json --root C:/private/apex-evidence
python -m unittest discover -s tests -p test_apex_compatibility_matrix.py
```

The manifest and evidence should remain private where retention/distribution
permissions require it. The command reads inputs without modifying them and
prints deterministic JSON to stdout. It returns 0 for complete supplied evidence,
1 for missing/invalid evidence, and 2 for JSON or file-read errors. Exit 0 means
only that the supplied evidence satisfies this schema; it does not pass an audit.

## Manifest contract (schema_version "1.0")

`editions` must contain separate `Standard` and `Pro` descriptor objects. Each
requires nonempty string `version`, `build`, `settings`, `observed_behavior`, an
`inventory_evidence` file reference, and an explicit `modules` list. An empty list
means that no modules were observed; it must be supported by inventory evidence.
Each module requires `name`, `version`, `observed_behavior`, boolean `enabled`,
and an `evidence` file reference. Missing module inventory is rejected; it is never
inferred from edition names or pricing.

Every file reference has exactly the necessary identity fields:

```json
{"path": "selected/source.native", "sha256": "<64 hexadecimal digits>"}
```

Paths resolve beneath `--root`; absolute paths, drive references, traversal,
missing files, and symlinks resolving outside the evidence root are rejected.
Every supplied hash must match the selected file. Extensions are never used as
proof of a native format.

`fixtures` is a nonempty list with unique string `id` values. It must cover all
five requirement IDs below and both editions:

| Requirement | Required declared native version |
| --- | --- |
| APX-COMPAT-001 | Explicit version string |
| APX-COMPAT-002 | Explicit version string |
| APX-NATIVE-AX5-001 | v5 |
| APX-NATIVE-AX7-001 | v7 |
| APX-NATIVE-LEGACY-001 | Explicit target legacy version string |

Each fixture requires `requirement`, `edition` (`Standard` or `Pro`), an explicit
`modules` list referencing enabled modules in that edition, and nonempty strings
for `provenance`, `permissions`, `native_version`, `operation`, `settings`,
`observed_behavior`, and `loss_report`. Record an explicit no-loss observation
when appropriate; do not omit the loss report. Record the actual consumer and
legacy target version in legacy provenance and behavior evidence. A native file
must be selected for every row; PDFs or synthetic Vertex files cannot
establish native compatibility even if their hashes validate.

Four file references are mandatory: `native_source`, `replacement_project`,
`expected_output`, and `observed_output`. `fields` must contain each of
`geometry`, `curves`, `classifications`, `dimensions`, `labels`, `symbols`,
`imagery`, `metadata`, `calculations`, and `print_export`. Every field requires
nonempty `expected`, `observed`, and `reason` strings, plus `classification` set
to `match`, `mismatch`, `unsupported`, `transformed`, or `not_applicable`.
These classifications are caller-supplied observations, not automatic findings.

## Interpreting the comparison

The report preserves per-field observations, output identities, behavior, and
loss reports. `output_hash_match` compares actual expected and observed bytes;
it does not imply semantic equality. Differing hashes are visible comparisons,
not malformed evidence, so they may coexist with `evidence_complete: true`.
The schema checks for presence and identity, not representativeness, correctness
of classifications, exhaustive capabilities, or native provenance authenticity.
Actual native import/export implementation, field fidelity, consumer round trips,
and independent edition/module audit remain required outside this tool.

Tests use deliberately synthetic bytes solely to exercise schema, containment,
hash comparisons, and fail-closed behavior. They provide no Apex fixture evidence.
