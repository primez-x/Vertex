#pragma once

#include "sketch/quantity.hpp"
#include "sketch/stair_object_edit.hpp"

namespace sketch {

// Scalar authority only. Actual form, placement, connected levels, topology,
// child identities and rail hosts are retained. All-null retains the member.
struct StairProfileEditIntent {
    std::string object_id;
    std::optional<Quantity> total_rise;
    std::optional<Quantity> going;
    std::optional<Quantity> width;
    std::optional<Quantity> top_landing_depth;
    std::optional<Quantity> top_landing_thickness;
    std::optional<Quantity> length;
    std::optional<Quantity> height;
    std::optional<Quantity> thickness;
    std::optional<Quantity> post_spacing;
};

// Closed canonical v1 with every nullable dimension above. Quantities use the
// exact shared constraint receipt codec; entity/profile payloads are absent.
[[nodiscard]] nlohmann::json encode_stair_profile_edit_intent(const StairProfileEditIntent& intent);
[[nodiscard]] StairProfileEditIntent decode_stair_profile_edit_intent(const nlohmann::json& value);
void validate_stair_profile_source_entity(const Entity& source);

// A supplied going/width applies to the default and every existing per-flight
// override. Inherited slots stay inherited; equal individual values retain
// their exact raw fields and receipts. Existing top landings permit their two
// dimensions. Hosted rail length is derived and cannot be authored here.
// Counts/turns stay exact.
[[nodiscard]] StairObjectEditIntent stair_object_edit_from_profile(
    const Entity& source, const StairProfileEditIntent& intent);

// Analytical preparation only, including retained members, in input order.
// Derives exact editor envelopes from the immutable complete actual map without
// native work or publication. Root may pass these to the existing ordinary or
// phase dependency inspector/review/producer; preparation grants no admission.
[[nodiscard]] std::vector<Entity> prepare_stair_profile_edit_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<StairProfileEditIntent>& intents);

// Ordinary topology-retaining replay. Rejects protected/inactive owners and any
// required explicit rail disposition. Existing full producers admit connected
// levels, physical rails and actual hosted catalogs on both sides of replay.
// Dependency review and baseline replacement remain separate existing lanes.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_stair_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<StairProfileEditIntent>& intents);

} // namespace sketch
