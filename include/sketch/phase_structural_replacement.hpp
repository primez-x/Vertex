#pragma once

#include "sketch/structural_object_edit.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

using PhaseStructuralReplacementEntities = std::map<std::string, Entity, std::less<>>;
using PhaseStructuralReplacementIdentityMap = std::map<std::string, std::string, std::less<>>;

struct PhaseStructuralReplacementDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const PhaseStructuralReplacementDiagnostic&) const = default;
};

// Source-derived identity inventory. This is discovery, never clone authority.
struct PhaseStructuralReplacementPlan {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_object_ids;
    std::vector<std::string> required_entity_ids;
    std::vector<std::string> required_child_ids;
    std::vector<PhaseStructuralReplacementDiagnostic> diagnostics;
    std::vector<std::pair<std::string, std::string>> required_hosted_instance_ids;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const PhaseStructuralReplacementPlan&) const = default;
};

[[nodiscard]] PhaseStructuralReplacementPlan inspect_phase_structural_replacement_plan(
    const PhaseStructuralReplacementEntities& actual, const std::vector<std::string>& seed_object_ids,
    const std::string& registry_id, const std::string& alternative_id, bool complete_hosted = false);

struct PhaseStructuralReplacementAuthoring {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_object_ids;
    PhaseStructuralReplacementIdentityMap identities;
    std::vector<StructuralObjectEditIntent> edits;
    bool demolition{};
    bool complete_hosted{};
    std::map<std::pair<std::string, std::string>, std::string> hosted_instance_identities;
};

// Closed v1: version, registry_id, alternative_id, seed_object_ids, identities,
// edits. Ordinary edits may share the cohort; only actually changed baseline
// seeds receive fresh owners. Source overlay IDs retain saved-view vocabulary;
// every destination is a strict fresh token. Document reserves retained history.
// Closed v2: version, registry_id, alternative_id, seed_object_ids,
// demolition:true. Identity and edit collections must be empty. Only the saved
// alternative's demolition roster changes; all physical/presentation rows stay.
// Closed v3: the six v1 fields, complete_hosted:true and an exact qualified
// hosted_instance_identities array of catalog_id, instance_id and
// proposed_instance_id rows. Catalogs use identities; render aliases are
// computed from actual source and the complete candidate, never authored keys.
[[nodiscard]] nlohmann::json encode_phase_structural_replacement_authoring(
    const PhaseStructuralReplacementAuthoring& authoring);
[[nodiscard]] PhaseStructuralReplacementAuthoring decode_phase_structural_replacement_authoring(
    const nlohmann::json& value);
[[nodiscard]] PhaseStructuralReplacementEntities replay_phase_structural_replacement_authoring(
    const PhaseStructuralReplacementEntities& actual, const PhaseStructuralReplacementAuthoring& authoring);

struct PhaseStructuralEditReplacementRequest {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_object_ids;
    bool operator==(const PhaseStructuralEditReplacementRequest&) const = default;
};

// Independently replay typed edits, discard exact no-ops, and classify every
// changed owner against actual saved membership. Foreign, inactive or multiple
// memberships refuse. No source-equivalent target gains replacement authority.
[[nodiscard]] std::optional<PhaseStructuralEditReplacementRequest> phase_structural_edit_replacement_request(
    const PhaseStructuralReplacementEntities& actual, const std::vector<StructuralObjectEditIntent>& edits);

// Ordinary selections return nullopt. Once one active shared-baseline owner is
// selected, every target must be an active baseline column/beam from that same
// saved registry; duplicate, inactive, foreign and mixed authority refuses.
[[nodiscard]] std::optional<PhaseStructuralEditReplacementRequest> phase_structural_demolition_request(
    const PhaseStructuralReplacementEntities& actual, const std::vector<std::string>& seed_object_ids);

} // namespace sketch
