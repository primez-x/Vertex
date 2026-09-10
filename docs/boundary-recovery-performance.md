# Boundary recovery memory investigation

These are internal Windows x64 measurements of the former deep semantic
snapshot history and its compact replacement, not a supported project-size
limit or production benchmark.
The fixture creates an unfinished chain with automatic dimensions, undoes 32
actions, performs an actual JSON encode/parse/decode/restore, then traverses
all undo and redo states while checking exact endpoint bits and stable IDs.
The source session stays alive alongside the restored session.

| Edges | Release private MB after construction | Release private MB after restore | Release total ms | Debug private MB after construction | Debug private MB after restore | Debug total ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 61.9 | 119.0 | 271 | 102.9 | 197.3 | 2,782 |
| 512 | 234.5 | 459.7 | 1,137 | 386.6 | 762.7 | 11,270 |
| 1,024 | 925.9 | 1,829.9 | 6,125 | 1,538.6 | 3,049.8 | 47,611 |

MB denotes decimal bytes. Private bytes are process-wide committed private
memory from `K32GetProcessMemoryInfo`, not an allocation count attributable
only to history. The measurement also records peak working set at construction,
codec, restore and navigation stages. Codec validation itself creates temporary
restored sessions, so its peak can exceed its current memory sample.

Each size runs in a separate hidden process with a 160-second external deadline.
The ordinary CTest fixture remains 256 edges with the existing 60-second limit.
The machine reported approximately 64 GiB of physical RAM and 51 GiB available
before these runs. The numerical results demonstrate quadratic amplification;
they do not establish performance on a minimum supported machine.

The former immutable shared snapshot wrapper removed recursive copying of all earlier
snapshots on each edit. Each individual snapshot still contains deep copies of
all accumulated geometry, receipts, dimensions and identifiers. Therefore the
current scalar replay budget does not provide a meaningful memory guarantee.
A shared byte admission policy still requires implementation, and the compact
history representation requires independent review before recovery integration.
No history truncation or silently unsavable live state is acceptable.

The selected internal repair uses immutable tail-linked chunks of at most 32
values, compact semantic roots, and a past/redo history cursor. An append
copies only its changed chunk tails; unchanged geometry and accepted chains
remain shared. Public values and checkpoint encoding remain unchanged. A
frozen pre-refactor reference corpus checks all eighteen actions at 66 history
positions, including branches, exact geometry bits and checkpoint bytes.
The reference runner passes against both the old and compact engines in Debug
and Release without changing the frozen fixture. Nine focused suites pass in
both configurations. Structural checks cover shared roots, linear chunk
retention, navigation without geometry allocation, concurrent independent
session copies, and iterative teardown. The history teardown regression now
exercises 100,000 actions through full undo, shared-copy release, cancellation,
and past-only reset; it passes in 1.33 seconds Debug and 0.22 seconds Release.

## Compact history measurements

The same workload with the compact engine produced:

| Edges | Release private MB after construction | Release private MB after restore | Release total ms | Debug private MB after construction | Debug private MB after restore | Debug total ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 15.2 | 29.2 | 24 | 20.8 | 41.1 | 167 |
| 512 | 27.8 | 55.5 | 52 | 39.3 | 79.7 | 332 |
| 1,024 | 52.7 | 108.5 | 107 | 75.6 | 156.4 | 674 |
| 2,048 | 103.5 | 215.1 | 211 | 149.4 | 310.6 | 1,377 |

These eight isolated runs all passed exact geometry, identity, codec, and full
history checks. The workload retained exactly three sequence chunks per edge
and one history entry per edge plus the initial anchor. Process memory grows
approximately linearly across the measured sizes. At 1,024 edges, Release
private memory after construction fell from 925.9 MB to 52.7 MB; restore alone
took 16 ms. This is evidence for this workload, not a guarantee for every action
mix, string payload, accepted-chain layout, or hardware configuration.

The compact measurements and source/executable hashes are preserved separately
in `artifacts/reviews/compact-history-scaling-v1/`. The earlier deep-layout
measurements remain in `artifacts/reviews/recovery-scaling/`. Neither evidence
directory should be overwritten. The resource estimator's earlier deep-layout
calibration is historical evidence; the current shared policy uses compact
history accounting and bounded canonical replay.

### Admission-enabled measurements (v24d)

All eight fresh runs in `artifacts/reviews/compact-history-scaling-v2/` pass with
the default shared resource policy and automatic dimensions enabled. The
construction, codec, restoration and complete navigation cycle remains linear
over the tested sizes. Admission validation adds measurable CPU work; these
local observations are not controlled hardware benchmarks.

| Edges | Release private MB after construction | Release private MB after restore | Release total ms | Debug private MB after construction | Debug private MB after restore | Debug total ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 15.2 | 29.3 | 46 | 20.9 | 41.4 | 505 |
| 512 | 27.9 | 55.5 | 94 | 39.7 | 79.9 | 1,011 |
| 1,024 | 52.9 | 108.9 | 187 | 76.3 | 156.2 | 2,016 |
| 2,048 | 104.0 | 215.8 | 380 | 149.3 | 312.1 | 4,175 |

The policy ceilings are accounted-byte limits, not process-memory limits. The
restore measurement includes both original and restored sessions plus test
data. Passing this workload does not establish a universal project-size limit.

Reproduction after the documented Windows desktop build:

```powershell
.\build\windows-release\boundary_recovery_scaling_tests.exe 256
.\build\windows-release\boundary_recovery_scaling_tests.exe 512
.\build\windows-release\boundary_recovery_scaling_tests.exe 1024
.\build\windows-release\boundary_recovery_scaling_tests.exe 2048
```

Repeat using `windows-debug` for Debug. Only these four sizes are accepted.
