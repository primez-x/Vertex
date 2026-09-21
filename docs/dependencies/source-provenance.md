# Source provenance boundary

The public GPL-3.0-or-later application source and the third-party records needed for
distribution review are kept in separate ownership classes. The checked-in
policy is [`third_party/source-provenance.json`](../../third_party/source-provenance.json):

- `first_party` covers the application, tests, build metadata, documentation,
  fixtures, and project-authored notices;
- `third_party_provenance` covers dependency manifests, notices, and other
  upstream provenance records;
- `external_excluded` covers build trees, dependency caches, generated files,
  and local artifacts that must never enter a source handoff.

Run the structural audit from the repository root:

```powershell
python scripts/source_provenance_audit.py --root .
```

The audit reads the cached Git path set, normalizes Windows separators, rejects
absolute and traversal paths, checks case-insensitive overlaps, verifies the
declared third-party manifests exist, and requires every tracked path to have
exactly one ownership class. It is also registered as the
`packaging_source_provenance_audit` CTest and is included in the completion
audit.

A passing result proves only structural separation of the current tracked
checkout. It does not establish contributor copyright or assignment, legal
title, upstream license clearance, corresponding-source completeness, or
commercial redistributability. Those questions remain explicit review gates in
the dependency inventory, SBOM, and production qualification records. The
manifest is therefore a source-ownership control, not a legal opinion.

The source-kit allowlist and manifest are separate handoff controls. The
allowlist identifies the files intended for a reproducible source/build kit, while this
audit checks the ownership boundary for the complete tracked checkout. Both
must pass before a source package is considered reviewable.
