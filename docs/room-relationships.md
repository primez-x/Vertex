# Room relationship semantic core

## Relationships during a physical room split or merge

The detached room review now lets the user choose **Remove** or **Retarget** for
each relationship incident to a room being retired. Retarget shows the actual
retained/new rooms assigned in that review. Affected endpoints require explicit
choices; unaffected endpoints stay fixed and the relationship kind stays the
same. Changing room assignments clears choices that have become invalid.
Membership retirement is acknowledged separately from the incident relation.

Apply validates the complete candidate graph and commits the wall/room edit,
dimensions and relationship changes together. Cancel publishes nothing. The
review cannot redirect to an unclassified space, unrelated room, retiring room
or decorative symbol. Duplicate relations, self relations, inconsistent roles,
cycles and conflicting drivers refuse the complete edit. Existing graph schema
and unknown metadata remain preserved. This coordinated operation differs from
the standalone relationship editor, which edits declarations separately.

Explicit replacements use room-review intent version five, native reader 162
and JSON/assets extraction 160. Earlier intents retain their contracts. This is
source implementation; desktop interaction, replay and reopening have not been
run for this increment, and production acceptance remains open.

`RoomRelationshipSnapshot` is the bounded ARCH-MOD-009 relationship model. A room boundary, an appraisal measurement boundary, and an architectural wall have distinct reference kinds and distinct identities. Coincident geometry never creates a relationship. Absence of a declaration means unspecified, not independent.

Dependencies read as **source follows target** or **source is derived from target**. `follows` has one driver. `derived_from` permits multiple explicit inputs, such as the walls surrounding a room. A source cannot mix these driver modes. Architectural walls may be targets but cannot be driven by this boundary model. Boundary-to-boundary chains are supported.

`independent` is an explicit symmetric declaration between two identities, canonically stored with the smaller ID first. It forbids a dependency path in either direction between those identities. It does not forbid them from sharing a third input, and it does not declare independence from every other reference.

Creation rejects unknown endpoints, invalid enum values, blank/control-containing IDs, duplicate identities, self relations, duplicate pairs, multiple follows drivers, mixed driver kinds, directed cycles, and independence contradicted by direct or indirect dependencies. Failed creation leaves existing snapshots untouched. Snapshot storage owns its input values and provides const access only; building a replacement never mutates a retained snapshot.

JSON schema version 1 contains `references` (`id`, `kind`) and `relations` (`source_id`, `target_id`, `kind`). Ordinary references retain that exact representation. Schema version 2 additionally permits an architectural reference's `wall_members`: an ordered list of 2 through 256 unique physical wall IDs, starting with the logical reference's `id`. Every member uses its native baseline direction; the joined path must be continuous, open and nonintersecting. A physical member cannot also belong to a different logical reference. Splitting any member inserts its second piece immediately after the retained member, preserving all declarations. Sync retains logical membership and suppresses separate child aliases. Once a record uses schema two, relationship editing and Sync retain that version.

Arrays are sorted by identity and relation endpoints, independent pairs are normalized, and object fields use deterministic JSON ordering. Known versions require exact fields, types and graph semantics. Document admission validates the union of all supported records, including member existence, architectural role, wall semantics and path geometry. Identical shared definitions are allowed; conflicting definitions, aliases, cycles and driver contradictions are rejected. Positive future versions preserve their entire opaque payload and make the document read-only; malformed version discriminators reject. The standalone known-model decoder never interprets future payloads.

This is a semantic contract, not an implicit geometry mutation. References do not prove existence in an authoritative document. The Windows workspace persists the model as a typed `room_relationships` entity and exposes a relationship editor from the More menu and command palette. The editor lists live room boundaries, measurement boundaries, and walls, keeps source/target roles explicit, synchronizes newly created references, and applies each declaration through normal validated Document history. **Retarget selected** replaces one declared target in a single revision after validating the complete replacement graph. Missing endpoints, no-op edits, wrong relation kinds, and cycles are rejected; the referenced geometry remains untouched until a separate propagation preview is confirmed. Retargets remain ordinary undoable edits and are revision-checked by the desktop host.

`propose_room_relationship_geometry` is the reviewed geometry boundary for linked edits. It accepts detached before/after geometry snapshots and returns a deterministic proposal that a caller can preview, confirm, and commit through the normal document command path. `follows` propagates one driver's rigid rotation and translation; `derived_from` requires every changed driver to imply the same transform, so a partial move or disagreement fails closed. Dependency chains resolve target first, while independent references never move. Scale, deformation, reordering, reflection, missing records, invalid boundaries, and a source that was edited to a conflicting position produce diagnostics and no change for that source. The proposal preserves the source segment order and leaves stable entity, edge, and vertex identities to the committing document adapter; the function itself never mutates either input snapshot.

The Document boundary has a revision-bound preview/apply adapter. It decodes the live room and measurement entities, resolves wall members from the authoritative model, preserves stable edge/vertex identities, moves dependent dimension anchors, carries construction receipts through a schema-three transform frame, checks the complete source digest, and publishes dependent changes as one undoable command. This core helper intentionally commits dependents only. The Windows editor separately composes the complete authored driver and dependent move through the ordinary typed transform adapters before showing its exact candidate. Logical wall roots expand to every physical member. The saved candidate is rebuilt against the revision fence before publishing one operation. Ordinary raw edits cannot rewrite construction receipts. Production cross-view and connected-solve qualification remain separate acceptance work.

`room_relationships_tests` covers value ownership, role separation, canonical serialization and strict parsing, valid multi-wall derivation and dependency chains, controlled dependency and independent retargeting, replacement-target validation, ambiguous drivers, cycles, and transitive independence contradictions. `room_relationship_geometry_tests` covers target-first rigid translation and rotation, chained propagation, multi-driver agreement, conflicting drivers, unsafe scale, missing geometry, and explicit independence. The Windows desktop smoke workflow covers the revision-bound retarget dialog, geometry preservation, undo/redo, and propagation from the new target.
