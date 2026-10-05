#include "sketch/physical_wall_spaces.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/noninteractive_errors.hpp"
#include "sketch/vertical_levels.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void assert_near(double actual, double expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-9) {
        std::ostringstream detail;
        detail << message << std::setprecision(17) << ": expected " << expected << ", got " << actual;
        throw std::runtime_error(detail.str());
    }
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid physical room geometry was accepted");
}
Entity entity(std::string id, std::string type, Json properties = Json::object()) {
    return {std::move(id), std::move(type), std::move(properties), false, Json::object()};
}
struct Spec { std::string id; Segment baseline; double thickness{0.2}; };
std::vector<Spec> rectangle(std::string prefix = "", Vec2 lo = {0,0}, Vec2 hi = {4,3}) {
    return {{prefix+"bottom",{lo,{hi.x,lo.y},0}}, {prefix+"right",{{hi.x,lo.y},hi,0}},
        {prefix+"top",{hi,{lo.x,hi.y},0}}, {prefix+"left",{{lo.x,hi.y},lo,0}}};
}
Json segment_json(const Segment& s) {
    return {{"start",{s.start.x,s.start.y}}, {"end",{s.end.x,s.end.y}},
        {"sweep_radians",s.sweep_radians}};
}
std::vector<Entity> entities(const std::vector<Spec>& walls) {
    std::vector<Entity> result{entity("property","property"),
        entity("building","building",{{"property_id","property"}}),
        entity("floor","floor",{{"building_id","building"}}),
        entity("layer","layer",{{"floor_id","floor"}})};
    for (const auto& w:walls) result.push_back(entity(w.id,"wall",{
        {"baseline",segment_json(w.baseline)}, {"thickness_m",w.thickness},
        {"height_m",3.0}, {"elevation_m",0.0}, {"layer_id","layer"}}));
    return result;
}
PhysicalWallSpaces detect(const std::vector<Spec>& walls, const char* selected = "bottom") {
    const auto d=Document::create(entities(walls));
    return detect_physical_wall_spaces(d.snapshot(),selected);
}
std::vector<double> areas(const PhysicalWallSpaces& result) {
    std::vector<double> value;
    for (const auto& s:result.spaces) value.push_back(s.area_square_metres);
    std::sort(value.begin(),value.end()); return value;
}
void exact_continuity(const PhysicalWallSpaces& result) {
    const auto check=[](const Boundary& boundary) {
        require(!boundary.empty(),"clear room boundary is empty");
        for (std::size_t i=0;i<boundary.size();++i) {
            const auto a=boundary[i].end,b=boundary[(i+1)%boundary.size()].start;
            require(a.x==b.x && a.y==b.y,"clear output must share bit-identical cyclic endpoints");
        }
    };
    for (const auto& room:result.spaces) {
        check(room.boundary); for (const auto& hole:room.holes) check(hole);
    }
}
void rectangles_and_overlapping_material() {
    const auto ordinary=detect(rectangle());
    require(ordinary.context.complete() && ordinary.spaces.size()==1,"ordinary room not detected");
    assert_near(ordinary.spaces[0].area_square_metres,10.64,"room must use inside wall faces");
    const auto bounds=boundary_bounds(ordinary.spaces[0].boundary);
    assert_near(bounds.minimum.x,0.1,"left clear wall face incorrect");
    assert_near(bounds.maximum.y,2.9,"top clear wall face incorrect");
    auto unequal=rectangle();
    unequal[0].thickness=0.2; unequal[1].thickness=0.4;
    unequal[2].thickness=0.6; unequal[3].thickness=0.8;
    assert_near(detect(unequal).spaces[0].area_square_metres,8.84,"unequal wall thickness was averaged");
    auto retraced=rectangle();
    retraced.push_back({"thicker-bottom",{{4,0},{0,0},0},0.6});
    const auto unioned=detect(retraced);
    assert_near(unioned.spaces[0].area_square_metres,9.88,"overlapping baseline material must use maximum thickness");
    bool saw_overlap=false;
    for (const auto& edge:unioned.spaces[0].source_lineage.at("outer").at("edges"))
        if (edge.at("source_uses").size()==2) saw_overlap=true;
    require(saw_overlap,"coincident wall owner provenance was lost");
}
void analytical_unsplit_partitions() {
    auto x=rectangle();
    x.push_back({"vertical",{{2,0},{2,3},0}});
    x.push_back({"horizontal",{{0,1.5},{4,1.5},0}});
    const auto split=detect(x);
    require(split.spaces.size()==4,"unsplit X partitions must analytically node four rooms");
    for (const auto area:areas(split)) assert_near(area,2.34,"X partition clear area incorrect");
    auto t=rectangle();
    t.push_back({"vertical",{{2,0},{2,3},0}});
    t.push_back({"half-horizontal",{{0,1.5},{2,1.5},0}});
    const auto rooms=detect(t);
    require(rooms.spaces.size()==3,"unsplit T partitions must node three rooms");
    const auto sorted=areas(rooms);
    assert_near(sorted[0],2.34,"T partition first small room area incorrect");
    assert_near(sorted[1],2.34,"T partition second small room area incorrect");
    assert_near(sorted[2],5.04,"T partition full-height room area incorrect");
    bool partial=false;
    for (const auto& room:split.spaces)
        for (const auto& edge:room.source_lineage.at("outer").at("edges"))
            for (const auto& use:edge.at("source_uses")) {
                require(use.at("segment_id")=="baseline","physical room must reference original baseline segments");
                if (use.at("parameter_start").get<double>()>0 || use.at("parameter_end").get<double>()<1) partial=true;
            }
    require(partial,"unsplit wall parameter intervals were lost");
}
void curved_room() {
    const std::vector<Spec> capsule{{"bottom",{{0,0},{4,0},0}},
        {"right",{{4,0},{4,3},std::numbers::pi}},
        {"top",{{4,3},{0,3},0}}, {"left",{{0,3},{0,0},std::numbers::pi}}};
    const auto result=detect(capsule);
    exact_continuity(result);
    require(result.spaces.size()==1,"analytical curved room missing");
    assert_near(result.spaces[0].area_square_metres,11.2+1.96*std::numbers::pi,
        "curved room must retain concentric clear arcs, not baseline or exterior area");
    std::size_t arcs=0;
    for (const auto& s:result.spaces[0].boundary) if (s.sweep_radians!=0) ++arcs;
    require(arcs==2,"clear curved room was tessellated");
}
void nested_physical_holes() {
    auto walls=rectangle("",{0,0},{10,10});
    const auto inner=rectangle("inner-",{3,3},{7,7});
    walls.insert(walls.end(),inner.begin(),inner.end());
    const auto result=detect(walls);
    require(result.spaces.size()==2,"nested physical island must preserve its distinct inner room");
    const auto sorted=areas(result);
    assert_near(sorted[0],14.44,"inner island clear room area incorrect");
    assert_near(sorted[1],78.4,"parent must exclude inner wall exterior plus inner room");
    const auto parent=std::find_if(result.spaces.begin(),result.spaces.end(),[](const auto& r){return !r.holes.empty();});
    require(parent!=result.spaces.end() && parent->holes.size()==1,"physical parent island hole missing");
    assert_near(std::abs(signed_area(parent->holes[0])),17.64,"island hole was offset inward");
    require(parent->source_lineage.at("holes").size()==1,"hole source lineage missing");
    require(!validate_boundary_holes(parent->boundary,parent->holes),"derived physical hole topology invalid");
}
void visibility_context_phase_and_plane() {
    auto fixture=entities(rectangle());
    fixture.push_back(entity("divider","wall",{{"baseline",segment_json({{2,0},{2,3},0})},
        {"thickness_m",0.2},{"height_m",3.0},{"elevation_m",0.0},{"layer_id","layer"}}));
    fixture.push_back(entity("other-floor","floor",{{"building_id","building"}}));
    fixture.push_back(entity("other-layer","layer",{{"floor_id","other-floor"}}));
    fixture.push_back(entity("other-divider","wall",{{"baseline",segment_json({{0,1.5},{4,1.5},0})},
        {"thickness_m",0.2},{"height_m",3.0},{"elevation_m",0.0},{"layer_id","other-layer"}}));
    fixture.push_back(entity("high-divider","wall",{{"baseline",segment_json({{0,1.5},{4,1.5},0})},
        {"thickness_m",0.2},{"height_m",3.0},{"elevation_m",3.0},{"layer_id","layer"}}));
    for (auto& e:fixture) if (e.id=="layer") e.properties["visible"]=false;
    auto d=Document::create(fixture);
    require(detect_physical_wall_spaces(d.snapshot(),"bottom").spaces.size()==2,
        "eye visibility or other floor/elevation must not alter current floor rooms");
    const auto phases=ModelPhases::create({"bottom","right","top","left","divider"},
        {"bottom","right","top","left","divider"},{{"remove","Remove divider",{"divider"},{}}},"remove");
    fixture.push_back(entity("phases","model_phases",{{"model",phases.to_json()}}));
    auto phased=Document::create(fixture);
    const auto phased_rooms=detect_physical_wall_spaces(phased.snapshot(),"bottom");
    require(phased_rooms.spaces.size()==1,
        "demolished wall must not divide active room");
    bool inactive_tracked=false;
    for (const auto& phase:phased_rooms.spaces[0].source_lineage.at("semantic_phases")) {
        require(!phase.contains("model"),"room lineage must not repeat whole remodeling registries");
        require(phase.at("active_alternative")=="remove","compact phase lineage lost active selection");
        for (const auto& owner:phase.at("owners"))
            if (owner.at("owner_id")=="divider" && owner.at("active_state")=="demolished") inactive_tracked=true;
    }
    require(inactive_tracked,"phase lineage omitted inactive context owner decisions");
    rejects([&]{(void)detect_physical_wall_spaces(phased.snapshot(),"divider");});
    fixture.back().properties["model"]=phases.with_active(std::nullopt).to_json();
    auto baseline=Document::create(fixture);
    require(detect_physical_wall_spaces(baseline.snapshot(),"bottom").spaces.size()==2,
        "shared baseline phase must preserve divider");
}
void resolved_plane_and_purity() {
    auto fixture=entities(rectangle());
    const VerticalLevelGraph levels({{"ground",0},{"upper",3}},{{"storey","ground","upper"}});
    fixture.push_back(entity("levels","vertical_levels",{{"model",Json::parse(levels.serialize())}}));
    for (auto& e:fixture) {
        if (e.id=="floor") e.properties["vertical_level_binding"]={{"version",1},{"graph_id","levels"},{"level_id","upper"}};
        if (e.type=="wall") {
            e.properties["vertical_placement"]={{"version",1},{"mode","level"},{"offset_m",e.id=="bottom" ? -1.0 : 0.0}};
            e.properties["elevation_m"]=e.id=="bottom" ? 1.0 : 0.0;
        }
    }
    const auto d=Document::create(fixture); const auto before=d.snapshot();
    assert_near(detect_physical_wall_spaces(before,"bottom").spaces[0].area_square_metres,10.64,
        "effective plane must resolve level and local elevation");
    require(before.entities()==d.snapshot().entities() && before.revision()==d.revision(),
        "room detection mutated input or minted document geometry");
}
void reversal_and_input_order() {
    auto walls=rectangle(); walls.push_back({"divider",{{2,0},{2,3},0}});
    const auto original=detect(walls);
    std::reverse(walls.begin(),walls.end());
    for (auto& w:walls) { std::swap(w.baseline.start,w.baseline.end); w.baseline.sweep_radians=-w.baseline.sweep_radians; }
    const auto reversed=detect(walls);
    require(areas(original)==areas(reversed),"source direction or insertion order changed physical room area");
    for (std::size_t i=0;i<original.spaces.size();++i) {
        const auto& a=original.spaces[i].boundary; const auto& b=reversed.spaces[i].boundary;
        require(a.size()==b.size(),"reversing wall input changed clear boundary pieces");
        for (std::size_t j=0;j<a.size();++j)
            require(a[j].start.x==b[j].start.x && a[j].start.y==b[j].start.y &&
                a[j].end.x==b[j].end.x && a[j].end.y==b[j].end.y && a[j].sweep_radians==b[j].sweep_radians,
                "reversal changed exact physical room geometry");
    }
}
void open_wall_and_hosted_opening() {
    auto open=rectangle(); open.pop_back();
    require(detect(open).spaces.empty(),"open wall chain fabricated a room");
    auto fixture=entities(rectangle());
    fixture.push_back(entity("door","opening",{{"wall_id","bottom"},{"opening_kind","door"},
        {"offset_m",1.0},{"width_m",1.0},{"height_m",2.0},{"sill_m",0.0}}));
    fixture.push_back(entity("outside-stub","wall",{{"baseline",segment_json({{4,3},{5,4},0})},
        {"thickness_m",0.2},{"height_m",3.0},{"elevation_m",0.0},{"layer_id","layer"}}));
    const auto d=Document::create(fixture);
    assert_near(detect_physical_wall_spaces(d.snapshot(),"bottom").spaces[0].area_square_metres,10.64,
        "hosted door or external open stub broke continuous room limits");
}
void intrusive_stubs_islands_and_components() {
    auto stub=rectangle(); stub.push_back({"stub",{{2,0},{2,1},0}});
    const auto notched=detect(stub);
    require(notched.spaces.size()==1 && notched.spaces[0].holes.empty(),"attached stub must form a clear-room notch");
    assert_near(notched.spaces[0].area_square_metres,10.46,"stub material was omitted from clear room area");
    auto island=rectangle(); island.push_back({"island",{{1,1},{3,1},0}});
    const auto holed=detect(island);
    require(holed.spaces.size()==1 && holed.spaces[0].holes.size()==1,"isolated wall must leave a physical obstacle hole");
    assert_near(holed.spaces[0].area_square_metres,10.24,"isolated wall material was omitted");
    bool tracked=false;
    for (const auto& source:holed.spaces[0].source_lineage.at("physical_sources"))
        if (source.at("owner_id")=="island") tracked=true;
    require(tracked,"intrusive owner has no authoritative source snapshot");
    auto blocked=rectangle(); blocked.push_back({"near-divider",{{2,0.05},{2,2.95},0},0.3});
    const auto components=detect(blocked);
    require(components.graph.faces.size()==1 && components.spaces.size()==2,
        "material-connected divider must split clear components without inventing baseline intersections");
    for (const auto& room:components.spaces) {
        require(room.baseline_face_index==0,"split clear component lost its baseline parent");
        assert_near(room.area_square_metres,4.9,"disconnected clear component area incorrect");
    }
}
void translated_rotated_and_stepped_material() {
    auto translated=rectangle("",{1000000,1000000},{1000004,1000003});
    assert_near(detect(translated).spaces[0].area_square_metres,10.64,
        "large world-coordinate translation changed clear analytical area");
    auto rotated=rectangle(); rotated.push_back({"divider",{{2,0},{2,3},0}});
    const double c=std::cos(0.37),s=std::sin(0.37);
    for (auto& wall:rotated) for (auto* p:{&wall.baseline.start,&wall.baseline.end}) {
        const auto old=*p; *p={c*old.x-s*old.y,s*old.x+c*old.y};
    }
    const auto rotation=detect(rotated);
    exact_continuity(rotation);
    require(rotation.spaces.size()==2,"rotated unsplit divider lost room topology");
    for (const auto& room:rotation.spaces) assert_near(room.area_square_metres,5.04,
        "rotation changed clear partition area");
    auto step=rectangle();
    step[0].baseline.end={2,0};
    step.push_back({"bottom-step",{{2,0},{4,0},0},0.4});
    const auto stepped=detect(step);
    require(stepped.spaces.size()==1,"collinear wall width step lost room");
    assert_near(stepped.spaces[0].area_square_metres,10.45,
        "collinear thickness step must retain the physical end-cap face");
    const auto step_edges=stepped.spaces[0].boundary.size();
    require(step_edges>=6,"clear wall width step was simplified to an invented diagonal join");
    auto mixed=entities(rectangle());
    for (auto& e:mixed) {
        if (e.id=="bottom" || e.id=="right") {
            e.properties["property_id"]="property"; e.properties["building_id"]="building";
            e.properties["floor_id"]="floor";
        }
    }
    const auto d=Document::create(mixed);
    assert_near(detect_physical_wall_spaces(d.snapshot(),"bottom").spaces[0].area_square_metres,10.64,
        "equivalent inherited/direct drawing contexts suppressed valid physical joins");
}
void invalid_and_collapsed() {
    auto invalid=entities(rectangle());
    for (auto& e:invalid) if (e.id=="bottom") e.properties["thickness_m"]=0.0;
    const auto bad=Document::create(invalid);
    rejects([&]{(void)detect_physical_wall_spaces(bad.snapshot(),"bottom");});
    auto collapsed=rectangle("",{0,0},{0.1,0.1});
    rejects([&]{(void)detect(collapsed);});
    auto no_context=entities(rectangle());
    for (auto& e:no_context) if (e.id=="bottom") e.properties.erase("layer_id");
    const auto detached=Document::create(no_context);
    rejects([&]{(void)detect_physical_wall_spaces(detached.snapshot(),"bottom");});
}
void scoped_and_bounded_lineage() {
    auto fixture=entities(rectangle());
    fixture.push_back(entity("foreign-floor","floor",{{"building_id","building"}}));
    fixture.push_back(entity("foreign-layer","layer",{{"floor_id","foreign-floor"}}));
    fixture.push_back(entity("foreign-wall","wall",{{"baseline",segment_json({{1,0},{1,3},0})},
        {"thickness_m",0.2},{"height_m",3.0},{"elevation_m",0.0},{"layer_id","foreign-layer"}}));
    const auto irrelevant=ModelPhases::create({"foreign-wall"},{"foreign-wall"},
        {{"foreign-alternative",std::string(10000,'x'),{},{}}},"foreign-alternative");
    fixture.push_back(entity("foreign-phases","model_phases",{{"model",irrelevant.to_json()}}));
    const auto d=Document::create(fixture);
    const auto room=detect_physical_wall_spaces(d.snapshot(),"bottom");
    require(room.spaces[0].source_lineage.at("semantic_phases").empty(),
        "unrelated phase model amplified current room lineage");
    std::vector<Spec> rooms;
    for (int i=0;i<12;++i) {
        const auto prefix="room-"+std::to_string(i)+"-"+std::string(106,'w')+"-";
        auto walls=rectangle(prefix,{10.0*i,0},{10.0*i+4,3});
        rooms.insert(rooms.end(),walls.begin(),walls.end());
    }
    auto oversized_fixture=entities(rooms);
    std::vector<std::string> owners;
    for (const auto& wall:rooms) {
        require(wall.id.size()<=128,"lineage fixture wall ID exceeds document admission limit");
        owners.push_back(wall.id);
    }
    // Each valid relevant registry contributes 48 compact active-state tokens.
    // The common payload is below the source construction limits, while its
    // repeated emission for 12 independent rooms exceeds the aggregate budget.
    for (int i=0;i<220;++i) {
        const auto registry_id="phase-"+std::string(110,'p')+"-"+std::to_string(i);
        const auto alternative_id="alternative-"+std::string(100,'a')+"-"+std::to_string(i);
        require(registry_id.size()<=128 && alternative_id.size()<=128,
            "lineage fixture phase identifiers exceed document admission limit");
        const auto phases=ModelPhases::create(owners,owners,
            {{alternative_id,"Relevant alternative",{},{}}},alternative_id);
        oversized_fixture.push_back(entity(registry_id,"model_phases",{{"model",phases.to_json()}}));
    }
    const auto oversized=Document::create(oversized_fixture);
    require(oversized.snapshot().entities().size()==oversized_fixture.size() &&
        oversized.snapshot().entities().contains(rooms.front().id),
        "lineage fixture must be admitted as a complete document before detector refusal");
    try { (void)detect_physical_wall_spaces(oversized.snapshot(),rooms.front().id); }
    catch (const std::invalid_argument& error) {
        const auto message=std::string(error.what());
        require(message.find("lineage")!=std::string::npos && message.find("aggregate byte budget")!=std::string::npos,
            "admitted provenance must fail specifically at the detector aggregate lineage byte budget"); return;
    }
    throw std::runtime_error("repeated room lineage escaped the aggregate encoded byte budget");
}
}
int main() {
    sketch::runtime::configure_noninteractive_errors();
    try {
        const std::pair<const char*,void(*)()> cases[]{
            {"rectangles_and_overlapping_material",rectangles_and_overlapping_material},
            {"analytical_unsplit_partitions",analytical_unsplit_partitions}, {"curved_room",curved_room},
            {"nested_physical_holes",nested_physical_holes},
            {"visibility_context_phase_and_plane",visibility_context_phase_and_plane},
            {"resolved_plane_and_purity",resolved_plane_and_purity},
            {"reversal_and_input_order",reversal_and_input_order},
            {"open_wall_and_hosted_opening",open_wall_and_hosted_opening},
            {"intrusive_stubs_islands_and_components",intrusive_stubs_islands_and_components},
            {"translated_rotated_and_stepped_material",translated_rotated_and_stepped_material},
            {"invalid_and_collapsed",invalid_and_collapsed},
            {"scoped_and_bounded_lineage",scoped_and_bounded_lineage}};
        for (const auto& [name,run]:cases) {
            try { run(); } catch (const std::exception& e) {
                throw std::runtime_error(std::string(name)+": "+e.what());
            }
        }
        std::cout << "physical wall spaces tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
