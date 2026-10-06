#pragma once
#include "sketch/assembly_model.hpp"
#include "sketch/document.hpp"

namespace sketch {
using AssemblyDocumentEntities = std::map<std::string, Entity, std::less<>>;
struct AssemblyDocumentInstance {
    std::string assembly_catalog_id;
    AssemblyInstance instance;
    bool operator==(const AssemblyDocumentInstance&) const = default;
};
// Only the adapter's envelope fields are replaced. Document context and opaque
// properties/extensions remain on the source entity.
[[nodiscard]] Entity encode_document_assembly_instance(const Entity& source,
    const AssemblyDocumentInstance& value);
[[nodiscard]] AssemblyDocumentInstance decode_document_assembly_instance(const Entity& entity);
[[nodiscard]] AssemblyExpansion expand_document_assembly_instance(const Entity& entity,
    const AssemblyDocumentEntities& entities, AssemblyExpansionBudget& budget);
// All catalogs, including unused catalogs/types, are decoded before expansion.
// The caller's budget is updated only if the complete document succeeds.
[[nodiscard]] std::map<std::string, AssemblyExpansion, std::less<>>
    expand_document_assembly_instances(const AssemblyDocumentEntities& entities,
        AssemblyExpansionBudget& budget);
void validate_document_assembly_instances(const AssemblyDocumentEntities& entities);
// Detached minimal catalogs for selected independent roots. Validates the entire
// source first; catalogs contain no legacy instances or host references.
[[nodiscard]] AssemblyDocumentEntities assembly_clipboard_dependencies(
    const DocumentSnapshot& source, const std::vector<std::string>& root_entity_ids);
// Only document/root/catalog identities are remapped; local identities and
// opaque properties/extensions are retained. Both mappings must be present.
[[nodiscard]] Entity remap_independent_assembly_instance(const Entity& source,
    const std::map<std::string, std::string>& identity_mapping);
struct AssemblyDocumentTypeUpdateImpact {
    std::string entity_id;
    AssemblyExpansion before;
    AssemblyExpansion after;
    AssemblyDocumentInstance retained_overrides;
};
// Includes all external root instances whose expansion directly or indirectly
// uses the changed type, including instances retaining equal-value overrides.
[[nodiscard]] std::vector<AssemblyDocumentTypeUpdateImpact> preview_document_assembly_type_update(
    const AssemblyDocumentEntities& entities, const std::string& catalog_id, AssemblyType replacement);
[[nodiscard]] ApplyEntityChanges assembly_instance_create_command(const DocumentSnapshot& source,
    Entity entity, const AssemblyDocumentInstance& value, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges assembly_instance_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const AssemblyDocumentInstance& value, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges assembly_instance_remove_command(const DocumentSnapshot& source,
    const std::string& entity_id, Revision expected_revision);
// Validates every external instance against the replacement catalog before
// returning one catalog upsert. No input snapshot is mutated.
[[nodiscard]] ApplyEntityChanges independent_assembly_type_update_command(const DocumentSnapshot& source,
    const std::string& catalog_id, AssemblyType replacement, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges independent_assembly_type_remove_command(const DocumentSnapshot& source,
    const std::string& catalog_id, const std::string& type_id, Revision expected_revision);
} // namespace sketch
