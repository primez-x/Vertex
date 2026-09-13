# Distribution SBOM

`scripts/distribution_sbom.py` converts one generated
`distribution-inventory.json` into a deterministic SPDX 2.3 JSON document.
It is a standard-library-only, offline step:

```powershell
python scripts/distribution_sbom.py `
  --inventory artifacts/runtime/distribution-inventory.json `
  --output artifacts/runtime/distribution-sbom.spdx.json
```

The document contains one SPDX package for every inventory component, one
hashed SPDX file for each runtime binary, source input, or notice, and
`DESCRIBES`, `CONTAINS`, and observed cross-package `DEPENDS_ON` relationships.
The document namespace is derived from the inventory SHA-256, and its JSON
ordering is stable for stable inventory bytes. The exporter validates IDs,
relative paths, package metadata, relationship endpoints, and SHA-256 values
before writing.

Portable staging writes the SBOM at
`metadata/distribution-sbom.spdx.json` and binds it from
`portable-package-manifest.json`. Offline bundle staging carries that same
document and binds it from `offline-bundle-manifest.json`; it is retained as
bundle metadata and is not required for the installed runtime to launch.

The SBOM is an evidence and handoff artifact. `audit_status` remains
`incomplete`, and the document does not prove complete dynamic-load coverage,
license clearance, corresponding-source completeness, commercial
redistributability, or production qualification. Those require the separate
component, legal, clean-machine, and acceptance gates.
