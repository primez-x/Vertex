#pragma once

#include "sketch/assembly_catalog_phase_edit.hpp"

namespace sketch {

enum class PhaseAssemblyCatalogDefinitionKind { type, material };
enum class PhaseAssemblyCatalogDefinitionDisposition { unchanged, global, private_only, not_applied };
struct PhaseAssemblyCatalogDefinitionEffect {
    std::string catalog_id;
    PhaseAssemblyCatalogDefinitionKind kind{};
    std::string definition_id;
    PhaseAssemblyCatalogDefinitionDisposition disposition{};
    // Actual active isolated AFTER expansions reaching this definition. Merely
    // copying an unused definition into a private catalog supplies no effect.
    std::size_t active_private_consumer_count{};
};
struct PhaseAssemblyCatalogIsolationRoot {
    AssemblyInstancePropertyEditTarget root;
    bool baseline_replacement{};
    // Actual saved cohort. Absence means an ordinary unregistered consumer.
    std::optional<std::string> registry_id;
    std::optional<std::string> alternative_id;
    // Actual companion used for embedded remapping/replacement, if present.
    std::optional<std::string> membership_entity_id;
};
struct PhaseAssemblyCatalogEditPlan {
    AssemblyCatalogPhaseEditPlan source;
    // Sorted exact changed ACTIVE roots only; inactive roots never allocate.
    // Empty when source.ordinary_replay_safe permits shared definition replay.
    std::vector<PhaseAssemblyCatalogIsolationRoot> isolation_roots;
    std::vector<std::string> required_membership_registry_ids;
    // Sorted catalog/kind/local ID inventory of EVERY requested definition.
    std::vector<PhaseAssemblyCatalogDefinitionEffect> definition_effects;
    // Whole residual, independently admitted by the source-bound foundation;
    // never a row-by-row guess at graph validity or protected preservation.
    std::optional<nlohmann::json> admitted_global_request;
    std::string global_scope_diagnostic;
};
struct PhaseAssemblyCatalogRootIdentity {
    std::string private_catalog_id;
    // Only a baseline independent owner needs a fresh document/instance ID.
    std::optional<std::string> replacement_entity_id;
    // Required for every embedded root and baseline independent root; absent
    // for an ordinary independent root, which keeps its actual owner/row ID.
    std::optional<std::string> replacement_instance_id;
    bool operator==(const PhaseAssemblyCatalogRootIdentity&) const = default;
};
struct PhaseAssemblyCatalogEditAllocation {
    std::map<AssemblyInstancePropertyEditTarget, PhaseAssemblyCatalogRootIdentity> roots;
    std::map<std::string, std::string, std::less<>> membership_entity_ids;
    bool operator==(const PhaseAssemblyCatalogEditAllocation&) const = default;
};
struct PhaseAssemblyCatalogEditResult {
    AssemblyDocumentEntities entities;
    // Complete source root inventory; unchanged roots map to themselves.
    std::map<AssemblyInstancePropertyEditTarget, AssemblyInstancePropertyEditTarget> original_to_proposed;
    std::vector<std::string> fresh_identity_ids; // Sorted, unique actual allocations.
    std::vector<PhaseAssemblyCatalogDefinitionEffect> definition_effects;
};

// Pure complete-map producer. Bounded source admission and foundation authority
// precede allocation/copy; allocation must exactly match actual active impacts.
// Copies real changed definitions and raw envelopes/rows into private catalogs,
// preserving baseline/inactive originals. No Document, native, preview, command,
// reference transport, lifetime/history reservation or publication authority.
// Inspect reports concrete residual graph/scope gaps. Replay refuses the WHOLE
// request if any actually changed definition is not_applied. Private-only edits
// must be reached by genuine active isolated consumers; unused edits cannot vanish.
[[nodiscard]] PhaseAssemblyCatalogEditPlan inspect_phase_assembly_catalog_edit_plan(
    const AssemblyDocumentEntities&, const nlohmann::json& catalog_request);
[[nodiscard]] PhaseAssemblyCatalogEditResult replay_phase_assembly_catalog_edit_entities(
    const AssemblyDocumentEntities&, const nlohmann::json& catalog_request,
    const PhaseAssemblyCatalogEditAllocation&);

// Closed bounded v1 allocation. Exact root records are explicit qualified pairs,
// never delimiter identities. Decode checks sorted/injective fresh inventory;
// replay additionally checks allocation roles against actual source cohorts.
[[nodiscard]] nlohmann::json encode_phase_assembly_catalog_edit_allocation(
    const PhaseAssemblyCatalogEditAllocation&);
[[nodiscard]] PhaseAssemblyCatalogEditAllocation decode_phase_assembly_catalog_edit_allocation(
    const nlohmann::json&);
struct PhaseAssemblyCatalogEditAuthoring {
    nlohmann::json catalog_request;
    PhaseAssemblyCatalogEditAllocation allocation;
};
[[nodiscard]] nlohmann::json encode_phase_assembly_catalog_edit_authoring(
    const PhaseAssemblyCatalogEditAuthoring&);
[[nodiscard]] PhaseAssemblyCatalogEditAuthoring decode_phase_assembly_catalog_edit_authoring(
    const nlohmann::json&);

} // namespace sketch
