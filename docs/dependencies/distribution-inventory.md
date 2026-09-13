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
bootstrap manifest, or PlaneGCS `SOURCE.md`. This includes the PROJ runtime DLL
and its explicitly staged `proj.db` and `proj.ini` resources. The output uses
repository-relative POSIX paths and records SHA-256 hashes, package/version, source location,
provenance evidence, and notice hashes.

The pinned Qt 6.8.3 SBOM currently carries stale SHA-1 values for the shipped
Qt binaries. The Qt component entries record each exception explicitly under
`source.hash_overrides`: the replacement digest, the pinned archive and its
SHA-256, the archive member, the repository-local 7-Zip tool, and a reason.
Inventory generation hashes the archive itself, extracts the named member
offline, and requires the member, installed binary, and replacement digest to
agree. An override cannot name an unknown SBOM file, omit its provenance, or
bypass verification of any other checksum supplied by the SBOM.

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
license clearance, or commercial redistributability. The runtime manifest
explicitly names the Qt PDF and SVG modules used by the application; their
license and source obligations remain part of the separate distribution review.

`stage_offline_bundle.py` consumes this report as an immutable input. It
retains the full inventory under `metadata/distribution-inventory.json`,
derives a sorted `license_inventory` and `dependency_closure` summary in the
bundle manifest, and checks every staged runtime byte against this report.
It also emits `metadata/distribution-sbom.spdx.json`, a deterministic SPDX 2.3
document containing the reviewed component packages, source/notice files,
SHA-256 checksums, and package relationships. The same SBOM is present in the
direct portable package so either handoff artifact carries the dependency
record needed for review. The exporter is implemented in
`scripts/distribution_sbom.py` and does not contact a package registry.
This makes ownership and integrity review portable with the bundle while
keeping it distinct from clean-machine installation and offline application
qualification. The bundle command is documented in
[Windows offline bundle](offline-installer.md).
