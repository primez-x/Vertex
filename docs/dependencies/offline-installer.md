# Windows offline bundle

`stage_offline_bundle.py` composes the reviewed runtime inventory, portable
package allowlist, and source-kit manifest into a deterministic directory that
can be carried to an offline Windows machine. The result is an installer
bundle with a PowerShell copy step. The script supports explicit `Install`,
`Repair`, and `Uninstall` actions, but the bundle is still not a signed MSI,
EXE installer, or production release.

Prepare the app-local runtime from the licensed SDK's x64 CRT directory using
`scripts/prepare_msvc_runtime.py --crt-dir <SDK-CRT-directory> --version 14.44.35211.0
--notice-file <Redist.txt> --notice-file <ThirdPartyNotices.txt>
--output .deps/msvc-runtime/14.44.35211.0`. This is a developer packaging step;
it does not install anything globally. The script validates x64 DLL headers,
copies only five named runtime DLLs, and records portable hashes and provenance.
The component catalog independently pins their reviewed hashes. Do not copy
DLLs from System32 or use debug runtimes. See Microsoft's
[deployment methods](https://learn.microsoft.com/en-us/cpp/windows/choosing-a-deployment-method?view=msvc-170)
and [redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution).
Local notice files are evidence inputs, not a licensing-clearance certificate;
the current Redist.txt is a link stub and licensing qualification remains open.

Run `scripts/prepare_cad_distribution.py` after the Release build and CAD runtime
preparation, then `scripts/inspect-runtime.ps1 -CadPayloadManifest
artifacts/runtime/cad-payload.json`. The payload generator reconstructs the SDK
from the locked offline archive cache and verifies every selected staged file.
Its generated component and portable allowlists augment the reviewed baseline;
use both generated inputs below to include the actual CAD runtime.
The offline bundler rejects an inventory that leaves a Visual C++ runtime DLL
classified as an installed Windows dependency. `bin/qt.conf` is also shipped
so Qt plugins resolve inside the installation without developer environment
variables.

Generate the three inputs from one checkout and one Release build. Every path
is explicit so the staging run cannot fall back to a developer SDK or `PATH`:

```powershell
python -B scripts/prepare_cad_distribution.py
& scripts/inspect-runtime.ps1 -CadPayloadManifest artifacts/runtime/cad-payload.json

python scripts/source_kit_manifest.py `
  --source-root . `
  --allowlist packaging/source-kit-allowlist.json `
  --output artifacts/source-kit-manifest.json

python scripts/distribution_inventory.py `
  --root . `
  --manifest artifacts/runtime/cad-components.json `
  --runtime-evidence artifacts/runtime/release-imports.json `
  --output artifacts/runtime/distribution-inventory.json

python scripts/stage_offline_bundle.py `
  --source-root . `
  --inventory artifacts/runtime/distribution-inventory.json `
  --allowlist artifacts/runtime/cad-allowlist.json `
  --source-kit artifacts/source-kit-manifest.json `
  --output-root artifacts/packages `
  --destination vertex-offline
```

The command validates every source-kit hash and size, delegates runtime file
selection and inventory hash checks to `stage_portable_package.py`, copies all
bytes with a fixed timestamp, writes sorted JSON with stable formatting, and
verifies the complete output before publishing the destination. A destination
that already exists is rejected. The output contains:

| Path | Purpose |
| --- | --- |
| `bin/`, `plugins/`, `assets/`, `help/`, `licenses/` | Runtime files, local assistance assets, offline user guide, and notices selected by the portable allowlist and inventory. |
| `source-kit/` | Files named by the source-kit manifest, including their category and hash. |
| `metadata/distribution-inventory.json` | Exact dependency, license, notice, source, and runtime ownership evidence used for staging. |
| `metadata/distribution-sbom.spdx.json` | SPDX 2.3 package/file/dependency record derived from the same inventory. |
| `metadata/source-kit-manifest.json` | The source-kit input whose file hashes were checked before copying. |
| `metadata/portable-package-manifest.json` | Lower-level runtime staging record. |
| `runtime-manifest.json` | The subset copied to an installation directory. |
| `offline-bundle-manifest.json` | Bundle file hashes, license inventory, dependency closure, system boundaries, and explicit qualification flags. |
| `install-offline-bundle.ps1` | Offline installer script with `Install`, `Repair`, and `Uninstall` actions. |
| `verify-offline-bundle.ps1` | Self-contained PowerShell hash and size verifier. |

Verify the carried bundle before installation:

```powershell
pwsh -NoProfile -NonInteractive `
  -File .\artifacts\packages\vertex-offline\verify-offline-bundle.ps1 `
  -Root .\artifacts\packages\vertex-offline
```

Install into a missing or empty directory. The script verifies the bundle,
builds the complete runtime tree in a sibling staging directory, verifies that
staged tree, and publishes it with a directory rename. If an empty destination
already exists, it is moved aside until the published tree passes its second
verification and is restored automatically if publication fails. The installer
copies only the files in `runtime-manifest.json`, plus the verifier and runtime
manifest, and verifies the installed bytes again:

```powershell
pwsh -NoProfile -NonInteractive `
  -File .\artifacts\packages\vertex-offline\install-offline-bundle.ps1 `
  -InstallRoot 'C:\Program Files\Vertex'

pwsh -NoProfile -NonInteractive `
  -File 'C:\Program Files\Vertex\verify-offline-bundle.ps1' `
  -Root 'C:\Program Files\Vertex' `
  -ManifestName runtime-manifest.json
```

After publication verification, the installer protects every manifest-owned
file and directory under `bin/` and `plugins/` with explicit, noninheriting
permissions. The installing user and the specific `Vertex.ImportWorker`
AppContainer profile receive read/execute access; SYSTEM and Administrators
retain full control. This includes the embedded CAD runtime. The broker still
checks file creation, subdirectory creation, and deletion separately for each
module root; a sharing conflict does not prove immutability. A noninheriting
deny of `FILE_DELETE_CHILD` for the installing user on the installation root
closes the parent-permission route to deleting `bin/` or `plugins/`. Other root
ACL entries and the directory outside the installation remain unchanged.
Known-file cleanup grants explicit deletion access on its declared objects.
The installer does not relax the broker's check. Run Vertex normally rather than with
an elevated administrator token when exercising the sandboxed import worker.

Repair and uninstall restore deletion access only to declared objects, after
ownership checks. Module hard links and reparse points are rejected before ACL
changes; each native ACL write also validates its opened object. Failed repair
that restores a partially removed backup protects its restored module paths
again. If an unknown backup object has inherited permissions that cannot be
preserved safely during refreezing, repair reports failure and retains both the
verified protected publication and the backup, with their paths in the error.
It does not replace the active installation with a writable partial backup.
Ownership checks immediately before directory ACL changes reject
observed late content; native writes validate the opened objects. These guards
address installer lifecycle and late-content preservation; they do not
establish immunity to an adversarial same-user
namespace race or prevent the file owner from changing permissions later.

Repair and uninstall require the installed runtime manifest to match the
carried bundle byte-for-byte. Retain the original bundle: these actions are
for that exact package, not an upgrade or a migration from another version.
Both actions refuse unowned files or directories before changing the target.
Move user projects, settings, and other added content outside the installation
directory first; runtime verification alone deliberately permits added files
and therefore cannot authorize whole-directory removal.
Deletion is limited to manifest-owned files and then-empty directories. A file
that appears after validation is preserved: uninstall leaves the containing
directory and reports failure, while repair restores the original runtime or
retains the backup for recovery instead of recursively deleting it.

Repair requires an existing destination containing the expected runtime
manifest and verifier, then replaces the payload through the same staged,
verified publication path. It can restore damaged payload and verifier bytes.
Uninstall requires a matching verifier and a passing installed-byte check
before removing the directory. That check executes the verified bundle's
verifier, including staging and post-publication checks, never the potentially
damaged installed script. An unmarked,
unowned, or tampered target is left untouched on rejection.
Both operations reject bundle-child paths, reparse-point chains, and unsafe
destinations, and neither action contacts a network service:

```powershell
pwsh -NoProfile -NonInteractive `
  -File .\artifacts\packages\vertex-offline\install-offline-bundle.ps1 `
  -InstallRoot 'C:\Program Files\Vertex' `
  -Action Repair

pwsh -NoProfile -NonInteractive `
  -File .\artifacts\packages\vertex-offline\install-offline-bundle.ps1 `
  -InstallRoot 'C:\Program Files\Vertex' `
  -Action Uninstall
```

Run a hidden installed-runtime smoke check after the installed-byte verifier:

```powershell
python scripts/test_installed_runtime.py `
  --install-root 'C:\Program Files\Vertex' `
  --evidence-root artifacts/installed-runtime
```

This launches both workspaces with private application-data directories, only
Windows directories on the child PATH, and developer Qt/QML settings removed.
Each workspace runs twice: the first process saves a source `.bldproj`, and the
second opens that file and saves a reopened copy. The evidence directory
therefore contains both project artifacts alongside the workspace captures and
native 3D view. Each source process also generates an encoded PNG and imports it
through the application's normal isolated reference decoder before saving. The
second process reopens the saved reference resources without importing another
source image. Per-run `reference_import` evidence records whether decoding was
requested and passed; each project pair separately records source decoder
acceptance and persisted reopen with matching stable project hashes. A decoder
failure, failed reopen, or changed stable project output fails the smoke check.
This generated PNG check does not cover PDF or arbitrary raster compatibility,
independently attest worker controls, or qualify production isolation.
The harness samples loaded module paths and checks observed
packaged modules against the installed manifest and hashes.
The five CRT DLLs and Qt Windows platform plugin must be observed inside the
installation. Each run writes a new evidence directory, including failure
reports. Sampling cannot establish complete dynamic-load coverage; screenshot
headers and dimensions are checked automatically, with visual review separate.
The check does not disable networking or isolate the Windows registry.

The bundle and runtime manifests keep `audit_status: "incomplete"`,
`installer_qualified: false`, and `offline_qualified: false`. Passing staging
or verification proves that the named bytes are present and match their
recorded hashes; the local repair/uninstall tests prove only the guarded
development-host behavior described above. It does not prove a signed installer, clean-machine
installation, Windows runtime availability, complete dynamic-load coverage,
network-denied application behavior, license clearance, corresponding-source
completeness, or commercial redistributability. Those remain separate
qualification gates requiring a clean Windows machine and current evidence.
