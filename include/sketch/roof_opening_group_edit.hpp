#pragma once

#include "sketch/phase_roof_edit.hpp"

namespace sketch {

struct RoofOpeningGroupMember {
    std::string roof_id;
    std::string opening_id;
};

struct RoofOpeningGroupTransform {
    std::vector<RoofOpeningGroupMember> members;
    Vec2 world_pivot{};
    Vec2 world_translation{};
    double rotation_radians{};
    double uniform_scale{1.0};
};

struct RoofOpeningGroupClone {
    RoofOpeningCloneSource source;
    std::string opening_id;
};

struct RoofOpeningGroupClonePlacement {
    std::vector<RoofOpeningGroupClone> clones;
    std::string destination_roof_id;
    Vec2 destination_anchor_world{};
};

// Reference centres in world XY, using the captured roof base and yaw. The
// anchor is the arithmetic centroid of those centres, independent of size.
// These queries decode source geometry; they do not prove final native fit.
[[nodiscard]] Vec2 roof_opening_group_member_center_world(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupMember& member);
[[nodiscard]] Vec2 roof_opening_group_anchor_world(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<RoofOpeningGroupMember>& members);
[[nodiscard]] Vec2 roof_opening_group_clone_anchor_world(
    const std::vector<RoofOpeningGroupClone>& clones);

// Transform each actual reference centre about the common world pivot, then
// translate. Keep its host, scale physical facet dimensions and add the common
// angle to its physical facet rotation. Rebase dimensions at its final centre.
// Only changed scalars receive exact quantity inputs. Complete actual-map
// replay admits the entire source/final cohort before any intent is returned.
// Identity operations return an empty vector, without schema promotion.
[[nodiscard]] std::vector<RoofEditIntent> prepare_roof_opening_group_transform(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupTransform& request);

// Map the passive source-centre centroid to the destination anchor, retaining
// relative world XY centres, physical facet sizes and source facet angles.
// The existing transfer replay preserves passive row/receipt opaque content,
// admits every fresh identity against actual and passive source data, and
// admits the complete destination once. Retained-history reservation remains
// the caller's responsibility. No new owner pose or dimensions are authored.
[[nodiscard]] RoofEditIntent prepare_roof_opening_group_clone_placement(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupClonePlacement& request);

} // namespace sketch
