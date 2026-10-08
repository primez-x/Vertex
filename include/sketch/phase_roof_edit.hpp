#pragma once

#include "sketch/phase_roof_opening_edit.hpp"
#include "sketch/phase_roof_pose_edit.hpp"
#include "sketch/phase_roof_profile_edit.hpp"

namespace sketch {

// One roof edit can change its envelope, openings and placement together.
// Every component binds the same actual source owner; component staging never
// substitutes an intermediate roof for the captured source.
struct RoofEditIntent {
    std::string roof_id;
    std::optional<RoofProfileEditIntent> profile;
    std::optional<RoofOpeningEditIntent> openings;
    std::optional<RoofPoseEditIntent> pose;
};

// Strict version one: version, roof_id, profile, openings, pose. At least one
// nonnull typed component is required; all component owner IDs must agree.
[[nodiscard]] nlohmann::json encode_roof_edit_intent(const RoofEditIntent& intent);
[[nodiscard]] RoofEditIntent decode_roof_edit_intent(const nlohmann::json& value);

// Source-admitted component deltas compose before final native admission.
// A resize and cut move/removal therefore need not fit a transient envelope.
[[nodiscard]] Entity replay_roof_edit_entity(const Entity& source, const RoofEditIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_edit_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofEditIntent>& intents);

// Inference is accepted only after the complete candidate equals independent
// typed replay, including retained opaque values and exact receipt metadata.
[[nodiscard]] std::optional<RoofEditIntent> capture_roof_edit(
    const Entity& original, const Entity& candidate);

[[nodiscard]] std::vector<RoofOpeningEditIntent> roof_edit_opening_intents(
    const std::vector<RoofEditIntent>& intents);

} // namespace sketch
