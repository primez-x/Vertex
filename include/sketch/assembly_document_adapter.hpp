#pragma once
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/document.hpp"
#include <span>
#include <string_view>
#include <utility>

namespace sketch {
using AssemblyDocumentEntities = std::map<std::string, Entity, std::less<>>;
struct AssemblyDocumentInstance {
    std::string assembly_catalog_id;
    AssemblyInstance instance;
    bool operator==(const AssemblyDocumentInstance&) const = default;
};
// Rebuilt presentation identity, independent of a displayed/synthetic entity ID.
// A document root is explicit; absence identifies an embedded catalog instance.
// Paths retain the expanded profile's exact local identities, including an
// empty path for profiles owned directly by the root type.
struct AssemblyProfilePresentationIdentity {
    std::string catalog_id;
    std::string instance_id;
    std::optional<std::string> document_entity_id;
    std::vector<std::string> part_path;
    std::string type_id;
    std::string profile_id;
    bool operator==(const AssemblyProfilePresentationIdentity&) const = default;
};
[[nodiscard]] nlohmann::json encode_assembly_profile_presentation_identity(
    const AssemblyProfilePresentationIdentity& identity);
// Requires the versioned, qualified identity; no unqualified legacy fallback.
// Additional provenance fields on native source records are ignored.
[[nodiscard]] AssemblyProfilePresentationIdentity decode_assembly_profile_presentation_identity(
    const nlohmann::json& value);
[[nodiscard]] nlohmann::json assembly_profile_presentation_key(const std::string& catalog_id,
    const AssemblyInstance& instance, const AssemblyExpandedProfile& profile,
    const std::optional<std::string>& document_entity_id = std::nullopt);
using EmbeddedAssemblyPresentationIds =
    std::map<std::pair<std::string, std::string>, std::string>;
// Render-only IDs retain a plain historical alias only when unique and unused
// by a persisted entity or annotation child. Compute once for a captured source;
// this lightweight index does not expand assembly geometry. Source admission
// remains the caller's responsibility; malformed identity records are refused.
[[nodiscard]] EmbeddedAssemblyPresentationIds embedded_assembly_presentation_ids(
    const AssemblyDocumentEntities& entities);
[[nodiscard]] std::string embedded_assembly_presentation_id(const AssemblyDocumentEntities& entities,
    const std::string& catalog_id, const std::string& instance_id);
// Resolves only the exact currently generated ID; persisted IDs take precedence.
[[nodiscard]] std::optional<AssemblyDocumentInstance> resolve_embedded_assembly_presentation(
    const AssemblyDocumentEntities& entities, std::string_view render_id);
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
struct EmbeddedAssemblyGroupTarget {
    std::string catalog_id;
    std::string instance_id;
    // A fresh identity in this catalog's embedded instance namespace. The
    // source record remains unchanged when this is present.
    std::optional<std::string> copy_instance_id;
    // Optional independently captured operator for this presentation's frame.
    // Absence retains the command's shared operator meaning.
    std::optional<ArchitecturalGroupTransform> transform{};
};
// Qualified embedded geometric roots receive the shared operator unless their
// target supplies an independently captured operator in its authored frame:
// positive uniform scale and yaw, global X/Y flips, then XYZ offset. Each
// catalog is upserted once, retaining raw model/instance data and metadata.
// Nonidentity poses become explicit world root transforms; identity Copy
// retains the source pose representation. Identity targets retain their exact
// raw pose even when other selected roots change. Metadata-only instances
// are refused.
// Full canonical document admission precedes return; no snapshot is mutated.
[[nodiscard]] ApplyEntityChanges embedded_assembly_group_transform_command(
    const DocumentSnapshot& source, std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& transform, Revision expected_revision);
// Materialize each selected embedded root as a fresh independently editable
// document instance sharing its immutable source catalog. Here copy_instance_id
// is required and names a fresh document entity, not an appended catalog row.
// Source catalogs/instances stay unchanged; the new root inherits catalog
// context and non-model metadata, with an explicit world presentation frame
// for its already world-authored pose. Per-target operators have the same
// fallback meaning as transform. Callers register phase/page membership and
// clone selected presentation through their complete authored command.
[[nodiscard]] ApplyEntityChanges embedded_assembly_group_copy_command(
    const DocumentSnapshot& source, std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& transform, Revision expected_revision);
} // namespace sketch
