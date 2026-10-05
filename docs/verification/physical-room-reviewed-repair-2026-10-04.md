# Reviewed physical-room repair and named area types

Reviewed 2026-10-04. This is an incomplete application checkpoint in the unified
release scope; full Pinc/Apex equivalence and production acceptance remain open.

## User-visible changes

Select a retained physical room in Layers, then use **Tools > Repair room from
walls** or Commands. Click inside its intended current clear space. The review
shows the previous dashed outline, current geometry, hole count and clear area.
**Review and apply** invokes existing explicit reference decisions when needed,
then applies one reversible edit. Cancel leaves the source and revision intact.
Room ID, name, classification and unrelated metadata are retained. Source walls
and independent exterior appraisal GLA are not changed by this command.

A split assigns only the explicitly chosen component. A merge does not acquire
other room metadata; affected other owners remain stale. Automatic correspondence,
atomic multi-room dispositions and physical-room dimensions with snapshot-aware
resolution remain required work. Unsupported dependencies block the operation.

All 22 Pinc named drawing choices now have palette counterparts: four floor
types, gross building area, two basements, garage, detached garage, ADU,
outbuilding, carport, porch, patio, deck, balcony, storage, low ceiling, open to
below, non-calculated, site and clear. New generic measurement projects include
their explicit rules. **Add types** fills missing rules in older measurement
profiles without overwriting existing rules or unrelated profile metadata.

In appraisal mode these named drawing types are descriptive annotations,
separate from the existing fact-derived appraisal categories. They do not move
an area to another floor, change eligibility facts, or qualify/exclude GLA on
their own. Clear removes only the descriptive type. A preset is not ANSI proof.
Equivalent fact-review shortcuts for every Pinc appraisal preset remain open;
named choices alone do not certify complete PINC-012 semantic parity.

## Source and format authority

Strict boundary-edit dialect 7 carries selected wall, strictly interior witness,
reviewed detector lineage and old descriptor digest. Apply and retained-history
restore independently rederive from the preceding entity map. Outer geometry
must match exactly; holes are regenerated, not supplied by callers. Another
current owner's destination is refused. Fresh child IDs and explicit reference
contracts remain enforced. Other source/authoring/classification lanes cannot be
mixed into this repair command. The proof is bounded below 1 MiB.

Retained repair requires native44/extraction42, including entity-only derivation
carriers and deleted history. Reader downgrade tests recompute the logical digest
and still receive an unsupported-format error. See [format details](../project-format.md).

## Observed verification

Root serialized all native jobs while every source writer was frozen. The first
build found a Qt fixture lookup using a class without Q_OBJECT; it was corrected
to the established QWidget lookup and dynamic cast. Independent review found
that unrelated contexts consumed the ownership budget; the context filter was
moved before the charge. A valid document containing 2,049 rooms on another
floor now verifies isolated repair without modifying unrelated entities.

The first focused run also exposed three C++20 fixture lifetime hazards from
temporary snapshots and two desktop observation mismatches (an earlier edited
classification, and a selection-scoped legacy total). Captured snapshots and
the actual persistent property GLA control corrected those fixtures. Failed
logs remain preserved; they are not replaced by the passing evidence.

All 15 distinct affected native checks have passing outcomes across the
applicable runs: 10 unchanged checks from `typed-repair-focused.json`; room
repair, area palette, document commands and storage from
`typed-repair-affected.json`; final desktop interaction from
`typed-repair-desktop-final.json`. Final builds exited 0. The palette cases apply
every measurement type, preserve customized rules, verify that appraisal types
cannot manufacture GLA, clear/Undo and save/reopen. Core repair cases cover
straight/curved/thickened/moved/replaced sources, holes, splits, merges,
conflicts, forged payloads, context/phase, identities, references and storage.
Fourteen Python package-staging cases also pass with native44/future45 bounds.

Root inspected actual repair and palette captures. The independent read-only
advisor approved this bounded checkpoint after the budget correction. Evidence
is retained locally under `artifacts/physical-room-repair-20261004/`. No physical
printer, original Pinc runtime, paired 134-operation qualification, clean-machine
installation or user acceptance follows from these checks.

Implementation commit `197928da8a25823fadd278a8d7b4a1d1c9c6c06b` was pushed
and its exact remote ref verified. All six packaging stages exited 0. The bundle
`artifacts/packages/vertex-offline-20261004-room-repair` contains 4,111 files,
including 2,645 runtime files and 1,459 explicitly allowlisted source-kit files.
The source-kit snapshot is that implementation commit; this subsequent delivery
documentation is separate from the packaged bytes.

Installation at `artifacts/installed/vertex-20261004-room-repair` exited 0.
All six installed source/reopen samples passed for measurement, residential
architecture and light-commercial architecture, with developer dependency paths
removed. Those samples complement the focused desktop repair and palette checks;
they do not individually exercise every new command. The report is retained at
`installed-runtime/run-20261004-220520-048e5c56/report.json` beneath the local
evidence directory above.

The installed executable SHA-256 is
`3238f951265e921232089dc4f184052cde2258690a891a97876f8697e0ae8fba`.
Root verified it matches the Release build, updated the Desktop Vertex shortcut
and read back its exact target. Earlier installations are preserved. No
clean-machine/network-denied installation, physical printer, full compatibility
or user-observed resolution is claimed by these developer-host checks.
