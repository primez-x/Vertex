#pragma once

#include "sketch/document.hpp"

#include <string_view>

namespace sketch {

struct RoofPlanResizeIntent {
    std::string roof_id;
    double scale_x{1.0};
    double scale_y{1.0};
    Vec2 anchor;
    double frame_rotation_radians{};
};

inline constexpr std::string_view roof_plan_resize_derivations_key =
    "roof_plan_resize_derivations";

// Closed v1: exactly version, roof_id, scale_x, scale_y, anchor_m and
// frame_rotation_radians. The two factors are positive finite scalars;
// anchor_m contains two finite world XY metre coordinates. No candidate,
// entered final dimension, Quantity or snapshot supplies resize authority.
[[nodiscard]] nlohmann::json encode_roof_plan_resize_intent(const RoofPlanResizeIntent& intent);
[[nodiscard]] RoofPlanResizeIntent decode_roof_plan_resize_intent(const nlohmann::json& value);

// The owned archive is closed {version,operations}: v1 admits roof schemas
// 1/2, v2 adds schema 3 skylight profiles, and v3 adds schema 4 surface angles.
// Older archive versions keep their original closed frame fences. Each operation has
// exactly operation, source, result and receipts. Physical source/result
// frames are closed canonical roof properties with closed opening rows;
// receipt maps retain the original complete raw receipts. Historical owner
// and opening identities remain historical through clone/replacement.
void validate_roof_plan_resize_derivations(const Entity& source);
// Only validated historical frames/identities and known receipt cores are
// omitted. Every opaque receipt and rational sibling survives in the residual.
// The residual is for reference scanning only; never publish it as an archive.
[[nodiscard]] nlohmann::json roof_plan_resize_opaque_remainder(const Entity& source);
void validate_roof_plan_resize_source_entity(const Entity& source);

// Calls the shared native geometry stage on the actual source. Changed known
// quantity_entries/opening input scalars move verbatim to the archive; future
// or unsupported affected bindings refuse. XY, profile, pitch and opening
// reference rectangles follow genuine math and retain their surface angle;
// schema/roster/opaque metadata, Z, rise,
// thickness, overhang and existing rigid-transform history remain exact.
[[nodiscard]] Entity stage_roof_plan_resize_entity(
    const Entity& actual_source, const RoofPlanResizeIntent& intent);
[[nodiscard]] Entity replay_roof_plan_resize_entity(
    const Entity& actual_source, const RoofPlanResizeIntent& intent);

// Unique active actual-map targets, resolved source/result levels and native
// roofs plus complete affected joins. Broken retained join membership refuses.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_plan_resize_entities(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const std::vector<RoofPlanResizeIntent>& intents);

// Actual producer intent is required. Complete exact source-derived replay
// comparison includes every opaque field and all historical archives.
// nullopt is an exact no-op; unsupported candidate differences throw.
[[nodiscard]] std::optional<RoofPlanResizeIntent> capture_roof_plan_resize(
    const Entity& original, const Entity& candidate,
    const RoofPlanResizeIntent& actual_intent);

} // namespace sketch
