#pragma once

#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/assembly_phase_membership.hpp"

namespace sketch {

// Existing definitions only. No row, owner, host, membership or allocation
// authority. Types/materials in each list must have sorted unique actual IDs.
struct AssemblyCatalogPhaseEditIntent {
    std::string catalog_id;
    std::vector<AssemblyType> replacement_types;
    std::vector<AssemblyMaterial> replacement_materials;
};

enum class AssemblyCatalogPhaseEditCohort { active_ordinary, baseline_protected, inactive_retained };

struct AssemblyCatalogPhaseEditBinding {
    std::string owner_id;
    std::string registry_id;
    std::optional<std::string> active_alternative;
    std::optional<ModelPhase> state;
    bool baseline{};
    // Present only for a qualified embedded root's actual companion.
    std::optional<std::string> membership_entity_id;
};

struct AssemblyCatalogPhaseEditRootImpact {
    // Empty part_path, exact embedded pair or independently persisted root.
    AssemblyInstancePropertyEditTarget root;
    AssemblyCatalogPhaseEditCohort cohort{AssemblyCatalogPhaseEditCohort::active_ordinary};
    bool dependency_changed{};
    bool expansion_changed{};
    // Includes type/material appearance and names absent from expansion totals.
    bool definitions_changed{};
    std::vector<std::string> before_type_ids, after_type_ids;
    std::vector<std::string> before_material_ids, after_material_ids;
    std::vector<std::string> host_closure;
    std::vector<AssemblyCatalogPhaseEditBinding> phase_bindings;
    AssemblyExpansion before, after;
};

struct AssemblyCatalogPhaseEditMaterialConsumerImpact {
    // Actual architectural owner and unique reached material pair, never an
    // assembly root, render alias or allocation.
    std::string owner_id;
    std::string catalog_id;
    std::string material_id;
    AssemblyCatalogPhaseEditCohort cohort{AssemblyCatalogPhaseEditCohort::active_ordinary};
    bool definitions_changed{};
    // Actual owner and applicable typed physical hosts. Proposed owners retain
    // edit permission on baseline hosts; host inactivity still decides scope.
    std::vector<AssemblyCatalogPhaseEditBinding> phase_bindings;
};

struct AssemblyCatalogPhaseEditPlan {
    std::string source_entities_digest;
    // Complete source root inventory, including unaffected support roots.
    std::vector<AssemblyCatalogPhaseEditRootImpact> roots;
    // Indices into roots. These changes need the enclosing producer's real
    // private catalog/proposed-root replacement; inspect never discards them.
    std::vector<std::size_t> replacement_required_roots;
    bool ordinary_replay_safe{};
    // v2 actual architectural root/layer materials, including support consumers.
    // Appended for aggregate compatibility; v1 retains its root-only meaning.
    std::vector<AssemblyCatalogPhaseEditMaterialConsumerImpact> material_consumers;
};

// Closed canonical source-bound request. Captures the actual complete entity
// map digest, exact original catalog binding and original edited definitions.
// Carries canonical typed definition replacements, never a candidate map. New
// preparation emits v2; validation/replay also retain historical v1 requests.
// Complete raw map/catalog work is admitted before semantic decoding.
[[nodiscard]] nlohmann::json prepare_assembly_catalog_phase_edit_request(
    const AssemblyDocumentEntities&, const std::vector<AssemblyCatalogPhaseEditIntent>&);
[[nodiscard]] nlohmann::json validate_assembly_catalog_phase_edit_request(const nlohmann::json&);

// Pure analytical inspection: source/candidate raw work admission precedes
// model/phase expansion. Expands every embedded and independent root using
// shared bounded budgets, including transitive nested/profile dependencies.
// Actual companion, registry choices, independent owner and inherited opening/
// corner host closure establish root cohorts. v2 also inventories all supported
// architectural root/layer materials under actual owner and typed host cohorts.
// No native or Document work occurs. Physical consumers grant no replacement
// authority.
[[nodiscard]] AssemblyCatalogPhaseEditPlan inspect_assembly_catalog_phase_edit(
    const AssemblyDocumentEntities&, const nlohmann::json& request);

// Re-derives from the bound original. Only ordinary edits whose original
// protected/inactive root expansions and reached definitions remain exact may
// replay. Otherwise inspect's replacement plan remains available and replay
// refuses. v2 also preserves protected/inactive architectural reached materials.
// Unrelated rows/definitions, dialect, envelopes and entities stay raw.
// No allocation, phase mutation, command or publication authority is supplied.
[[nodiscard]] AssemblyDocumentEntities replay_assembly_catalog_phase_edit_entities(
    const AssemblyDocumentEntities&, const nlohmann::json& request);

// Current-authoring safeguard for legacy/raw catalog and instance writers.
// This comparison grants no candidate authority and does not reinterpret old
// retained commands. Actual baseline-protected/inactive roots must retain raw
// instance/envelope bindings, analytical expansion and reached definitions.
// Actual protected/inactive architectural material references also preserve
// their original raw material definitions and catalog carrier envelopes.
void validate_current_assembly_catalog_phase_preservation(
    const AssemblyDocumentEntities& source, const AssemblyDocumentEntities& candidate);

} // namespace sketch
