# Vertex source handoff

Vertex is maintained in a private Git repository. The repository is the
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

The current executable and package paths retain the internal
`property-studio` compatibility IDs while the user-facing product is Vertex.
Those IDs are part of the current packaging and test contracts and should only
change with an explicit format/package migration.

## What remains a release gate

The private remote protects the source but does not certify the product. The
single production gate still requires current Apex native fixtures, device and
integration observations, clean-machine offline installation, licensing review,
and the qualification runs described in
[production-qualification.md](production-qualification.md).
