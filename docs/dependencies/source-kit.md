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
available for a private handoff and are not silently presented as a successful
clean-checkout rebuild. Bundle-level and installed-runtime verification use
the self-contained `verify-offline-bundle.ps1` script and retain the same
incomplete qualification status.
