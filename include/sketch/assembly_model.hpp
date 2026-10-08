#pragma once

#include "sketch/geometry.hpp"

#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

// Explicit SI dimensions; values are per instance, never inferred from geometry.
enum class AssemblyQuantityUnit { count, metre, square_metre, cubic_metre, kilogram };
struct AssemblyQuantityProperty {
    double value{};
    AssemblyQuantityUnit unit{AssemblyQuantityUnit::count};
    bool operator==(const AssemblyQuantityProperty&) const = default;
};
struct AssemblyMaterial {
    std::string id;
    std::string name;
    // Optional opaque surface color, encoded as #RRGGBB in sRGB.
    std::optional<std::string> color_srgb;
    bool operator==(const AssemblyMaterial&) const = default;
};
struct AssemblyPoint3 {
    double x{}, y{}, z{};
    bool operator==(const AssemblyPoint3&) const = default;
};
// Optional local Y reflection, positive uniform scale, yaw about local Z,
// then translation in parent metres: t + s R(yaw) D_y^mirrored_y p.
struct AssemblyTransform {
    AssemblyPoint3 translation_m{};
    double rotation_radians{};
    double scale{1.0};
    bool mirrored_y{false};
    bool operator==(const AssemblyTransform&) const = default;
};
[[nodiscard]] AssemblyPoint3 transform_assembly_point(AssemblyPoint3 point, const AssemblyTransform& transform);
[[nodiscard]] AssemblyTransform compose_assembly_transform(const AssemblyTransform& parent, const AssemblyTransform& local);
struct AssemblyProfile {
    std::string id;
    Boundary outer;
    std::vector<Boundary> holes;
    double elevation_m{};
    double height_m{};
    std::optional<std::string> material_slot;
    bool operator==(const AssemblyProfile& other) const;
};
struct AssemblyPart {
    std::string id;
    std::string type_id;
    AssemblyTransform transform;
    std::map<std::string, std::string> property_overrides;
    std::map<std::string, std::string> material_overrides;
    std::map<std::string, AssemblyQuantityProperty> quantity_overrides;
    bool operator==(const AssemblyPart&) const = default;
};
struct AssemblyPathOverride {
    // Stable local part identities. Empty paths are refused; root overrides
    // live on AssemblyInstance. No delimiter-encoded path identities.
    std::vector<std::string> part_path;
    std::optional<AssemblyTransform> transform;
    std::map<std::string, std::string> property_overrides;
    std::map<std::string, std::string> material_overrides;
    std::map<std::string, AssemblyQuantityProperty> quantity_overrides;
    bool operator==(const AssemblyPathOverride&) const = default;
};
struct AssemblyType {
    std::string id;
    std::string name;
    std::map<std::string, std::string> properties;
    // Named material slots reference the model's material catalog.
    std::map<std::string, std::string> materials;
    std::map<std::string, AssemblyQuantityProperty> quantities;
    std::vector<AssemblyProfile> profiles;
    std::vector<AssemblyPart> parts;
    bool operator==(const AssemblyType&) const = default;
};
// Optional placement binds an instance to an existing document geometry
// object.  The host remains the source of geometric truth; the placement is a
// deterministic transform used for repeated assembly previews and takeoffs.
struct AssemblyPlacement {
    std::string host_entity_id;
    Vec2 translation_m{};
    double rotation_radians{};
    double scale{1.0};
    bool mirrored_y{false};
    // Appended for existing aggregate callers; legacy placements default to Z=0.
    double translation_z_m{};
    bool operator==(const AssemblyPlacement& other) const noexcept {
        return host_entity_id == other.host_entity_id &&
               translation_m.x == other.translation_m.x &&
               translation_m.y == other.translation_m.y &&
               rotation_radians == other.rotation_radians &&
               scale == other.scale && mirrored_y == other.mirrored_y &&
               translation_z_m == other.translation_z_m;
    }
};
// Retains the source host identity. If its geometry also follows G, conjugate
// the placement as G*A*G^-1; otherwise compose G*A (type-owned profiles).
[[nodiscard]] AssemblyPlacement transform_assembly_placement(const AssemblyPlacement& source,
    const AssemblyTransform& world_transform, bool host_geometry_transformed);
// Patch actual hosted catalog instances without reconstructing saved definitions,
// overrides or unchanged numeric representations. Unknown/unhosted IDs fail.
[[nodiscard]] nlohmann::json transform_hosted_assembly_model(const nlohmann::json& actual_model,
    const std::map<std::string, AssemblyTransform, std::less<>>& instance_transforms);
