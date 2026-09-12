# Packaging inputs

The two JSON files in this directory are the reviewed input lists for a
private Windows handoff:

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
