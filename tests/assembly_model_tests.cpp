#include "sketch/assembly_model.hpp"

#include <algorithm>
#include <cmath>
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
void placement_round_trip_and_validation() {
    using namespace sketch;
    AssemblyType type{"panel", "Panel", {}, {}, {{"count", {2, AssemblyQuantityUnit::count}}}};
    AssemblyInstance instance{"panel-a", "panel", {}, {}, {},
        AssemblyPlacement{"wall-host", {1.25, -0.5}, 0.5, 1.2}};
    const auto model = AssemblyModel::create({}, {type}, {instance});
    require(model.to_json().at("schema") == "sketch.assemblies.v3",
            "placed instances use the v3 assembly schema");
    require(AssemblyModel::from_json(model.to_json()).instances().front() == instance,
            "placement survives assembly serialization");
    auto json = model.to_json();
    json.at("instances")[0].at("placement")["scale"] = 0.0;
    invalid([&] { (void)AssemblyModel::from_json(json); });
    json = model.to_json();
    json.at("instances")[0].at("placement")["translation_m"][0] = nullptr;
    invalid([&] { (void)AssemblyModel::from_json(json); });
    auto unplaced = instance;
    unplaced.placement.reset();
    require(AssemblyModel::create({}, {type}, {unplaced}).to_json().at("schema") ==
                "sketch.assemblies.v1",
            "unplaced instances retain the legacy schema");
    unplaced.placement = AssemblyPlacement{"", {}, 0.0, 1.0};
    invalid([&] { (void)AssemblyModel::create({}, {type}, {unplaced}); });
}
} // namespace
void material_appearance() {
    using namespace sketch;
    const auto legacy = AssemblyModel::create({{"wood", "Wood"}}, {}, {});
    require(legacy.to_json().at("schema") == "sketch.assemblies.v1", "uncolored catalogs retain v1");
    const auto colored = AssemblyModel::create({{"wood", "Wood", "#Ae8042"}}, {}, {});
    require(colored.to_json().at("schema") == "sketch.assemblies.v2" &&
        AssemblyModel::from_json(colored.to_json()).materials() == colored.materials(), "sRGB colors round-trip in v2");
    auto json = colored.to_json(); json["schema"] = "sketch.assemblies.v1";
    invalid([&] { (void)AssemblyModel::from_json(json); });
    for (const auto& color : {"red", "#fff", "#12345678", "#12345g", "", " #123456"})
        invalid([&] { (void)AssemblyModel::create({{"wood", "Wood", color}}, {}, {}); });
    json = colored.to_json(); json["materials"][0]["color_srgb"] = nullptr;
    invalid([&] { (void)AssemblyModel::from_json(json); });
}
void nested_expansion() {
    using namespace sketch;
    const Boundary outer{{{0,0},{2,0}},{{2,0},{2,1}},{{2,1},{0,1}},{{0,1},{0,0}}};
    const Boundary hole{{{.25,.25},{.75,.25}},{{.75,.25},{.75,.75}},{{.75,.75},{.25,.75}},{{.25,.75},{.25,.25}}};
    AssemblyType leaf{"leaf", "Leaf", {{"finish","paint"}}, {{"core","wood"}},
        {{"pieces",{1,AssemblyQuantityUnit::count}}, {"weight",{7,AssemblyQuantityUnit::kilogram}}}};
    leaf.profiles.push_back({"body",outer,{hole},.1,.2,"core"});
    AssemblyType middle{"middle", "Middle", {}, {}, {{"weight",{2,AssemblyQuantityUnit::metre}}}};
    middle.parts.push_back({"leaf/a", "leaf", {{1,0,3},0,2}});
    AssemblyType parent{"parent","Parent"};
    parent.parts.push_back({"b","leaf",{{0,0,0},0,1}});
    parent.parts.push_back({"a","middle",{{0,2,4},1.5707963267948966,1}});
    AssemblyInstance instance{"independent","parent"};
    instance.root_transform = AssemblyTransform{{10,20,5},1.5707963267948966,1};
    instance.nested_overrides.push_back({{"a","leaf/a"},std::nullopt,{{"finish","paint"}},{{"core","brick"}}, {}});
    const auto model = AssemblyModel::create({{"wood","Wood"},{"brick","Brick"}}, {leaf,middle,parent}, {instance});
    const auto result = model.expand("independent");
    require(result.nodes.size()==4 && result.profiles.size()==2, "each node/profile expanded exactly once");
    require(result.nodes[1].part_path==std::vector<std::string>{"b"} &&
        result.nodes[2].part_path==std::vector<std::string>{"a"} &&
        result.nodes[3].part_path==std::vector<std::string>{"a","leaf/a"},
        "authored traversal order and structured paths retain delimiter identities");
    const auto& nested = result.profiles[1];
    const auto vertex = transform_assembly_point({2,1,.3},nested.transform);
    require(std::abs(vertex.x-3)<1e-12 && std::abs(vertex.y-18)<1e-12 && std::abs(vertex.z-12.6)<1e-12, "three-level independently expected XYZ composition");
    require(std::abs(result.volume_m3-3.15)<1e-12 && std::abs(nested.volume_m3-2.8)<1e-12, "scale cubed and hole removed exactly once");
    require(std::abs(result.material_volumes_m3.at("brick")-2.8)<1e-12 && std::abs(result.material_volumes_m3.at("wood")-.35)<1e-12, "nested resolved materials drive computed volume");
    require(result.declared_quantities.at({"pieces",AssemblyQuantityUnit::count})==2 && result.declared_quantities.at({"weight",AssemblyQuantityUnit::kilogram})==14 && result.declared_quantities.at({"weight",AssemblyQuantityUnit::metre})==2, "declared quantities unscaled and separated by name/unit");
    require(result.nodes[3].path_override.has_value() && result.nodes[3].path_override->property_overrides.at("finish")=="paint", "equal-value nested override provenance retained");
    require(model.to_json().at("schema")=="sketch.assemblies.v4" && AssemblyModel::from_json(model.to_json()).to_json()==model.to_json(), "V4 profiles nested overrides round trip");
    require(decode_assembly_instance(encode_assembly_instance(instance))==instance, "independent V1 round trip");
    const auto original = model.to_json();
    auto changed=leaf; changed.properties["finish"]="plaster";
    auto impact=model.preview_type_update(changed);
    require(impact.size()==1 && impact.front().instance_id=="independent" && impact.front().retained_overrides==model.instances().front(), "indirect update impact retains overrides");
    require(impact.front().before_expansion.nodes[3].properties.at("finish")=="paint" &&
        impact.front().after_expansion.nodes[3].properties.at("finish")=="paint" &&
        impact.front().after_expansion.nodes[1].properties.at("finish")=="plaster", "impact preview includes inherited and retained leaf values");
    auto moved=instance;
    moved.nested_overrides[0].transform=AssemblyTransform{{2,0,5},0,.5};
    const auto moved_expansion=model.with_instance(moved).expand("independent");
    const auto moved_vertex=transform_assembly_point({2,1,.3},moved_expansion.profiles[1].transform);
    require(std::abs(moved_vertex.x-5)<1e-12 && std::abs(moved_vertex.y-19.5)<1e-12 &&
        std::abs(moved_vertex.z-14.15)<1e-12, "path local transform replaces the source local transform before parent composition");
    changed=parent; std::erase_if(changed.parts,[](const auto& part){return part.id=="a";});
    invalid([&]{ (void)model.with_type(changed); });
    require(model.to_json()==original, "deleted overridden path refuses without partial mutation");
    invalid([&]{ (void)model.without_type("leaf"); });
    auto bad=instance; bad.nested_overrides[0].part_path={"a/leaf/a"};
    invalid([&]{ (void)model.with_instance(bad); });
    bad=instance; bad.nested_overrides.push_back(bad.nested_overrides.front());
    invalid([&]{ (void)model.with_instance(bad); });
    bad=instance; bad.nested_overrides[0].quantity_overrides["weight"]={2,AssemblyQuantityUnit::metre};
    invalid([&]{ (void)model.with_instance(bad); });
    bad=instance; bad.nested_overrides[0].part_path.clear();
    invalid([&]{ (void)model.with_instance(bad); });
    changed=leaf; changed.parts.push_back({"cycle","parent"});
    invalid([&]{ (void)model.with_type(changed); });
    AssemblyType unused{"unused","Unused"}; unused.parts.push_back({"bad","missing"});
    invalid([&]{ (void)AssemblyModel::create({}, {unused}, {}); });
    unused.parts[0].type_id="unused";
    invalid([&]{ (void)AssemblyModel::create({}, {unused}, {}); });
    changed=leaf; changed.profiles[0].holes[0][0].start.x=-1;
    invalid([&]{ (void)model.with_type(changed); });
    changed=leaf; changed.profiles[0].outer.resize(1025,changed.profiles[0].outer.front());
    invalid([&]{ (void)model.with_type(changed); });
    bad=instance; bad.root_transform->scale=std::numeric_limits<double>::max();
    invalid([&]{ (void)model.with_instance(bad); });
    AssemblyExpansionBudget budget; budget.max_nodes=3;
    invalid([&]{ (void)model.expand(instance,budget); });
    require(budget.consumed_nodes==0 && budget.consumed_profile_segments==0, "budget consumption commits only complete expansion");
    budget.max_nodes=7; (void)model.expand(instance,budget);
    invalid([&]{ (void)model.expand(instance,budget); });
    require(budget.consumed_nodes==4, "documentwide shared budget refuses later expansion atomically");
    AssemblyExpansionBudget segment_budget;segment_budget.max_profile_segments=15;
    invalid([&]{ (void)model.expand(instance,segment_budget); });
    require(segment_budget.consumed_nodes==0 && segment_budget.consumed_profile_segments==0 && segment_budget.volume_m3==0,
        "profile segment refusal rolls back all resource and quantity state");
    AssemblyType declared{"declared","Declared",{}, {}, {{"weight",{std::numeric_limits<double>::max(),AssemblyQuantityUnit::kilogram}}}};
    const auto declared_model=AssemblyModel::create({}, {declared}, {});
    AssemblyInstance declaration{"one","declared"};AssemblyExpansionBudget aggregate;
    (void)declared_model.expand(declaration,aggregate);
    invalid([&]{ (void)declared_model.expand(declaration,aggregate); });
    require(aggregate.consumed_nodes==1 && aggregate.declared_quantities.at({"weight",AssemblyQuantityUnit::kilogram})==std::numeric_limits<double>::max(),
        "documentwide declared overflow preserves previous aggregate");
    auto mixed_json=model.to_json();mixed_json["schema"]="sketch.assemblies.v3";
    invalid([&]{ (void)AssemblyModel::from_json(mixed_json); });
    mixed_json=model.to_json();mixed_json["types"][0]["profiles"][0]["height_m"]=true;
    invalid([&]{ (void)AssemblyModel::from_json(mixed_json); });
    AssemblyType curved{"curved","Curved"};
    curved.profiles.push_back({"disc",{{{1,0},{-1,0},3.141592653589793},{{-1,0},{1,0},3.141592653589793}}, {},0,.2,std::nullopt});
    const auto curved_model=AssemblyModel::create({}, {curved}, {{"circle","curved"}});
    require(std::abs(curved_model.expand("circle").volume_m3-.6283185307179586)<1e-12 &&
        AssemblyModel::from_json(curved_model.to_json()).types()==curved_model.types(), "analytical arcs stay exact in profiles and serialization");
    auto unreferenced=model.types();unreferenced.push_back({"unused","Unused"});
    const auto removable=AssemblyModel::create(model.materials(),unreferenced,model.instances());
    require(removable.without_type("unused").to_json()==model.to_json(), "unreferenced type deletion retains complete source semantics");
    std::vector<AssemblyType> deep;
    for (int i=0;i<33;++i) { AssemblyType t{std::to_string(i),"Depth"}; if(i<32)t.parts.push_back({"child",std::to_string(i+1)}); deep.push_back(t); }
    invalid([&]{ (void)AssemblyModel::create({},deep,{}); });
    std::vector<AssemblyType> wide;
    for (int i=0;i<13;++i) { AssemblyType t{std::to_string(i),"Wide"}; if(i<12) {t.parts.push_back({"a",std::to_string(i+1)});t.parts.push_back({"b",std::to_string(i+1)});} wide.push_back(t); }
    invalid([&]{ (void)AssemblyModel::create({},wide,{}); });
}
void unused_shared_graph_validation_work_is_bounded() {
    using namespace sketch;
    Boundary circle;
    constexpr int count=1024;
    for (int i=0;i<count;++i) {
        const auto point=[](int vertex) {
            const auto angle=2*3.14159265358979323846*vertex/count;
            return Vec2{std::cos(angle),std::sin(angle)};
        };
        circle.push_back({point(i),point((i+1)%count),0});
    }
    AssemblyType leaf{"leaf","Circular profile"};
    leaf.profiles={{"profile",circle,{},0,1,std::nullopt}};
    AssemblyType shared{"shared","Shared subtree"};
    for (int i=0;i<256;++i)
        shared.parts.push_back({"part-"+std::to_string(i),leaf.id});
    std::vector<AssemblyType> types{leaf,shared};
    for (int i=0;i<4;++i) {
        AssemblyType wrapper{"wrapper-"+std::to_string(i),"Unused wrapper"};
        wrapper.parts={{"child",shared.id}};types.push_back(wrapper);
    }
    invalid([&]{ (void)AssemblyModel::create({},types,{}); });
    shared.parts.resize(4);types={leaf,shared};
    const auto ordinary=AssemblyModel::create({},types,{});
    AssemblyExpansionBudget budget;
    require(ordinary.expand(AssemblyInstance{"root",shared.id},budget).profiles.size()==4,
        "ordinary shared geometry remains usable within the combined validation budget");
}
void authored_profile_part_order() {
    using namespace sketch;
    const Boundary square{{{0,0},{1,0}},{{1,0},{1,1}},{{1,1},{0,1}},{{0,1},{0,0}}};
    AssemblyType leaf{"leaf", "Ordered solids"};
    leaf.profiles = {{"z-upper",square,{},1,.2,std::nullopt},
                     {"a-lower",square,{},0,.1,std::nullopt}};
    AssemblyType parent{"parent", "Ordered parts"};
    parent.parts = {{"z-first","leaf",{{10,0,0},0,1}},
                    {"a-second","leaf",{{20,0,0},0,1}}};
    const auto model = AssemblyModel::create({}, {parent,leaf}, {});
    const auto restored = AssemblyModel::from_json(model.to_json());
    const auto expected = std::find_if(restored.types().begin(), restored.types().end(),
        [](const auto& type) { return type.id == "parent"; });
    require(expected->parts == parent.parts, "serialization preserves user-reordered stable parts");
    AssemblyInstance instance{"root","parent"};
    instance.root_transform = AssemblyTransform{};
    AssemblyExpansionBudget budget;
    const auto expanded = restored.expand(instance,budget);
    require(expanded.profiles.size()==4 && expanded.profiles[0].part_path==std::vector<std::string>{"z-first"} &&
        expanded.profiles[0].profile.id=="z-upper" && expanded.profiles[1].profile.id=="a-lower" &&
        expanded.profiles[2].part_path==std::vector<std::string>{"a-second"},
        "profile and part reordering survives expansion without rebinding stable identities");
    auto reordered = parent;
    std::reverse(reordered.parts.begin(),reordered.parts.end());
    AssemblyExpansionBudget reordered_budget;
    const auto changed = restored.with_type(reordered).expand(instance,reordered_budget);
    require(changed.profiles[0].part_path==std::vector<std::string>{"a-second"} &&
        expanded.profiles[0].transform.translation_m.x==10 && changed.profiles[0].transform.translation_m.x==20,
        "editing order changes traversal while retaining each part's placement");
}
int main() {
    unused_shared_graph_validation_work_is_bounded();
    authored_profile_part_order();
    nested_expansion();
    material_appearance();
    update_and_undo(); validation_and_serialization(); placement_round_trip_and_validation();
    std::cout << "assembly_model_tests passed\n";
}
