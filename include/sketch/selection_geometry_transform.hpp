#pragma once

#include "sketch/document.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/project_organization.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace sketch {

inline constexpr std::size_t maximum_selection_geometry_transform_entities = 4096;

// Only explicitly selected geometric roots confer rigid placement authority.
// Each operator is captured in that source root's saved plan frame. Dependency
// operators are inherited from actual source lineage, never supplied by callers.
struct SelectionGeometryTransformRoot {
    std::string root_id;
    PlanarTransform transform;
};
struct SelectionGeometryTransformRequest {
    Revision expected_revision{};
    std::vector<SelectionGeometryTransformRoot> roots;
    std::string message;
};

// Required explicit review of an affected retained physical-room context/plane.
// These are discovery inputs, not approved correspondence or disposition.
struct SelectionGeometryRoomReviewRequirement {
    DrawingContext context;
    double effective_elevation_m{};
    std::vector<std::string> retained_room_ids;
    std::vector<std::string> source_wall_ids;
    // Selected room carriers retain their inherited source operators for
    // source-derived area-callout completion after explicit repaired geometry.
    std::vector<RigidOwnerTransformation> room_transformations;
    // Source-derived owned callout displacement for explicitly selected room
    // carriers (including selected deduction descendants). These must enter
    // the room review's existing selected_dimension_placements typed lane.
    std::vector<PhysicalWallRoomDimensionPlacement> dimension_placements;
};

class SelectionGeometryTransformPreparation final {
public:
    [[nodiscard]] const SelectionGeometryTransformRequest& request() const noexcept;
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept;
    [[nodiscard]] const std::vector<std::string>& dependency_ids() const noexcept;
    [[nodiscard]] const Command& geometry_command() const noexcept;
    [[nodiscard]] const DocumentSnapshot& geometry_snapshot() const noexcept;
    [[nodiscard]] const std::vector<SelectionGeometryRoomReviewRequirement>&
    required_room_reviews() const noexcept;
    // Whether the detached geometry stage changes; false does not discharge
    // required room review or its source-derived callout placements.
    [[nodiscard]] bool makes_change() const noexcept;

private:
    SelectionGeometryTransformPreparation(SelectionGeometryTransformRequest request,
        std::string digest, std::vector<std::string> dependencies, Command command,
        DocumentSnapshot candidate,
        std::vector<SelectionGeometryRoomReviewRequirement> room_reviews, bool changed);

    SelectionGeometryTransformRequest request_;
    std::string source_digest_;
    std::vector<std::string> dependency_ids_;
    Command command_;
    DocumentSnapshot candidate_;
    std::vector<SelectionGeometryRoomReviewRequirement> room_reviews_;
    bool changed_{};

    friend SelectionGeometryTransformPreparation prepare_selection_geometry_transform(
        const DocumentSnapshot&, const SelectionGeometryTransformRequest&);
};

// Closed nested v1 codec: version, expected_revision, roots, message; roots are
// {root_id, transform}, with exactly the existing rigid XY operator fields.
// The codec validates shape, IDs, finite operators and budgets, not selection.
[[nodiscard]] nlohmann::json encode_selection_geometry_transform_request(
    const SelectionGeometryTransformRequest& request);
[[nodiscard]] SelectionGeometryTransformRequest decode_selection_geometry_transform_request(
    const nlohmann::json& request);

// Pure original-snapshot preparation and replay. Authenticates identity requests
// as well as changes, closes deduction/physical/measured sources, and delegates
// the complete hard-connected solve and callouts to per-owner rigid completion.
// Corner-window leg callouts retain their source placement metadata under the
// same complete operator only when both actual hosts are explicit selections.
// Raw supplements, candidate maps and incidental rendered owner IDs are absent.
// Current physical-room roots/descendants select their actual source walls;
// their retained room geometry remains unchanged until explicit repair review.
// geometry_command()/geometry_snapshot() are detached review inputs: publication
// MUST incorporate every required_room_reviews() decision atomically. This API
// permits an unchanged geometry stage with required callout review directly
// against the captured original source; absence of geometry is not approval.
// does not invent approvals and intentionally exposes no final apply method.
[[nodiscard]] SelectionGeometryTransformPreparation prepare_selection_geometry_transform(
    const DocumentSnapshot& source, const SelectionGeometryTransformRequest& request);

// Revalidates the exact full captured source, then rederives the request. This
// cannot accept a changed head with the same document ID and revision.
[[nodiscard]] SelectionGeometryTransformPreparation replay_selection_geometry_transform(
    const DocumentSnapshot& source, const SelectionGeometryTransformPreparation& prepared);

} // namespace sketch
