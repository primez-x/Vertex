#pragma once

#include "sketch/stair_object_edit.hpp"
#include "sketch/stair_transform.hpp"
#include "sketch/stair_compound_edit.hpp"

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sketch {

using PhaseStairReplacementEntities = std::map<std::string, Entity, std::less<>>;
using PhaseStairReplacementIdentityMap = std::map<std::string, std::string, std::less<>>;
using PhaseStairReplacementChildKey = std::pair<std::string, std::string>;
using PhaseStairReplacementChildIdentityMap = std::map<PhaseStairReplacementChildKey, std::string>;
using PhaseStairReplacementHostedInstanceKey = std::pair<std::string, std::string>;
using PhaseStairReplacementHostedInstanceIdentityMap = std::map<PhaseStairReplacementHostedInstanceKey, std::string>;
using PhaseStairReplacementOverlayKey = std::tuple<std::string, std::string, std::string>;
using PhaseStairReplacementOverlayIdentityMap = std::map<PhaseStairReplacementOverlayKey, std::string>;

struct PhaseStairReplacementDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const PhaseStairReplacementDiagnostic&) const = default;
};

struct PhaseStairReplacementRequest {
    std::string registry_id;
    std::string alternative_id;
    // Only independently replayed, actually changed baseline targets.
    std::vector<std::string> seed_object_ids;
    bool operator==(const PhaseStairReplacementRequest&) const = default;
};

// Actual-source discovery, never supplied clone authority. Physical closure
// includes active baseline rails attached to replaced stairs. Active proposed
// rails retain their owners and membership and are rehosted during replay.
struct PhaseStairReplacementPlan {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_object_ids;
    std::vector<std::string> required_entity_ids;
    // Derived edited topology, including newly introduced typed child names.
    // Keys are qualified; destination names remain globally fresh.
    std::vector<PhaseStairReplacementChildKey> required_child_ids;
    std::vector<PhaseStairReplacementHostedInstanceKey> required_hosted_instance_ids;
    std::vector<PhaseStairReplacementOverlayKey> required_overlay_ids;
    std::vector<std::string> retained_rehost_object_ids;
    // Actual inactive rails attached to replaced baseline stairs. Nonempty
    // discovery selects additive retained-topology staging (closed child v4).
    std::vector<std::string> preserved_inactive_rail_ids;
    std::vector<PhaseStairReplacementDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const PhaseStairReplacementPlan&) const = default;
};

[[nodiscard]] std::optional<PhaseStairReplacementRequest> phase_stair_replacement_request(
    const PhaseStairReplacementEntities& actual, const std::vector<StairObjectEditIntent>& edits);
[[nodiscard]] PhaseStairReplacementPlan inspect_phase_stair_replacement_plan(
    const PhaseStairReplacementEntities& actual, const std::vector<StairObjectEditIntent>& edits,
    const std::string& registry_id = {}, const std::string& alternative_id = {});
[[nodiscard]] std::optional<PhaseStairReplacementRequest> phase_stair_replacement_request(
    const PhaseStairReplacementEntities& actual, const std::vector<StairTransformIntent>& transforms);
[[nodiscard]] PhaseStairReplacementPlan inspect_phase_stair_replacement_plan(
    const PhaseStairReplacementEntities& actual, const std::vector<StairTransformIntent>& transforms,
    const std::string& registry_id = {}, const std::string& alternative_id = {});
[[nodiscard]] std::optional<PhaseStairReplacementRequest> phase_stair_replacement_request(
    const PhaseStairReplacementEntities& actual, const std::vector<StairCompoundEditIntent>& compound_edits);
[[nodiscard]] PhaseStairReplacementPlan inspect_phase_stair_replacement_plan(
    const PhaseStairReplacementEntities& actual, const std::vector<StairCompoundEditIntent>& compound_edits,
    const std::string& registry_id = {}, const std::string& alternative_id = {});

// Capture complete edited owner envelopes through ordinary typed authority,
// using additive actual-source copies when inactive baseline attachments need
// their original topology. Never grants supplied entity-map authority. Other
// sources retain the existing compound capture and admission contract.
[[nodiscard]] std::vector<StairCompoundEditIntent> capture_phase_stair_replacement_compound_edits(
    const PhaseStairReplacementEntities& actual, const std::vector<Entity>& edited_entities);

struct PhaseStairReplacementAuthoring {
    std::string registry_id;
    std::string alternative_id;
    std::vector<StairObjectEditIntent> edits;
    PhaseStairReplacementIdentityMap identities;
    PhaseStairReplacementChildIdentityMap child_identities;
    PhaseStairReplacementHostedInstanceIdentityMap hosted_instance_identities;
    PhaseStairReplacementOverlayIdentityMap overlay_identities;
    // Exclusive with edits. Without a retained witness, selects closed v2.
    std::vector<StairTransformIntent> transforms;
    // Exclusive with both existing lanes. Without a witness, selects closed v3.
    std::vector<StairCompoundEditIntent> compound_edits;
    // Nonempty, ascending actual-source witness selects v4 for any one lane.
    // Empty preserves the original v1/v2/v3 staging and replay meanings.
    std::vector<std::string> preserved_inactive_rail_ids;
};

// Closed v1: version, registry_id, alternative_id, edits, identities,
// child_identities, hosted_instance_identities, overlay_identities. Qualified
// arrays are canonical and have exact known string fields. Actual-map replay
// owns scope, physical results, closure and aliases. Pose and vertical placement
// must remain semantically equal to the actual source on every typed edit.
// Closed v2 replaces edits with transforms; all other qualified fields retain
// their v1 meanings. Captured operators replay against the actual source only.
// Closed v3 replaces edits with compound_edits. Typed actual-source profiles
// precede anchored rigid placement, including complete entered-input receipts.
// Closed v4 adds preserved_inactive_rail_ids to exactly one existing lane.
// The nonempty ascending witness must equal independently discovered inactive
// actual rails; typed edits stage on additive copies before final replacement.
[[nodiscard]] nlohmann::json encode_phase_stair_replacement_authoring(
    const PhaseStairReplacementAuthoring& authoring);
[[nodiscard]] PhaseStairReplacementAuthoring decode_phase_stair_replacement_authoring(
    const nlohmann::json& value);

// Complete source-derived candidate: baseline physical/catalog envelopes and
// other alternatives remain exact, and saved presentation rows append copies.
// Only selected actual hosted instances enter private catalogs, preserving
// raw schema/definitions/overrides/placement except qualified ID/host slots.
// Retained history/assets/render names require enclosing Document reservation.
[[nodiscard]] PhaseStairReplacementEntities replay_phase_stair_replacement_authoring(
    const PhaseStairReplacementEntities& actual, const PhaseStairReplacementAuthoring& authoring);

} // namespace sketch
