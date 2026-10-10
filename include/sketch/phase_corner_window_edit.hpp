#pragma once

#include "sketch/phase_wall_replacement_request.hpp"

#include <optional>

namespace sketch {

// Same-owner name and dimensional profile edits only. Null name leaves the
// source name unchanged; dimensions use the nine canonical corner pointers.
// Hosts, cuts, leg endpoints, family and opaque payload remain source-owned.
struct CornerWindowProfileEditIntent {
    std::string owner_id;
    std::optional<std::string> name;
    std::map<std::string, double, std::less<>> dimensions;
};

[[nodiscard]] nlohmann::json encode_corner_window_profile_edit_intent(
    const CornerWindowProfileEditIntent& intent);
[[nodiscard]] CornerWindowProfileEditIntent decode_corner_window_profile_edit_intent(
    const nlohmann::json& value);
// Derives only actual changes from the complete actual source. Shared corner
// completion validates the supplied replacement and source-owned receipts.
// Unknown edits refuse; a same-value edit retains exact source representation.
[[nodiscard]] std::optional<CornerWindowProfileEditIntent> make_corner_window_profile_edit_intent(
    const std::map<std::string, Entity, std::less<>>& source, const Entity& replacement);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_corner_window_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<CornerWindowProfileEditIntent>& intents,
    bool validate_final_constraints = true);
// Actual changed saved-active baseline aggregates require their owner, both
// hosts and both cuts in one registry's baseline. Ordinary/proposed edits and
// exact no-ops return null; mixed authority or partial baseline cohorts refuse.
[[nodiscard]] std::optional<PhaseWallReplacementRequest> phase_corner_window_profile_replacement_request(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<CornerWindowProfileEditIntent>& intents);

} // namespace sketch
