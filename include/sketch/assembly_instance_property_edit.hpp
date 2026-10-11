#pragma once

#include "sketch/assembly_document_adapter.hpp"

#include <span>
#include <tuple>

namespace sketch {

enum class AssemblyInstancePropertyPatchOperation { unchanged, set, clear };

template<class T> struct AssemblyInstancePropertyPatch {
    AssemblyInstancePropertyPatchOperation operation{AssemblyInstancePropertyPatchOperation::unchanged};
    // Only set carries a value. Clear removes this instance's override.
    std::optional<T> value;
};

// These are actual source identities, never displayed aliases. An empty path
// selects the root; a nonempty path selects an existing expanded part node.
// Independent roots must name their document entity as well as their catalog
// and instance. Embedded rows have no document_entity_id.
struct AssemblyInstancePropertyEditTarget {
    std::string catalog_id;
    std::string instance_id;
    std::optional<std::string> document_entity_id;
    std::vector<std::string> part_path;
    bool operator==(const AssemblyInstancePropertyEditTarget&) const = default;
    bool operator<(const AssemblyInstancePropertyEditTarget& other) const {
        return std::tie(catalog_id, instance_id, document_entity_id, part_path) <
            std::tie(other.catalog_id, other.instance_id, other.document_entity_id, other.part_path);
    }
};

struct AssemblyInstancePosePropertyPatch {
    AssemblyInstancePropertyPatchOperation operation{AssemblyInstancePropertyPatchOperation::unchanged};
    // Set changes supplied components in their authoritative parent frame.
    // Root: independent root_transform, embedded root_transform, or actual
    // legacy placement (retaining its host). Path: local part/path transform.
    // Vertical scale multiplies uniform XY scale, as in AssemblyTransform.
    std::optional<double> x_metres, y_metres, z_metres;
    std::optional<double> rotation_radians, scale, vertical_scale;
    std::optional<bool> mirrored_y;
    // Clear is supported only on a path and restores its source part pose.
    // An independent root's required pose cannot be cleared.
};

struct AssemblyInstancePropertyEditIntent {
    AssemblyInstancePropertyEditTarget target;
    std::map<std::string, AssemblyInstancePropertyPatch<std::string>> properties;
    std::map<std::string, AssemblyInstancePropertyPatch<std::string>> materials;
    std::map<std::string, AssemblyInstancePropertyPatch<AssemblyQuantityProperty>> quantities;
    AssemblyInstancePosePropertyPatch pose;
    // Only the selected node's own solid profiles. Set may be empty to suppress
    // them while other node profiles retain an actual solid; clear returns to
    // the shared type without changing its definition.
    AssemblyInstancePropertyPatch<std::vector<AssemblyProfile>> profile_geometry;
};

// Closed canonical v1/v2 codec. V2 adds profile_geometry and is emitted only
// for a non-unchanged profile operation. V1 rejects that field. All operations
// are explicit; set carries a value and unchanged/clear carry null. Pose fields are always present
// (unsupplied components are null). Values are finite, scales positive, counts
// integral/nonnegative; property strings <=4096 bytes and local keys <=1024.
// No shared type/host/context/identity/child creation or deletion authority.
[[nodiscard]] nlohmann::json encode_assembly_instance_property_edit_intent(
    const AssemblyInstancePropertyEditIntent&);
[[nodiscard]] AssemblyInstancePropertyEditIntent decode_assembly_instance_property_edit_intent(
    const nlohmann::json&);

struct AssemblyInstancePropertyEditImpact {
    // Exact qualified root identity; part_path is always empty.
    AssemblyInstancePropertyEditTarget root;
    bool changed{};
    std::vector<std::string> protected_registry_ids;
    std::vector<std::string> protected_host_registry_ids;
};

// Read-only consumer foundation for standalone-owner and embedded-catalog
// baseline membership. Requires the complete actual original map and the same
// 1..1000 sorted unique closed typed targets as replay. All selected paths in
// one root compose against its original expansion. Returns sorted qualified
// root impacts, including no-ops, after source/final typed expansion and genuine
// geometry admission. Only active-alternative baseline registries are reported:
// the actual document owner for independent roots, catalog for embedded rows,
// and exact placement host separately. Inactive sources and overlapping registry
// memberships refuse. No input mutation, snapshot fabrication, publishable map,
// replacement authorization, dependency completion or publication is supplied.
// Enclosing replacement producers must establish those prerequisites themselves;
// this standalone/embedded membership foundation remains runtime-unverified.
[[nodiscard]] std::vector<AssemblyInstancePropertyEditImpact> inspect_assembly_instance_property_edits(
    const AssemblyDocumentEntities&, std::span<const AssemblyInstancePropertyEditIntent>);

// Derives one immutable whole-map result exclusively from the actual original
// map. 1..1000 sorted unique qualified targets; multiple distinct paths in one
// root are composed against that root's original expansion. All catalog rows,
// definitions, metadata, assets and unrelated owners remain in place. Equal
// sets and absent clears retain raw fields, quantities and authored row order.
// Only affected rows gain new envelopes; geometry requires embedded v8 or
// independent v3 without rewriting other rows or downconverting. Existing
// override rows survive
// even when emptied. Actual declared slots, material identities and quantity
// dimensions are checked. Source/final expansion, saved-active constraints,
// frame/host dependencies and native profile or genuine host-copy geometry are
// admitted; no raw candidate is accepted as proof. Protected/inactive changes
// refuse; unchanged protected members may be retained. Full baseline replacement
// and dependency/annotation completion remain the enclosing workflow's job.
[[nodiscard]] AssemblyDocumentEntities replay_assembly_instance_property_entities(
    const AssemblyDocumentEntities&, std::span<const AssemblyInstancePropertyEditIntent>);

// A sealed drawing contribution, not a Document token or publication command.
// Captures complete history/assets/navigation/names/save/editability authority.
class AssemblyInstancePropertyEditPreparation final {
public:
    [[nodiscard]] const nlohmann::json& request() const noexcept { return request_; }
    [[nodiscard]] const AssemblyDocumentEntities& preview_entities() const noexcept { return entities_; }
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept { return digest_; }
private:
    AssemblyInstancePropertyEditPreparation(nlohmann::json, AssemblyDocumentEntities);
    nlohmann::json request_;
    AssemblyDocumentEntities entities_;
    std::string digest_;
    friend AssemblyInstancePropertyEditPreparation replay_assembly_instance_property_edit_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
};
[[nodiscard]] nlohmann::json validate_assembly_instance_property_edit_request(const nlohmann::json&);
[[nodiscard]] AssemblyInstancePropertyEditPreparation prepare_assembly_instance_property_edit(
    const DocumentSnapshot&, std::span<const AssemblyInstancePropertyEditIntent>);
[[nodiscard]] AssemblyInstancePropertyEditPreparation replay_assembly_instance_property_edit_preparation(
    const DocumentSnapshot&, const nlohmann::json&);
void require_assembly_instance_property_edit_source_current(
    const DocumentSnapshot&, const AssemblyInstancePropertyEditPreparation&);

} // namespace sketch
