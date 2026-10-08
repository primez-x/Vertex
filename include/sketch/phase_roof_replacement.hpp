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
    const std::string& registry_id, const std::string& alternative_id);

// Reinspects the full retained map, independently replays typed edits on source
// owners, then derives copies. Only registry and qualified presentation owners
// change in place. The enclosing Document reserves identities across history.
// Historical profile/opening slices remain separate. The combined roof edit
// dialect composes profile, openings and pose against the same actual source.
[[nodiscard]] PhaseRoofReplacementResult replay_phase_roof_replacement(
    const PhaseRoofReplacementEntities& source, const PhaseRoofReplacementPlan& plan,
    const PhaseRoofReplacementIdentityMap& identities,
    const std::vector<RoofProfileEditIntent>& roof_profiles,
    const std::vector<RoofOpeningEditIntent>& roof_opening_edits = {},
    const std::vector<RoofEditIntent>& roof_edits = {});

struct PhaseRoofReplacementAuthoring {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_roof_ids;
    PhaseRoofReplacementIdentityMap identities;
    std::vector<RoofProfileEditIntent> roof_profiles;
    std::vector<RoofOpeningEditIntent> roof_opening_edits{};
    std::vector<RoofEditIntent> roof_edits{};
};

// Version 1 retains exactly its six profile fields. Version 2 adds only
// roof_opening_edits and requires nonempty opening edits with empty profiles.
// Version 3 adds roof_edits, requiring nonempty combined edits and empty
// historical profile/opening arrays. Every dialect has an exact field set.
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
