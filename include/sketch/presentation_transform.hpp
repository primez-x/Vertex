#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace sketch {

// Child identity is scoped by its annotation owner, never by a joined string.
struct PresentationAnnotationTarget {
    std::string owner_id;
    std::string child_id;
};

struct PresentationAnnotationTransformTarget {
    PresentationAnnotationTarget target;
    PlanarTransform transform;
};

struct PresentationReferenceTransformTarget {
    std::string reference_id;
    PlanarTransform transform;
};

inline constexpr std::size_t maximum_presentation_group_targets = 1000;
// A 4096-entity numerical dependency graph can contain two generated role
// callouts per area even when only its physical roots were explicitly selected.
inline constexpr std::size_t maximum_area_callout_placement_targets = 8192;

// Generated text and quantities stay derived from the closed area owner. The
// position is absolute canonical source-model XY, independent of view frames.
struct AreaCalloutPlacement {
    std::string owner_id;
    std::string role; // area (combined), area_name, area_calculation
    Vec2 position;
    double rotation_radians{};
    bool pin_position{false}; // Persist an explicit offset even at the default anchor.
};

// Changes only placement/rotation of the unique exact owner/role provider.
// Missing nonidentity roles share one fresh annotation-only container. Identity
// intent never creates providers or upgrades legacy annotation states unless an
// explicit position pin is requested.
[[nodiscard]] ApplyEntityChanges area_callout_placement_command(
    const DocumentSnapshot& source, std::span<const AreaCalloutPlacement> placements,
    std::string_view fresh_annotation_owner_id, Revision expected_revision);

// Reconstructs existing annotation owners from the original source, using only
// the caller's captured operators for selected rigid closed boundaries. Final
// geometry supplies independently derived anchors, never annotation authority.
// Authored combined offsets and effective separated placements follow the
// operator; automatic combined placement remains automatic. Missing separated
// counterparts are added only to their existing provider when needed. Returned
// upserts preserve raw sibling/context fields and create no entity identities.
[[nodiscard]] std::vector<EntityChange> transformed_area_callout_entities(
    const std::map<std::string, Entity, std::less<>>& source_entities,
    const std::map<std::string, Entity, std::less<>>& final_geometry_entities,
    const std::map<std::string, PlanarTransform, std::less<>>& owner_transforms);

// Atomic rigid edit of selected label/symbol children and reference owners.
// The caller must qualify one compatible stored position AND orientation frame
// for every target and the shared pivot/offset. Plan/Site projection and frame
// handedness belong to the adapter; model_plan and owner frames are preserved.
// Local symbol/reference X flips are retained; a reflection toggles local Y.
// Text follows the transformed anchor/baseline without persisting mirrored
// glyphs. Dimensions, styles, asset bytes and unselected placements are retained.
// A legacy symbol reflection materializes the required axis-format defaults
// for its owner without changing the unselected symbols' effective artwork.
// All targets are validated even for canonical identity intent, which returns
// no changes or legacy upgrades. Refusal never returns a partial command.
[[nodiscard]] ApplyEntityChanges presentation_group_transform_command(
    const DocumentSnapshot& source,
    std::span<const PresentationAnnotationTarget> annotations,
    std::span<const std::string> reference_ids,
    const PlanarTransform& transform, Revision expected_revision);

// Each transform is already conjugated into its target's stored position and
// baseline frame. Children sharing an owner are aggregated into one upsert even
// when their frames differ. Identity targets are validated but their placement
// is never rewritten; another child's reflection may require owner-wide defaults.
[[nodiscard]] ApplyEntityChanges presentation_group_transform_command(
    const DocumentSnapshot& source,
    std::span<const PresentationAnnotationTransformTarget> annotations,
    std::span<const PresentationReferenceTransformTarget> references,
    Revision expected_revision);

// Detached presentation edits. The caller supplies source-plan anchors and
// rotation with the source frame's handedness already applied.
[[nodiscard]] ApplyEntityChanges annotation_transform_command(
    const DocumentSnapshot& source, std::string_view owner_id,
    std::string_view child_id, double relative_scale, double rotation_radians,
    Revision expected_revision);
[[nodiscard]] ApplyEntityChanges annotation_axis_resize_command(
    const DocumentSnapshot& source, std::string_view owner_id,
    std::string_view child_id, double scale_x, double scale_y,
    Vec2 source_anchor, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges reference_transform_command(
    const DocumentSnapshot& source, std::string_view reference_id,
    double relative_scale, double rotation_radians, Revision expected_revision);

}  // namespace sketch
