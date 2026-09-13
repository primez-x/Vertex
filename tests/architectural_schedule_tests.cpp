#include "sketch/architectural_schedule.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json rectangle(double x, double y, double width, double depth) {
    Json result = Json::array();
    const std::vector<Vec2> points{{x,y},{x+width,y},{x+width,y+depth},{x,y+depth}};
    for (std::size_t i=0; i<4; ++i) {
        const auto a=points[i], b=points[(i+1)%4];
        result.push_back({{"start", {a.x,a.y}}, {"end", {b.x,b.y}}, {"sweep_radians", 0.0}});
    }
    return result;
}
Entity assigned(Entity value) {
    value.properties["mark"] = value.id;
    value.properties["material_assignment"] = {{"version",1},{"catalog_id","catalog"},{"material_id","solid"}};
    return value;
}
const ScheduleRow& row(const DocumentScheduleProjection& projection, const std::string& id) {
    const auto found = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
        [&](const auto& value) { return value.object_id == id + ":material"; });
    if (found == projection.snapshot.rows.end()) throw std::runtime_error("missing material row");
    return *found;
}
const ScheduleRow& material_summary(const DocumentScheduleProjection& projection) {
    const auto found = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
        [](const auto& value) { return value.kind == ScheduleRowKind::material_summary; });
    if (found == projection.snapshot.rows.end()) throw std::runtime_error("missing material summary row");
    return *found;
}
void volume(const DocumentScheduleProjection& projection, const std::string& id, double expected) {
    const auto& cell = row(projection,id).cells.at("volume");
    const auto value = std::get<ScheduleQuantity>(cell.value);
    require(value.unit == ScheduleUnit::cubic_metre && std::abs(value.value-expected)<1e-7 && !cell.editable,
        "net volume must match analytic quantity and remain read-only");
}
void test_net_volumes() {
    auto catalog=Entity::create("assembly_model", {{"version",1},
        {"model",AssemblyModel::create({{"solid","Solid material"}}, {}, {}).to_json()}});
    catalog.id="catalog";
    auto wall=Entity::create("wall", {{"baseline",{{"start",{0,0}},{"end",{10,0}},{"sweep_radians",0}}},
        {"height_m",3},{"thickness_m",0.2},{"elevation_m",0}});
    wall.id="wall";
    auto opening=Entity::create("opening", {{"wall_id","wall"},{"opening_kind","door"},{"mark","D1"},
        {"offset_m",2},{"width_m",1},{"height_m",2},{"sill_m",0}});
    opening.id="door";
    auto slab=Entity::create("slab",{{"boundary",rectangle(0,0,10,8)},
        {"holes",Json::array({rectangle(2,2,2,2)})},{"thickness_m",0.25},{"elevation_m",0},
        {"element_kind", "floor"}});
    slab.id="slab";
    auto roof=encode_building_entity(HipRoof{"roof",{0,0,4},0,10,8,2,std::atan(0.5),0,0.2,
        {{"cut",-1,-1,2,2}}});
    auto column=encode_building_entity(RectangularColumn{"column",{0,0,0},0.4,0.5,3,0.3});
    auto document=Document::create({catalog,assigned(wall),opening,assigned(slab),assigned(roof),assigned(column)});
    const auto projection=build_architectural_schedules(document.snapshot());
    require(projection.diagnostics.empty(),"valid assigned solids must have complete volume projection");
    volume(projection,"wall",5.6);
    volume(projection,"slab",19.0);
    require(std::get<std::string>(row(projection, "slab").cells.at("element_kind").value) == "floor",
        "material schedules must expose the semantic floor element kind");
    volume(projection,"roof",76*0.2/std::cos(std::atan(0.5)));
    volume(projection,"column",0.6);
    require(row(projection,"wall").cells.at("volume").sources.size()==3,
        "wall volume provenance must include its hosted opening");
    const auto& summary = material_summary(projection);
    require(std::get<std::string>(summary.cells.at("name").value) == "Solid material" &&
        std::get<std::int64_t>(summary.cells.at("count").value) == 4 &&
        std::abs(std::get<ScheduleQuantity>(summary.cells.at("volume").value).value -
                 (5.6 + 19.0 + 76 * 0.2 / std::cos(std::atan(0.5)) + 0.6)) < 1e-7 &&
        !summary.cells.at("volume").editable && summary.cells.at("volume").sources.size() >= 8,
        "assigned material summary must aggregate count, net volume, and provenance");
    const auto visible=build_architectural_schedules(document.snapshot(),{"wall"});
    require(visible.snapshot.rows.size()==2,"takeoff visibility must select objects and its summary");
    volume(visible,"wall",5.6);
    require(std::get<std::int64_t>(material_summary(visible).cells.at("count").value) == 1,
        "visibility filtering must scope grouped material counts");
    bool rejected=false;
    try { (void)make_schedule_edit(projection.snapshot,"wall:material","volume",ScheduleQuantity{7,ScheduleUnit::cubic_metre}); }
    catch (const std::invalid_argument&) { rejected=true; }
    require(rejected,"derived solid volume must not become an authored schedule edit");
    opening.properties["width_m"]=2;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(opening)},{},"widen opening"});
    volume(build_architectural_schedules(document.snapshot()),"wall",5.2);
    document.undo(document.revision());
    volume(build_architectural_schedules(document.snapshot()),"wall",5.6);
    auto broken=document.snapshot().entities().at("wall");
    broken.properties.erase("baseline");
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(broken)},{},"incomplete wall"});
    const auto invalid=build_architectural_schedules(document.snapshot());
    require(!row(invalid,"wall").cells.contains("volume") && !invalid.diagnostics.empty(),
        "invalid geometry must omit volume and expose a diagnostic without losing valid rows");
    volume(invalid,"slab",19);
}

