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
- Debug and Release CTest terminal logs, checked against each build's generated
  `CTestTestfile.cmake`; focused runs, disabled tests, skip-return fixtures,
  missing or duplicate test records, wrong execution directories, and failed
  entries remain partial or blocked evidence rather than a passing matrix
- named independent analytical fixtures for lines, arcs, holes, overlaps,
  winding, tangency, units, area calculations, constraints, and persistence;
  each required family must be present in the generated inventory and pass in
  both Debug and Release
- the Release PE import report and direct application network-import audit
- the tracked source ownership boundary and third-party provenance manifest
- the newest installed-runtime report (from either the historical `current`
  layout or a task-owned smoke run), including clean-machine, network,
  registry-isolation, and production-qualification flags
- offline package manifests and SBOM inputs
- the staged offline bundle's declared payload sizes and SHA-256 values,
  including source-kit, runtime-manifest, and SBOM references
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

The `qa_fixture_coverage` check is deliberately narrower than production
qualification. A pass proves that the named analytical fixture families have
source anchors and passing Debug/Release CTest records. It does not prove Apex
format fidelity, clean-machine behavior, physical print review, or third-party
integration behavior.

Workflow acceptance generation also requires source/build/test provenance.
After finishing all source edits, use the normal compiler environment and the
already configured Debug and Release build trees:

```powershell
python scripts/workflow_test_provenance.py --root .
python scripts/generate_workflow_acceptance_evidence.py --root .
```

The runner reconfigures each tree, performs a clean rebuild, and runs the full
CTest matrix before requiring every named workflow fixture to have passed exactly
once. `--configuration windows-debug` or `windows-release` limits
one invocation; both receipts are required for acceptance generation. Use
`--cmake` and `--ctest` to specify tool executables if they are not on `PATH`.
This run replaces `LastTest.log` with the complete enabled test matrix. It does
not enable the separately disabled production gate or turn skipped host-dependent
fixtures into production evidence.

The runner writes `build/<configuration>/workflow-test-provenance.json` and an
immutable `workflow-test-provenance.log` copy only after successful build and
test commands and structured passing records for
every requested fixture. It binds the unchanged source fingerprint, CMake cache,
generated test inventories, compiled payload hashes, and exact CTest log.
Acceptance generation checks these bindings before and after reading the
immutable log and records the receipt hash. Later CTest inventory queries or
unrelated test runs may replace CTest's working `LastTest.log` without destroying
the receipt. Source edits, changed binaries, replaced provenance logs, missing
receipts, or failed reruns require rebuilding and rerunning; existing logs cannot
be retrospectively stamped as current. No file timestamp is used as proof of
freshness.

These local receipts prevent accidental stale evidence reuse. They are not
signed attestations and assume trusted build tools and an otherwise quiescent
workspace; they do not authenticate operators or protect against deliberate
receipt forgery or changes reverted between fingerprint checks. External
toolchains and dependencies outside the source/build trees remain outside this
local provenance boundary.
