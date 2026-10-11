#pragma once

#include "sketch/phase_assembly_catalog_edit.hpp"

namespace sketch {

struct PhaseAssemblyCatalogPresentationReferenceGap {
    std::string entity_id;
    // Exact JSON pointer into the actual source entity properties/extensions.
    std::string field;
    std::string referenced_id;
    std::string reason;
    bool operator==(const PhaseAssemblyCatalogPresentationReferenceGap&) const = default;
};
struct PhaseAssemblyCatalogEditPresentationPlan {
    PhaseAssemblyCatalogEditPlan producer;
    // Sorted exact baseline overlay roster only. Ordinary retargets retain IDs.
    std::vector<std::string> required_overlay_ids;
    // Sorted exact baseline standalone assembly-extent entity roster.
    std::vector<std::string> required_dimension_ids;
    // No candidate is produced when these source-bound references need a
    // translation unsupported by the owning typed codec.
    std::vector<PhaseAssemblyCatalogPresentationReferenceGap> unsupported_references;
};
struct PhaseAssemblyCatalogEditPresentationAllocation {
    std::map<std::string, std::string, std::less<>> overlay_ids;
    std::map<std::string, std::string, std::less<>> dimension_ids;
    bool operator==(const PhaseAssemblyCatalogEditPresentationAllocation&) const = default;
};

// Pure inspection of complete actual source and source-bound catalog request.
// No caller-authored candidate, target remap, or preview confers authority.
[[nodiscard]] PhaseAssemblyCatalogEditPresentationPlan inspect_phase_assembly_catalog_edit_presentation_plan(
    const AssemblyDocumentEntities&, const nlohmann::json& catalog_request);
// Reconstructs the actual producer result through its typed allocation/replay,
// then patches only admitted typed presentation fields. Baseline rows remain
// raw exact; ordinary references retain their existing allocation identities.
// Exact unsupported gaps refuse completion. Fresh IDs include the producer's
// actual aliases, overlays and standalone dimensions, for the caller's retained-
// history lifetime fence. Qualified source/proposed dimension geometry is
// validated under complete-map batches before presentation copies are made.
[[nodiscard]] PhaseAssemblyCatalogEditResult replay_phase_assembly_catalog_edit_presentation_entities(
    const AssemblyDocumentEntities&, const nlohmann::json& catalog_request,
    const PhaseAssemblyCatalogEditAllocation&,
    const PhaseAssemblyCatalogEditPresentationAllocation&);

// Closed, bounded v2 overlay/dimension allocation; source replay checks both
// exact rosters. Empty dimension allocations retain historical v1 encoding
// and its overlays-only meaning.
[[nodiscard]] nlohmann::json encode_phase_assembly_catalog_edit_presentation_allocation(
    const PhaseAssemblyCatalogEditPresentationAllocation&);
[[nodiscard]] PhaseAssemblyCatalogEditPresentationAllocation decode_phase_assembly_catalog_edit_presentation_allocation(
    const nlohmann::json&);

} // namespace sketch
