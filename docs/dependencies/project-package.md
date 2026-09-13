# Portable project packages

`stage_project_package.py` creates a local transfer package for one saved
`.bldproj`. The project database remains the file the application opens. The
package adds a copy of every stored revision asset under a content-addressed
`assets/<sha256>.bin` path and writes `project-package-manifest.json` with the
document identity, format version, revision range, asset references, and
integrity hashes.

The command reads the project through SQLite's read-only URI and checks the
database integrity, required metadata, revision head, asset metadata, and
asset bytes before publishing. It stages into a private directory and publishes
only after all payloads and the manifest have been written. Existing package
destinations are never replaced.

From the repository root:

```powershell
python scripts/stage_project_package.py `
  --project projects/sample.bldproj `
  --source-root . `
  --output-root artifacts/project-packages `
  --destination sample
```

Optional templates, profiles, and documentation are selected by an explicit
resource manifest. It is version 1 JSON with an `entries` array; each entry has
`kind` (`template`, `profile`, or `documentation`), a repository-relative
`path`, and a package-relative `destination`. An optional `sha256` is checked
against the current source. The manifest itself is copied to
`metadata/project-resources.json` so the package records the exact resource
selection used to build it:

```json
{
  "schema_version": 1,
  "entries": [
    {
      "kind": "template",
      "path": "templates/residential.json",
      "destination": "templates/residential.json"
    },
    {
      "kind": "profile",
      "path": "profiles/imperial.json",
      "destination": "profiles/imperial.json"
    },
    {
      "kind": "documentation",
      "path": "docs/project-format.md",
      "destination": "documentation/project-format.md"
    }
  ]
}
```

Use the Python API to verify a copied package before opening it on another
offline Windows machine:

```python
from scripts.stage_project_package import verify_package
verify_package(r"C:\Transfers\sample")
```

To materialize a verified transfer into a new local project directory, use the
copy-only restore helper. It keeps the source package unchanged, refuses an
existing destination, re-hashes every payload after copying, and returns the
relative project/resource paths a local consumer can open or register:

```python
from scripts.stage_project_package import restore_project_package

result = restore_project_package(
    r"C:\Transfers\sample",
    r"C:\Projects\Imported",
    "sample",
)
# Open C:\Projects\Imported\sample\project\sample.bldproj
print(result["project_path"], result["resource_paths"])
```

Verification rejects modified or missing files, unsafe paths, symlinked
payloads, unlisted files, inconsistent project, asset, or resource records, and
packages that claim offline or production qualification. The package is an
offline ownership and transfer artifact; it does not certify Apex compatibility,
installer behavior, or cross-machine output equivalence. Those remain part of
the unified production acceptance gate.
