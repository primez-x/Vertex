#pragma once

#include "sketch/assembly_document_adapter.hpp"

namespace sketch {

// Detached source-derived preview only. This seal grants no publication or
// companion-edit authority and does not remove companion quarantine.
class QualifiedEmbeddedAssemblyCopyPreparation final {
public:
    [[nodiscard]] const nlohmann::json& request() const noexcept { return request_; }
    [[nodiscard]] const AssemblyDocumentEntities& preview_entities() const noexcept { return entities_; }
    [[nodiscard]] Revision expected_revision() const noexcept { return revision_; }
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept { return digest_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
private:
    QualifiedEmbeddedAssemblyCopyPreparation(nlohmann::json, AssemblyDocumentEntities);
    nlohmann::json request_;
    AssemblyDocumentEntities entities_;
    std::string digest_, message_;
    Revision revision_{};
    friend QualifiedEmbeddedAssemblyCopyPreparation replay_qualified_embedded_assembly_copy_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
};

// Closed bounded v1 request; canonical target order is catalog/local source.
// Every target contains an explicit full operator and fresh catalog-local ID.
// Admitted numeric representations and raw source properties remain intact.
[[nodiscard]] nlohmann::json validate_qualified_embedded_assembly_copy_request(const nlohmann::json&);
[[nodiscard]] QualifiedEmbeddedAssemblyCopyPreparation prepare_qualified_embedded_assembly_copy(
    const DocumentSnapshot&, std::span<const EmbeddedAssemblyGroupTarget>,
    const ArchitecturalGroupTransform& fallback_transform,
    const std::map<std::string, std::string, std::less<>>& copied_dimension_ids,
    std::string message);
[[nodiscard]] QualifiedEmbeddedAssemblyCopyPreparation replay_qualified_embedded_assembly_copy_preparation(
    const DocumentSnapshot&, const nlohmann::json&);
void require_qualified_embedded_assembly_copy_source_current(
    const DocumentSnapshot&, const QualifiedEmbeddedAssemblyCopyPreparation&);
// Exclusive request-only envelope. Document independently authenticates and
// replays its actual source; this carries no raw preview entities or bypass.
[[nodiscard]] ApplyBoundaryConstraintChanges qualified_embedded_assembly_copy_command(
    const QualifiedEmbeddedAssemblyCopyPreparation&);

} // namespace sketch
