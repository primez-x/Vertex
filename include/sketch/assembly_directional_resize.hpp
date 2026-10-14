#pragma once

#include "sketch/assembly_model.hpp"

#include <optional>

namespace sketch {

struct AssemblyDirectionalResize {
    Vec2 world_anchor_m{};
    double x_factor{1.0};
    double y_factor{1.0};
    // Appended for aggregate compatibility. nullopt uses the actual root axes;
    // a finite angle selects one unreflected world frame for every component,
    // or the local axes of operation_frame when that frame is supplied.
    std::optional<double> world_axis_rotation_radians{};
    // Appended producer-only frame. It must be finite, orientation-preserving,
    // and rigid (unit XY/Z scales). Requires an explicit finite axis angle;
    // world_anchor_m and world_axis_rotation_radians then denote frame-local
    // values despite their historical names. No frame retains the original
    // world-anchor/natural-root-axis behavior exactly.
    std::optional<AssemblyTransform> operation_frame{};
};

// One actual independent document root and its captured source-space operator.
// Shared by analytical aggregate admission and the desktop geometry producer.
struct AssemblyDirectionalResizeTarget {
    std::string independent_entity_id;
    AssemblyDirectionalResize operation;
};

struct AssemblyDirectionalResizeNodeDraft {
    std::vector<std::string> part_path;
    std::string type_id;
    AssemblyTransform actual_world_transform;
    // nullopt follows the shared type; an explicit empty/equal override remains
    // explicit. Source/candidate arrays are the resolved node's OWN profiles.
    std::optional<std::vector<AssemblyProfile>> source_profile_overrides;
    std::vector<AssemblyProfile> source_profiles;
    std::vector<AssemblyProfile> candidate_profiles;
};

struct AssemblyDirectionalResizeDraft {
    AssemblyInstance source_instance;
    // Includes geometry-free nodes; their descendants still participate.
    std::vector<AssemblyDirectionalResizeNodeDraft> nodes;
    bool changed{};
};

// Supply the complete admitted expansion from this exact model/snapshot, never
// a filtered presentation expansion. Axes default to the actual empty-path
// root's yaw and reflection. An explicit world_axis_rotation_radians selects
// that unreflected world frame; factors act in physical XY metres, independently
// of Z. The explicit angle must be finite even for unit factors.
// Let R be the actual binary64 yaw basis supplied by
// assembly_rotation_components for the selected frame angle. The operation is
// a + R*diag(x_factor,y_factor)*R^-1*(q-a). Its inverse uses the exact squared
// norm of those components; their stored basis need not be exactly orthonormal.
// With operation_frame F, that operation is expressed in F's local metres and
// pulled through F*D*F^-1 using exact products of the actual binary64 frame/axis
// bases. Neither a summed angle nor a rounded absolute world anchor is made.
// Frame translation (including Z), yaw and unit scales are authenticated even
// for unit factors; Z geometry remains unchanged.
// Each local profile map is pulled through its actual world pose. Circular
// source rings, topology, extrusion/material fields, definitions and ALL root/
// path transforms stay exact. No topology/dimension policy or intent is made.
//
// Every node returns its before/candidate own roster and original override
// provenance. The caller must publish only changed rosters through one exact
// reviewed atomic intent and bind source_instance to the captured source.
// Unit factors return equal rosters without creating maps or overrides.
// Nonunit candidates are admitted by model.expand on a detached instance with
// node-local overrides; the supplied remaining document ledger is committed
// only on success. Admission can refuse otherwise mathematical maps when
// physical tolerance, numeric range or the remaining work budget fails.
// Optional-frame coefficient work also shares that transactional ledger,
// including frame/axis setup for unit factors.
[[nodiscard]] AssemblyDirectionalResizeDraft draft_assembly_directional_resize(
    const AssemblyModel& model, const AssemblyExpansion& actual,
    const AssemblyDirectionalResize& operation, AssemblyExpansionBudget& available_work);

// Analytical complete profile extents, including mapped circular-arc extrema,
// in the actual root's oriented/reflected basis about world_origin_m. Values are
// metre offsets obtained with that actual basis inverse; root scale is NOT
// divided out. Reconstruct world points with the same actual yaw/reflection
// basis. The explicit origin avoids
// cancellation between distant world bounds. Empty geometry refuses.
// Source is already admitted as above; this is bounds work, not new topology
// admission. Charges nodes/segments and linear affine work to the same ledger,
// atomically on success. No tessellation or presentation projection is used.
[[nodiscard]] Bounds2 assembly_root_oriented_profile_bounds(
    const AssemblyExpansion& actual, Vec2 world_origin_m,
    AssemblyExpansionBudget& available_work);

// The same complete analytical extents in an explicit, unreflected world yaw
// basis, shared across independently rotated/reflected components. The origin
// and angle must be finite. Values are metre offsets using the actual binary64
// assembly_rotation_components(angle) basis inverse, without dividing out root
// scale. Source validation, empty-geometry refusal and transactional work charges
// are the same as assembly_root_oriented_profile_bounds.
[[nodiscard]] Bounds2 assembly_world_axis_profile_bounds(
    const AssemblyExpansion& actual, Vec2 world_origin_m,
    double world_axis_rotation_radians, AssemblyExpansionBudget& available_work);

} // namespace sketch
