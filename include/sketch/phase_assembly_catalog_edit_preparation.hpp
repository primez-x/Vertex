#pragma once

#include "sketch/phase_assembly_catalog_edit_presentation.hpp"

namespace sketch {

// Shared bounded admission for source-bound assembly authoring preparations.
// Visits the complete retained and undone source, assets and typed replay work
// before codecs, digests or copies; requires an editable actual snapshot.
// This establishes resource/input admission, never publication authority.
void admit_retained_assembly_authoring_source(const DocumentSnapshot&);

// Detached, sealed source-derived contribution. No publication token or
// fabricated Document snapshot; companion quarantine remains in force.
class PhaseAssemblyCatalogEditPreparation final {
public:
    [[nodiscard]] const nlohmann::json& request() const noexcept { return request_; }
    [[nodiscard]] const PhaseAssemblyCatalogEditResult& result() const noexcept { return result_; }
    [[nodiscard]] const AssemblyDocumentEntities& preview_entities() const noexcept { return result_.entities; }
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept { return digest_; }
    [[nodiscard]] Revision expected_revision() const noexcept { return revision_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
private:
    PhaseAssemblyCatalogEditPreparation(nlohmann::json, PhaseAssemblyCatalogEditResult);
    nlohmann::json request_;
    PhaseAssemblyCatalogEditResult result_;
    std::string digest_, message_;
    Revision revision_{};
    friend PhaseAssemblyCatalogEditPreparation replay_phase_assembly_catalog_edit_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
};

// Closed bounded v1 full snapshot request. Inner catalog/typed allocations
// retain their exact producer dialects; no caller-authored candidate authority.
[[nodiscard]] nlohmann::json validate_phase_assembly_catalog_edit_request(const nlohmann::json&);
[[nodiscard]] PhaseAssemblyCatalogEditPreparation prepare_phase_assembly_catalog_edit(
    const DocumentSnapshot&, const PhaseAssemblyCatalogEditAuthoring&,
    const PhaseAssemblyCatalogEditPresentationAllocation&, std::string message);
[[nodiscard]] PhaseAssemblyCatalogEditPreparation replay_phase_assembly_catalog_edit_preparation(
    const DocumentSnapshot&, const nlohmann::json&);
void require_phase_assembly_catalog_edit_source_current(
    const DocumentSnapshot&, const PhaseAssemblyCatalogEditPreparation&);

} // namespace sketch
