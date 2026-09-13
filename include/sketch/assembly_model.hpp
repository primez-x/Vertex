#pragma once

#include "sketch/geometry.hpp"

#include <nlohmann/json.hpp>
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
struct AssemblyType {
    std::string id;
    std::string name;
    std::map<std::string, std::string> properties;
    // Named material slots reference the model's material catalog.
    std::map<std::string, std::string> materials;
    std::map<std::string, AssemblyQuantityProperty> quantities;
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
    bool operator==(const AssemblyPlacement& other) const noexcept {
        return host_entity_id == other.host_entity_id &&
               translation_m.x == other.translation_m.x &&
               translation_m.y == other.translation_m.y &&
               rotation_radians == other.rotation_radians &&
               scale == other.scale;
    }
};
struct AssemblyInstance {
    std::string id;
    std::string type_id;
    std::map<std::string, std::string> property_overrides;
    std::map<std::string, std::string> material_overrides;
    std::map<std::string, AssemblyQuantityProperty> quantity_overrides;
    std::optional<AssemblyPlacement> placement;
    bool operator==(const AssemblyInstance&) const = default;
};
struct ResolvedAssembly {
    std::string instance_id;
    std::string type_id;
    std::map<std::string, std::string> properties;
    std::map<std::string, std::string> materials;
    std::map<std::string, AssemblyQuantityProperty> quantities;
    bool operator==(const ResolvedAssembly&) const = default;
};
struct AssemblyTypeUpdateImpact {
    std::string instance_id;
    ResolvedAssembly before;
    ResolvedAssembly after;
    // Retains provenance even for overrides equal to the previous default.
    AssemblyInstance retained_overrides;
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
    // Full replacement; removed overridden keys and changed overridden dimensions fail.
    [[nodiscard]] AssemblyModel with_type(AssemblyType replacement) const;
    [[nodiscard]] AssemblyModel with_instance(AssemblyInstance replacement) const;
    // Includes every instance referencing this type, sorted by ID, even if unchanged.
    [[nodiscard]] std::vector<AssemblyTypeUpdateImpact> preview_type_update(AssemblyType replacement) const;
private:
    AssemblyModel() = default;
    std::vector<AssemblyMaterial> materials_;
    std::vector<AssemblyType> types_;
    std::vector<AssemblyInstance> instances_;
};

} // namespace sketch
