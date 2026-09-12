#include "sketch/assembly_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void identifier(const std::string& value) {
    require(!value.empty() && !std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); }), "assembly identifier/name must not be blank");
}
const char* unit_name(AssemblyQuantityUnit unit) {
    switch (unit) {
    case AssemblyQuantityUnit::count: return "count";
    case AssemblyQuantityUnit::metre: return "m";
    case AssemblyQuantityUnit::square_metre: return "m2";
    case AssemblyQuantityUnit::cubic_metre: return "m3";
    case AssemblyQuantityUnit::kilogram: return "kg";
    }
    throw std::invalid_argument("unknown assembly quantity unit");
}
void quantities(const std::map<std::string, AssemblyQuantityProperty>& values) {
    for (const auto& [key, property] : values) {
        identifier(key);
        (void)unit_name(property.unit);
        require(std::isfinite(property.value) && property.value >= 0, "assembly quantity must be finite and nonnegative");
        require(property.unit != AssemblyQuantityUnit::count || std::floor(property.value) == property.value,
            "assembly count must be integral");
    }
}
template<class T> void canonical(std::vector<T>& values) {
    std::set<std::string> ids;
    for (const auto& value : values) {
        identifier(value.id);
        require(ids.insert(value.id).second, "duplicate assembly identity");
    }
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
}
template<class T> const T& find(const std::vector<T>& values, const std::string& id) {
    const auto item = std::find_if(values.begin(), values.end(), [&](const auto& value) { return value.id == id; });
    require(item != values.end(), "unknown assembly reference");
    return *item;
}
template<class T> void overlay(std::map<std::string, T>& base, const std::map<std::string, T>& changes) {
    for (const auto& [key, value] : changes) base.at(key) = value;
}
template<class T> void override_keys(const std::map<std::string, T>& base, const std::map<std::string, T>& changes) {
    for (const auto& [key, value] : changes) {
        (void)value;
        require(base.contains(key), "assembly override references undeclared property/slot");
    }
}
void fields(const nlohmann::json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "invalid assembly JSON fields");
    for (const auto* key : keys) require(value.contains(key), "missing assembly JSON field");
}
nlohmann::json encode_quantities(const std::map<std::string, AssemblyQuantityProperty>& values) {
    auto result = nlohmann::json::object();
    for (const auto& [key, property] : values) result[key] = {{"value", property.value}, {"unit", unit_name(property.unit)}};
    return result;
}
std::map<std::string, AssemblyQuantityProperty> decode_quantities(const nlohmann::json& values) {
    require(values.is_object(), "assembly quantities must be an object");
    std::map<std::string, AssemblyQuantityProperty> result;
    for (const auto& [key, value] : values.items()) {
        fields(value, {"value", "unit"});
        require(value.at("value").is_number(), "assembly quantity value must be numeric");
        const auto name = value.at("unit").get<std::string>();
        bool known = false;
        for (const auto unit : {AssemblyQuantityUnit::count, AssemblyQuantityUnit::metre,
            AssemblyQuantityUnit::square_metre, AssemblyQuantityUnit::cubic_metre, AssemblyQuantityUnit::kilogram}) {
            if (name == unit_name(unit)) {
                result[key] = {value.at("value").get<double>(), unit}; known = true; break;
            }
        }
        require(known, "unknown assembly quantity unit");
    }
    return result;
}
} // namespace

