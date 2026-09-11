# Room relationship semantic core

`RoomRelationshipSnapshot` is the bounded ARCH-MOD-009 relationship model. A room boundary, an appraisal measurement boundary, and an architectural wall have distinct reference kinds and distinct identities. Coincident geometry never creates a relationship. Absence of a declaration means unspecified, not independent.

Dependencies read as **source follows target** or **source is derived from target**. `follows` has one driver. `derived_from` permits multiple explicit inputs, such as the walls surrounding a room. A source cannot mix these driver modes. Architectural walls may be targets but cannot be driven by this boundary model. Boundary-to-boundary chains are supported.

`independent` is an explicit symmetric declaration between two identities, canonically stored with the smaller ID first. It forbids a dependency path in either direction between those identities. It does not forbid them from sharing a third input, and it does not declare independence from every other reference.

Creation rejects unknown endpoints, invalid enum values, blank/control-containing IDs, duplicate identities, self relations, duplicate pairs, multiple follows drivers, mixed driver kinds, directed cycles, and independence contradicted by direct or indirect dependencies. Failed creation leaves existing snapshots untouched. Snapshot storage owns its input values and provides const access only; building a replacement never mutates a retained snapshot.

JSON schema version 1 contains `references` (`id`, `kind`) and `relations` (`source_id`, `target_id`, `kind`). Arrays are sorted by identity and relation endpoints, independent pairs are normalized, and object fields use deterministic JSON ordering. Parsing requires the exact schema fields and revalidates the graph; unknown fields, types, kinds, and schema versions fail rather than silently dropping intent.

This is a semantic contract, not a geometry solver. References do not prove existence in an authoritative document. The model does not move boundaries, regenerate rooms, compute appraisal area, infer wall faces, persist project entities, apply change propagation, or expose desktop relationship authoring. Those integrations must validate live entity roles and translate explicit relationships into separate reviewed operations.

`room_relationships_tests` covers value ownership, role separation, canonical serialization and strict parsing, valid multi-wall derivation and dependency chains, ambiguous drivers, cycles, and transitive independence contradictions.
