# Packaging inputs

The two JSON files in this directory are the reviewed input lists for a
reproducible Windows source and runtime package:

- `source-kit-allowlist.json` names the tracked source, build metadata,
  documentation, fixtures, and license files that can be copied into a source
  kit. It is generated from `git ls-files` so the list is explicit and does not
  search a developer checkout during staging.
- `portable-allowlist.json` names the runtime asset and notice files that may
  accompany the binaries selected by the distribution inventory. Runtime DLLs
  are still selected only by `third_party/distribution-components.json` and a
  freshly generated inventory.

Both staging commands remain fail-closed. A missing dependency, stale hash,
or incomplete runtime inventory blocks package creation; these files do not
claim installer, licensing, or clean-machine qualification by themselves.

Refresh the source-kit list from the Git index after adding or removing tracked
files:

```powershell
python scripts/update_source_kit_allowlist.py `
  --root . `
  --output packaging/source-kit-allowlist.json
```

Repository checks can verify the committed list without writing it:

```powershell
python scripts/update_source_kit_allowlist.py `
  --root . `
  --output packaging/source-kit-allowlist.json `
  --check
```

The check requires every tracked path in the declared source-kit scope and
also verifies the actual filename spelling and deterministic category. It
reads only `git ls-files`; untracked files such as local `temp.txt` are not
inputs. Build trees, `.deps`, generated output, and secret-like local files
are excluded by policy. Qt and SDK trees remain dependency-inventory inputs,
with their existing licensing and provenance records kept separate from this
source list.

The allowlist is an explicit source handoff inventory. Its completeness check
covers tracked source paths only; it does not assert that test vectors,
dependency archives, runtime DLL closure, license review, or clean-machine
offline installation have been qualified. The manifest and staging tools
continue to consume the reviewed allowlist and never expand it by globbing.
