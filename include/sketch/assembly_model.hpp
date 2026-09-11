#pragma once

#include <nlohmann/json.hpp>
#include <map>
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
struct AssemblyInstance {
    std::string id;
    std::string type_id;
    std::map<std::string, std::string> property_overrides;
    std::map<std::string, std::string> material_overrides;
    std::map<std::string, AssemblyQuantityProperty> quantity_overrides;
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
