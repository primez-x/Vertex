#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Internal preview/commit binding, not a persisted project-format digest.
// Includes identity, complete retained history and assets (actual bytes as
// well as declared hashes), navigation, names, saved and read-only state.
// Equal document ID/revision alone is insufficient for command authority.
[[nodiscard]] std::string document_snapshot_digest(const DocumentSnapshot& snapshot);

// Version-one persisted authoring-source binding. Retains document identity,
// head, complete history/assets/navigation and names, but excludes save
// bookkeeping and derived editability. This never replaces the stronger
// full snapshot guard used when applying a prepared command.
[[nodiscard]] std::string document_authoring_source_digest_v1(const DocumentSnapshot& snapshot);

// Historical baseline binding: hashes only the retained prefix through revision
// and names introduced in that prefix. The enclosing archive must independently
// validate the complete Document history; this digest is not a history validator.
[[nodiscard]] std::string document_authoring_source_digest_v1_at_revision(
    const DocumentSnapshot& snapshot, Revision revision);

// Binds every entity field in the displayed candidate, including opaque
// metadata. This digest does not replace semantic Document validation.
[[nodiscard]] std::string entity_map_digest(
    const std::map<std::string, Entity, std::less<>>& entities);

} // namespace sketch
