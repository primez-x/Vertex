# Explicit vertical levels

`VerticalLevelGraph` supplies the core representation for GEO-CON-006 and
ARCH-MOD-007. A level has a stable ID and finite elevation in metres. A
floor-to-floor link explicitly names a lower and upper level. Merely creating
levels never infers links from elevations or proximity. Split-level and branching
graphs are permitted; different levels may share an elevation unless linked.

Construction and edits return validated value snapshots. IDs are sorted, unique
within their level/link namespace, and limited to 256 valid UTF-8 bytes. Graphs
are limited to 4096 levels and 8192 links. Missing references, duplicate retained
endpoint pairs, self-links, directed cycles, nonpositive rises, and overflowing
elevation differences are rejected with typed error codes. Cycle checks are
iterative. Frozen links participate in cycle and monotonicity checks; disconnected
links retain valid endpoint references but impose no graph constraints.

Connected heights follow endpoint elevations. `with_elevation` changes only the
named level, without moving neighbouring levels. Freezing captures the current
height; edits that change that height by more than 1e-9 metres are rejected.
Disconnecting from either connected or frozen state retains the last height and
endpoint provenance, and releases the constraint. Repeated transitions and
reconnecting a historical link are rejected. A new connected link can be created
under a new ID. Constructor imports require retained heights on frozen and
disconnected records and reject retained heights on connected records.

`serialize()` emits deterministic version-1 JSON with metre-qualified fields,
sorted level/link arrays, state, and live or retained height. Signed zero elevations
are normalized. `from_json()` accepts only that exact versioned shape, validates
every field and state, checks finite positive heights, and verifies that encoded
connected heights agree with the endpoint elevations. The `vertical_levels`
Document entity uses this decoder at admission, so invalid level data cannot
enter project history or a saved `.bldproj` file.

The Windows Architectural workspace exposes **Levels and floor-to-floor links**
from the More menu and command palette. The editor creates or edits level IDs
and metre elevations, adds validated connected links, and can freeze or
disconnect an existing link while retaining its measured height. Every action
is a revision-checked Document command, so undo/redo and save/reopen preserve
the graph exactly.

This is foundational persistence coverage, not complete requirement delivery.
Building-object floor/ceiling binding, automatic elevation propagation,
coordinated plan/section/3D views, and file import remain separate work.
Tests cover independent levels, immutable edits, retained heights, malformed
graphs, deterministic output, and a maximum-size chain.
