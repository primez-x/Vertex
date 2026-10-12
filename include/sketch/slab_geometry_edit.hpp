#pragma once

#include "sketch/architectural_footprint_edit.hpp"
#include "sketch/stair_semantics.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

enum class SlabGeometryEditKind { move_vertex, transform_plan, resize_plan, transform_model };

struct SlabPlanAxisResize {
    double scale_x{1};
    double scale_y{1};
    Vec2 anchor_m{};
    double frame_rotation_radians{};
};

struct SlabModelTransform {
    Vec3 pivot_m{};
    Vec3 offset_m{};
    double rotation_radians{};
    double uniform_scale{1};
    bool flip_horizontal{false};
    bool flip_vertical{false};
};

struct SlabGeometryEditIntent {
    std::string slab_id;
    SlabGeometryEditKind kind{SlabGeometryEditKind::move_vertex};
    std::optional<FootprintVertexEdit> vertex;
    std::optional<PlanarTransform> transform;
    std::optional<SlabPlanAxisResize> resize;
    // transform_plan only: positive uniform plan scale about transform.pivot,
    // before its rigid operation. Other edit kinds require an exact one.
    double uniform_scale{1};
    // transform_model only: positive physical scale and model-space XYZ move,
    // Z yaw and plan reflections. The legacy plan scale remains exactly one.
    std::optional<SlabModelTransform> model_transform;
    // v3 plan/model and v4 directional plan resize edits coordinate type-owned
    // world profiles through the actual Site frame. False retains historical
    // v1/v2 hosted semantics and the historical v1 resize arithmetic.
    bool coordinate_world_hosted_geometry{false};
    // v5 keeps true source lines/circles and composes their positive planar
    // map. Vertex positions are effective plan coordinates. Never inferred
    // for historical leaves, including edits of an already mapped slab.
    bool preserve_planar_source_geometry{false};
};

inline constexpr std::string_view slab_geometry_derivations_key = "slab_geometry_derivations";

// Closed bounded v1 plan operation or v2 model operation, with the same six
// wire fields, v3 plan/model or v4 resize_plan with
// coordinate_world_hosted_geometry:true. v4 exclusively uses resize_plan;
// v3 exclusively uses transform_plan/transform_model.
// v5 has those seven fields plus preserve_planar_source_geometry:true,
// admits all four kinds, and requires a boolean coordination field (false
// for move_vertex). Historical leaves refuse actual mapped slabs.
// Exactly the selected operation is present;
// coordinates and scales are mathematical intent, never entered quantities.
// Producers choose the leaf here: true when the actual slab already carries a
// planar map, or when an unequal plan resize would carry a circular source arc
// to an ellipse. Otherwise historical leaves remain exact and older readers
// keep opening the project. Never infer v5 for already-recorded leaves.
[[nodiscard]] bool slab_geometry_edit_requires_planar_source(
    const Entity& actual_slab, const SlabGeometryEditIntent& intent);
[[nodiscard]] nlohmann::json encode_slab_geometry_edit_intent(const SlabGeometryEditIntent& intent);
[[nodiscard]] SlabGeometryEditIntent decode_slab_geometry_edit_intent(const nlohmann::json& value);

// Validates retained historical mathematical frames and retired receipts.
// Archived slab IDs are provenance and must never be treated as live references
// or remapped by copy/replacement. Model frames admit their numerical native
// profile, without supplying live-map identity, placement or material authority.
void validate_slab_geometry_derivation(const Entity& source);

// Actual-source replay preserves all unrelated payload, layer identities and
// materials. Only model transforms scale physical profile/layer thickness.
// Straight rings permit positive axis scaling in a rotated local frame;
// circular arcs require equal scales. Reflections retain signed arc semantics.
// v4 uses a + R*diag(scale_x,scale_y)*R^-1*(p-a), with the actual binary64
// assembly_rotation_components(frame_rotation_radians) basis and its exact
// squared-norm inverse. Matrix fields round once before anchored endpoint
// evaluation, matching assembly directional resize. v1 retains R*diag*R^T.
// Directional plan resizing never changes Z, layers, materials or ring topology.
// Changed understood geometry receipts retire verbatim into the bounded
// archive. Unknown affected bindings refuse. Exact no-ops return the source.
// Understood receipt pointers are /boundary/I/start/0|1, /boundary/I/end/0|1,
// /boundary/I/sweep_radians, and the same under /holes/H/I. A v1 receipt is
// decoded against that actual source scalar; no computed receipt is authored.
// Closed archive: {version:1,operations:[{operation,source,result,receipts}]}.
// Frames are {boundary,holes} with closed {start,end,sweep_radians} segments;
// receipts is {quantity_entries:{pointer:verbatim_retired_receipt}}.
// Model archives use version 2 and may retain earlier unchanged v1 records.
// Coordinated archives use version 3, require at least one v3 operation, and
// retain earlier unchanged plan/model records and their original frame policy.
// Directional coordinated archives use version 4, require at least one v4
// resize_plan operation, and retain unchanged v1/v2/v3 records. Earlier archive
// versions cannot contain v4 operations; appending an earlier operation never
// downgrades a v4 archive. Its resize frames remain {boundary,holes} only.
// Model frames additionally contain profile (canonical/alias thickness and
// elevation, null for absent fields, and null or ordered numeric layers) and
// elevation_shift_m from actual resolved placement. Layer IDs are not frames.
// Known changed profile receipts retire by the same policy. Detached model
// replay refuses level placement; use actual-map replay for resolved context.
// v5 frames additionally contain planar_affine (null for absent source maps).
// Archive5 requires a v5 operation and retains exact historical frames. v5
// resize composes the exact directional map, preserving source arcs and Z;
// plan/model transforms compose effective geometry, with odd reflections
// represented by exact source reflection and a positive residual map.
[[nodiscard]] Entity replay_slab_geometry_entity(
    const Entity& actual_source, const SlabGeometryEditIntent& intent);

// Unique active actual-map owners, actual native source/result payloads and
// resolved vertical placement/material context. No candidate geometry wire.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_slab_geometry_entities(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const std::vector<SlabGeometryEditIntent>& intents);

} // namespace sketch
