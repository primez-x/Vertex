#pragma once

#include "sketch/assembly_instance_property_edit.hpp"

namespace sketch {

struct PhaseAssemblyInstanceReplacementRoot {
    AssemblyInstancePropertyEditTarget root; // Exact root, always empty part_path.
    std::string registry_id;
    std::string alternative_id; // Actual saved active choice.
    // Embedded only. Absent means a new companion for this registry is required.
    std::optional<std::string> membership_entity_id;
    bool operator==(const PhaseAssemblyInstanceReplacementRoot&) const = default;
};
struct PhaseAssemblyInstancePropertyEditPlan {
    std::vector<PhaseAssemblyInstanceReplacementRoot> replacement_roots;
    std::vector<AssemblyInstancePropertyEditTarget> ordinary_roots;
    std::vector<AssemblyInstancePropertyEditTarget> unchanged_roots;
    std::vector<std::string> required_membership_registry_ids;
    bool operator==(const PhaseAssemblyInstancePropertyEditPlan&) const = default;
};
struct PhaseAssemblyInstanceReplacementIdentity {
    // Independent: the same fresh entity/instance ID, no catalog. Embedded: fresh private
    // catalog + instance, no entity. Existing definition/material IDs stay local.
    std::optional<std::string> proposed_entity_id;
    std::optional<std::string> proposed_catalog_id;
    std::string proposed_instance_id;
    bool operator==(const PhaseAssemblyInstanceReplacementIdentity&) const = default;
};
using PhaseAssemblyInstanceReplacementIdentityMap =
    std::map<AssemblyInstancePropertyEditTarget, PhaseAssemblyInstanceReplacementIdentity>;
struct PhaseAssemblyInstancePropertyEditAllocation {
    PhaseAssemblyInstanceReplacementIdentityMap roots;
    // Exactly the plan's required registries: registry -> fresh required companion.
    std::map<std::string, std::string, std::less<>> membership_entity_ids;
    bool operator==(const PhaseAssemblyInstancePropertyEditAllocation&) const = default;
};
struct PhaseAssemblyInstancePropertyEditResult {
    AssemblyDocumentEntities entities;
    // Complete selected target roster, including unchanged/ordinary targets.
    // Replacement destinations retain each selected target's exact part path.
    std::map<AssemblyInstancePropertyEditTarget, AssemblyInstancePropertyEditTarget> original_to_proposed;
    std::vector<std::string> fresh_identity_ids;
};

// Reinspect grouped typed intents on the complete actual source, admitting both
// expansions/geometry, actual activity, constraints, hosts and Site frames.
// Declared proposed rows retain their own authority. Undeclared embedded rows
// inherit protection from their actual catalog or placement host; replacement
// copies the selected component only, retaining the unchanged actual host.
// Multiple independent registry/alternative cohorts compose; genuine overlap
// or conflicting actual catalog/host registry coupling refuses explicitly,
// including active proposed dependencies that a new companion cannot encode.
[[nodiscard]] PhaseAssemblyInstancePropertyEditPlan inspect_phase_assembly_instance_property_edit_plan(
    const AssemblyDocumentEntities&, std::span<const AssemblyInstancePropertyEditIntent>);
[[nodiscard]] PhaseAssemblyInstancePropertyEditResult replay_phase_assembly_instance_property_edits(
    const AssemblyDocumentEntities&, std::span<const AssemblyInstancePropertyEditIntent>,
    const PhaseAssemblyInstancePropertyEditAllocation&);
// Shared live/history collision fence, including catalog-local rows and IDs.
void validate_phase_assembly_instance_property_edit_fresh_identities(
    const AssemblyDocumentEntities&, const std::vector<std::string>& fresh_identity_ids);

struct PhaseAssemblyInstancePropertyEditAuthoring {
    std::vector<AssemblyInstancePropertyEditIntent> intents;
    PhaseAssemblyInstancePropertyEditAllocation allocation;
};
// Closed bounded v1 mathematical intents + exact allocation; no candidate map.
[[nodiscard]] nlohmann::json encode_phase_assembly_instance_property_edit_authoring(
    const PhaseAssemblyInstancePropertyEditAuthoring&);
[[nodiscard]] PhaseAssemblyInstancePropertyEditAuthoring decode_phase_assembly_instance_property_edit_authoring(
    const nlohmann::json&);

// Complete source/history/assets/save/editability fences, lifetime reservation
// through retained and undone history. Detached contribution, no publish token.
// Companion-bearing Documents remain quarantined until dependency integration.
class PhaseAssemblyInstancePropertyEditPreparation final {
public:
    [[nodiscard]] const nlohmann::json& request() const noexcept { return request_; }
    [[nodiscard]] const PhaseAssemblyInstancePropertyEditResult& result() const noexcept { return result_; }
    [[nodiscard]] const AssemblyDocumentEntities& preview_entities() const noexcept { return result_.entities; }
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept { return digest_; }
private:
    PhaseAssemblyInstancePropertyEditPreparation(nlohmann::json, PhaseAssemblyInstancePropertyEditResult);
    nlohmann::json request_;
    PhaseAssemblyInstancePropertyEditResult result_;
    std::string digest_;
    friend PhaseAssemblyInstancePropertyEditPreparation replay_phase_assembly_instance_property_edit_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
};
[[nodiscard]] nlohmann::json validate_phase_assembly_instance_property_edit_request(const nlohmann::json&);
[[nodiscard]] PhaseAssemblyInstancePropertyEditPreparation prepare_phase_assembly_instance_property_edit(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditAuthoring&);
[[nodiscard]] PhaseAssemblyInstancePropertyEditPreparation replay_phase_assembly_instance_property_edit_preparation(
    const DocumentSnapshot&, const nlohmann::json&);
void require_phase_assembly_instance_property_edit_source_current(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&);

} // namespace sketch
