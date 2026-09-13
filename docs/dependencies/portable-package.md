# Portable package staging

`stage_portable_package.py` creates a reviewable portable file set from a
generated distribution inventory. It requires an explicit source root and an
allowlist; it never searches a build directory, the process `PATH`, an
installed SDK, or another developer-machine location for missing inputs.

Generate the inventory after a Release build, then stage a package with:

```powershell
python scripts/stage_portable_package.py `
  --inventory artifacts/runtime/distribution-inventory.json `
  --allowlist packaging/portable-allowlist.json `
  --source-root . `
  --output-root artifacts/packages `
  --destination property-studio-portable
```

The allowlist is versioned JSON with `schema_version: 1` and an `entries`
array. Every entry names one inventory component, one repository-relative
source file, and one package-relative destination. The entry kind is one of
`asset`, `source`, or `notice`:

```json
{
  "schema_version": 1,
  "entries": [
    {
      "kind": "asset",
      "inventory_entry": "inter-font",
      "path": "assets/fonts/Inter.ttf",
      "destination": "assets/fonts/Inter.ttf"
    },
    {
      "kind": "notice",
      "inventory_entry": "inter-font",
      "path": "assets/fonts/OFL.txt",
      "destination": "licenses/Inter-OFL.txt"
    }
  ]
}
```

Runtime binaries are copied from the inventory's `binaries` records. An
allowlist `source` or `asset` entry must match a `source_inputs` record for
the named inventory component, and a `notice` entry must match its `notices`
record. The stager verifies every input hash from the inventory before it
writes anything. It rejects absolute or traversal paths, duplicate
case-insensitive package destinations, missing files, symlinks, excluded
components, stale hashes, and allowlist references that are absent from the
inventory. The output package directory is always resolved below
`--output-root`.

The generated `portable-package-manifest.json` records only relative source
and destination paths, a SHA-256 for every copied file, the SHA-256 of the
source inventory, and `audit_status: "incomplete"`. Its installer and offline
qualification flags remain false. A successful staging run is an integrity
and file-selection record; it does not qualify an installer, clean-machine
installation, complete dynamic-load coverage, licensing, or offline
operation. The package also carries `metadata/distribution-sbom.spdx.json`, a
deterministic SPDX 2.3 document generated from the same inventory. The
manifest's `sbom` reference binds its format and hash to the copied document;
the SBOM records packages, notices, source inputs, runtime checksums, and
runtime package dependencies without claiming legal clearance.

For the Windows handoff, use the higher-level
[`stage_offline_bundle.py`](offline-installer.md) command. It consumes this
portable file set, adds the checked source-kit payload and dependency/license
metadata, emits a runtime manifest, and includes a self-contained PowerShell
verifier and copy installer. The generated bundle remains an integrity record
with all qualification flags false.
