#pragma once

#include "sketch/phase_slab_profile_edit.hpp"
#include "sketch/slab_layer_stack_edit.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

using PhaseSlabReplacementEntities = std::map<std::string, Entity, std::less<>>;
using PhaseSlabReplacementIdentityMap = std::map<std::string, std::string, std::less<>>;

struct PhaseSlabReplacementDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const PhaseSlabReplacementDiagnostic&) const = default;
};

// Inventory derived from the actual source, never supplied clone authority.
struct PhaseSlabReplacementPlan {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_slab_ids;
    std::vector<std::string> required_entity_ids;
    // Actual slab layers and bound view overlays share the document namespace.
    std::vector<std::string> required_child_ids;
    std::vector<PhaseSlabReplacementDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const PhaseSlabReplacementPlan&) const = default;
};

struct PhaseSlabReplacementResult {
    PhaseSlabReplacementEntities entities;
    // Complete source roster, including removed layers whose mapped IDs are
    // reserved without becoming live proposed children.
    PhaseSlabReplacementIdentityMap original_to_proposed;
    // Mapping outputs plus actual newly authored stack row IDs.
    std::vector<std::string> fresh_identity_ids;
};

[[nodiscard]] PhaseSlabReplacementPlan inspect_phase_slab_replacement_plan(
    const PhaseSlabReplacementEntities& source, const std::vector<std::string>& seed_slab_ids,
    const std::string& registry_id, const std::string& alternative_id);

// Reinspect the source, independently replay typed inputs and derive fresh
// owners. Originals remain exact; qualified presentation rows are additive.
// Exactly one edit family is nonempty. New stack rows retain their declared
// fresh IDs; existing rows remap through the complete source-derived mapping.
// The enclosing Document reserves fresh identities across retained history.
[[nodiscard]] PhaseSlabReplacementResult replay_phase_slab_replacement(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementPlan& plan,
    const PhaseSlabReplacementIdentityMap& identities,
    const std::vector<SlabProfileEditIntent>& slab_profiles,
    const std::vector<SlabLayerStackEditIntent>& slab_stacks = {});

struct PhaseSlabReplacementAuthoring {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_slab_ids;
    PhaseSlabReplacementIdentityMap identities;
    std::vector<SlabProfileEditIntent> slab_profiles;
    std::vector<SlabLayerStackEditIntent> slab_stacks;
};

// Exact v1 fields: version, registry_id, alternative_id, seed_slab_ids,
// identities and slab_profiles. Exclusive v2 replaces slab_profiles with
// slab_stacks. Seeds must exactly match changed edit targets in either family.
[[nodiscard]] nlohmann::json encode_phase_slab_replacement_authoring(
    const PhaseSlabReplacementAuthoring& authoring);
[[nodiscard]] PhaseSlabReplacementAuthoring decode_phase_slab_replacement_authoring(
    const nlohmann::json& value);
[[nodiscard]] PhaseSlabReplacementEntities replay_phase_slab_replacement_authoring(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementAuthoring& authoring);

struct PhaseSlabProfileReplacementRequest {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_slab_ids;
    bool operator==(const PhaseSlabProfileReplacementRequest&) const = default;
};

// Source-equivalent batches and ordinary/proposed edits require no allocation.
// Shared-baseline edits require one actual active alternative; mixed scopes,
// inactive/demolished owners and overlapping membership are refused.
[[nodiscard]] std::optional<PhaseSlabProfileReplacementRequest> phase_slab_profile_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabProfileEditIntent>& slab_profiles);
[[nodiscard]] std::optional<PhaseSlabProfileReplacementRequest> phase_slab_layer_stack_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabLayerStackEditIntent>& slab_stacks);

} // namespace sketch
