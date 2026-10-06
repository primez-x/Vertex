#include "sketch/architectural_schedule.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/geometry.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/architecture.hpp"
#include <BRepAlgoAPI_Common.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>

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
void architectural_context(Entity& value) {
    value.properties.update(Json{{"property_id", "property"}, {"building_id", "building"},
        {"floor_id", "floor"}, {"layer_id", "layer"}});
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
                 {"mass", {24.5, AssemblyQuantityUnit::kilogram}},
                 {"volume", {0.18, AssemblyQuantityUnit::cubic_metre}}}}},
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
    require(found->object_id == "assembly-catalog:instance:lintel-1" &&
                std::get<std::string>(found->cells.at("name").value) == "Lintel" &&
                std::get<std::string>(found->cells.at("host_entity_id").value) == "host-wall" &&
                std::get<std::int64_t>(found->cells.at("quantity:count").value) == 2 &&
                std::get<ScheduleQuantity>(found->cells.at("quantity:length").value).unit ==
                    ScheduleUnit::metre &&
                std::get<ScheduleQuantity>(found->cells.at("quantity:mass").value).unit ==
                    ScheduleUnit::kilogram,
            "assembly schedule should expose resolved placement and quantities");
    for (const auto& [name, cell] : found->cells)
        require(!cell.editable, "assembly schedule source data should be read-only");
    const auto material_row = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
        [](const auto& row) {
            return row.kind == ScheduleRowKind::material &&
                   row.cells.contains("instance_id") &&
                   std::get<std::string>(row.cells.at("instance_id").value) == "lintel-1";
        });
    require(material_row != projection.snapshot.rows.end(),
            "placed assembly material slot should have a material schedule row");
    require(std::get<std::string>(material_row->cells.at("name").value) == "Steel" &&
                std::get<std::string>(material_row->cells.at("catalog_id").value) ==
                    "assembly-catalog" &&
                std::get<std::string>(material_row->cells.at("material_id").value) == "steel" &&
                !material_row->cells.contains("volume") &&
                std::get<ScheduleQuantity>(material_row->cells.at("declared_volume").value).unit ==
                    ScheduleUnit::cubic_metre &&
                std::abs(std::get<ScheduleQuantity>(material_row->cells.at("declared_volume").value).value -
                         0.18) < 1e-9 &&
                !material_row->cells.at("declared_volume").editable,
            "declaration-only material rows must keep authored volume separate from measured takeoff");
    const auto material_summary_row = std::find_if(
        projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
        [](const auto& row) {
            return row.kind == ScheduleRowKind::material_summary &&
                   row.cells.contains("name") &&
                   std::get<std::string>(row.cells.at("name").value) == "Steel";
        });
    require(material_summary_row != projection.snapshot.rows.end() &&
                std::get<std::int64_t>(material_summary_row->cells.at("count").value) == 1 &&
                !material_summary_row->cells.contains("volume"),
            "declaration-only assemblies must withhold unmeasured grouped material volume");
    const auto visible = build_architectural_schedules(document.snapshot(), {"host-wall"});
    require(std::any_of(visible.snapshot.rows.begin(), visible.snapshot.rows.end(),
                        [](const auto& row) { return row.kind == ScheduleRowKind::assembly; }),
            "assembly schedule visibility should follow the placed host");
    const auto hidden = build_architectural_schedules(document.snapshot(), {"other-wall"});
    require(std::none_of(hidden.snapshot.rows.begin(), hidden.snapshot.rows.end(),
                         [](const auto& row) { return row.kind == ScheduleRowKind::assembly; }),
            "hidden assembly hosts should be omitted from scoped schedules");
    require(std::none_of(hidden.snapshot.rows.begin(), hidden.snapshot.rows.end(),
                         [](const auto& row) {
                             return row.kind == ScheduleRowKind::material &&
                                    row.cells.contains("instance_id");
                         }),
            "hidden assembly hosts should omit their derived material rows");
}