void test_explicit_material_grouping_is_stable_and_normalized() {
    auto first = Entity::create("room", {{"mark", "R1"}, {"area_m2", 12.0},
        {"material_name", "  Finish   Paint  "}, {"volume_m3", 0.4}});
    first.id = "room-a";
    auto second = Entity::create("room", {{"mark", "R2"}, {"area_m2", 10.0},
        {"material_name", "finish paint"}, {"volume_m3", 0.6}});
    second.id = "room-b";
    auto document = Document::create({first, second});
    const auto projection = build_architectural_schedules(document.snapshot());
    const auto& summary = material_summary(projection);
    require(std::get<std::string>(summary.cells.at("name").value) == "  Finish   Paint  " &&
        std::get<std::int64_t>(summary.cells.at("count").value) == 2 &&
        std::abs(std::get<ScheduleQuantity>(summary.cells.at("volume").value).value - 1.0) < 1e-9,
        "explicit material names must group case-insensitively with collapsed whitespace");
    const auto repeat = build_architectural_schedules(document.snapshot());
    require(repeat.snapshot == projection.snapshot && repeat.diagnostics == projection.diagnostics,
        "material summary IDs and ordering must be deterministic");
    require(!summary.cells.at("name").editable && !summary.cells.at("count").editable,
        "grouped material summary properties must remain read-only");
}

