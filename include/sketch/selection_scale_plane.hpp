#pragma once

#include "sketch/site_frame.hpp"
#include "sketch/vertical_levels.hpp"

namespace sketch {

// The gesture's Z domain, not the persisted object's presentation-frame mode.
// Local gestures cannot combine different actual source frames. Site/native
// gestures compare displayed world planes and inverse-place each result once.
enum class SelectionScalePlaneDomain { source_local, presentation_world };
enum class SelectionScalePlaneOrigin { selected_wall, floor_level, legacy_unbound_zero };

struct SelectionScalePlane {
    // ArchitecturalGroupTransform.pivot.z: after vertical placement, before
    // Site placement. The architectural producer owns raw-level compensation.
    double effective_source_z_m{};
    SelectionScalePlaneOrigin origin{SelectionScalePlaneOrigin::legacy_unbound_zero};
    DrawingContext drawing_context;
    SiteFrameIdentity source_frame;
    SiteRigidTransform source_to_world;
    std::optional<VerticalLevelBinding> floor_binding;
    std::vector<std::string> selected_wall_ids;
    // Actual source dependencies, including the target, context, graph and
    // contributing walls. This receipt is derived data, not command authority.
    std::vector<std::string> dependency_ids;
};

// Resolve NEW complete-selection uniform-scale intents from one immutable
// actual source map. Actual architectural targets and selected wall IDs must
// each be distinct; their union is limited to 1000. Embedded catalog children,
// annotation IDs and presentation aliases are deliberately outside this API.
//
// Matching selected effective wall bases take precedence. Without such a wall,
// a qualified bound floor supplies its validated level elevation; object-local
// offsets do not replace the floor datum. The floor plane is placed into world
// Z then inverse-placed into the target's actual source frame once; independent
// geometry is never implicitly enrolled in the floor's frame. Conflicting wall
// planes, incomplete/contradictory explicit context, invalid binding/graph or
// incompatible local wall frames throw
// std::invalid_argument. Floors/levels/frames are never inferred by proximity.
//
// Zero is an explicit historical rule only for a validated unbound floor or an
// unassigned world-coordinate source with no explicit organizational context.
// It proves no common physical floor plane. Absolute/missing object placement
// does not suppress an existing bound FLOOR datum. Geometry/family admission,
// complete selection closure and the full captured-source fence remain caller
// responsibilities. No source, history, receipt or command is changed here;
// recorded transforms must continue replaying their original explicit pivots.
[[nodiscard]] std::map<std::string, SelectionScalePlane, std::less<>>
resolve_selection_scale_planes(const SiteFrameEntities& entities,
    std::span<const std::string> architectural_target_ids,
    std::span<const std::string> selected_wall_ids,
    SelectionScalePlaneDomain domain = SelectionScalePlaneDomain::source_local);
[[nodiscard]] std::map<std::string, SelectionScalePlane, std::less<>>
resolve_selection_scale_planes(const DocumentSnapshot& snapshot,
    std::span<const std::string> architectural_target_ids,
    std::span<const std::string> selected_wall_ids,
    SelectionScalePlaneDomain domain = SelectionScalePlaneDomain::source_local);

} // namespace sketch
