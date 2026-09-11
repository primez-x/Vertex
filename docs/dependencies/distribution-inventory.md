# Distribution inventory

`third_party/distribution-components.json` is the versioned ownership map for
the current Windows dependency closure. Run the inventory after a Release build
and after provisioning native and Qt dependencies:

```powershell
python scripts/distribution_inventory.py `
  --output artifacts/runtime/distribution-inventory.json
```

The command reads `artifacts/runtime/release-imports.json` and verifies every
recorded module hash against the file on disk. Local imports must resolve to a
recorded module with one manifest owner and one unique package destination;
Windows API contracts and installed system runtimes are retained as explicit
system boundaries. Third-party runtime and header inputs are checked against
their local vcpkg SPDX document and `copyright` file, the Qt SBOM, the pinned
bootstrap manifest, or PlaneGCS `SOURCE.md`. The output uses repository-relative
POSIX paths and records SHA-256 hashes, package/version, source location,
provenance evidence, and notice hashes.

The generator exits nonzero without writing an inventory when ownership is
unknown, evidence is missing or malformed, a recorded hash is stale, a source
or notice file is absent, an SPDX file does not contain the binary, or two
components claim the same destination. Re-run `scripts/inspect-runtime.ps1`
to create fresh PE evidence when a build changes; editing the old hash by hand
is not valid evidence.

The report deliberately carries `audit_status: "incomplete"` and both
qualification flags are false. A passing inventory binds the observed build to
its evidence; it does not establish clean-machine installation, offline
operation, complete dynamic-load coverage, corresponding-source completeness,
license clearance, or commercial redistributability. The Qt SDK SBOM includes
broader installed modules than the current runtime allowlist, so `qtpdf` and
`qtsvg` remain recorded as excluded build-only components until their use and
distribution obligations are separately qualified.

`stage_offline_bundle.py` consumes this report as an immutable input. It
retains the full inventory under `metadata/distribution-inventory.json`,
derives a sorted `license_inventory` and `dependency_closure` summary in the
bundle manifest, and checks every staged runtime byte against this report.
This makes ownership and integrity review portable with the bundle while
keeping it distinct from clean-machine installation and offline application
qualification. The bundle command is documented in
[Windows offline bundle](offline-installer.md).
