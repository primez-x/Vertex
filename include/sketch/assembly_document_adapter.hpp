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
// Existing instances must pass the closed codec; unchanged typed values retain
// raw numeric forms, optional fields, v2 dialect and surviving path-row order.
// Changed values use the canonical codec, including removal of optional defaults.
// An absent instance permits creation; duplication may supply a fresh Entity.id.
// Document context and opaque properties/extensions remain on the source entity.
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
struct ArchitecturalMaterialSourceReference {
    std::string catalog_id;
    std::string material_id;
    bool operator==(const ArchitecturalMaterialSourceReference&) const = default;
};
// Sorted unique catalog/local-material pairs from architectural root assignments
// and wall/slab layer assignments. Validates the typed envelopes and layer stack,
// including its count bound, without resolving catalog existence or material rows.
// Root assignment extras and unrelated nested JSON are opaque.
[[nodiscard]] std::vector<ArchitecturalMaterialSourceReference>
    architectural_material_source_refs(const Entity& source);
// Requires a valid explicit destination document ID for every reached catalog.
// Patches only typed catalog_id slots; entity/local IDs, raw numeric forms, layer
// order, assignment extras and all other properties/extensions remain unchanged.
// Catalog transport and destination admission remain the caller's responsibility.
[[nodiscard]] Entity remap_architectural_material_source_refs(const Entity& source,
    const std::map<std::string, std::string, std::less<>>& catalog_mapping);
// One operation shares this budget across capture, inventory and both sides of
// remapping. Limits may be lowered, never raised. Failed work stays charged.
// Bytes are a conservative JSON encoding bound, not a serialized byte count;
// work includes quadratic profile topology and repeated graph/model validation.
struct AssemblyCatalogTransferBudget {
    std::size_t max_json_bytes{16777216};
    std::size_t max_json_nodes{262144};
    std::size_t max_validation_work{67108864};
    std::size_t consumed_json_bytes{};
    std::size_t consumed_json_nodes{};
    std::size_t consumed_validation_work{};
};
struct AssemblyCatalogSourceReferences {
    std::vector<std::string> hosted_entity_ids;
    std::vector<std::string> context_owner_ids;
    bool operator==(const AssemblyCatalogSourceReferences&) const = default;
};
// Raw shape, graph and work admission only; does not decode a model. Admit all
// catalogs in an operation before allowing the first semantic model decode.
void admit_complete_assembly_catalog_source(const Entity& source, AssemblyCatalogTransferBudget& budget);
// Reserves decoding of an existing catalog without treating its canonical
// owner references as transport candidates. Those references stay in place.
void admit_existing_assembly_catalog_work(const Entity& existing, AssemblyCatalogTransferBudget& budget);
// Complete catalog carriers and isolated catalog fields share these framing
// limits. Opaque strings (including escaped NUL) remain ordinary JSON; typed
// identity and model admission still enforce their own stricter contracts.
inline constexpr int assembly_catalog_transport_depth_limit = 68;
inline constexpr std::size_t assembly_catalog_transport_byte_limit = 16 * 1024 * 1024;
inline constexpr std::size_t assembly_catalog_transport_node_limit = 1'000'000;
[[nodiscard]] nlohmann::json parse_assembly_catalog_transport_json(std::string_view bytes);
// Validates the complete actual catalog after raw work admission. Inventories
// only persisted placement hosts and property/building/floor/layer owner slots.
// Other canonical owner references (including phase) are explicitly refused;
// opaque nested metadata is not scanned for reference-looking strings.
[[nodiscard]] AssemblyCatalogSourceReferences complete_assembly_catalog_source_refs(
    const Entity& source, AssemblyCatalogTransferBudget& budget);
// Actual embedded placement hosts use the same role contract in captured
// documents and detached source graphs. A registry/catalog/context is no host.
[[nodiscard]] bool is_complete_assembly_catalog_host_type(std::string_view type) noexcept;
// Requires mappings for the actual catalog owner and all reached hosts/context.
// Patches only Entity.id, placement.host_entity_id and the four context slots;
// complete rows, dialect/order, local identities, numeric forms and metadata
// remain raw. Destination snapshot admission remains the caller's responsibility.
[[nodiscard]] Entity remap_complete_assembly_catalog_source_refs(const Entity& source,
    const std::map<std::string, std::string, std::less<>>& catalog_owner_mapping,
    const std::map<std::string, std::string, std::less<>>& host_owner_mapping,
    const std::map<std::string, std::string, std::less<>>& context_owner_mapping,
    AssemblyCatalogTransferBudget& budget);
// Copies real requested assembly_model owners from an actual captured snapshot;
// validates reached hosts/context against that source's identities and roles.
// No catalog pruning, fabricated owners or destination authority is supplied.
[[nodiscard]] AssemblyDocumentEntities capture_complete_assembly_catalog_sources(
    const DocumentSnapshot& source, const std::vector<std::string>& catalog_ids,
    AssemblyCatalogTransferBudget& budget);
// Detached minimal catalogs for selected independent roots. Validates the entire
// source first; catalogs contain no legacy instances or host references.
[[nodiscard]] AssemblyDocumentEntities assembly_clipboard_dependencies(
    const DocumentSnapshot& source, const std::vector<std::string>& root_entity_ids);
// Validates before/after patching only Entity.id, instance.id and the catalog
// reference. Raw dialect, row order, numeric forms, local identities and opaque
// properties/extensions are retained. Both mappings must be present.
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
