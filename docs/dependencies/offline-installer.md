# Windows offline bundle

`stage_offline_bundle.py` composes the reviewed runtime inventory, portable
package allowlist, and source-kit manifest into a deterministic directory that
can be carried to an offline Windows machine. The result is an installer
bundle with a PowerShell copy step; it is not a signed MSI, EXE installer, or
production release.

Generate the three inputs from one checkout and one Release build. Every path
is explicit so the staging run cannot fall back to a developer SDK or `PATH`:

```powershell
python scripts/source_kit_manifest.py `
  --source-root . `
  --allowlist packaging/source-kit-allowlist.json `
  --output artifacts/source-kit-manifest.json

python scripts/distribution_inventory.py `
  --root . `
  --runtime-evidence artifacts/runtime/release-imports.json `
  --output artifacts/runtime/distribution-inventory.json

python scripts/stage_offline_bundle.py `
  --source-root . `
  --inventory artifacts/runtime/distribution-inventory.json `
  --allowlist packaging/portable-allowlist.json `
  --source-kit artifacts/source-kit-manifest.json `
  --output-root artifacts/packages `
  --destination property-studio-offline
```

The command validates every source-kit hash and size, delegates runtime file
selection and inventory hash checks to `stage_portable_package.py`, copies all
bytes with a fixed timestamp, writes sorted JSON with stable formatting, and
verifies the complete output before publishing the destination. A destination
that already exists is rejected. The output contains:

| Path | Purpose |
| --- | --- |
| `bin/`, `plugins/`, `assets/`, `licenses/` | Runtime files selected by the portable allowlist and inventory. |
| `source-kit/` | Files named by the source-kit manifest, including their category and hash. |
| `metadata/distribution-inventory.json` | Exact dependency, license, notice, source, and runtime ownership evidence used for staging. |
| `metadata/source-kit-manifest.json` | The source-kit input whose file hashes were checked before copying. |
| `metadata/portable-package-manifest.json` | Lower-level runtime staging record. |
| `runtime-manifest.json` | The subset copied to an installation directory. |
| `offline-bundle-manifest.json` | Bundle file hashes, license inventory, dependency closure, system boundaries, and explicit qualification flags. |
| `install-offline-bundle.ps1` | Offline installer script. |
| `verify-offline-bundle.ps1` | Self-contained PowerShell hash and size verifier. |

Verify the carried bundle before installation:

```powershell
pwsh -NoProfile -NonInteractive `
  -File .\artifacts\packages\property-studio-offline\verify-offline-bundle.ps1 `
  -Root .\artifacts\packages\property-studio-offline
```

Install into a missing or empty directory. The script verifies the bundle,
copies only the files in `runtime-manifest.json`, copies the verifier and
runtime manifest, and verifies the installed bytes again:

```powershell
pwsh -NoProfile -NonInteractive `
  -File .\artifacts\packages\property-studio-offline\install-offline-bundle.ps1 `
  -InstallRoot 'C:\Program Files\Property Studio'

pwsh -NoProfile -NonInteractive `
  -File 'C:\Program Files\Property Studio\verify-offline-bundle.ps1' `
  -Root 'C:\Program Files\Property Studio' `
  -ManifestName runtime-manifest.json
```

The bundle and runtime manifests keep `audit_status: "incomplete"`,
`installer_qualified: false`, and `offline_qualified: false`. Passing staging
or verification proves that the named bytes are present and match their
recorded hashes. It does not prove a signed installer, clean-machine
installation, Windows runtime availability, complete dynamic-load coverage,
network-denied application behavior, license clearance, corresponding-source
completeness, or commercial redistributability. Those remain separate
qualification gates requiring a clean Windows machine and current evidence.