void test_building_object_rows_expose_dimensions_and_solid_volume() {
    auto column = encode_building_entity(
        RectangularColumn{"column-1", {0.0, 0.0, 0.0}, 0.4, 0.5, 3.0, 0.25});
    column.properties["mark"] = "C-1";
    auto beam = encode_building_entity(
        Beam{"beam-1", {0.0, 0.0, 2.4}, {5.0, 0.0, 2.4}, {0.0, 0.0, 1.0}, 0.3, 0.2});
    beam.properties["mark"] = "B-1";
    auto stair = encode_building_entity(
        StairFlight{"stair-1", {0.0, 1.0, 0.0}, 0.0, 10, 2.5, 0.28, 1.1, std::nullopt});
    stair.properties["mark"] = "S-1";
    auto railing = encode_building_entity(
        Railing{"rail-1", {0.0, 0.0, 0.0}, 0.0, 4.0, 1.0, 0.05, 1.0});
    railing.properties["mark"] = "R-1";
    const auto document = Document::create({column, beam, stair, railing});
    const auto projection = build_architectural_schedules(document.snapshot());
    require(projection.diagnostics.empty(),
            "valid building-object schedule rows must not produce diagnostics");

    const auto find = [&](const std::string& id) -> const ScheduleRow& {
        const auto found = std::find_if(
            projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
            [&](const auto& candidate) { return candidate.object_id == id; });
        if (found == projection.snapshot.rows.end())
            throw std::runtime_error("missing building-object schedule row");
        return *found;
    };
    const auto& column_row = find("column-1");
    require(column_row.kind == ScheduleRowKind::building &&
                std::get<std::string>(column_row.cells.at("form").value) ==
                    "rectangular_column" &&
                std::get<ScheduleQuantity>(column_row.cells.at("width").value).value == 0.4,
            "column schedule should expose its semantic form and dimensions");
    require(!column_row.cells.at("volume").editable &&
                std::abs(std::get<ScheduleQuantity>(column_row.cells.at("volume").value).value -
                         0.6) < 1e-9 &&
                column_row.cells.at("volume").sources ==
                    std::vector<ScheduleSourceRef>{{"column-1", "geometry"}},
            "column schedule volume should be read-only and geometry-backed");

    const auto& beam_row = find("beam-1");
    require(std::get<ScheduleQuantity>(beam_row.cells.at("length").value).value == 5.0 &&
                std::abs(std::get<ScheduleQuantity>(beam_row.cells.at("volume").value).value -
                         0.3) < 1e-9,
            "beam schedule should expose length and derived volume");
    require(find("stair-1").kind == ScheduleRowKind::building &&
                find("rail-1").kind == ScheduleRowKind::building,
            "all supported building object families should have schedule rows");

    const auto visible = build_architectural_schedules(document.snapshot(), {"beam-1"});
    require(std::any_of(visible.snapshot.rows.begin(), visible.snapshot.rows.end(),
                        [](const auto& candidate) {
                            return candidate.object_id == "beam-1" &&
                                   candidate.kind == ScheduleRowKind::building;
                        }) &&
                std::none_of(visible.snapshot.rows.begin(), visible.snapshot.rows.end(),
                             [](const auto& candidate) { return candidate.object_id == "column-1"; }),
            "building schedule visibility should follow the shared source filter");

    const auto levels = VerticalLevelGraph({{"ground", 0.0}, {"first", 3.0}},
                                           {{"ground-first", "ground", "first"}});
    auto connected_stair = encode_building_entity(StairFlight{
        "stair-connected", {0.0, 0.0, 0.0}, 0.0, 6, 3.0, 0.25, 1.1,
        std::nullopt,
        StairLevelConnection{"levels-1", "ground-first", "ground", "first"}});
    connected_stair.properties["mark"] = "S-2";
    auto graph_entity = Entity::create(
        "vertical_levels", {{"model", Json::parse(levels.serialize())}});
    graph_entity.id = "levels-1";
    const auto connected_document = Document::create({graph_entity, connected_stair});
    const auto connected_projection = build_architectural_schedules(
        connected_document.snapshot());
    const auto connected_row = std::find_if(
        connected_projection.snapshot.rows.begin(), connected_projection.snapshot.rows.end(),
        [](const auto& candidate) { return candidate.object_id == "stair-connected"; });
    require(connected_row != connected_projection.snapshot.rows.end() &&
                std::get<std::string>(connected_row->cells.at("level_graph_id").value) ==
                    "levels-1" &&
                std::get<std::string>(connected_row->cells.at("level_link_id").value) ==
                    "ground-first" &&
                std::get<std::string>(connected_row->cells.at("lower_level_id").value) ==
                    "ground" &&
                std::get<std::string>(connected_row->cells.at("upper_level_id").value) ==
                    "first" &&
                !connected_row->cells.at("level_link_id").editable,
            "building schedule should expose connected stair level provenance");
}

