# Vertex source handoff

Vertex is maintained in a public Git repository under GPL-3.0-or-later. The repository is the
authoritative source backup for the Windows application, its build scripts,
project-format documentation, fixtures, and qualification tooling.

## Verify the remote

From a clean Windows checkout:

```powershell
git remote -v
gh repo view --json nameWithOwner,isPrivate,defaultBranchRef,url
git fetch origin
git rev-parse HEAD
git ls-remote origin refs/heads/main
```

The repository must report `isPrivate: true`, and the local commit should match
the `refs/heads/main` hash after fetching. Keep credentials in GitHub or the
local credential manager; never add tokens, private keys, or generated build
state to this checkout.

## Rebuild from source

Use the Windows build instructions in the [README](../README.md): provision the
pinned dependencies, run `scripts/build.ps1 -Desktop`, and use the matching
Debug or Release output for local qualification. The documented `.bldproj`
format and migration rules are in [project-format.md](project-format.md).

The desktop executable, command-line tool, import worker, packages, schemas,
runtime namespaces, and generated files use the Vertex name. Readers may
recognize identifiers written by earlier development builds solely to migrate
those files; new output must always use Vertex identifiers.

## What remains a release gate

The public remote preserves the source history but does not certify the product. The
single production gate still requires current Apex native fixtures, device and
integration observations, clean-machine offline installation, licensing review,
and the qualification runs described in
[production-qualification.md](production-qualification.md).
