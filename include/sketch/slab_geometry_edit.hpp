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
};

inline constexpr std::string_view slab_geometry_derivations_key = "slab_geometry_derivations";

// Closed bounded v1 plan operation or v2 model operation, with the same six
// wire fields. Exactly the selected operation is present;
// coordinates and scales are mathematical intent, never entered quantities.
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
// Changed understood geometry receipts retire verbatim into the bounded
// archive. Unknown affected bindings refuse. Exact no-ops return the source.
// Understood receipt pointers are /boundary/I/start/0|1, /boundary/I/end/0|1,
// /boundary/I/sweep_radians, and the same under /holes/H/I. A v1 receipt is
// decoded against that actual source scalar; no computed receipt is authored.
// Closed archive: {version:1,operations:[{operation,source,result,receipts}]}.
// Frames are {boundary,holes} with closed {start,end,sweep_radians} segments;
// receipts is {quantity_entries:{pointer:verbatim_retired_receipt}}.
// Model archives use version 2 and may retain earlier unchanged v1 records.
// Model frames additionally contain profile (canonical/alias thickness and
// elevation, null for absent fields, and null or ordered numeric layers) and
// elevation_shift_m from actual resolved placement. Layer IDs are not frames.
// Known changed profile receipts retire by the same policy. Detached model
// replay refuses level placement; use actual-map replay for resolved context.
[[nodiscard]] Entity replay_slab_geometry_entity(
    const Entity& actual_source, const SlabGeometryEditIntent& intent);

// Unique active actual-map owners, actual native source/result payloads and
// resolved vertical placement/material context. No candidate geometry wire.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_slab_geometry_entities(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const std::vector<SlabGeometryEditIntent>& intents);

} // namespace sketch
