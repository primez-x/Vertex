# Requirements and production gates

This directory is the handoff contract for the new Windows-only application.
It is intentionally independent of Apex source code and of any hosted service.

- [apex-parity.json](apex-parity.json) is the machine-readable requirement
  ledger. It contains the Apex 7 baseline, required compatibility work, both
  workspaces, modern improvements, optional-to-use assisted tools, and
  production quality requirements.
- [production-gates.json](production-gates.json) assigns every ledger ID to a
  named required gate. All gates are mandatory for the one production release;
  they are not alternative release tracks.
- [../production-plan.md](../production-plan.md) explains product intent,
  architecture contracts, implementation order, and the unified acceptance
  rule.

## Status meanings

`evidence_status` describes how firmly the requirement is established:

- `documented` means the requested behavior is stated in the user-approved
  plan or a supplied authoritative source. It does not mean implemented or
  verified.
- `needs_evidence` means implementation cannot be certified until the exact
  edition/module, native file, device, integration protocol, or other external
  fixture is obtained and tested.

Every row starts with `implementation_status: "not_started"`. Partial work is
tracked as `in_progress` with implementation source links; this is not acceptance
evidence. A verified row requires passing acceptance artifacts tied to the current
source and test tree. No requirement may be deleted, silently
renamed, or marked complete because a workaround covers only part of it.

`package` is the internal construction package (1 through 9). It is sequencing
metadata, not a release boundary. In particular, Package 1 discovery,
Package 6 Apex compatibility, and Package 7 assisted workflows remain part of
the same final production gate.

## Row contract

Each requirement has:

- a stable unique `id`;
- an `area` used for ownership and reporting;
- a concrete `requirement` statement;
- `source_urls` when the plan supplied an authoritative URL;
- optional `source_refs` into the typed `source_catalog`; rows without this
  field inherit `default_source_refs: ["task_plan"]`;
- `evidence_status` and `implementation_status`;
- a verifiable `acceptance` statement;
- its internal `package`; and
- `blocker`, which is `null` for requirements fully specified by the plan or
  describes the external evidence required for an open row.

The source catalog distinguishes `task_required` requirements from
`official_apex_evidence` and other official component/platform evidence. A
public URL is supporting evidence for a behavior; it is not proof that the
replacement is implemented or compatible with an installed build.

Requirement IDs are API-like identifiers. Once implementation or test data
refers to one, keep it stable; correct wording in place and record any needed
history in the owning implementation evidence.

## Gate contract

`production-gates.json` contains:

- `gates`: the named gates and their required pass condition;
- `requirement_to_gate`: one mapping for every ledger ID; and
- `release_rule`: the single all-required-gates rule.

The mapping must be total and reference only declared gates. A gate may contain
many requirements, but every requirement has exactly one required gate in this
file. A development checkpoint may report partial progress, but it cannot
declare production completion while any mapped requirement or gate is open.

## Standard-library validation

From the repository root, these commands validate the contract and evaluate the
single production gate:

```powershell
python scripts/requirement_audit.py --contract
python scripts/requirement_audit.py --release
```

The contract command checks only the requirement and gate schema and reports
`Requirement contract: PASS` when that structure is valid. It does not claim
that implementation or production evidence is complete. The `--release`
command evaluates the single production acceptance gate and fails while any
requirement lacks current passing evidence.

CTest runs `requirement_schema_contract` in ordinary matrices. The separate
`production_acceptance_gate` is disabled there; run the `--release` command
explicitly for release qualification. Contract mode does not read acceptance
evidence, and `--contract --json` reports `release_evaluated: false` without
production acceptance fields. `--contract` and `--release` are mutually exclusive.

The root implementation/audit tooling may add stricter checks, fixture links,
license scans, and status evidence. Those checks must preserve the contracts
above, especially semantic authority, typed links, immutable save snapshots,
output fingerprints, offline operation, AppContainer import isolation, and
PlaneGCS rather than Ceres for planar constraints.

Acceptance evidence is bound to a source-tree fingerprint that includes the
requirements and gate contract plus tracked packaging inputs. The evidence file
itself is excluded from that fingerprint so recording a report does not create
a circular hash. Changing a requirement, gate, package allowlist, or source
file therefore invalidates earlier acceptance evidence.

## Local compatibility evidence

On 2026-09-09, a bounded check found no ApexSketch/Medina registration in the
current user's uninstall registry or either machine uninstall registry view,
and no `.ax7`/`.ax5` fixtures in this project outside generated directories.
This does not prove Apex is absent elsewhere on the machine. Exact installed
edition/module behavior and native-file compatibility remain unverified; the
public documentation and our native format tests cannot certify either.
