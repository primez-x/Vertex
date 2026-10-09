#pragma once

#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/phase_roof_opening_edit.hpp"
#include "sketch/phase_roof_edit.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

using PhaseRoofReplacementEntities = std::map<std::string, Entity, std::less<>>;
using PhaseRoofReplacementIdentityMap = std::map<std::string, std::string, std::less<>>;

struct PhaseRoofReplacementDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const PhaseRoofReplacementDiagnostic&) const = default;
};

// Source-derived inventory only, never authority for caller-supplied clones.
struct PhaseRoofReplacementPlan {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_roof_ids;
    std::vector<std::string> required_entity_ids;
    // Roof openings and copied bound view overlays share the document namespace.
    std::vector<std::string> required_child_ids;
    std::vector<PhaseRoofReplacementDiagnostic> diagnostics;
    // Opt-in source-derived sharing. Retained members never receive copy or
    // role authority from the caller's identity map.
    bool phase_qualified_joins{};
    std::vector<std::string> retained_join_roof_ids;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const PhaseRoofReplacementPlan&) const = default;
};

struct PhaseRoofReplacementResult {
    PhaseRoofReplacementEntities entities;
    PhaseRoofReplacementIdentityMap original_to_proposed;
    // Includes every mapped identity and each explicitly authored new opening.
    std::vector<std::string> fresh_identity_ids;
};

[[nodiscard]] PhaseRoofReplacementPlan inspect_phase_roof_replacement_plan(
    const PhaseRoofReplacementEntities& source, const std::vector<std::string>& seed_roof_ids,
    const std::string& registry_id, const std::string& alternative_id,
    bool phase_qualified_joins = false);

// Reinspects the full retained map, independently replays typed edits on source
// owners, then derives copies. Registry and qualified presentation owners change
// in place; the opt-in ordinary combined list also changes actual ordinary roofs
// with their existing IDs. The enclosing Document reserves identities across history.
// Historical profile/opening slices remain separate. The combined roof edit
// dialect composes profile, openings and pose against the same actual source.
// Phase-qualified combined edits copy baseline join members and retain actual
// ordinary/proposed members, with mutually exclusive join roles proved from source.
[[nodiscard]] PhaseRoofReplacementResult replay_phase_roof_replacement(
    const PhaseRoofReplacementEntities& source, const PhaseRoofReplacementPlan& plan,
    const PhaseRoofReplacementIdentityMap& identities,
    const std::vector<RoofProfileEditIntent>& roof_profiles,
    const std::vector<RoofOpeningEditIntent>& roof_opening_edits = {},
    const std::vector<RoofEditIntent>& roof_edits = {},
    const std::vector<RoofEditIntent>& ordinary_roof_edits = {},
    bool phase_qualified_joins = false);

struct PhaseRoofReplacementAuthoring {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_roof_ids;
    PhaseRoofReplacementIdentityMap identities;
    std::vector<RoofProfileEditIntent> roof_profiles;
    std::vector<RoofOpeningEditIntent> roof_opening_edits{};
    std::vector<RoofEditIntent> roof_edits{};
    // Version four retains selected baseline roofs as demolition. Joined
    // survivors receive unchanged proposed owners; no body edit can accompany it.
    bool demolition{};
    // Exact additional component/overlay slots keyed by actual original IDs.
    std::map<std::string, std::vector<std::string>, std::less<>> demolition_additional_identities;
    // Version five independently classifies both combined edit lists from the
    // actual saved source. Ordinary owners retain their identities in place.
    std::vector<RoofEditIntent> ordinary_roof_edits{};
    // Version six qualifies combined joins from actual registry activity. It
    // also admits pure baseline combined edits with an empty ordinary edit list.
    // Version seven uses this opt-in for demolition without body edit authority.
    bool phase_qualified_joins{};
    // Version eight preserves singleton material relationships during demolition.
    bool preserve_singleton_material{};
};

// Version 1 retains exactly its six profile fields. Version 2 adds only
// roof_opening_edits and requires nonempty opening edits with empty profiles.
// Version 3 adds roof_edits, requiring nonempty combined edits and empty
// historical profile/opening arrays. Every dialect has an exact field set.
// Version 4 contains exactly version, registry_id, alternative_id, seed_roof_ids,
// identities, demolition:true, demolition_additional_identities.
// Version 5 adds only ordinary_roof_edits to version 3's eight fields. Both
// combined lists must be nonempty, historical arrays empty, and targets disjoint.
// Version 6 has the nine version-five fields plus phase_qualified_joins:true.
// Its ordinary list may be empty; its baseline combined list remains nonempty.
// Version 7 has version four's seven fields plus phase_qualified_joins:true.
// Version 8 adds only preserve_singleton_material:true to version 7.
[[nodiscard]] nlohmann::json encode_phase_roof_replacement_authoring(
    const PhaseRoofReplacementAuthoring& authoring);
[[nodiscard]] PhaseRoofReplacementAuthoring decode_phase_roof_replacement_authoring(
    const nlohmann::json& value);
[[nodiscard]] PhaseRoofReplacementEntities replay_phase_roof_replacement_authoring(
    const PhaseRoofReplacementEntities& source, const PhaseRoofReplacementAuthoring& authoring);

struct PhaseRoofProfileReplacementRequest {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_roof_ids;
    bool operator==(const PhaseRoofProfileReplacementRequest&) const = default;
};

struct PhaseRoofGeometryEditPartition {
    std::vector<RoofEditIntent> baseline_roof_edits;
    std::vector<RoofEditIntent> ordinary_roof_edits;
    std::optional<PhaseRoofProfileReplacementRequest> replacement;
};

// Replay the complete actual typed edit group before classifying changed targets
// from all saved registry memberships. Exact no-ops confer no role or identity
// authority. Changed shared-baseline owners require one saved active alternative.
[[nodiscard]] PhaseRoofGeometryEditPartition partition_phase_roof_geometry_edits(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofEditIntent>& roof_edits);

// Source-equivalent edits return nullopt before callers allocate identities.
// A shared-baseline request requires one actual saved active alternative and
// refuses mixed baseline/ordinary, foreign, inactive or dangling targets.
[[nodiscard]] std::optional<PhaseRoofProfileReplacementRequest> phase_roof_profile_replacement_request(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofProfileEditIntent>& roof_profiles);

// Opening edits use the same actual membership rules. Mixed changed/unchanged
// targets cannot add replacement authority; new children keep explicit fresh IDs.
[[nodiscard]] std::optional<PhaseRoofProfileReplacementRequest> phase_roof_opening_replacement_request(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofOpeningEditIntent>& roof_opening_edits);

// Atomic component edits retain the same saved active baseline rules. Every
// seed must actually change; source-equivalent batches allocate no identities.
[[nodiscard]] std::optional<PhaseRoofProfileReplacementRequest> phase_roof_edit_replacement_request(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofEditIntent>& roof_edits);

} // namespace sketch
