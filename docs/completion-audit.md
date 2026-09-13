# Completion and release-readiness audit

`scripts/completion_audit.py` produces a deterministic checklist from the
current source tree and captured local artifacts. It is intended to answer
what the repository proves at a checkpoint without treating a green internal
build as a production release.

Run it from the repository root:

```powershell
python scripts/completion_audit.py --root .
python scripts/completion_audit.py --root . --strict
```

The report includes these checks:

- requirement and gate schema, implementation-status counts, and the real
  release-gate result from `scripts/requirement_audit.py`
- required plan, format, qualification, and gate documentation
- CMake/preset configuration and explicit production-gate registration
- Debug and Release CTest terminal logs
- the Release PE import report and direct application network-import audit
- the tracked source ownership boundary and third-party provenance manifest
- the newest installed-runtime report (from either the historical `current`
  layout or a task-owned smoke run), including clean-machine, network,
  registry-isolation, and production-qualification flags
- offline package manifests and SBOM inputs
- Apex/native compatibility, caller, and device evidence status
- the production qualification manifest and its declared runs

Each check is `pass`, `partial`, `blocked`, or `missing`. `production_ready`
is true only when the production gate, runtime boundary, packaging, Apex and
integration evidence, static offline import audit, source provenance audit, and
qualification manifest all pass. The default command
returns zero so it can be used to inspect an incomplete checkpoint; `--strict`
returns `2` while any required production check remains unresolved.

The audit does not authenticate operators, inspect semantic or visual fidelity,
prove that a process actually produced a file, or certify a third-party
integration. Those remain independent acceptance activities recorded in the
qualification and compatibility evidence contracts.
