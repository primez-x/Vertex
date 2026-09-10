# Identified analytical boundary entities

The strict codec recognizes `boundary`, `measurement_boundary` and
`room_boundary`. An identified entity has `properties.boundary_model_version: 1`
and a canonical `properties.segments` array. Each segment contains:

```json
{
  "segment_id": "edge-1",
  "start_vertex_id": "vertex-1",
  "end_vertex_id": "vertex-2",
  "start": [0.0, 0.0],
  "end": [4.0, 0.0],
  "sweep_radians": 0.0
}
```

Coordinates are metres and sweep is radians; nonzero sweeps remain analytical
circular arcs. Coordinates are the sole geometry authority. IDs use the same
1–128 byte ASCII identifier vocabulary as Document and are independent of
coordinates. Segment IDs are unique within a boundary. Adjacent edges must
share the same vertex ID and exactly equal endpoint coordinates, including the
last-to-first join. A simple cycle cannot revisit a vertex identity. Existing
analytical geometry validation rejects invalid, open, degenerate, intersecting
or overlapping cycles. The decoder never drops malformed entries.

`decode_identified_boundary_entity` accepts only the known entity version.
Version inspection distinguishes anonymous legacy geometry, identified version
1, and unsupported positive integer versions. Malformed known versions reject;
an unknown version preserves its opaque geometry and makes Document read-only,
including when it occurs in retained history.

`encode_identified_boundary_entity(model, original)` retains the original entity
identity, required flag, extensions and unrelated properties. Unknown segment
metadata follows stable segment IDs, not array positions. Unchanged numeric JSON
values retain their original representation. Typed reversal reverses edge order,
swaps endpoint identities/coordinates and negates the sweep while retaining IDs.
The entity reversal helper also applies metadata guards.

The codec does not interpret receipt or dependent-reference semantics yet.
Changes affecting reserved receipt, dimension, constraint, source-reference or
direction fields reject until the corresponding service can handle them. A
geometry-preserving round trip retains those fields. This gate does not replace
the future document-level validation of dimension and source-edge references.

## Legacy upgrade and storage compatibility

`upgrade_legacy_boundary_entity` requires exactly one legacy geometry array:
`segments` or `boundary`. It retains all segment JSON values, adds supplied or
fresh random IDs in topology order, and writes the canonical `segments` field.
The entity ID and unrelated metadata remain intact. It rejects ambiguous dual
arrays, partial preexisting identities and tolerance-only joins; coordinate
repair is a separate explicit action. The helper returns a candidate and does
not mutate Document.

Unversioned legacy fields do not acquire new meanings merely because a vendor
used names such as `segment_id`. They remain opaque and readable. An explicit
upgrade rejects such name collisions instead of overwriting them; the document
transition check separately rejects stripping the version from an already
identified entity.

Every ordinary legacy-to-v1 transition is checked against the exact upgrade
helper result using the candidate's supplied IDs. Raw commands therefore cannot
reinterpret colliding vendor fields by flipping the version marker, overwrite
partial collisions, or combine an upgrade with geometry/type/metadata changes.
An unrelated edit must be a separate operation after the explicit upgrade.

The ledger also observes recognized anonymous legacy boundary IDs. Legacy-only
delete/recreate and retyping histories remain readable, editable and eligible
for storage v1. Ordinary reuse after absence or a type/subtype change marks
that legacy identity ambiguous for future identification. This marker remains
through abandoned branches and exact navigation. Identification must then use
a fresh entity ID. An unambiguous direct upgrade still uses the same ID, and
deletion followed only by exact undo does not make the identity ambiguous.
This closes both direct delete-to-identified reuse and the two-step path of
recreating anonymous geometry before upgrading it.

The same known parent-lineage checks apply to unsupported future boundary
versions. A continuous same-subtype legacy-to-future transition is preserved
read-only without interpreting opaque children; an ambiguous or absent legacy
ID requires rekeying even for a future version. Legacy boundary-to-dimension
conversion also requires a fresh ID. This policy does not impose universal
nonreuse on unrelated generic IDs before any legacy/protected observation.

Ordinary document upserts cannot strip identified semantics or change an
identified boundary's type. Undo may restore the exact anonymous state from
before an upgrade; redo restores the exact identified state. Restore verifies
those navigation records against their source revisions. The geometry itself
must pass state validation in every supported historical revision.

Document derives a lifetime identity ledger from all retained history and caches
it for ordinary edits. Deleting an identified boundary or dimension retires its
entity ID; only exact source-linked undo/redo may resurrect it. Segment and
vertex IDs are reserved per owning boundary, including IDs on abandoned undo
branches. An anonymous pre-upgrade state restored by undo may pass unchanged
through an unrelated edit. Editing it requires a fresh valid upgrade, with
fresh child IDs; abandoned upgrade IDs remain reserved.

A surviving segment retains its ordered endpoint vertex identities. Moving
the coordinates is permitted. Changing those endpoint identities requires
retiring the segment and allocating a fresh ID. The sole reversal exception is
the exact full typed boundary reversal, including reversed edge order, swapped
endpoints, negated sweeps and preserved metadata. Ordered pairs protect even
two-edge lenses where unordered pairs would be ambiguous. Split/merge retires
all affected segments and atomically remaps dependent dimensions; existing
unaffected vertices may survive. These state checks do not prove every
coordinated vertex/segment permutation or operation intent. Typed topology
commands and durable lineage receipts remain a production requirement.

Exact navigation compares canonical serialized JSON for entity properties,
extensions and asset metadata. It distinguishes integer/floating representation
and signed zero, while accepting signed/unsigned in-memory integers that
serialize identically. Original JSON lexical spellings are not a storage promise.

Any identified boundary anywhere in retained history requires storage format
v2, even if the current head is anonymous or the boundary was deleted. Both
SQLite `user_version` and `metadata.format_version` declare the version; the
logical digest includes it. Legacy-only history can still use v1. A v1 file
containing identified history is nonconforming and rejects. Older v16 binaries
reject format v2 rather than opening it editable and stripping IDs. This changes
the version guard, not the SQLite table structure or the coordinates.

The session state machine, placed dimensions, exact construction receipts,
source-edge provenance, reference-aware split/merge, explicit upgrade UI and
production migration qualification remain required. This codec and its focused
tests are an internal foundation, not complete drawing-workflow parity.
