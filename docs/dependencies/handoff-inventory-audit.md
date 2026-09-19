# Local handoff inventory audit

`scripts/handoff_inventory_audit.py` checks the local integrity and declared
artifact presence portion of OPS-PACK-002. It reads an already staged offline
bundle and the repository policy in `packaging/handoff-inventory-contract.json`.
It uses only the Python standard library and existing sibling packaging helpers;
run it from the source/build kit or checkout with the `scripts` and `packaging`
directories intact. It needs no network, installed application, Git, or ignored
local build artifacts.

```powershell
python scripts/handoff_inventory_audit.py --bundle-root artifacts/packages/property-studio-offline
```

The command prints a JSON report to stdout and exits 0 for a passing local audit
or 1 for a failed audit. `--manifest` optionally names a different bundle-relative
manifest. Redirect stdout to a report **outside the audited bundle**, otherwise
strict file inventory verification will reject the new unlisted report. The
audit never changes the bundle. Reports have sorted keys and stable artifact
ordering, with no generated timestamps. Identical inputs at the same location
produce identical reports.

The shipped command disables Python bytecode writes before loading its bundled
helper modules, so running it in place does not add `__pycache__` files that
would invalidate the strict bundle inventory.

The contract requires these classes:

| Class | Required evidence |
| --- | --- |
| Offline help | `source-kit/docs/user-guide.html` |
| Keyboard/workflow reference | `source-kit/docs/desktop-workflow.md` |
| Source/build kit | Nonempty `source`, `build`, `docs`, `licenses`, and `fixtures` categories |
| Dependency sources | Source-category file beneath `source-kit/third_party/` |
| Dependency notices | A bundle `notice` record |
| SBOM | An SPDX SBOM reference validated by the existing bundle verifier and a `sbom` record |
| Project format | `source-kit/docs/project-format.md` |
| Adapter documentation | `source-kit/docs/integration-adapters.md` |
| Fixtures | A source-kit fixture-category file |
| Verification reports | `source-kit/docs/requirements/acceptance-evidence.json` |

Artifact matches must have nonzero byte size. These are presence checks, not
semantic reviews of the documents or evidence reports. Category counts alone
do not establish corresponding-source or dependency closure completeness.

The audit reuses `stage_offline_bundle.verify_bundle` with strict file checking.
Every listed file must exist and match its SHA-256 and byte size; unknown files,
duplicate JSON keys, unsafe paths, links/junctions, and runtime-list disagreement
fail closed. Required source-kit, source-inventory, runtime, and SBOM references
must exist. Source-kit schema, category counts, file order, and summaries are
validated by `source_kit_manifest`; each source entry must match its bundle entry
under `source-kit/` in hash, size, category, role, and non-install status. The two
source file sets must agree exactly. Exact filename casing is verified even on
Windows. The bundle manifest itself is the input trust anchor, not a signed or
externally authenticated statement.

`passed: true` means only that this local check passed. The report always keeps
`audit_status: "incomplete"` and false qualification flags for clean-machine
rebuild, legal clearance, and production certification. It does not certify
offline operation, installer behavior, authenticity, semantic report validity,
or a complete handoff. Those qualifications require separate evidence and review.

Fixture checks:

```powershell
python -m unittest discover -s tests -p test_handoff_inventory_audit.py
```

Tests stage temporary bundles with the existing stager, exercise success and
tampering, and require no ignored local artifacts.
