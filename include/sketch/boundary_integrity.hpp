#pragma once
#include "sketch/document.hpp"
#include <set>

namespace sketch {
[[nodiscard]] std::map<std::string, Entity, std::less<>> transformed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTransformation& transformation);
// Reconstruct all owners/dimensions from the same source revision. Plain
// identified owners retain their topology origin as explicit derivation.
[[nodiscard]] std::map<std::string, Entity, std::less<>> transformed_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryTransformation>& transformations);
// Reconstruct an entire entity state from a qualified receipt-backed offset.
// Preserves identities and all unrelated entities; invalid derivations throw.
[[nodiscard]] std::map<std::string, Entity, std::less<>> translated_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTranslation& translation);
// Deterministically applies a stable-ID coordinate edit. Receipt-backed input
// is retained as immutable derivation evidence and the edit list is replayed
// during integrity validation and project-history restoration.
[[nodiscard]] std::map<std::string, Entity, std::less<>> edited_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit);
// Pure reconstruction within an already complete room-review assignment.
// Only the dedicated intent replayer may publish this candidate. Other reviewed
// retained owners may temporarily occupy destinations while all assignments
// are rebuilt; ordinary single-owner commands keep the exclusive-owner guard.
[[nodiscard]] std::map<std::string, Entity, std::less<>> edited_boundary_entities_for_room_review(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit, const std::set<std::string>& reviewed_owners);
// Applies solved vertex positions together when sequential intermediate geometry
// would be invalid. Existing sequential derivations remain byte-compatible.
[[nodiscard]] std::map<std::string, Entity, std::less<>> edited_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits);
// Retained history only: after stable derivation fails to reproduce a source
// replacement, accept a complete exact original-v1 kernel result. Live edits
// continue to use edited_boundary_entities[_batch]. No history is rewritten.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replayed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replayed_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits);
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
    const std::map<std::string, Entity, std::less<>>& after,
    const BoundaryGeometryEdit* typed_edit = nullptr);
} // namespace sketch