struct AssemblyInstance {
    std::string id;
    std::string type_id;
    std::map<std::string, std::string> property_overrides;
    std::map<std::string, std::string> material_overrides;
    std::map<std::string, AssemblyQuantityProperty> quantity_overrides;
    std::optional<AssemblyPlacement> placement;
    // Independent world placement; mutually exclusive with legacy host placement.
    std::optional<AssemblyTransform> root_transform;
    std::vector<AssemblyPathOverride> nested_overrides;
    bool operator==(const AssemblyInstance&) const = default;
};
// Strict independent-instance codec (sketch.assembly-instance.v1). Legacy
// embedded catalog instances use the separate V1-V5 catalog representation.
[[nodiscard]] nlohmann::json encode_assembly_instance(const AssemblyInstance& instance);
[[nodiscard]] AssemblyInstance decode_assembly_instance(const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_assembly_transform(const AssemblyTransform& transform);
[[nodiscard]] AssemblyTransform decode_assembly_transform(const nlohmann::json& value);
struct ResolvedAssembly {
    std::string instance_id;
    std::string type_id;
    std::map<std::string, std::string> properties;
    std::map<std::string, std::string> materials;
    std::map<std::string, AssemblyQuantityProperty> quantities;
    bool operator==(const ResolvedAssembly&) const = default;
};
struct AssemblyExpandedNode {
    std::vector<std::string> part_path;
    std::string type_id;
    AssemblyTransform transform;
    std::map<std::string, std::string> properties;
    std::map<std::string, std::string> materials;
    std::map<std::string, AssemblyQuantityProperty> quantities;
    // Explicit source overrides survive even when equal to defaults.
    std::optional<AssemblyPart> source_part;
    std::optional<AssemblyPathOverride> path_override;
};
struct AssemblyExpandedProfile {
    std::vector<std::string> part_path;
    std::string type_id;
    AssemblyProfile profile;
    AssemblyTransform transform;
    std::optional<std::string> material_id;
    double volume_m3{};
};
struct AssemblyQuantityKey {
    std::string name;
    AssemblyQuantityUnit unit{};
    bool operator==(const AssemblyQuantityKey&) const = default;
    bool operator<(const AssemblyQuantityKey& other) const noexcept {
        return name < other.name || (name == other.name && unit < other.unit);
    }
};
struct AssemblyExpansion {
    AssemblyInstance source_instance;
    std::vector<AssemblyExpandedNode> nodes;
    std::vector<AssemblyExpandedProfile> profiles;
    // Authored totals are never scaled from geometry. Each node contributes
    // its explicit declarations; each owned profile contributes volume once.
    std::map<AssemblyQuantityKey, double> declared_quantities;
    std::map<std::string, double> material_volumes_m3;
    double volume_m3{};
};
struct AssemblyTypeUpdateImpact {
    std::string instance_id;
    ResolvedAssembly before;
    ResolvedAssembly after;
    // Retains provenance even for overrides equal to the previous default.
    AssemblyInstance retained_overrides;
    AssemblyExpansion before_expansion;
    AssemblyExpansion after_expansion;
};
// Pass one budget across all catalog and independent document instances. Consumption is
// committed only on success. Limits may be lowered, never raised above caps.
struct AssemblyExpansionBudget {
    std::size_t max_nodes{4096};
    std::size_t max_profile_segments{262144};
    std::size_t consumed_nodes{};
    std::size_t consumed_profile_segments{};
    // Running authored/computed totals across every successful expansion.
    std::map<AssemblyQuantityKey, double> declared_quantities;
    std::map<std::string, double> material_volumes_m3;
    double volume_m3{};
};

// Detached immutable semantic state. Retain the previous model for undo.
class AssemblyModel final {
public:
    [[nodiscard]] static AssemblyModel create(std::vector<AssemblyMaterial> materials,
        std::vector<AssemblyType> types, std::vector<AssemblyInstance> instances);
    [[nodiscard]] static AssemblyModel from_json(const nlohmann::json& value);
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] const std::vector<AssemblyMaterial>& materials() const noexcept { return materials_; }
    [[nodiscard]] const std::vector<AssemblyType>& types() const noexcept { return types_; }
    [[nodiscard]] const std::vector<AssemblyInstance>& instances() const noexcept { return instances_; }
    [[nodiscard]] ResolvedAssembly resolve(const std::string& instance_id) const;
    [[nodiscard]] AssemblyExpansion expand(const std::string& instance_id) const;
    [[nodiscard]] AssemblyExpansion expand(const AssemblyInstance& instance,
        AssemblyExpansionBudget& budget) const;
    // Full replacement; removed overridden keys and changed overridden dimensions fail.
    [[nodiscard]] AssemblyModel with_type(AssemblyType replacement) const;
    [[nodiscard]] AssemblyModel with_instance(AssemblyInstance replacement) const;
    // Refuses direct/indirect references from parts and root instances.
    [[nodiscard]] AssemblyModel without_type(const std::string& type_id) const;
    // Includes every instance referencing this type, sorted by ID, even if unchanged.
    [[nodiscard]] std::vector<AssemblyTypeUpdateImpact> preview_type_update(AssemblyType replacement) const;
private:
    AssemblyModel() = default;
    std::vector<AssemblyMaterial> materials_;
    std::vector<AssemblyType> types_;
    std::vector<AssemblyInstance> instances_;
};

} // namespace sketch
