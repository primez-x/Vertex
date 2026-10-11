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
};

// Closed canonical v1 codec. All patch operations are explicit; set carries a
// value and unchanged/clear carry null. Pose component fields are always present
// (unsupplied components are null). Values are finite, scales positive, counts
// integral/nonnegative; property strings <=4096 bytes and local keys <=1024.
// No type/profile/host/context/identity/child creation or deletion authority.
[[nodiscard]] nlohmann::json encode_assembly_instance_property_edit_intent(
    const AssemblyInstancePropertyEditIntent&);
[[nodiscard]] AssemblyInstancePropertyEditIntent decode_assembly_instance_property_edit_intent(
    const nlohmann::json&);

// Derives one immutable whole-map result exclusively from the actual original
// map. 1..1000 sorted unique qualified targets; multiple distinct paths in one
// root are composed against that root's original expansion. All catalog rows,
// definitions, metadata, assets and unrelated owners remain in place. Equal
// sets and absent clears retain raw fields, quantities and authored row order.
// Only affected rows gain new pose envelopes; an embedded dialect needing new
// fields uses v7 without rewriting other rows. Existing override rows survive
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
