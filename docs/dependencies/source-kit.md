# Source/build handoff manifest

`scripts/source_kit_manifest.py` creates a small, reviewable handoff record for
the Windows offline build. It reads an explicit allowlist below an explicit
source root, verifies each selected file, and writes one deterministic JSON
manifest. The command does not search the checkout, a build directory, the
process `PATH`, or an installed SDK for additional inputs.

Create an allowlist such as:

```json
{
  "schema_version": 1,
  "entries": [
    {"category": "source", "path": "include/sketch/document.hpp"},
    {"category": "build", "path": "build/windows-release/vertex.exe"},
    {"category": "docs", "path": "docs/production-plan.md"},
    {"category": "licenses", "path": "LICENSE"},
    {"category": "fixtures", "path": "tests/fixtures/example.json"}
  ]
}
```

Each entry needs one category and one repository-relative `path`. The allowed
categories are `source`, `build`, `docs`, `licenses`, and `fixtures`. An entry
may include an optional `sha256` value (64 hexadecimal digits) and optional
`size` value. Supplied values are checked against the bytes under the source
root; a stale value fails the command. Paths are normalized to POSIX separators
in the output, and duplicate paths are rejected case-insensitively for Windows
compatibility.

Generate the manifest with all three operational paths explicit:

```powershell
python scripts/source_kit_manifest.py `
  --source-root . `
  --allowlist packaging/source-kit-allowlist.json `
  --output artifacts/source-kit-manifest.json
```

The generator rejects absolute or traversal paths in the allowlist, symlinks
and junctions in the selected path, missing or non-file inputs, unknown
categories, duplicate entries, invalid hashes, and stale optional hashes or
sizes. It validates every input before creating or replacing the output
manifest. The manifest contains only relative `files` entries with their
category, SHA-256, and byte `size`; no developer-machine root is serialized.
Files are sorted by canonical path and JSON keys use stable sorted formatting,
so changing allowlist order does not change the output bytes.

The output always carries `audit_status: "incomplete"`. It is an inventory of
the files named by the allowlist, not proof that the list is complete. It does
not claim a complete source kit, licensing review, SBOM, or reproducible
Windows rebuild. Clean-machine offline build tests, dependency closure,
license obligations, and rebuild evidence remain separate qualification work.

Tracked CMake inputs, `vcpkg.json`, and `scripts/build.ps1` use the `build`
category. Keeping those inputs distinct lets the handoff inventory fail when a
source kit contains source code but omits the files needed to configure and
build it.

To carry the checked files with the runtime and dependency evidence, compose
an offline bundle after generating this manifest. The source-kit manifest is
validated again, copied under `source-kit/`, and retained under `metadata/`:

```powershell
python scripts/stage_offline_bundle.py `
  --source-root . `
  --inventory artifacts/runtime/distribution-inventory.json `
  --allowlist packaging/portable-allowlist.json `
  --source-kit artifacts/source-kit-manifest.json `
  --output-root artifacts/packages `
  --destination vertex-offline
```

This creates an installer bundle and a separate `runtime-manifest.json`. The
PowerShell installer copies the runtime subset only; source-kit files remain
available for a reproducible public-source build and are not silently presented as a successful
clean-checkout rebuild. Bundle-level and installed-runtime verification use
the self-contained `verify-offline-bundle.ps1` script and retain the same
incomplete qualification status.

## Historical IFC SDK sources

The IFC candidate uses a separate static OCCT 7.8.1 SDK and Boost 1.86/Eigen
3.3.9 SDK. Their historical vcpkg recipes differ from the manager checkout's
current ports. `scripts/stage_ifc_sdk_sources.py` stages the exact installed
recipe and source inputs rather than copying current ports or binary caches:

```powershell
.deps/cad-runtime/3.13.15/python.exe -I -B scripts/stage_ifc_sdk_sources.py `
  --workspace . --output C:/Build/Vertex/ifc-sdk-sources
.deps/cad-runtime/3.13.15/python.exe -I -B tests/test_stage_ifc_sdk_sources.py
```

Use a fresh external destination with an existing parent. The command reads the
two explicit SDK statuses and their SPDX/ABI/resource receipts, recovers regular
Git blobs from each recorded recipe tree, and verifies the exact recipe file set
and hashes. It matches source archives by SHA-512 in the explicit local downloads
cache. No archive is extracted, source executed, dependency downloaded, or
existing output overwritten. Notices, original receipts, manifests, triplet,
helper, recipes, and source archives travel with a relative-path manifest.

The actual source handoff for candidate 787 contains 91 package instances,
91 historical recipe trees, 85 source archives, and 682 payload files. The two
staged SDK status hashes match that successful IFC build's recorded inputs.
The final stager passed 12 regression methods, including forbidden workspace
Git execution, reserved Windows names, metadata-only tampering, and incomplete
helper-payload rejection. The recipe-only Boost.Uninstall helper uses the exact
pinned manager's MIT notice with an explicit origin, rather than a fabricated
installed copyright file.

All inputs are validated before reserving the destination. Changes during copy
reject the result and retain the partial output. The manifest is published last;
`verify(output)` checks its portable inventory and internal bindings. It is not a
signature or independent licensing conclusion. The vcpkg manager is identified
but its full source is not included in this SDK payload; compiler/Windows SDK
redistribution and a clean offline dependency rebuild remain separate work.
The complete application's corresponding-source and production qualification
flags therefore remain false. Compose the final offline source handoff only
after adding those remaining inputs and recording a clean rebuild.

## Composed dependency inputs

`scripts/qualification/compose_dependency_source_kit.py` copies the explicitly
declared archive, recipe, source-directory and notice inputs from a checked
source-closure report. Source directories include the selected Boost and Eigen
headers, not only native libraries. Stable traversal rejects links, ambiguous
Windows names, oversized trees and changes during reading or copying. Tree
identities use the same `Vertex-source-tree-v2` framing as the inventory producer,
IFC materializer and offline-bundle verifier.

The limits distinguish physical copies from metadata replay: 20,000 unique
dependency payload files, 32,768 frozen bundle receipts and 65,536 replay records.
Repeated references do not consume another physical-copy slot. The frozen audit
maps delivered SDK source directories through their checked dependency manifest;
it does not substitute writable developer headers for source inputs carried in
that bundle. Source, redistribution and offline-rebuild qualification remain
separate from a successful copy or audit.
