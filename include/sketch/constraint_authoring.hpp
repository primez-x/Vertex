#pragma once

#include "sketch/constraint_entity.hpp"
#include "sketch/constraints.hpp"
#include "sketch/document.hpp"
#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

class ConstraintAuthoringBuilder;

enum class WallResizeAnchor { start, end };

// Rebase an existing exact-length receipt after a rigid transform. The wall
// must still contain its original baseline; properties are never changed.
// Missing receipts are a no-op. Invalid metadata or a length-changing/curved
// transform throws without mutation, preserving all unknown receipt fields.
void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline);

struct WallResizeIntent {
    std::string wall_id;
    Quantity exact_length;
    WallResizeAnchor anchored_endpoint{WallResizeAnchor::start};
    bool move_connected_walls{true};
};

enum class ConstraintRelationMutationKind { upsert, remove };

struct ConstraintRelationMutation {
    ConstraintRelationMutationKind kind{ConstraintRelationMutationKind::upsert};
    PersistentConstraint constraint;
    std::string constraint_id;

    [[nodiscard]] static ConstraintRelationMutation upsert(PersistentConstraint constraint);
    [[nodiscard]] static ConstraintRelationMutation remove(std::string constraint_id);
};

// relation_anchor and relation_move_connected_walls apply to relation-only
// solves. With no relation anchor, affected geometry is frozen and an upsert
// succeeds only when the current geometry already satisfies the relation.
// Removal-only intents always preserve geometry.
struct ConstraintAuthoringIntent {
    std::optional<WallResizeIntent> wall_resize;
    std::vector<ConstraintRelationMutation> relation_mutations;
    std::optional<WallEndpointBinding> relation_anchor;
    bool relation_move_connected_walls{true};
    std::string message;
};

struct ConstraintWallChange {
    std::string wall_id;
    Segment old_baseline;
    Segment proposed_baseline;
};

// A preview is copyable for dialog ownership but cannot be constructed or
// modified through the public API. candidate_entities() is a read-only display
// and independent geometry-validation surface; Apply never trusts it as commit
// authority and instead recomputes from the retained normalized intent.
class ConstraintAuthoringPreview final {
public:
    ConstraintAuthoringPreview(const ConstraintAuthoringPreview&) = default;
    ConstraintAuthoringPreview& operator=(const ConstraintAuthoringPreview&) = default;
    ConstraintAuthoringPreview(ConstraintAuthoringPreview&&) noexcept = default;
    ConstraintAuthoringPreview& operator=(ConstraintAuthoringPreview&&) noexcept = default;

    [[nodiscard]] bool accepted() const noexcept;
    [[nodiscard]] const std::string& document_id() const noexcept;
    [[nodiscard]] Revision expected_revision() const noexcept;
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept;
    [[nodiscard]] const std::string& candidate_digest() const noexcept;
    [[nodiscard]] const std::vector<ConstraintWallChange>& changed_walls() const noexcept;
    [[nodiscard]] const std::map<std::string, Entity, std::less<>>&
    candidate_entities() const noexcept;
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept;

private:
    ConstraintAuthoringPreview() = default;

    bool accepted_{};
    std::string document_id_;
    Revision expected_revision_{};
    std::string source_snapshot_digest_;
    std::string candidate_digest_;
    std::string shown_result_digest_;
    std::vector<ConstraintWallChange> changed_walls_;
    std::map<std::string, Entity, std::less<>> candidate_entities_;
    std::vector<std::string> diagnostics_;
    ConstraintAuthoringIntent normalized_intent_;

    friend ConstraintAuthoringPreview preview_constraint_authoring(
        const DocumentSnapshot&, const ConstraintAuthoringIntent&);
    friend Revision apply_constraint_authoring(Document&, const ConstraintAuthoringPreview&);
    friend class ConstraintAuthoringBuilder;
};

// Preview is side-effect free. Invalid, contradictory, unsupported, or no-op
// intents return accepted()==false with diagnostics and the original entity map.
[[nodiscard]] ConstraintAuthoringPreview preview_constraint_authoring(
    const DocumentSnapshot& snapshot,
    const ConstraintAuthoringIntent& intent);

// Apply verifies identity, revision, the complete source snapshot digest, and
// the integrity of the shown preview. It recomputes from normalized intent and
// commits the resulting geometry and relation changes as one Document command.
// A rejected, stale, foreign, or modified preview throws without mutation.
Revision apply_constraint_authoring(Document& document,
                                     const ConstraintAuthoringPreview& preview);

}  // namespace sketch
