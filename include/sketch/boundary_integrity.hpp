#pragma once
#include "sketch/document.hpp"

namespace sketch {
[[nodiscard]] std::map<std::string, Entity, std::less<>> transformed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTransformation& transformation);
// Reconstruct an entire entity state from a qualified receipt-backed offset.
// Preserves identities and all unrelated entities; invalid derivations throw.
[[nodiscard]] std::map<std::string, Entity, std::less<>> translated_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTranslation& translation);
// Validate every supported boundary, qualified construction receipt and
// dimension, including stable child references, even when another entity has an unknown version. Unknown
// versions return a document-wide read-only reason.
[[nodiscard]] std::optional<std::string> validate_boundary_integrity(
    const std::map<std::string, Entity, std::less<>>& entities);
// Ordinary commands cannot strip identities. Exact undo/redo restoration is
// separately checked against its retained source revision by Document.
void validate_boundary_transition(
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    bool allow_explicit_relationship_transform = false);
void record_boundary_identities(BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& entities);
void record_boundary_identity_transition(BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after);
void validate_boundary_identity_transition(const BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after);
} // namespace sketch
