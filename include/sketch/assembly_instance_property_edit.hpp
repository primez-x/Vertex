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
    // the shared type without changing its definition. Those historical edits
    // require anonymous source geometry. Identified Set retains the complete
    // actual own-source profile roster, rings/edges and topology; only profile
    // material_slot/elevation_m/height_m may change. It may materialize an own
    // override from the resolved type. These historical lanes refuse identified
    // redraw/clear; explicit reviewed geometry below supplies separate authority.
    AssemblyInstancePropertyPatch<std::vector<AssemblyProfile>> profile_geometry;
    // Explicit initial identity authoring only: Set must retain the complete
    // actual own-source roster and every geometry/scalar/material field, keep
    // existing topology unchanged and identify at least one anonymous profile.
    // New ring/segment/vertex identities are profile-local; the actual same-ID
    // shared type's topology is reserved even when an anonymous override hides it.
    // This does not authorize redraw, additions, removals or clear-to-type.
    bool initialize_profile_topology{false};
    // Explicit geometry authority for an actual identified own-profile source.
    // Set supplies the complete final roster; unchanged same-ID XY and holes
    // retain exact topology, including anonymous rows in a mixed source, while
    // redraw/new rows are identified and require entirely
    // fresh profile-local ring/segment/vertex identities. Clear returns to the
    // exact selected type, which may be anonymous. Clear also admits an
    // anonymous own override hiding actual identified type profiles.
    // Mutually exclusive with
    // initial identity authoring. Enclosing reviewed workflows own dimension
    // correspondence/removal, dependencies and publication.
    bool review_profile_topology{false};
    // Atomic initial identity authoring plus actual planar-affine deformation.
    // Set retains the complete actual own-source profile roster, source rings,
    // sweeps, scalars and materials. Unchanged maps retain exact topology;
    // each changed map requires entirely fresh profile-local identities,
    // reserving actual same-ID own and hidden shared-type topology. At least
    // one actually deformed anonymous source profile receives initial IDs.
    // Representation-only map changes do not supply deformation authority.
    // Mutually exclusive with both topology authorities above.
    bool initialize_deformed_profile_topology{false};
    // Reviewed complete own-profile geometry editing from an actual entirely
    // anonymous own source, including mapped or empty sources. Set may redraw,
    // add/remove rows or change scalars/materials. Same-ID source rings and an
    // equal exact affine operator retain topology, even after map reanchoring;
    // genuinely changed/new rows require entirely fresh identified topology,
    // reserving actual same-ID own and hidden type identities. Unchanged
    // anonymous geometry cannot acquire initial identities through this lane.
    // Clear requires an actual anonymous own override and restores the exact
    // type, which may be identified. Enclosing reviewed workflows own dimension
    // correspondence/removal and publication. Mutually exclusive with all
    // three topology authorities above.
    bool review_initial_profile_geometry{false};
};

// Closed canonical v1/v2/v3/v4/v5/v6/v7 codec. V2 adds anonymous profile_geometry and is
// emitted for a non-unchanged anonymous profile operation. V3 requires Set
// containing identified profiles and explicit topology-preserving authority;
// source replay checks the exact selected node's own geometry. V4 requires the
// literal initialize_profile_topology:true field and identified Set; actual
// source replay admits only initial identity authoring as specified above.
// V5 requires literal review_profile_topology:true and non-unchanged geometry;
// actual source replay admits only the reviewed geometry authority above.
// V6 requires literal initialize_deformed_profile_topology:true and identified
// Set; actual source replay admits only the atomic initial affine authority above.
// V7 requires literal review_initial_profile_geometry:true and non-unchanged
// geometry; actual source replay admits only reviewed anonymous-source geometry.
// Scalar-only or empty Set need not contain newly identified profiles.
// Historical dialects reject the new flags and cannot acquire their authority. V1
// rejects the geometry field; historical V2 rejects identified profiles. All
// operations are explicit; set carries a value and unchanged/clear carry null.
// Pose fields are always present (unsupplied components are null). Values are
// finite, scales positive, counts
// integral/nonnegative; property strings <=4096 bytes and local keys <=1024.
// No shared type/host/context/root or assembly-child identity creation/deletion
// authority; v5/v6/v7 change only the selected node's own profile geometry.
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
// Request v7 requires at least one intent v7 and permits compatible intents
// v1..v6. Requests v1..v6 reject intent v7.
// Request v6 requires at least one intent v6 and permits compatible intents
// v1..v5. Requests v1..v5 reject intent v6. Request v5 requires at least one
// intent v5 and permits compatible intents v1..v4. Requests v1..v4 reject
// intent v5. Request v4 requires at
// least one intent v4 and permits compatible intents
// v1/v2/v3. Request v3 is manufactured only when an intent v3 is present and no
// intent v4 is present. Requests v1/v2 retain historical authority and reject
// intent v3; requests v1/v2/v3 reject intent v4. Every request dialect uses
// the same complete source snapshot, authoring, entity and save-state fences.
[[nodiscard]] AssemblyInstancePropertyEditPreparation prepare_assembly_instance_property_edit(
    const DocumentSnapshot&, std::span<const AssemblyInstancePropertyEditIntent>);
[[nodiscard]] AssemblyInstancePropertyEditPreparation replay_assembly_instance_property_edit_preparation(
    const DocumentSnapshot&, const nlohmann::json&);
void require_assembly_instance_property_edit_source_current(
    const DocumentSnapshot&, const AssemblyInstancePropertyEditPreparation&);

} // namespace sketch