void test_multi_flight_and_hosted_railing_quantities() {
    StairFlight stair{"multi", {}, 0, 8, 2.0, 0.25, 1.0};
    stair.flights = {{"first", 4}, {"second", 4}};
    stair.landings = {{"turn", 1.0, 0.15, StairTurn::left_quarter, 0}};
    Railing railing{"hosted", {}, 0, 0, 0.9, 0.05, 0.5};
    railing.host = StairRailingHost{"multi", "second", StairRailingSide::right, 0, 1};
    auto catalog = Entity::create("assembly_model", {{"model", AssemblyModel::create(
        {{"solid", "Solid material"}}, {}, {}).to_json()}});
    catalog.id = "catalog";
    auto stair_entity = assigned(encode_building_entity(stair));
    architectural_context(stair_entity);
    stair_entity.properties["run_m"] = 99.0;
    stair_entity.properties["rise_m"] = 99.0;
    auto rail_entity = assigned(encode_building_entity(railing));
    architectural_context(rail_entity);
    rail_entity.properties["length"] = 99.0;
    rail_entity.properties["post_count"] = 99;
    auto document = Document::create({catalog,
        {"property", "property", Json::object()},
        {"building", "building", {{"property_id", "property"}}},
        {"floor", "floor", {{"building_id", "building"}}},
        {"layer", "layer", {{"floor_id", "floor"}}}, stair_entity, rail_entity});
    const auto source = document.snapshot();
    const auto projected = build_architectural_schedules(document.snapshot());
    require(projected.diagnostics.empty(), "valid multi-flight stair and hosted rail produce quantities");
    const auto building_row = [](const auto& result, const std::string& id) -> const ScheduleRow& {
        const auto found = std::find_if(result.snapshot.rows.begin(), result.snapshot.rows.end(),
            [&](const auto& candidate) { return candidate.object_id == id; });
        if (found == result.snapshot.rows.end()) throw std::runtime_error("missing stair quantity row");
        return *found;
    };
    const auto& stairs = building_row(projected, "multi");
    require(std::get<ScheduleQuantity>(stairs.cells.at("rise").value).value == 2.0 &&
        std::get<ScheduleQuantity>(stairs.cells.at("riser_height").value).value == 0.25 &&
        std::get<ScheduleQuantity>(stairs.cells.at("run").value).value == 2.0 &&
        std::get<std::int64_t>(stairs.cells.at("flight_count").value) == 2 &&
        std::get<std::int64_t>(stairs.cells.at("landing_count").value) == 1,
        "canonical stair layout overrides unrelated opaque rise/run properties");
    const auto& rails = building_row(projected, "hosted");
    const auto expected_length = std::hypot(0.95, 0.95);
    require(std::abs(std::get<ScheduleQuantity>(rails.cells.at("length").value).value - expected_length) < 1e-9 &&
        std::get<std::int64_t>(rails.cells.at("post_count").value) == 4 &&
        std::get<std::string>(rails.cells.at("host_stair_id").value) == "multi" &&
        std::get<std::string>(rails.cells.at("host_flight_id").value) == "second" &&
        std::get<std::string>(rails.cells.at("side").value) == "right",
        "hosted rail quantities include actual pitch-line length and stable attachment");
    require(document.snapshot().entities() == source.entities() &&
        document.snapshot().entities().at("multi").properties.at("run_m") == 99.0 &&
        document.snapshot().entities().at("multi").properties.at("rise_m") == 99.0 &&
        document.snapshot().entities().at("hosted").properties.at("length") == 99.0 &&
        document.snapshot().entities().at("hosted").properties.at("post_count") == 99,
        "schedule derivation preserves opaque source properties without trusting them");
    require(std::find(rails.cells.at("volume").sources.begin(), rails.cells.at("volume").sources.end(),
        ScheduleSourceRef{"multi", "geometry"}) != rails.cells.at("volume").sources.end() &&
        std::find(row(projected, "hosted").cells.at("volume").sources.begin(),
            row(projected, "hosted").cells.at("volume").sources.end(), ScheduleSourceRef{"multi", "geometry"}) !=
            row(projected, "hosted").cells.at("volume").sources.end(),
        "hosted solid and material volumes retain current stair provenance");
    const auto visible = build_architectural_schedules(document.snapshot(), {"hosted"});
    require(building_row(visible, "hosted").cells.contains("length") && visible.diagnostics.empty(),
        "a visible railing can use an existing host outside the schedule filter");
    stair.going = 0.35;
    auto changed_stair = assigned(encode_building_entity(stair));
    architectural_context(changed_stair);
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(std::move(changed_stair))}, {}, "extend host"});
    const auto changed = build_architectural_schedules(document.snapshot());
    require(std::get<ScheduleQuantity>(building_row(changed, "hosted").cells.at("length").value).value > expected_length,
        "unchanged rail schedule recalculates after host geometry changes");
    auto future_rail = assigned(encode_building_entity(railing));
    architectural_context(future_rail);
    future_rail.properties["version"] = 3;
    const auto unavailable = build_architectural_schedules(Document::create({catalog,
        {"property", "property", Json::object()},
        {"building", "building", {{"property_id", "property"}}},
        {"floor", "floor", {{"building_id", "building"}}},
        {"layer", "layer", {{"floor_id", "floor"}}}, stair_entity, future_rail}).snapshot());
    require(std::none_of(unavailable.snapshot.rows.begin(), unavailable.snapshot.rows.end(),
        [](const auto& candidate) { return candidate.object_id == "hosted"; }) &&
        !row(unavailable, "hosted").cells.contains("volume") && !unavailable.diagnostics.empty(),
        "unsupported future hosted rail withholds building and material quantities");
    Railing guard{"landing-guard",{},0,0,.9,.05,.5};
    guard.landing_host=StairLandingRailingHost{"multi",StairLandingRole::connecting,"turn","first","second",0,0,1};
    auto guard_entity=assigned(encode_building_entity(guard));architectural_context(guard_entity);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(guard_entity)}, {},"landing guard"});
    const auto with_guard=build_architectural_schedules(document.snapshot());
    require(with_guard.diagnostics.empty(),"landing guard schedules derive native geometry");
    const auto& landing_row=building_row(with_guard,guard.id);
    require(std::abs(std::get<ScheduleQuantity>(landing_row.cells.at("length").value).value-.95)<1e-9 &&
        std::get<std::string>(landing_row.cells.at("host_role").value)=="connecting" &&
        std::get<std::string>(landing_row.cells.at("host_landing_id").value)=="turn" &&
        std::get<std::string>(landing_row.cells.at("host_incoming_flight_id").value)=="first" &&
        std::get<std::string>(landing_row.cells.at("host_outgoing_flight_id").value)=="second" &&
        std::get<std::int64_t>(landing_row.cells.at("edge_index").value)==0 &&
        !landing_row.cells.contains("host_flight_id") && !landing_row.cells.contains("side"),
        "landing rows expose actual role/witnesses/edge without fictional flight-side values");
    require(std::find(landing_row.cells.at("volume").sources.begin(),landing_row.cells.at("volume").sources.end(),
        ScheduleSourceRef{"multi","geometry"})!=landing_row.cells.at("volume").sources.end(),"landing quantity retains host provenance");
    stair.top_landing=StairLanding{1.2,.15};
    changed_stair=assigned(encode_building_entity(stair));architectural_context(changed_stair);
    guard.landing_host=StairLandingRailingHost{"multi",StairLandingRole::top,"","second","",0,0,1};
    guard_entity=assigned(encode_building_entity(guard));architectural_context(guard_entity);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(changed_stair),EntityChange::upsert(guard_entity)}, {},"top landing guard"});
    const auto top_projection=build_architectural_schedules(document.snapshot());
    const auto& top_row=building_row(top_projection,guard.id);
    require(top_projection.diagnostics.empty()&&std::get<std::string>(top_row.cells.at("host_role").value)=="top" &&
        !top_row.cells.contains("host_landing_id")&&!top_row.cells.contains("host_outgoing_flight_id"),"top schedule has explicit role and no invented child");
}
void test_joined_roof_schedule_net_priority_and_gross_provenance() {
    SlopedRoofPanel pa{"a",{0,0,0},0,2,4,1,std::atan(.5),0,.1,{}};
    auto pb = pa; pb.id = "b"; pb.base_position = {1.9,0,.95};
    auto a = assigned(encode_building_entity(pa)), b = assigned(encode_building_entity(pb));
    a.properties["material_assignment"]["material_id"] = "red";
    b.properties["material_assignment"]["material_id"] = "blue";
    Entity catalog{"catalog","assembly_model", {{"model",AssemblyModel::create(
        {{"red","Red","#ff0000"},{"blue","Blue","#0000ff"}}, {}, {}).to_json()}}};
    RoofJoin semantic{"join",{"a","b"}};
    Entity join{"join","roof_join",roof_join_json(semantic)};
    auto document = Document::create({a,b,catalog,join});
    const auto sa = make_sloped_roof_panel(pa), sb = make_sloped_roof_panel(pb);
    BRepAlgoAPI_Common common(sa,sb); common.Build();
    require(common.IsDone() && !common.HasErrors(), "independent joined schedule fixture common must succeed");
    const auto va = solid_volume(sa), vb = solid_volume(sb), overlap = solid_volume(common.Shape());
    const auto expected = va + vb - overlap;
    const auto find = [](const DocumentScheduleProjection& projection, const std::string& id) -> const ScheduleRow& {
        const auto found = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
            [&](const auto& candidate) { return candidate.object_id == id; });
        if (found == projection.snapshot.rows.end()) throw std::runtime_error("missing joined schedule row");
        return *found;
    };
    const auto summary_total = [](const DocumentScheduleProjection& projection) {
        double total = 0;
        for (const auto& candidate : projection.snapshot.rows)
            if (candidate.kind == ScheduleRowKind::material_summary)
                total += std::get<ScheduleQuantity>(candidate.cells.at("volume").value).value;
        return total;
    };
    const auto projected = build_architectural_schedules(document.snapshot());
    require(projected.diagnostics.empty(), "joined roof schedule should have complete measured quantities");
    require(std::abs(std::get<ScheduleQuantity>(find(projected,"join").cells.at("volume").value).value-expected) < 1e-8 &&
        std::abs(summary_total(projected)-expected) < 1e-8, "joined material summaries must exclude source gross quantities");
    const auto& second = find(projected,roof_join_material_schedule_row_id("join","b"));
    require(std::abs(std::get<ScheduleQuantity>(second.cells.at("volume").value).value-(vb-overlap)) < 1e-8 &&
        std::get<std::string>(second.cells.at("material_id").value) == "blue" &&
        second.cells.at("volume").explanation.find("earlier members") != std::string::npos &&
        std::find(second.cells.at("volume").sources.begin(),second.cells.at("volume").sources.end(),
            ScheduleSourceRef{"a","geometry"}) != second.cells.at("volume").sources.end(),
        "net rows expose source binding, authored priority, and upstream overlap provenance");
    require(std::get<std::string>(row(projected,"a").cells.at("takeoff_basis").value) == "source_gross" &&
        std::abs(std::get<ScheduleQuantity>(row(projected,"a").cells.at("gross_volume").value).value-va) < 1e-8,
        "source gross rows must remain identifiable and independently measurable");
    semantic.roof_ids = {"b","a"}; join.properties = roof_join_json(semantic);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(join)}, {},"reverse material priority"});
    const auto reversed = build_architectural_schedules(document.snapshot());
    require(std::abs(summary_total(reversed)-expected) < 1e-8 &&
        std::abs(std::get<ScheduleQuantity>(find(reversed,roof_join_material_schedule_row_id("join","a")).cells.at("volume").value).value-(va-overlap)) < 1e-8,
        "order reversal changes net material ownership and preserves union total");
    semantic.material_assignment = RoofJoinMaterialAssignment{"catalog","blue"};
    join.properties = roof_join_json(semantic);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(join)}, {},"override joined material"});
    const auto override_projection = build_architectural_schedules(document.snapshot());
    require(override_projection.diagnostics.empty() && std::abs(summary_total(override_projection)-expected) < 1e-8 &&
        std::get<std::string>(find(override_projection,roof_join_material_schedule_row_id("join","a")).cells.at("material_binding").value) == "join_override" &&
        std::get<std::string>(find(override_projection,roof_join_material_schedule_row_id("join","a")).cells.at("material_id").value) == "blue",
        "explicit join assignment overrides all net regions without adding gross quantities");
    pb.base_position = pa.base_position;
    b = assigned(encode_building_entity(pb)); b.properties["material_assignment"]["material_id"] = "blue";
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(b)}, {},"fully occlude member"});
    const auto occluded = build_architectural_schedules(document.snapshot());
    require(occluded.diagnostics.empty() &&
        std::get<ScheduleQuantity>(find(occluded,roof_join_material_schedule_row_id("join","a")).cells.at("volume").value).value == 0 &&
        std::abs(summary_total(occluded)-va) < 1e-8, "zero occluded quantities must remain complete material takeoffs");
    const auto visible = build_architectural_schedules(document.snapshot(), {"join","a","b"});
    require(std::abs(summary_total(visible)-va) < 1e-8, "visibility scope containing join members uses joined net totals");
}

