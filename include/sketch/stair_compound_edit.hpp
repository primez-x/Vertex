#pragma once

#include "sketch/stair_object_edit.hpp"
#include "sketch/stair_transform.hpp"

namespace sketch {

// Closed source-derived profile then placement intent. Both lanes name the same
// actual owner. The profile retains the actual source base and orientation;
// vertical bindings/offsets remain profile authority. Placement is a rigid yaw
// and translation about the resolved, profile-edited source base, never scale
// or reflection. Its complete quantity map, when present, describes final pose.
struct StairCompoundEditIntent {
    StairObjectEditIntent profile_edit;
    StairTransformIntent placement_edit;
};

[[nodiscard]] nlohmann::json encode_stair_compound_edit_intent(const StairCompoundEditIntent& intent);
[[nodiscard]] StairCompoundEditIntent decode_stair_compound_edit_intent(const nlohmann::json& value);

// Replay the complete actual map in two typed stages. Profiles admit actual
// attachments/levels first; placement then moves active dependent rails and
// actual hosted catalog rows exactly once. Explicit hosted rail placement still
// requires its selected actual stair and equivalent captured operator.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_stair_compound_edit_entities(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const std::vector<StairCompoundEditIntent>& intents);

// Admit a related edit cohort together, then derive each neutral profile and
// placement from the same intermediate map. Coupled hosted edits never acquire
// a temporary singleton geometry assumption.
[[nodiscard]] std::vector<StairCompoundEditIntent> capture_stair_compound_edits(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const std::vector<Entity>& edited_entities);

// The original must be the exact actual owner. Validate the complete edited
// entity before neutralizing only base/orientation and coordinate receipts.
// Nonplacement metadata/receipts remain under the ordinary typed edit contract.
// Replayed position may differ only by transform roundoff; changed orientation
// may use an equivalent whole-turn encoding. Exact no-op returns nullopt.
[[nodiscard]] std::optional<StairCompoundEditIntent> capture_stair_compound_edit(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const Entity& original, const Entity& edited);
[[nodiscard]] std::optional<StairCompoundEditIntent> capture_stair_compound_edit(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const std::string& object_id, const Entity& edited);

} // namespace sketch
