# Typed relationship graph

`TypedRelationshipGraph` is the bounded, GUI-independent provenance component for
CORE-DOC-002. It does not yet integrate with `Document`, revision commands,
project storage, or geometry regeneration. CORE-DOC-002 remains partial.

The caller supplies stable object and relationship IDs; operations never rewrite
them. The object inventory must include every owner, source, and target. Object
IDs and relationship IDs occupy separate namespaces. IDs contain 1–256 valid
UTF-8 bytes. Graphs accept at most 4,096 objects and 8,192 relationship records.

Each link explicitly records `owner_id`, `source_id`, `target_id`, and one of
`wall_derived`, `room_boundary`, or `appraisal_measurement_boundary`. Source points
to target in dependency order. Owner records the responsible object and does not
create a dependency edge. This layer validates references, not the semantic type
or geometric compatibility of their external entities. Callers must validate
those when integrating with the document model.

An independent object has no retained incoming relationship. Each target may
have at most one retained incoming relationship, across all owners and kinds.
This deliberately models single-source provenance; a room assembled from several
walls needs a separately modeled aggregate source. Duplicate relationship IDs,
multiple retained links to one target, and cycles across relationship kinds are
rejected. Constructors apply the same validation as operations, so restoring a
snapshot cannot bypass checks.

`create` accepts a connected link. `freeze` changes a connected link to frozen;
its ownership and dependency remain reserved. The frozen state marks a request to
retain the derived result; this component stores no geometry or captured value.
`disconnect` changes either a connected or frozen link to disconnected, retaining
its IDs and provenance while releasing its target and dependency edge. All
references, including disconnected history, must still exist in the inventory.
Repeated transitions, thaw, reconnect, and transitions on unknown IDs are rejected.

Operations return fully validated value snapshots and never mutate the original.
Keeping the prior graph and restoring it provides copy/undo semantics to a future
command layer; there is no built-in undo stack. Read access is const and copies do
not share writable storage. Failures throw `RelationshipError` with a structured
code, leaving all prior snapshots unchanged.

`serialize()` emits compact JSON with `version: 1`, sorted `object_ids`, and
relationships sorted by stable ID. Kind and state use explicit string names;
the JSON library escapes identifiers. Equal graphs serialize identically
regardless of insertion order. This is a staging format for later persistence,
not a document schema change. JSON deserialization, schema migration, document
transactions, GUI actions, actual frozen geometry, and persisted undo remain open.

The focused test covers creation, copy isolation, freeze/disconnect transitions,
cross-kind ambiguity and cycles, dangling references (including history), invalid
IDs/enums, resource limits, and deterministic escaped serialization.
