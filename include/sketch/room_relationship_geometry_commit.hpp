#pragma once

#include "sketch/room_relationship_geometry.hpp"

#include <string>
#include <vector>

namespace sketch {

// A side-effect-free, revision-bound relationship edit. The preview stores
// only validated detached changes and digest bindings; it does not retain a
// mutable Document or authorize a later commit against a different snapshot.
class RoomRelationshipGeometryPreview final {
public:
    RoomRelationshipGeometryPreview(const RoomRelationshipGeometryPreview&) = default;
    RoomRelationshipGeometryPreview& operator=(const RoomRelationshipGeometryPreview&) = default;
    RoomRelationshipGeometryPreview(RoomRelationshipGeometryPreview&&) noexcept = default;
    RoomRelationshipGeometryPreview& operator=(RoomRelationshipGeometryPreview&&) noexcept = default;

    [[nodiscard]] bool accepted() const noexcept { return accepted_; }
    [[nodiscard]] const std::string& document_id() const noexcept { return document_id_; }
    [[nodiscard]] Revision expected_revision() const noexcept { return expected_revision_; }
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept {
        return source_snapshot_digest_;
    }
    [[nodiscard]] const std::string& candidate_entity_digest() const noexcept {
        return candidate_entity_digest_;
    }
    [[nodiscard]] const std::vector<RelationshipGeometryChange>& changes() const noexcept {
        return changes_;
    }
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    RoomRelationshipGeometryPreview() = default;

    bool accepted_{};
    std::string document_id_;
    Revision expected_revision_{};
    std::string source_snapshot_digest_;
    std::string candidate_entity_digest_;
    std::vector<RelationshipGeometryChange> changes_;
    std::vector<std::string> diagnostics_;

    friend RoomRelationshipGeometryPreview preview_room_relationship_geometry(
        const DocumentSnapshot&, const RoomRelationshipSnapshot&,
        const std::vector<RelationshipGeometry>&);
    friend Revision apply_room_relationship_geometry(
        Document&, const RoomRelationshipGeometryPreview&);
};

// Decode the model's live references into detached analytical geometry and
// build a revision-bound preview against the edited after snapshot. The
// `edited_after` vector normally starts as the output of the same document
// snapshot and changes only the target geometry the user moved.
[[nodiscard]] RoomRelationshipGeometryPreview preview_room_relationship_geometry(
    const DocumentSnapshot& source,
    const RoomRelationshipSnapshot& relationships,
    const std::vector<RelationshipGeometry>& edited_after);

// Rechecks the full source snapshot digest, rebuilds the entity changes, and
// applies them as one ordinary validated Document command. Stale, foreign,
// rejected, or modified previews throw without mutating the document.
[[nodiscard]] Revision apply_room_relationship_geometry(
    Document& document, const RoomRelationshipGeometryPreview& preview);

} // namespace sketch