AssemblyModel AssemblyModel::create(std::vector<AssemblyMaterial> materials,
    std::vector<AssemblyType> types, std::vector<AssemblyInstance> instances) {
    canonical(materials); canonical(types); canonical(instances);
    for (const auto& material : materials) {
        identifier(material.name);
        if (material.color_srgb) {
            const auto& color = *material.color_srgb;
            require(color.size() == 7 && color.front() == '#' &&
                std::all_of(color.begin() + 1, color.end(), [](char c) {
                    return std::string_view("0123456789abcdefABCDEF").find(c) != std::string_view::npos;
                }), "material color must be #RRGGBB in sRGB");
        }
    }
    for (const auto& type : types) {
        identifier(type.name);
        for (const auto& [key, value] : type.properties) { (void)value; identifier(key); }
        for (const auto& [key, id] : type.materials) { identifier(key); (void)find(materials, id); }
        quantities(type.quantities);
    }
    for (const auto& instance : instances) {
        const auto& type = find(types, instance.type_id);
        override_keys(type.properties, instance.property_overrides);
        override_keys(type.materials, instance.material_overrides);
        override_keys(type.quantities, instance.quantity_overrides);
        for (const auto& [key, id] : instance.material_overrides) { (void)key; (void)find(materials, id); }
        quantities(instance.quantity_overrides);
        for (const auto& [key, value] : instance.quantity_overrides)
            require(type.quantities.at(key).unit == value.unit, "assembly override changes quantity dimension");
    }
    AssemblyModel model;
    model.materials_ = std::move(materials); model.types_ = std::move(types); model.instances_ = std::move(instances);
    return model;
}
ResolvedAssembly AssemblyModel::resolve(const std::string& instance_id) const {
    const auto& instance = find(instances_, instance_id);
    const auto& type = find(types_, instance.type_id);
    ResolvedAssembly result{instance.id, type.id, type.properties, type.materials, type.quantities};
    overlay(result.properties, instance.property_overrides);
    overlay(result.materials, instance.material_overrides);
    overlay(result.quantities, instance.quantity_overrides);
    return result;
}
AssemblyModel AssemblyModel::with_type(AssemblyType replacement) const {
    (void)find(types_, replacement.id);
    auto types = types_;
    for (auto& type : types) if (type.id == replacement.id) { type = std::move(replacement); break; }
    return create(materials_, std::move(types), instances_);
}
AssemblyModel AssemblyModel::with_instance(AssemblyInstance replacement) const {
    (void)find(instances_, replacement.id);
    auto instances = instances_;
    for (auto& instance : instances) if (instance.id == replacement.id) { instance = std::move(replacement); break; }
    return create(materials_, types_, std::move(instances));
}
std::vector<AssemblyTypeUpdateImpact> AssemblyModel::preview_type_update(AssemblyType replacement) const {
    const auto id = replacement.id;
    const auto updated = with_type(std::move(replacement));
    std::vector<AssemblyTypeUpdateImpact> impacts;
    for (const auto& instance : instances_) if (instance.type_id == id)
        impacts.push_back({instance.id, resolve(instance.id), updated.resolve(instance.id), instance});
    return impacts;
}
nlohmann::json AssemblyModel::to_json() const {
    nlohmann::json result{{"schema", "sketch.assemblies.v1"}, {"materials", nlohmann::json::array()},
        {"types", nlohmann::json::array()}, {"instances", nlohmann::json::array()}};
    for (const auto& material : materials_) {
        nlohmann::json value{{"id", material.id}, {"name", material.name}};
        if (material.color_srgb) {
            result["schema"] = "sketch.assemblies.v2";
            value["color_srgb"] = *material.color_srgb;
        }
        result["materials"].push_back(std::move(value));
    }
    for (const auto& type : types_) result["types"].push_back({{"id", type.id}, {"name", type.name},
        {"properties", type.properties}, {"materials", type.materials}, {"quantities", encode_quantities(type.quantities)}});
    for (const auto& instance : instances_) result["instances"].push_back({{"id", instance.id}, {"type_id", instance.type_id},
        {"property_overrides", instance.property_overrides}, {"material_overrides", instance.material_overrides},
        {"quantity_overrides", encode_quantities(instance.quantity_overrides)}});
    return result;
}
AssemblyModel AssemblyModel::from_json(const nlohmann::json& value) {
    try {
        fields(value, {"schema", "materials", "types", "instances"});
        const bool appearance = value.at("schema") == "sketch.assemblies.v2";
        require(appearance || value.at("schema") == "sketch.assemblies.v1", "unsupported assembly schema");
        for (const auto* key : {"materials", "types", "instances"})
            require(value.at(key).is_array(), "assembly collections must be arrays");
        std::vector<AssemblyMaterial> materials;
        std::vector<AssemblyType> types;
        std::vector<AssemblyInstance> instances;
        using Strings = std::map<std::string, std::string>;
        for (const auto& item : value.at("materials")) {
            if (appearance && item.contains("color_srgb")) fields(item, {"id", "name", "color_srgb"});
            else fields(item, {"id", "name"});
            materials.push_back({item.at("id").get<std::string>(), item.at("name").get<std::string>(),
                item.contains("color_srgb") ? std::optional{item.at("color_srgb").get<std::string>()} : std::nullopt});
        }
        for (const auto& item : value.at("types")) {
            fields(item, {"id", "name", "properties", "materials", "quantities"});
            types.push_back({item.at("id").get<std::string>(), item.at("name").get<std::string>(),
                item.at("properties").get<Strings>(), item.at("materials").get<Strings>(), decode_quantities(item.at("quantities"))});
        }
        for (const auto& item : value.at("instances")) {
            fields(item, {"id", "type_id", "property_overrides", "material_overrides", "quantity_overrides"});
            instances.push_back({item.at("id").get<std::string>(), item.at("type_id").get<std::string>(),
                item.at("property_overrides").get<Strings>(), item.at("material_overrides").get<Strings>(),
                decode_quantities(item.at("quantity_overrides"))});
        }
        return create(std::move(materials), std::move(types), std::move(instances));
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly JSON: ") + error.what());
    }
}
} // namespace sketch
