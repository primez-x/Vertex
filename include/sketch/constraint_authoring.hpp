#pragma once

#include "sketch/constraint_entity.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraints.hpp"
#include "sketch/document.hpp"
#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <map>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

class ConstraintAuthoringBuilder;

struct PersistentConstraintComponentAnalysis {
    bool supported{};
    int degrees_of_freedom{-1};
    std::size_t point_count{};
    std::vector<std::string> owner_ids;
    std::vector<std::string> constraint_ids;
    std::vector<std::string> redundant_constraint_ids;
    std::vector<std::string> conflicting_constraint_ids;
    std::vector<std::string> diagnostics;
};

// Local freedom of owner endpoint coordinates at fixed signed sweeps under persisted
// relations only. No edit anchors, frozen neighbors, implicit coincidence,
// wall-join union, thickness or height variables participate. Multiple seeds
// allow before/after reports to use the same owner universe after removal.
[[nodiscard]] PersistentConstraintComponentAnalysis analyze_persistent_constraint_component(
    const DocumentSnapshot& snapshot, const std::vector<std::string>& seed_owner_ids);
[[nodiscard]] PersistentConstraintComponentAnalysis analyze_persistent_constraint_component(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::vector<std::string>& seed_owner_ids,
    std::optional<Revision> revision = std::nullopt);

enum class WallResizeAnchor { start, end };

// Rebase an existing exact-length receipt after a rigid transform. The wall
// must still contain its original baseline; properties are never changed.
// Missing receipts are a no-op. Invalid metadata, changed sweep magnitude or length-changing
// transform throws without mutation, preserving all unknown receipt fields.
void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline);

struct WallResizeIntent {
    std::string wall_id;
    Quantity exact_length; // Physical baseline length, including circular arcs.
    WallResizeAnchor anchored_endpoint{WallResizeAnchor::start};
    bool move_connected_walls{true};
};

struct WallGeometryMoveTarget {
    std::string wall_id;
    Vec2 proposed_start;
    Vec2 proposed_end;
};

// Explicit endpoint targets for selected walls. Targets are independent of
// vector order; each selected wall retains its physical length and source
// signed sweep. Explicit relations may move connected owners.
struct WallGeometryMoveIntent {
    std::vector<WallGeometryMoveTarget> targets;
    bool move_connected_walls{true};
};

// Keep the selected boundary's existing anchored/local-chain resize semantics;
// solve only other owners reached through explicit persisted relations.
struct BoundaryResizeIntent {
    BoundaryGeometryEdit edit;
    bool move_related_objects{true};
};

// Move the requested stable vertex while preserving every other selected
// boundary vertex; explicit relations may move other endpoint owners.
struct BoundaryVertexMoveIntent {
    BoundaryGeometryEdit edit;
    bool move_related_objects{true};
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
// Removal-only intents always preserve geometry. Connected movement includes
// identified boundary owners reached through explicit relations. Orientation
// relations use endpoint/chord directions; fixed_length is endpoint distance.
// fixed_arc_length measures one curved segment at its unchanged signed sweep.
struct ConstraintAuthoringIntent {
    std::optional<WallResizeIntent> wall_resize;
    std::optional<WallGeometryMoveIntent> wall_geometry_move;
    std::vector<ConstraintRelationMutation> relation_mutations;
    std::optional<WallEndpointBinding> relation_anchor;
    bool relation_move_connected_walls{true};
    std::string message;
    std::optional<BoundaryResizeIntent> boundary_resize;
    std::optional<BoundaryVertexMoveIntent> boundary_vertex_move;
};

struct ConstraintWallChange {
    std::string wall_id;
    Segment old_baseline;
    Segment proposed_baseline;
};

struct ConstraintBoundaryChange {
    IdentifiedBoundary before;
    IdentifiedBoundary after;
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
    [[nodiscard]] const std::vector<ConstraintBoundaryChange>& changed_boundaries() const noexcept;
    [[nodiscard]] int degrees_of_freedom() const noexcept;
    [[nodiscard]] const std::vector<BoundaryGeometryEdit>& boundary_edits() const noexcept;
    [[nodiscard]] const std::vector<BoundaryGeometryEdit>& exterior_source_edits() const noexcept;
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
    std::vector<ConstraintBoundaryChange> changed_boundaries_;
    int degrees_of_freedom_{-1};
    std::vector<BoundaryGeometryEdit> boundary_edits_;
    std::vector<BoundaryGeometryEdit> exterior_source_edits_;
    std::map<std::string, Entity, std::less<>> candidate_entities_;
    std::vector<std::string> diagnostics_;
    ConstraintAuthoringIntent normalized_intent_;

    friend ConstraintAuthoringPreview preview_constraint_authoring(
        const DocumentSnapshot&, const ConstraintAuthoringIntent&);
    friend Revision apply_constraint_authoring(Document&, const ConstraintAuthoringPreview&);
    friend Command constraint_authoring_verified_command(
        const DocumentSnapshot&, const ConstraintAuthoringPreview&,
        std::optional<DocumentSnapshot>*);
    friend class ConstraintAuthoringBuilder;
};

// Preview is side-effect free. Invalid, contradictory, unsupported, or no-op
// intents return accepted()==false with diagnostics and the original entity map.
[[nodiscard]] ConstraintAuthoringPreview preview_constraint_authoring(
    const DocumentSnapshot& snapshot,
    const ConstraintAuthoringIntent& intent);

// Reproduce an accepted preview through the same guarded typed command as
// Apply, without modifying the source document. The returned snapshot is a
// candidate at source revision + 1, suitable for derived calculations only.
// Rejected, stale, foreign or modified previews throw without mutation.
[[nodiscard]] DocumentSnapshot preview_constraint_authoring_snapshot(
    const DocumentSnapshot& source, const ConstraintAuthoringPreview& preview);

// Apply verifies identity, revision, the complete source snapshot digest, and
// the integrity of the shown preview. It recomputes from normalized intent and
// commits the resulting geometry and relation changes as one Document command.
// A rejected, stale, foreign, or modified preview throws without mutation.
Revision apply_constraint_authoring(Document& document,
                                     const ConstraintAuthoringPreview& preview);

}  // namespace sketch
