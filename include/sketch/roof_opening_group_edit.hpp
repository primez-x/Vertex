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
    Vec2 axis_scale{1.0, 1.0};
    double axis_rotation_radians{};
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

// In world XY, A = R(axis_rotation) diag(axis_scale) R(-axis_rotation).
// Transform each actual reference centre about the common pivot by
// uniform_scale * R(rotation) * A, then translate. Keep each rectangular mouth
// and its host; scale each physical facet dimension by uniform_scale times
// |A p| / |p| for its actual projected world width/depth direction p. Preserve
// its physical facet angle except for the common rotation, and rebase physical
// dimensions using the final centre's facet scales. This is a parametric
// rectangular resize; the mouth itself does not undergo an affine shear.
// Only changed scalars receive exact quantity inputs. Complete actual-map
// replay admits the entire source/final cohort before any intent is returned.
// Identity operations return an empty vector, without schema promotion.
[[nodiscard]] std::vector<RoofEditIntent> prepare_roof_opening_group_transform(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupTransform& request);

// Removal-only actual-source authority. Every named member must be a real
// profiled child of its named roof; containing roofs are never selected roots.
// Complete replay admits the whole roster before returning any removal intent.
[[nodiscard]] std::vector<RoofEditIntent> prepare_roof_opening_group_removal(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<RoofOpeningGroupMember>& members);

// Map the passive source-centre centroid to the destination anchor, retaining
// relative world XY centres, physical facet sizes and source facet angles.
// The existing transfer replay preserves passive row/receipt opaque content,
// admits every fresh identity against actual and passive source data, and
// admits the complete destination once. Retained-history reservation remains
// the caller's responsibility. No new owner pose or dimensions are authored.
// A destination absent from actual may bind only an explicitly supplied,
// already prepared fresh roof. The caller authenticates these exact roofs to
// its ordinary producer and jointly reserves their owner/carried-child IDs.
// Fresh replay validates the complete physical roof without manufacturing an
// actual map. Its returned intent is preparation authority only: replay against
// the exact fresh roof and replace that addition, never submit it as an edit to
// an original actual owner. Multiple groups on one host require combined replay.
[[nodiscard]] RoofEditIntent prepare_roof_opening_group_clone_placement(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupClonePlacement& request,
    const std::map<std::string, Entity, std::less<>>& fresh_roofs = {});

} // namespace sketch
