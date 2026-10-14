#pragma once

#include "sketch/phase_roof_edit.hpp"

#include <span>

namespace sketch {

struct RoofOpeningGroupMember {
    std::string roof_id;
    std::string opening_id;
};

struct RoofOpeningDirectionalResizeRequest {
    RoofOpeningGroupMember target;
    double sx{1.0};
    double sy{1.0};
    Vec2 anchor_m{};
    double frame_rotation_radians{0.0};
};

struct PreparedRoofOpeningDirectionalResizes {
    std::vector<RoofEditIntent> intents;
    std::map<std::string, Entity, std::less<>> candidate_entities;
};

// Derive every selected existing child from the original map, combine one
// openings leaf per host, then admit the complete cohort once. Unit requests
// retain their authenticated upserts and source schema without host authority.
[[nodiscard]] PreparedRoofOpeningDirectionalResizes prepare_roof_opening_directional_resizes(
    const std::map<std::string, Entity, std::less<>>& actual,
    std::span<const RoofOpeningDirectionalResizeRequest> requests);

struct RoofOpeningGroupTransform {
    std::vector<RoofOpeningGroupMember> members;
    Vec2 world_pivot{};
    Vec2 world_translation{};
    double rotation_radians{};
    double uniform_scale{1.0};
    Vec2 axis_scale{1.0, 1.0};
    double axis_rotation_radians{};
    bool flip_horizontal{false};
    bool flip_vertical{false};
    bool retain_invariant_members{false};
};

struct RoofOpeningGroupProperties {
    std::vector<RoofOpeningGroupMember> members;
    // Mouth dimensions are physical lengths on each actual roof facet;
    // construction dimensions use the existing fixed skylight profile.
    std::optional<RoofOpeningQuantityInput> width;
    std::optional<RoofOpeningQuantityInput> depth;
    std::optional<RoofOpeningQuantityInput> frame_width;
    std::optional<RoofOpeningQuantityInput> curb_height;
    std::optional<RoofOpeningQuantityInput> glazing_thickness;
    std::optional<double> rotation_radians;
    bool retain_invariant_members{false};
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
// uniform_scale * F * R(rotation) * A, then translate, where F negates world X
// for flip_horizontal and world Y for flip_vertical. Keep each rectangular mouth
// and its host; scale each physical facet dimension by uniform_scale times
// |A p| / |p| for its actual projected world width/depth direction p. Preserve
// its physical facet angle except for the common rotation when F is identity.
// Reflection derives both actual projected axes through F * R(rotation) and
// the final facet metric. It requires an isometry of those physical axes;
// a nonrectangular or physically stretched mirror refuses the whole cohort.
// The supported fixed profile is symmetric, so reversing an axis carries no
// handing state; equivalent half-turns retain the closest source angle.
// Rebase physical dimensions using the final centre's facet scales. This is a parametric
// rectangular resize; the mouth itself does not undergo an affine shear.
// Only changed scalars receive exact quantity inputs. Complete actual-map
// replay admits the entire source/final cohort before any intent is returned.
// Identity operations return an empty vector, without schema promotion, unless
// retain_invariant_members is explicitly requested. That preparation mode keeps
// admitted unchanged-child upserts for complete mixed-roster proof; it grants no
// publication or host authority and still preserves exact unchanged roof data.
[[nodiscard]] std::vector<RoofEditIntent> prepare_roof_opening_group_transform(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupTransform& request);

// Apply optional common Properties to a unique complete requested cohort of
// actual profiled children. Quantities must reproduce their exact expressions
// under their captured default units. Mouth/frame/glazing lengths are positive;
// curb height is nonnegative and the physical facet angle is finite in [-pi,pi].
// Retain each actual reference centre, host, row metadata and opaque siblings.
// Convert physical mouth lengths using the final reference centre's facet
// metric and admit the final rotated mouth; unspecified lengths retain their
// original physical sizes, including through rotation. The existing facet
// metric is independent of angle: rotation changes projected axes, not scales.
// Equal values retain actual quantities, profile and angle without rewriting
// receipts or promoting schemas. Complete actual-map replay admits every row
// before any intent is returned. No caller name/context/host/property-map
// authority is accepted. Unchanged roofs are omitted unless
// retain_invariant_members requests admitted rows for complete mixed proof;
// those rows authenticate actual source and confer no publication authority.
[[nodiscard]] std::vector<RoofEditIntent> prepare_roof_opening_group_properties(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupProperties& request);

// Original-source composite Properties preparation. Scalar profiles stage
// against their actual owners; child reference centres and physical sizes read
// those original owners, then rebase to the complete final profile's facet.
// Only the complete roof/profile/child cohort receives native admission. Empty
// members permit profile-only preparation; no caller candidate is accepted.
[[nodiscard]] std::vector<RoofEditIntent> prepare_roof_opening_group_properties(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupProperties& request,
    const std::vector<RoofProfileEditIntent>& profiles);

struct PreparedRoofOpeningGroupProperties {
    std::vector<RoofEditIntent> intents;
    std::map<std::string, Entity, std::less<>> candidate_entities;
};
// The same preparation with its complete independently replayed map, avoiding
// a second full native admission just to retrieve the result. This return
// value is original-source evidence only, never command/publication authority.
[[nodiscard]] PreparedRoofOpeningGroupProperties prepare_roof_opening_group_properties_composite(
    const std::map<std::string, Entity, std::less<>>& actual,
    const RoofOpeningGroupProperties& request,
    const std::vector<RoofProfileEditIntent>& profiles);

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
