#include "sketch/assembly_model.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) { std::cerr << "assembly_model_tests: " << message << '\n'; std::exit(1); }
}
template<class F> void invalid(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    require(false, "expected validation failure");
}
sketch::AssemblyModel fixture() {
    using namespace sketch;
    AssemblyType type{"wall", "Wall", {{"finish", "paint"}}, {{"core", "brick"}},
        {{"area", {4, AssemblyQuantityUnit::square_metre}}, {"pieces", {1, AssemblyQuantityUnit::count}}}};
    AssemblyInstance standard{"a", "wall", {}, {}, {}};
    AssemblyInstance custom{"b", "wall", {{"finish", "paint"}}, {{"core", "wood"}},
        {{"area", {6, AssemblyQuantityUnit::square_metre}}}};
    return AssemblyModel::create({{"wood", "Wood"}, {"brick", "Brick"}}, {type}, {custom, standard});
}
void update_and_undo() {
    auto original = fixture();
    const auto saved = original.to_json().dump();
    auto replacement = original.types().front();
    replacement.properties["finish"] = "plaster";
    replacement.materials["core"] = "wood";
    replacement.quantities.at("area").value = 8;
    auto impact = original.preview_type_update(replacement);
    require(impact.size() == 2 && impact[0].instance_id == "a" && impact[1].instance_id == "b", "canonical complete preview");
    require(impact[0].before.quantities.at("area").value == 4 && impact[0].after.quantities.at("area").value == 8, "inherited quantity changes");
    require(impact[1].after.properties.at("finish") == "paint", "equal-to-default explicit override survives");
    require(impact[1].after.quantities.at("area").value == 6, "quantity override survives");
    require(impact[1].retained_overrides == original.instances()[1], "override provenance survives");
    auto updated = original.with_type(replacement);
    require(updated.resolve("a") == impact[0].after && updated.resolve("b") == impact[1].after, "preview matches commit");
    require(original.to_json().dump() == saved, "preview and update cannot mutate prior snapshot");
    auto cleared = updated.instances()[1];
    cleared.property_overrides.clear(); cleared.material_overrides.clear(); cleared.quantity_overrides.clear();
    require(updated.with_instance(cleared).resolve("b").quantities.at("area").value == 8, "clearing override restores inheritance");
    require(updated.with_type(original.types().front()).to_json() == original.to_json(), "type replacement undo restores source snapshot");
}
void validation_and_serialization() {
    using namespace sketch;
    const auto model = fixture();
    auto reversed_instances = model.instances(); std::reverse(reversed_instances.begin(), reversed_instances.end());
    require(AssemblyModel::create(model.materials(), model.types(), reversed_instances).to_json().dump() == model.to_json().dump(), "canonical input ordering");
    require(AssemblyModel::from_json(model.to_json()).to_json() == model.to_json(), "round trip preserves overrides");
    auto type = model.types()[0];
    type.properties.erase("finish"); invalid([&] { (void)model.with_type(type); });
    type = model.types()[0]; type.quantities.at("area").unit = AssemblyQuantityUnit::metre;
    invalid([&] { (void)model.preview_type_update(type); });
    type = model.types()[0]; type.materials["core"] = "missing";
    invalid([&] { (void)model.with_type(type); });
    type = model.types()[0]; type.quantities.at("area").value = std::numeric_limits<double>::infinity();
    invalid([&] { (void)model.with_type(type); });
    type = model.types()[0]; type.quantities.at("pieces").value = 0.5;
    invalid([&] { (void)model.with_type(type); });
    type = model.types()[0]; type.quantities.at("area").value = -1;
    invalid([&] { (void)model.with_type(type); });
    type = model.types()[0]; type.quantities.at("area").unit = static_cast<AssemblyQuantityUnit>(99);
    invalid([&] { (void)model.with_type(type); });
    type = model.types()[0]; type.id = "missing";
    invalid([&] { (void)model.with_type(type); });
    auto instance = model.instances()[0]; instance.quantity_overrides["unknown"] = {};
    invalid([&] { (void)model.with_instance(instance); });
    instance = model.instances()[0]; instance.type_id = "missing";
    invalid([&] { (void)model.with_instance(instance); });
    invalid([&] { (void)model.resolve("missing"); });
    invalid([&] { (void)AssemblyModel::create(model.materials(), model.types(), {model.instances()[0], model.instances()[0]}); });
    invalid([&] { (void)AssemblyModel::create(model.materials(), {model.types()[0], model.types()[0]}, {}); });
    invalid([&] { (void)AssemblyModel::create({model.materials()[0], model.materials()[0]}, {}, {}); });
    auto json = model.to_json(); json["extra"] = true;
    invalid([&] { (void)AssemblyModel::from_json(json); });
    json = model.to_json(); json["types"][0]["quantities"]["area"]["value"] = true;
    invalid([&] { (void)AssemblyModel::from_json(json); });
    json = model.to_json(); json["instances"][0]["material_overrides"] = nlohmann::json::array();
    invalid([&] { (void)AssemblyModel::from_json(json); });
    json = model.to_json(); json["types"][0]["quantities"]["area"]["unit"] = "acres";
    invalid([&] { (void)AssemblyModel::from_json(json); });
    json = model.to_json(); json["materials"][0]["name"] = "  ";
    invalid([&] { (void)AssemblyModel::from_json(json); });
    require(AssemblyModel::from_json(AssemblyModel::create({}, {}, {}).to_json()).instances().empty(), "empty catalog supported");
}
} // namespace
int main() {
    update_and_undo(); validation_and_serialization();
    std::cout << "assembly_model_tests passed\n";
}
