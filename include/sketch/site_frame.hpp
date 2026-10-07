#pragma once

#include "sketch/project_organization.hpp"
#include "sketch/stair_semantics.hpp"

#include <cstddef>
#include <optional>
#include <tuple>

namespace sketch {

using SiteFrameEntities = std::map<std::string, Entity, std::less<>>;

// Physical metres, right-handed XY yaw and Z translation only. This frame
// cannot scale/shear geometry or grade. GeoreferencingContract is a separate
// world-XY -> declared projected-CRS mapping, applied only at that boundary.
struct SiteRigidTransform {
    Vec3 translation_m{};
    double rotation_radians{};
};
struct SiteVerticalDatum {
    std::string identifier;
    double height_at_origin_m{};
};
struct SiteFrame {
    SiteRigidTransform to_world;
    SiteVerticalDatum vertical_datum;
};
enum class SiteFrameMode { world, site, building };
struct SiteFrameIdentity {
    SiteFrameMode mode{SiteFrameMode::world};
    std::string property_id;
    std::string building_id;
    bool operator==(const SiteFrameIdentity&) const = default;
};
struct TerrainElevationBinding {
    enum class Mode { relative_site_origin, declared_absolute };
    Mode mode{Mode::relative_site_origin};
    std::string datum_identifier;
};
struct SiteFrameLimits {
    std::size_t maximum_entities{100000};
    std::size_t maximum_join_members{4096};
    std::size_t maximum_dependencies{8192};
    std::size_t maximum_dependency_bytes{64 * 1024 * 1024};
    // Total entries retained by owner/host/join dependency sets in one call;
    // bounds repeated large joins independently of each owner's limits.
    std::size_t maximum_cached_dependency_entries{1000000};
};

// Strict v1 JSON contracts. All required fields must exist; extra fields,
// unknown versions, blank identifiers and nonfinite values refuse.
// site_frame: {version:1,origin_m:[x,y,z],rotation_radians:r,
//   vertical_datum:{identifier:s,height_at_origin_m:h}}
// site_placement: {version:1,translation_m:[x,y,z],rotation_radians:r}
// presentation_frame: {version:1,mode:"world"|"site"|"building"}
// terrain_elevation_binding: {version:1,mode:"relative_site_origin"} or
//   {version:1,mode:"declared_absolute",datum_identifier:s}
[[nodiscard]] SiteFrame decode_site_frame(const nlohmann::json& value);
[[nodiscard]] SiteRigidTransform decode_building_site_placement(const nlohmann::json& value);
[[nodiscard]] SiteFrameMode decode_presentation_frame(const nlohmann::json& value);
[[nodiscard]] TerrainElevationBinding decode_terrain_elevation_binding(const nlohmann::json& value);

[[nodiscard]] Vec3 site_transform_point(Vec3 point, const SiteRigidTransform& transform);
[[nodiscard]] Vec3 site_transform_delta(Vec3 delta, const SiteRigidTransform& transform);
[[nodiscard]] Boundary site_transform_boundary(const Boundary& boundary, const SiteRigidTransform& transform);
// compose(outer,inner)(p) = outer(inner(p)).
[[nodiscard]] SiteRigidTransform compose_site_transforms(const SiteRigidTransform& outer,
                                                        const SiteRigidTransform& inner);
[[nodiscard]] SiteRigidTransform inverse_site_transform(const SiteRigidTransform& transform);

// Command-edge gesture: p' = scale * R_z(rotation) * p + translation.
// Adapter converts this value to ArchitecturalTransform; avoiding a layer
// dependency here keeps the frame contract usable by core/render/output.
struct SiteEditTransform {
    Vec3 translation_m{};
    double rotation_radians{};
    double scale{1.0};
};
[[nodiscard]] Vec3 site_edit_point(Vec3 point, const SiteEditTransform& transform);
// Exact F^-1 M_world F, including the origin terms for yaw and uniform scale.
[[nodiscard]] SiteEditTransform conjugate_site_edit(const SiteEditTransform& world,
                                                    const SiteRigidTransform& frame);

// A pure, captured placement result. It describes authored source coordinates
// -> presentation world. It never changes an Entity or measurement/calculation
// fact. Apply it once AFTER local host openings/rails/joins/assembly expansion
// and resolve_vertical_placement; ordinary floor editors keep the local frame.
// Do not apply this to already placed geometry. This digest binds the owner,
// relevant source containers/hosts/members and their complete metadata, not
// command authority: root must retain its full document_snapshot_digest guard.
struct SitePresentationPlacement {
    SiteFrameIdentity source_frame;
    SiteRigidTransform forward;
    SiteRigidTransform inverse;
    DrawingContext drawing_context;
    std::vector<std::string> dependency_ids;
    std::string dependency_digest;
    // Map overload leaves these empty/zero. Snapshot overload captures them;
    // identity/revision alone never proves snapshot equality.
    std::string snapshot_document_id;
    Revision snapshot_revision{};
};

// Child IDs are local to their persisted annotation_state owner. Keep both
// IDs typed: concatenated IDs cannot represent this namespace without collisions.
struct SiteAnnotationTarget {
    std::string owner_entity_id;
    std::string child_id;
    bool operator==(const SiteAnnotationTarget&) const = default;
    bool operator<(const SiteAnnotationTarget& other) const {
        return std::tie(owner_entity_id,child_id)<std::tie(other.owner_entity_id,other.child_id);
    }
};

// Context policy:
// * Authored building.site_placement enrolls descendant boundary/room/wall/
//   slab/roof/stair/rail/column/beam geometry in property * building pose.
//   Missing building placement keeps legacy world geometry unchanged even if
//   property.site_frame exists. Opening/hosted railing uses its host pose.
// * Both wall_join and roof_join require matching property/building membership
//   and one source frame (two legacy unassigned world members are compatible).
//   Member geometry is joined locally, then the cluster is placed once.
// * Independent assembly_instance root_transform is world by default; only
//   its explicit presentation_frame enrolls expanded geometry. No host or
//   organization metadata implicitly changes independent assembly coordinates.
// * Annotation children use their OWN layer context, not referenced geometry
//   context. Annotations, grids, references, measurement_linework and all other
//   nonarchitectural types remain world unless presentation_frame is explicit.
//   building/site binding uses the object's explicit container references;
//   this API never guesses a context from parent/target/geometry references.
// * Terrain ignores building placement. Explicit terrain_elevation_binding
//   uses property yaw/origin. Absolute h maps to originZ+h-datumHeight and
//   requires the exact property datum. Relative h maps to originZ+h. Missing
//   binding retains legacy world terrain. Terrain cannot also carry a
//   presentation_frame, which would introduce two competing Z contracts.
// Malformed/unknown explicit frames and unknown/contradictory container/host
// references throw std::invalid_argument; they never fall back to identity.
[[nodiscard]] SitePresentationPlacement resolve_site_presentation(
    const SiteFrameEntities& entities, std::string_view entity_id,
    const SiteFrameLimits& limits = {});
[[nodiscard]] SitePresentationPlacement resolve_site_presentation(
    const DocumentSnapshot& snapshot, std::string_view entity_id,
    const SiteFrameLimits& limits = {});
// One index and shared frame/dependency caches for the captured immutable map.
// Requested IDs must be distinct and bounded by maximum_entities. Results are
// identical to singular resolution, independent of requested ID order.
[[nodiscard]] std::map<std::string, SitePresentationPlacement, std::less<>>
resolve_site_presentations(const SiteFrameEntities& entities,
    std::span<const std::string> entity_ids, const SiteFrameLimits& limits = {});
[[nodiscard]] std::map<std::string, SitePresentationPlacement, std::less<>>
resolve_site_presentations(const DocumentSnapshot& snapshot,
    std::span<const std::string> entity_ids, const SiteFrameLimits& limits = {});
// Labels/symbols use their own placement.layer_id, never the owner's context
// or an override's geometry witness. Owner presentation_frame is the only
// supported frame namespace; annotation-state children have no frame property.
// Without it, legacy children remain world, including those without a layer.
// Explicit frames require every selected child to have a valid layer. The site
// namespace is validated separately before public annotation decoding; source
// remains unchanged. One resolver, decoded-owner index and shared budget per call.
[[nodiscard]] std::map<SiteAnnotationTarget,SitePresentationPlacement>
resolve_site_annotation_presentations(const SiteFrameEntities& entities,
    std::span<const SiteAnnotationTarget> targets, const SiteFrameLimits& limits = {});
[[nodiscard]] std::map<SiteAnnotationTarget,SitePresentationPlacement>
resolve_site_annotation_presentations(const DocumentSnapshot& snapshot,
    std::span<const SiteAnnotationTarget> targets, const SiteFrameLimits& limits = {});
// Validate persisted contracts and cross-frame joins at the Document boundary.
// Supported multi-owner geometric constraints must also share the same typed
// frame and rigid pose if any owner is enrolled in a non-world frame. All-world
// legacy relations remain valid; opaque future constraints gain no authority.
// This performs derived validation only and deliberately does not replace
// existing entity, geometry, history or vertical-placement validation.
void validate_document_site_frames(const SiteFrameEntities& entities,
                                   const SiteFrameLimits& limits = {});

} // namespace sketch
