#include "sketch/assembly_document_adapter.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) { std::cerr << "assembly_document_adapter_tests: " << message << '\n'; std::exit(1); }
}
template<class F> void invalid(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    require(false, "expected validation failure");
}
sketch::AssemblyModel model() {
    using namespace sketch;
    AssemblyType leaf;
    leaf.id = "leaf"; leaf.name = "Leaf";
    leaf.properties = {{"finish", "paint"}};
    leaf.materials = {{"surface", "wood"}};
    leaf.quantities = {{"pieces", {1, AssemblyQuantityUnit::count}}};
    AssemblyProfile profile;
    profile.id = "box";
    profile.outer = {{{0,0},{1,0}}, {{1,0},{1,1}}, {{1,1},{0,1}}, {{0,1},{0,0}}};
    profile.height_m = 2; profile.material_slot = "surface";
    leaf.profiles = {profile};
    AssemblyType root;
    root.id = "root"; root.name = "Root";
    AssemblyPart part; part.id = "child"; part.type_id = "leaf";
    part.transform.translation_m = {2,0,0};
    root.parts = {part};
    return AssemblyModel::create({{"wood", "Wood"}}, {root, leaf}, {});
}
sketch::Entity catalog(std::string id = "catalog") {
    sketch::Entity result;
    result.id = std::move(id); result.type = "assembly_model";
    result.properties = {{"model", model().to_json()}, {"note", "retain catalog metadata"}};
    return result;
}
sketch::AssemblyDocumentInstance value(std::string id = "instance", std::string catalog_id = "catalog") {
    sketch::AssemblyDocumentInstance result;
    result.assembly_catalog_id = std::move(catalog_id);
    result.instance.id = std::move(id); result.instance.type_id = "root";
    result.instance.root_transform = sketch::AssemblyTransform{{10,20,3}, 0, 1};
    sketch::AssemblyPathOverride nested;
    nested.part_path = {"child"}; nested.property_overrides = {{"finish", "paint"}};
    nested.material_overrides = {{"surface", "wood"}};
    nested.quantity_overrides = {{"pieces", {3, sketch::AssemblyQuantityUnit::count}}};
    result.instance.nested_overrides = {nested};
    return result;
}
sketch::Entity instance(std::string id = "instance", std::string catalog_id = "catalog") {
    sketch::Entity source;
    source.id = id; source.type = "assembly_instance";
    source.properties = {{"floor_id", "floor"}, {"layer_id", "layer"}, {"phase", "existing"},
        {"opaque", {{"placement", "application metadata"}}}};
    source.extensions = {{"vendor", {{"host", "opaque"}}}};
    return sketch::encode_document_assembly_instance(source, value(std::move(id), std::move(catalog_id)));
}
std::vector<sketch::Entity> drawing_context_entities() {
    return {{"property","property",nlohmann::json::object()},
        {"building","building",{{"property_id","property"}}},
        {"floor","floor",{{"building_id","building"}}},
        {"layer","layer",{{"floor_id","floor"}}}};
}
void codec_and_geometry() {
    using namespace sketch;
    const auto source = instance();
    require(decode_document_assembly_instance(source) == value(), "instance codec retains all nested override provenance");
    auto changed = value(); changed.instance.root_transform->translation_m.x = 30;
    const auto encoded = encode_document_assembly_instance(source, changed);
    for (const auto* key : {"floor_id", "layer_id", "phase", "opaque"})
        require(encoded.properties.at(key) == source.properties.at(key), "document context and opaque properties retained");
    require(encoded.extensions == source.extensions, "opaque extensions retained");
    AssemblyExpansionBudget budget;
    const auto expanded = expand_document_assembly_instance(source, {{"catalog", catalog()}}, budget);
    require(expanded.nodes.size() == 2 && expanded.profiles.size() == 1, "independent instance expands without a host");
    require(expanded.profiles.front().transform.translation_m == AssemblyPoint3{12,20,3}, "root and child transforms compose");
    require(expanded.declared_quantities.at({"pieces", AssemblyQuantityUnit::count}) == 3, "nested quantity override contributes once");
    require(expanded.volume_m3 == 2, "independent profile geometry contributes volume");
    for (const auto* key : {"host", "host_entity_id", "placement", "type_id", "root_transform"}) {
        auto bad = source; bad.properties[key] = "forbidden";
        invalid([&] { (void)decode_document_assembly_instance(bad); });
    }
    for (const auto* key : {"version", "form", "assembly_catalog_id", "instance"}) {
        auto bad = source; bad.properties.erase(key);
        invalid([&] { (void)decode_document_assembly_instance(bad); });
    }
    auto bad = source; bad.properties["version"] = true;
    invalid([&] { (void)decode_document_assembly_instance(bad); });
    bad = source; bad.properties["instance"]["unexpected"] = 1;
    invalid([&] { (void)decode_document_assembly_instance(bad); });
    bad = source; bad.properties["instance"].erase("root_transform");
    invalid([&] { (void)decode_document_assembly_instance(bad); });
    bad = source; bad.properties["instance"]["id"] = "different";
    invalid([&] { (void)decode_document_assembly_instance(bad); });
    changed = value(); changed.instance.root_transform.reset();
    invalid([&] { (void)encode_document_assembly_instance(source, changed); });
    changed = value(); changed.instance.placement = AssemblyPlacement{"host"};
    invalid([&] { (void)encode_document_assembly_instance(source, changed); });
}
void references_and_atomic_budget() {
    using namespace sketch;
    AssemblyDocumentEntities entities{{"catalog", catalog()}, {"instance", instance()}};
    validate_document_assembly_instances(entities);
    auto bad = entities; bad.erase("catalog");
    invalid([&] { validate_document_assembly_instances(bad); });
    bad = entities; bad.at("catalog").type = "wall";
    invalid([&] { validate_document_assembly_instances(bad); });
    auto replacement = value(); replacement.instance.type_id = "missing";
    bad = entities; bad.at("instance") = encode_document_assembly_instance(instance(), replacement);
    invalid([&] { validate_document_assembly_instances(bad); });
    replacement = value(); replacement.instance.nested_overrides[0].part_path = {"deleted"};
    bad = entities; bad.at("instance") = encode_document_assembly_instance(instance(), replacement);
    invalid([&] { validate_document_assembly_instances(bad); });
    // Unused catalogs still undergo strict model decoding and graph validation.
    bad = entities; auto unused = catalog("unused");
    for (auto& type : unused.properties["model"]["types"])
        if (type.at("id") == "root") type["parts"][0]["type_id"] = "missing";
    bad.emplace("unused", unused);
    invalid([&] { validate_document_assembly_instances(bad); });
    bad = entities; unused = catalog("unused");
    for (auto& type : unused.properties["model"]["types"])
        if (type.at("id") == "root") type["parts"][0]["type_id"] = "root";
    bad.emplace("unused", unused);
    invalid([&] { validate_document_assembly_instances(bad); });
    entities.emplace("other", catalog("other")); entities.emplace("second", instance("second", "other"));
    const auto semantic=model();
    auto leaf=semantic.types().front();
    for(const auto& type:semantic.types())if(type.id=="leaf")leaf=type;
    leaf.profiles.front().height_m=3;
    const auto impacts=preview_document_assembly_type_update(entities,"catalog",leaf);
    require(impacts.size()==1 && impacts.front().entity_id=="instance" &&
        impacts.front().before.volume_m3==2 && impacts.front().after.volume_m3==3 &&
        impacts.front().retained_overrides==value(),"indirect external impact has actual geometry and override provenance");
    AssemblyExpansionBudget budget; budget.max_nodes = 3;
    invalid([&] { (void)expand_document_assembly_instances(entities, budget); });
    require(budget.consumed_nodes == 0 && budget.consumed_profile_segments == 0, "failed document expansion commits no budget");
    budget.max_nodes = 4;
    require(expand_document_assembly_instances(entities, budget).size() == 2 && budget.consumed_nodes == 4,
        "one shared budget spans separate catalogs and instances");
}
void commands() {
    using namespace sketch;
    auto entities = drawing_context_entities();
    entities.push_back(catalog());
    auto document = Document::create(entities);
    const auto before = document.snapshot();
    auto fresh = instance();
    const auto create = assembly_instance_create_command(before, fresh, value(), before.revision());
    require(create.entity_changes.size() == 1 && before.entities().size() == 5, "create is one detached atomic command");
    document.apply(Command{create});
    const auto source = document.snapshot();
    auto changed = value(); changed.instance.root_transform->translation_m.x = 100;
    const auto update = assembly_instance_update_command(source, "instance", changed, source.revision());
    require(update.entity_changes[0].entity.extensions == fresh.extensions, "update preserves extensions");
    const auto types = model().types();
    auto leaf = types.front();
    // Select by identity rather than relying on canonical type ordering.
    for (const auto& type : types) if (type.id == "leaf") leaf = type;
    leaf.properties["finish"] = "plaster";
    const auto catalog_update = independent_assembly_type_update_command(source, "catalog", leaf, source.revision());
    require(catalog_update.entity_changes.size() == 1 &&
        catalog_update.entity_changes[0].entity.properties.at("note") == source.entities().at("catalog").properties.at("note"),
        "valid type update returns one catalog upsert preserving catalog metadata");
    leaf.properties.erase("finish");
    invalid([&] { (void)independent_assembly_type_update_command(source, "catalog", leaf, source.revision()); });
    require(source.entities() == document.snapshot().entities(), "invalid catalog replacement cannot partially change source");
    auto root = types.front();
    for (const auto& type : types) if (type.id == "root") root = type;
    root.parts.clear();
    invalid([&] { (void)independent_assembly_type_update_command(source, "catalog", root, source.revision()); });
    invalid([&] { (void)independent_assembly_type_remove_command(source,"catalog","root",source.revision()); });
    invalid([&] { (void)independent_assembly_type_remove_command(source,"catalog","leaf",source.revision()); });
    const auto remove = assembly_instance_remove_command(source, "instance", source.revision());
    require(remove.entity_changes.size() == 1 && remove.entity_changes[0].kind == EntityChangeKind::erase,
        "remove returns one erase command");
    require(source.entities().contains("instance"), "command construction never mutates source");
}
void clipboard_closure() {
    using namespace sketch;
    auto types = model().types();
    AssemblyType leaf;
    for (const auto& type : types) if (type.id == "leaf") leaf = type;
    leaf.profiles.front().holes = {{{{.2,.2},{.2,.4}}, {{.2,.4},{.4,.4}},
        {{.4,.4},{.4,.2}}, {{.4,.2},{.2,.2}}}};
    auto second_profile = leaf.profiles.front(); second_profile.id = "second/profile";
    leaf.profiles.push_back(second_profile);
    AssemblyType middle; middle.id = "middle/raw"; middle.name = "Middle";
    AssemblyPart child; child.id = "child/raw"; child.type_id = "leaf";
    child.material_overrides = {{"surface", "child-material"}};
    middle.parts = {child};
    AssemblyType root; root.id = "root"; root.name = "Root";
    root.materials = {{"surface", "wood"}};
    AssemblyPart first; first.id = "child/raw"; first.type_id = middle.id;
    AssemblyPart second = first; second.id = "other"; second.transform.translation_m.x = 4;
    root.parts = {second, first};
    AssemblyType unused; unused.id = "unused"; unused.name = "Unused";
    unused.materials = {{"surface", "unused-material"}};
    AssemblyInstance legacy; legacy.id = "legacy"; legacy.type_id = "unused";
    legacy.placement = AssemblyPlacement{"not-a-clipboard-host"};
    auto cat = catalog();
    cat.properties["model"] = AssemblyModel::create({{"wood","Wood"},
        {"child-material","Child"}, {"path-material","Path"},
        {"root-material","Root"}, {"unused-material","Unused"}},
        {root, middle, leaf, unused}, {legacy}).to_json();
    auto first_value = value();
    first_value.instance.material_overrides = {{"surface", "root-material"}};
    first_value.instance.nested_overrides.front().part_path = {"child/raw", "child/raw"};
    first_value.instance.nested_overrides.front().material_overrides = {{"surface", "path-material"}};
    auto first_entity = encode_document_assembly_instance(instance(), first_value);
    auto second_value = first_value; second_value.instance.id = "second_root";
    second_value.instance.nested_overrides.front().material_overrides = {{"surface", "wood"}};
    auto second_entity = encode_document_assembly_instance(instance("second_root"), second_value);
    auto entities = drawing_context_entities();
    entities.insert(entities.end(), {cat, first_entity, second_entity, catalog("unrelated"),
        Entity{"not-a-clipboard-host","wall",nlohmann::json::object()}});
    auto document = Document::create(entities);
    const auto snapshot = document.snapshot();
    const auto dependencies = assembly_clipboard_dependencies(snapshot, {"instance", "second_root"});
    require(dependencies.size() == 1 && dependencies.contains("catalog"), "two roots share one minimal catalog");
    const auto retained = AssemblyModel::from_json(dependencies.at("catalog").properties.at("model"));
    require(retained.types().size() == 3 && retained.materials().size() == 4 && retained.instances().empty(),
        "transitive types and all authored override materials retained without unused types or host instances");
    for (const auto& type : retained.types()) {
        if (type.id == "root") require(type.parts == root.parts, "authored part order and raw local IDs retained");
        if (type.id == "leaf") require(type.profiles == leaf.profiles, "profile order and holes retained");
    }
    auto closure = dependencies; closure.emplace(first_entity.id, first_entity); closure.emplace(second_entity.id, second_entity);
    AssemblyExpansionBudget before_budget, after_budget;
    const auto before = expand_document_assembly_instances(snapshot.entities(), before_budget);
    const auto after = expand_document_assembly_instances(closure, after_budget);
    for (const auto& id : {"instance", "second_root"}) {
        require(before.at(id).source_instance == after.at(id).source_instance &&
            before.at(id).volume_m3 == after.at(id).volume_m3 &&
            before.at(id).material_volumes_m3 == after.at(id).material_volumes_m3,
            "clipboard retains expanded geometry and explicit/default override provenance");
        const auto& original = before.at(id); const auto& copied = after.at(id);
        require(original.nodes.size() == copied.nodes.size() && original.profiles.size() == copied.profiles.size(),
            "clipboard expansion has the same node and profile cardinality");
        for (std::size_t i = 0; i < original.nodes.size(); ++i)
            require(original.nodes[i].part_path == copied.nodes[i].part_path &&
                original.nodes[i].transform == copied.nodes[i].transform &&
                original.nodes[i].source_part == copied.nodes[i].source_part &&
                original.nodes[i].path_override == copied.nodes[i].path_override,
                "clipboard retains node transforms and typed source provenance");
        for (std::size_t i = 0; i < original.profiles.size(); ++i)
            require(original.profiles[i].profile == copied.profiles[i].profile &&
                original.profiles[i].transform == copied.profiles[i].transform &&
                original.profiles[i].material_id == copied.profiles[i].material_id,
                "clipboard retains profile geometry, transforms and material slots");
    }
    const auto remapped = remap_independent_assembly_instance(first_entity,
        {{"instance", "new_root"}, {"catalog", "new_catalog"}});
    auto expected = first_value; expected.instance.id = "new_root"; expected.assembly_catalog_id = "new_catalog";
    require(decode_document_assembly_instance(remapped) == expected && remapped.extensions == first_entity.extensions &&
        remapped.properties.at("opaque") == first_entity.properties.at("opaque"), "typed remap retains local paths and opaque context");
    invalid([&] { (void)remap_independent_assembly_instance(first_entity, {{"instance", "new"}}); });
    invalid([&] { (void)remap_independent_assembly_instance(first_entity, {{"instance", "same"}, {"catalog", "same"}}); });
    invalid([&] { (void)remap_independent_assembly_instance(first_entity, {{"instance", " "}, {"catalog", "new"}}); });
    invalid([&] { (void)remap_independent_assembly_instance(first_entity, {{"instance", "invalid/root"}, {"catalog", "new"}}); });
    invalid([&] { (void)remap_independent_assembly_instance(first_entity, {{"instance", "new"}, {"catalog", std::string(129, 'a')}}); });
    invalid([&] { (void)assembly_clipboard_dependencies(snapshot, {"instance", "instance"}); });
    invalid([&] { (void)assembly_clipboard_dependencies(snapshot, {"catalog"}); });
    require(snapshot.entities() == document.snapshot().entities(), "failed clipboard preparation never mutates source");
}
}
int main() { codec_and_geometry(); references_and_atomic_budget(); commands(); clipboard_closure(); }