void test_joined_roof_row_keys_preserve_colon_bearing_identities() {
    const std::string first_source = "b:roof:c:layer:literal", second_source = "c:layer:literal";
    const std::string first_join = "a", second_join = "a:roof:b";
    require(first_join + ":roof:" + first_source == second_join + ":roof:" + second_source,
        "collision fixture must exercise ambiguous legacy row concatenation");
    const auto make_roof = [](const std::string& id, double x) {
        return assigned(encode_building_entity(SlopedRoofPanel{id,{x,0,0},0,2,4,0,0,0,.1,{}}));
    };
    Entity catalog{"catalog","assembly_model", {{"model",AssemblyModel::create(
        {{"solid","Solid"}}, {}, {}).to_json()}}};
    auto document = Document::create({catalog,make_roof(first_source,0),make_roof("first-peer",1.9),
        make_roof(second_source,10),make_roof("second-peer",11.9),
        Entity{first_join,"roof_join",roof_join_json(RoofJoin{first_join,{first_source,"first-peer"}})},
        Entity{second_join,"roof_join",roof_join_json(RoofJoin{second_join,{second_source,"second-peer"}})}});
    const auto projection = build_architectural_schedules(document.snapshot());
    require(projection.diagnostics.empty(), "colon-bearing source IDs must resolve complete material identities");
    const auto first_key = roof_join_material_schedule_row_id(first_join,first_source);
    const auto second_key = roof_join_material_schedule_row_id(second_join,second_source);
    require(first_key != second_key, "length-prefixed row paths must distinguish colliding source pairs");
    for (const auto& [key,source,join] : std::vector<std::tuple<std::string,std::string,std::string>>{
        {first_key,first_source,first_join},{second_key,second_source,second_join}}) {
        const auto found = std::find_if(projection.snapshot.rows.begin(),projection.snapshot.rows.end(),
            [&](const auto& candidate) { return candidate.object_id == key; });
        require(found != projection.snapshot.rows.end() &&
            std::get<std::string>(found->cells.at("source_roof_id").value) == source &&
            std::get<std::string>(found->cells.at("joined_roof_id").value) == join,
            "encoded row keys must preserve readable exact source and join provenance");
        require(std::get<std::string>(row(projection,source).cells.at("takeoff_basis").value) == "source_gross",
            "colon-bearing source gross rows must remain excluded from joined summaries");
    }
    std::set<std::string> keys;
    for (const auto& candidate : projection.snapshot.rows)
        require(keys.insert(candidate.object_id).second, "distinct joins must produce distinct schedule row IDs");
    require(std::abs(std::get<ScheduleQuantity>(material_summary(projection).cells.at("volume").value).value - 3.12) < 1e-8,
        "colon-bearing sources must not double count gross roof quantities");
}
}
void test_independent_nested_assembly_takeoff_and_refresh() {
    const auto profile_rectangle = [](double w,double h) {
        return Boundary{{{0,0},{w,0},0},{{w,0},{w,h},0},{{w,h},{0,h},0},{{0,h},{0,0},0}};
    };
    AssemblyType leaf{"leaf","Leaf",{},{{"core","timber"}},
        {{"volume",{99,AssemblyQuantityUnit::cubic_metre}},{"pieces",{2,AssemblyQuantityUnit::count}}}};
    leaf.profiles={{"ring",profile_rectangle(4,3),
        {Boundary{{{1,1},{2,1},0},{{2,1},{2,2},0},{{2,2},{1,2},0},{{1,2},{1,1},0}}},0,1,"core"},
        {"cap",profile_rectangle(1,1),{},2,0.5,"core"}};
    AssemblyType root{"root","Root"};
    root.parts={{"part:stable","leaf",{{1,2,3},0.5,2}}};
    AssemblyInstance legacy{"legacy","root"};
    legacy.placement=AssemblyPlacement{"legacy-host",{0,0},0,1};
    const auto model=AssemblyModel::create({{"timber","Timber"},{"steel","Steel"}},{root,leaf},{legacy});
    Entity catalog{"catalog","assembly_model",{{"model",model.to_json()}}};
    AssemblyInstance instance{"independent","root"};
    instance.root_transform=AssemblyTransform{{10,20,4},0.25,1};
    AssemblyPathOverride override;
    override.part_path={"part:stable"};
    override.material_overrides={{"core","steel"}};
    override.quantity_overrides={{"pieces",{7,AssemblyQuantityUnit::count}}};
    instance.nested_overrides={override};
    Entity source{"independent","assembly_instance",Json::object()};
    source=encode_document_assembly_instance(source,{catalog.id,instance});
    const Entity host{"legacy-host","wall",Json::object()};
    auto document=Document::create({catalog,source,host});
    const auto before=document.snapshot();
    const auto projected=build_architectural_schedules(before,{source.id});
    require(projected.diagnostics.empty(),"valid independent assembly resolves an invisible supporting catalog");
    double material_volume=0;
    std::size_t assembly_rows=0,material_rows=0;
    for(const auto& row:projected.snapshot.rows) {
        if(row.kind==ScheduleRowKind::assembly) {
            ++assembly_rows;
            require(row.object_id==source.id && std::get<std::int64_t>(row.cells.at("count").value)==1 &&
                std::get<ScheduleQuantity>(row.cells.at("quantity:volume").value).value==99 &&
                std::get<std::int64_t>(row.cells.at("quantity:pieces").value)==7 &&
                std::abs(std::get<ScheduleQuantity>(row.cells.at("volume").value).value-92)<1e-7,
                "one independent owner retains declared quantities distinct from its actual 92 cubic metres");
        }
        if(row.kind==ScheduleRowKind::material) {
            ++material_rows;
            material_volume+=std::get<ScheduleQuantity>(row.cells.at("volume").value).value;
            require(std::get<std::string>(row.cells.at("part_path").value)==Json::array({"part:stable"}).dump() &&
                std::get<std::string>(row.cells.at("catalog_id").value)==catalog.id &&
                std::get<std::string>(row.cells.at("material_id").value)=="steel" &&
                std::get<std::string>(row.cells.at("name").value)=="Steel" &&
                std::find(row.cells.at("volume").sources.begin(),row.cells.at("volume").sources.end(),
                    ScheduleSourceRef{source.id,"instance"})!=row.cells.at("volume").sources.end(),
                "actual material rows retain exact part path, catalog identity and independent source");
        }
    }
    require(assembly_rows==1 && material_rows==2 && std::abs(material_volume-92)<1e-7 &&
        std::abs(std::get<ScheduleQuantity>(material_summary(projected).cells.at("volume").value).value-92)<1e-7,
        "profile takeoff and material summary conserve volume without duplicate assembly rows");
    const auto complete=build_architectural_schedules(before);
    double complete_volume=0;
    std::size_t complete_assemblies=0,complete_profiles=0;
    for(const auto& candidate:complete.snapshot.rows) {
        if(candidate.kind==ScheduleRowKind::assembly) ++complete_assemblies;
        if(candidate.kind==ScheduleRowKind::material) {
            ++complete_profiles;
            complete_volume+=std::get<ScheduleQuantity>(candidate.cells.at("volume").value).value;
        }
    }
    require(complete.diagnostics.empty() && complete_assemblies==2 && complete_profiles==4 &&
        std::abs(complete_volume-184)<1e-7,
        "legacy and independent owners each contribute their profiles exactly once, without duplicate slot volume");
    instance.root_transform->scale=0.5;
    source=encode_document_assembly_instance(source,{catalog.id,instance});
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(source)}, {},"scale independent assembly"});
    const auto after=build_architectural_schedules(document.snapshot(),{source.id});
    require(after.snapshot.revision==document.revision() &&
        std::abs(std::get<ScheduleQuantity>(material_summary(after).cells.at("volume").value).value-11.5)<1e-7 &&
        std::get<ScheduleQuantity>(material_summary(projected).cells.at("volume").value).value>91.9,
        "new snapshot refreshes scaled solids and old derived snapshot remains immutable");
    document.undo(document.revision());
    require(build_architectural_schedules(document.snapshot(),{source.id}).snapshot.rows==projected.snapshot.rows,
        "undo restores source-bound derived values and stable profile row identities");
    require(build_architectural_schedules(document.snapshot(),{}).snapshot.rows.empty(),
        "hidden independent owner contributes no takeoff despite its supporting catalog");
    auto broken=document.snapshot().entities();
    broken.at(source.id).properties["assembly_catalog_id"]="missing";
    try {
        const auto malformed=Document::create({broken.at(catalog.id),broken.at(source.id),host});
        const auto unavailable=build_architectural_schedules(malformed.snapshot(),{source.id});
        require(!unavailable.diagnostics.empty() && unavailable.snapshot.rows.empty(),
            "invalid independent source reports a failed takeoff instead of partial rows");
    } catch (const DocumentError& error) {
        require(error.code()==DocumentErrorCode::dangling_reference ||
            error.code()==DocumentErrorCode::invalid_entity,
            "the integrated document boundary may reject invalid assembly references before projection");
    }
}

int main() {
    try {
        test_independent_nested_assembly_takeoff_and_refresh();
        test_net_volumes();
        test_joined_roof_schedule_net_priority_and_gross_provenance();
        test_joined_roof_row_keys_preserve_colon_bearing_identities();
        test_explicit_material_grouping_is_stable_and_normalized();
        test_composite_wall_layers_produce_material_quantities();
        test_composite_slab_layers_produce_material_quantities();
        test_placed_assemblies_produce_read_only_quantity_rows();
        test_building_object_rows_expose_dimensions_and_solid_volume();
        test_multi_flight_and_hosted_railing_quantities();
        std::cout<<"architectural schedule tests passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