void test_composite_wall_layers_produce_material_quantities() {
    auto catalog = Entity::create("assembly_model", {{"version", 1},
        {"model", AssemblyModel::create({{"brick", "Brick"}, {"paint", "Paint"}}, {}, {}).to_json()}});
    catalog.id = "catalog";
    auto wall = Entity::create("wall", {{"baseline", {{"start", {0, 0}}, {"end", {10, 0}},
                                                           {"sweep_radians", 0}}},
        {"height_m", 3.0}, {"thickness_m", 0.20}, {"elevation_m", 0.0},
        {"layers", Json::array({
            Json{{"id", "outer"}, {"thickness_m", 0.02},
                 {"material_assignment", {{"version", 1}, {"catalog_id", "catalog"},
                                             {"material_id", "brick"}}}},
            Json{{"id", "core"}, {"thickness_m", 0.16},
                 {"material_assignment", {{"version", 1}, {"catalog_id", "catalog"},
                                             {"material_id", "brick"}}}},
            Json{{"id", "inner"}, {"thickness_m", 0.02},
                 {"material_assignment", {{"version", 1}, {"catalog_id", "catalog"},
                                             {"material_id", "paint"}}}},
        })}});
    wall.id = "layered-wall";
    auto opening = Entity::create("opening", {{"wall_id", "layered-wall"},
        {"opening_kind", "door"}, {"mark", "D-layered"}, {"offset_m", 2.0}, {"width_m", 1.0},
        {"height_m", 2.0}, {"sill_m", 0.0}});
    opening.id = "layered-door";
    const auto document = Document::create({catalog, wall, opening});
    const auto projection = build_architectural_schedules(document.snapshot());
    require(projection.diagnostics.empty(), "valid layered material schedules must have no diagnostics");

    const auto find_layer = [&](const std::string& layer_id) -> const ScheduleRow& {
        const auto expected = "layered-wall:layer:" + layer_id + ":material";
        const auto found = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
            [&](const auto& value) { return value.object_id == expected; });
        if (found == projection.snapshot.rows.end()) throw std::runtime_error("missing layer material row");
        return *found;
    };
    const auto layer_volume = [&](const std::string& layer_id, double expected) {
        const auto& cell = find_layer(layer_id).cells.at("volume");
        const auto quantity = std::get<ScheduleQuantity>(cell.value);
        require(quantity.unit == ScheduleUnit::cubic_metre &&
                    std::abs(quantity.value - expected) < 1e-7 && !cell.editable,
                "layer material volume must include the hosted opening cut");
    };
    layer_volume("outer", 0.56);
    layer_volume("core", 4.48);
    layer_volume("inner", 0.56);
    const auto& summary = material_summary(projection);
    require(std::get<std::int64_t>(summary.cells.at("count").value) == 2 &&
                std::abs(std::get<ScheduleQuantity>(summary.cells.at("volume").value).value - 5.04) < 1e-7,
            "layer material summary must aggregate net quantities by catalog material");
}

