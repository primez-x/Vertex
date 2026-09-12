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
        {"holes",Json::array({rectangle(2,2,2,2)})},{"thickness_m",0.25},{"elevation_m",0}});
    slab.id="slab";
    auto roof=encode_building_entity(HipRoof{"roof",{0,0,4},0,10,8,2,std::atan(0.5),0,0.2,
        {{"cut",-1,-1,2,2}}});
    auto column=encode_building_entity(RectangularColumn{"column",{0,0,0},0.4,0.5,3,0.3});
    auto document=Document::create({catalog,assigned(wall),opening,assigned(slab),assigned(roof),assigned(column)});
    const auto projection=build_architectural_schedules(document.snapshot());
    require(projection.diagnostics.empty(),"valid assigned solids must have complete volume projection");
    volume(projection,"wall",5.6);
    volume(projection,"slab",19.0);
    volume(projection,"roof",76*0.2/std::cos(std::atan(0.5)));
    volume(projection,"column",0.6);
    require(row(projection,"wall").cells.at("volume").sources.size()==3,
        "wall volume provenance must include its hosted opening");
    const auto visible=build_architectural_schedules(document.snapshot(),{"wall"});
    require(visible.snapshot.rows.size()==1,"takeoff visibility must select objects");
    volume(visible,"wall",5.6);
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
}
int main() {
    try { test_net_volumes(); std::cout<<"architectural schedule tests passed\n"; return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
