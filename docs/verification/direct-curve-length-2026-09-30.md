# Direct curved-wall length editing

The wall editor now offers **Change curve length** in both workspaces. Its
prefill and proposed measurement are physical arc length. An exact new entry
preserves the source's signed sweep and chord direction, with either endpoint
anchored. Connected geometry follows only the explicit movement policy; fixed
relationships, frozen geometry and invalid hosted openings reject atomically.
This direct edit does not add a permanent length constraint.

The implementation separates this input from older curved endpoint proofs:
wall proof 3 requires an exact physical entry inside command 5. The wall keeps
receipt version 2 inside its existing version-1 constraint-authoring section.
Document restoration independently validates known receipts. Retained proofs
and known imported physical receipts require project format 13 / exchange 10.
Older formats and proof encodings retain their previous meanings.

## Scope and ownership

Core authoring and receipt implementation, native editor implementation, and
root-owned command/storage/exchange integration were separate ownership areas.
The root integrated the changes and inspected the resulting interfaces, tests,
documentation and rendered editor before delivery. An independent read-only
review challenged compatibility and measurement integrity.

## Technical evidence

- Baseline Release authoring regression failed in 0.10 seconds with the existing
  straight-wall-only resize diagnostic.
- Release authoring passed in 0.68 seconds. Cases include both anchors,
  positive/negative minor and major sweeps, measured construction provenance,
  joined geometry, frozen and physical-lock conflicts, repeated exact entries,
  opaque nested metadata, rigid transforms, receipt invalidation and replay.
- Document commands passed in 0.14 seconds; project storage in 4.38 seconds;
  exchange in 0.35 seconds. These check strict proof/envelope versions,
  independent restoration, imported receipts, undo/reopen/redo, retained floors,
  recovery-aware archives, and matching chord forgery with recomputed digests.
- Architectural adapter passed in 0.84 seconds. Existing malformed known
  receipt fixtures now assert rejection at document admission and atomic upsert,
  rather than expecting the malformed state to survive until a transform.
- Native constraint dialog passed in 4.51 seconds. Tests enter the real editor
  in both workspaces, resize 5 to 6 to 7 metres with both anchors, check no new
  constraint, exercise connected boundaries and physical opening stations,
  and verify cancellation, conflicts and undo.
- Project CLI passed in 1.44 seconds; requirement schema contract in 0.13 seconds.
- Root inspected actual native captures under
  `artifacts/direct-curve-length-20260930/native`, including both anchored
  previews and themed Measurement/Architectural workspace editor instances.

The first integration run found two existing tests that deliberately admitted
malformed known receipts. Independent restore validation now refuses those
states earlier; the tests were corrected to assert that stronger boundary.
Validation was not weakened to accept stale measurements.

The final rebuild of `vertex`, `desktop_smoke`, authoring and native dialog
completed successfully. Its affected authoring/dialog/schema checks passed in
0.57 / 3.33 / 0.10 seconds. Independent source review approved this bounded
checkpoint after resolving the receipt-only reflection restriction; the listed
production and numerical-domain limits remain open.

## Remaining boundaries

Numeric curves without original measured construction provenance can reflect
after a direct length input; the physical receipt rebases its signed baseline
while retaining exact arc length. Original measured curve construction
provenance still does not support reflection. Such a transform rejects without
changing the document. Scaling a wall with an exact entry remains unsupported
instead of rewriting measurement evidence. Full numerical-domain, pen/touch,
performance, clean-machine/offline, Apex compatibility and production
qualification are not established by these focused checks.

Installed-runtime evidence will be recorded after the corresponding bundle is
staged and exercised. This document is an internal implementation checkpoint,
not certification of the full production replacement.