void test_composite_slab_layers_produce_material_quantities() {
    auto catalog = Entity::create("assembly_model", {{"version", 1},
        {"model", AssemblyModel::create({{"concrete", "Concrete"}, {"finish", "Finish"}}, {}, {}).to_json()}});
    catalog.id = "catalog";
    auto slab = Entity::create("slab", {
        {"boundary", rectangle(0, 0, 10, 8)}, {"holes", Json::array({rectangle(2, 2, 2, 2)})},
        {"thickness_m", 0.25}, {"elevation_m", 0.0}, {"element_kind", "floor"},
        {"layers", Json::array({
            Json{{"id", "structure"}, {"thickness_m", 0.20},
                 {"material_assignment", {{"version", 1}, {"catalog_id", "catalog"},
                                             {"material_id", "concrete"}}}},
            Json{{"id", "finish"}, {"thickness_m", 0.05},
                 {"material_assignment", {{"version", 1}, {"catalog_id", "catalog"},
                                             {"material_id", "finish"}}}},
        })}});
    slab.id = "layered-slab";
    const auto document = Document::create({catalog, slab});
    const auto projection = build_architectural_schedules(document.snapshot());
    require(projection.diagnostics.empty(), "valid layered slab schedules must have no diagnostics");

    const auto find_layer = [&](const std::string& layer_id) -> const ScheduleRow& {
        const auto expected = "layered-slab:layer:" + layer_id + ":material";
        const auto found = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
            [&](const auto& value) { return value.object_id == expected; });
        if (found == projection.snapshot.rows.end()) throw std::runtime_error("missing slab layer material row");
        return *found;
    };
    const auto layer_volume = [&](const std::string& layer_id, double expected) {
        const auto& cell = find_layer(layer_id).cells.at("volume");
        const auto quantity = std::get<ScheduleQuantity>(cell.value);
        require(quantity.unit == ScheduleUnit::cubic_metre &&
                    std::abs(quantity.value - expected) < 1e-7 && !cell.editable,
                "slab layer material volume must include the slab hole cut");
    };
    layer_volume("structure", 15.2);
    layer_volume("finish", 3.8);
    const auto find_summary = [&](const std::string& name) -> const ScheduleRow& {
        const auto found = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
            [&](const auto& value) {
                return value.kind == ScheduleRowKind::material_summary &&
                       value.cells.contains("name") &&
                       std::get<std::string>(value.cells.at("name").value) == name;
            });
        if (found == projection.snapshot.rows.end()) throw std::runtime_error("missing slab material summary row");
        return *found;
    };
    const auto& concrete_summary = find_summary("Concrete");
    const auto& finish_summary = find_summary("Finish");
    require(std::get<std::int64_t>(concrete_summary.cells.at("count").value) == 1 &&
                std::abs(std::get<ScheduleQuantity>(concrete_summary.cells.at("volume").value).value - 15.2) < 1e-7 &&
                std::get<std::int64_t>(finish_summary.cells.at("count").value) == 1 &&
                std::abs(std::get<ScheduleQuantity>(finish_summary.cells.at("volume").value).value - 3.8) < 1e-7,
            "slab layer material summaries must aggregate net quantities by catalog material");
}
void test_placed_assemblies_produce_read_only_quantity_rows() {
    auto catalog = Entity::create("assembly_model", {
        {"model", AssemblyModel::create(
            {{"steel", "Steel"}},
            {AssemblyType{"lintel", "Lintel", {}, {{"finish", "steel"}},
                {{"count", {2, AssemblyQuantityUnit::count}},
                 {"length", {1.8, AssemblyQuantityUnit::metre}},
                 {"mass", {24.5, AssemblyQuantityUnit::kilogram}}}}},
            {AssemblyInstance{"lintel-1", "lintel", {}, {}, {},
                AssemblyPlacement{"host-wall", {0.25, 1.2}, 0.1, 1.0}}}).to_json()}});
    catalog.id = "assembly-catalog";
    auto host = Entity::create("wall", {{"mark", "W1"}});
    host.id = "host-wall";
    const auto document = Document::create({catalog, host});
    const auto projection = build_architectural_schedules(document.snapshot());
    const auto found = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
        [](const auto& row) { return row.kind == ScheduleRowKind::assembly; });
    require(found != projection.snapshot.rows.end(), "placed assembly should have a schedule row");
    require(std::get<std::string>(found->cells.at("name").value) == "Lintel" &&
                std::get<std::string>(found->cells.at("host_entity_id").value) == "host-wall" &&
                std::get<std::int64_t>(found->cells.at("quantity:count").value) == 2 &&
                std::get<ScheduleQuantity>(found->cells.at("quantity:length").value).unit ==
                    ScheduleUnit::metre &&
                std::get<ScheduleQuantity>(found->cells.at("quantity:mass").value).unit ==
                    ScheduleUnit::kilogram,
            "assembly schedule should expose resolved placement and quantities");
    for (const auto& [name, cell] : found->cells)
        require(!cell.editable, "assembly schedule source data should be read-only");
    const auto visible = build_architectural_schedules(document.snapshot(), {"host-wall"});
    require(std::any_of(visible.snapshot.rows.begin(), visible.snapshot.rows.end(),
                        [](const auto& row) { return row.kind == ScheduleRowKind::assembly; }),
            "assembly schedule visibility should follow the placed host");
    const auto hidden = build_architectural_schedules(document.snapshot(), {"other-wall"});
    require(std::none_of(hidden.snapshot.rows.begin(), hidden.snapshot.rows.end(),
                         [](const auto& row) { return row.kind == ScheduleRowKind::assembly; }),
            "hidden assembly hosts should be omitted from scoped schedules");
}
}
int main() {
    try {
        test_net_volumes();
        test_explicit_material_grouping_is_stable_and_normalized();
        test_composite_wall_layers_produce_material_quantities();
        test_composite_slab_layers_produce_material_quantities();
        test_placed_assemblies_produce_read_only_quantity_rows();
        std::cout<<"architectural schedule tests passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
