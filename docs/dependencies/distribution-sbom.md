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

Each package's `sourceInfo` retains the complete inventory `package.source`
object as sorted JSON text. This carries exact upstream revisions, source
locations, declared archive hashes, evidence hashes, and directory digests
without guessing missing revisions. Individual `upstream_files`, file-valued
`source_paths`, and the hashed provenance/SPDX/bootstrap records also become
package-member SPDX files. Directory-valued `source_paths` remain in `sourceInfo`
and are not added as file-byte hashes. The same rule applies to component and
static-input `source_inputs`, so aggregate Boost, Eigen, or other tree digests
remain provenance context instead of being mislabeled as SPDX files.
Upstream candidates in a
vendor SPDX document remain provenance and are not automatically classified
as shipped packages.

For example, the PlaneGCS inventory already verifies FreeCAD commit
`145529fe741292ff0b3977a01195bf0247425794`, its 13 upstream file hashes, and
`third_party/planegcs/SOURCE.md`. The exporter preserves those records alongside
the DLL instead of reducing that package to its release label and notice.
This is preservation of previously recorded evidence; the exporter does not
rehash the source checkout or establish that the recorded inventory is current.

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
