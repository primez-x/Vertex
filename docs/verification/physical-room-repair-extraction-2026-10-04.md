# Physical room repair: geometry extraction checkpoint

Reviewed 2026-10-04. This implements the dependency prerequisite for retained
room repair, not the repair command or a production acceptance gate.

## Changes

- Closed-boundary detection and segment assembly moved into a precision-only
  topology target. Existing geometry-operation callers retain their API.
- Physical wall contacts and miters moved into a Document-free network target.
  The document wrapper retains context, phase and elevation admission.
- Analytical physical clear-space subtraction moved into a bounded kernel.
  A shared entity-map source adapter retains source admission and exact v1
  lineage; Snapshot discovery delegates to that adapter.
- Document can now rederive physical spaces from a preceding revision without
  linking its own high-level consumers or using a caller-provided validator.

The extraction preserves physical wall material, virtual room closure across
hosted openings, analytical curves, holes, ordering and resource limits. It adds
no repair authorization and changes neither native nor extraction format floors.

## Observed verification

Root-owned hidden native jobs were serialized after every writer returned
ownership. The first build failed because the face graph also uses segment
assembly. Assembly was moved into the same lower topology module; the corrected
build then exited 0. Both logs are retained rather than replacing the failed run.

The focused run passed 12/12 checks: closed-boundary detection, wall-plan network,
physical clear kernel, measurement area graph, physical wall spaces, physical
wall room, physical wall room desktop, area-class palette desktop, document wall
plan, geometry operations, project storage and project exchange. Pure-module
fixtures cover ordering, stubs, invalid geometry, curves, contact domains,
openings, miters, material holes and mismatched graph/material inputs.

Actual generated linker inputs for all three new standalone test executables
exclude Document. An independent read-only review found no scoped P1/P2 issue
with this extraction. Evidence is retained locally under
`artifacts/physical-room-repair-20261004/`, including
`assembly-corrected-build.json` and `extraction-focused.json`.

## Remaining work and installation boundary

Same-ID reviewed repair, dependency decisions, retained-history validation,
reader floors and desktop preview/apply/cancel remain required. Automatic
one-to-one lifecycle handling and multi-room split/merge dispositions are also
open. This checkpoint does not establish PINC-002 completion or full parity.

The installed shortcut still points to the physical-room creation/stale build
recorded in [the installation evidence](pinc-physical-wall-rooms-2026-10-04.md).
This behavior-preserving source checkpoint has not been packaged or installed.
No Pinc runtime, physical printer, clean-machine installation or user acceptance
claim follows from these checks.
