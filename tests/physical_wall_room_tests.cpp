#include "sketch/physical_wall_room.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/noninteractive_errors.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/project_store.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/ifc_project_exchange.hpp"
#include <regex>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
void assert_near(double actual,double expected) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>1e-9) {
        std::ostringstream detail;
        detail<<std::setprecision(17)<<"expected "<<expected<<", got "<<actual;
        throw std::runtime_error(detail.str());
    }
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid physical room proposal accepted");
}
template<class F> void rejects_document(F action) {
    try { action(); } catch (const DocumentError&) { return; }
    throw std::runtime_error("protected physical room descriptor edit was accepted");
}
Entity entity(std::string id,std::string type,Json properties=Json::object()) {
    return {std::move(id),std::move(type),std::move(properties),false,Json::object()};
}
Json segment(Vec2 start,Vec2 end) { return {{"start",{start.x,start.y}},{"end",{end.x,end.y}},{"sweep_radians",0.0}}; }
Entity wall(std::string id,Vec2 start,Vec2 end) {
    return entity(std::move(id),"wall",{{"baseline",segment(start,end)},{"thickness_m",0.2},
        {"height_m",3.0},{"elevation_m",0.0},{"layer_id","layer"}});
}
void rectangle(std::vector<Entity>& values,const std::string& prefix,Vec2 lo,Vec2 hi) {
    values.push_back(wall(prefix+"bottom",lo,{hi.x,lo.y}));
    values.push_back(wall(prefix+"right",{hi.x,lo.y},hi));
    values.push_back(wall(prefix+"top",hi,{lo.x,hi.y}));
    values.push_back(wall(prefix+"left",{lo.x,hi.y},lo));
}
std::vector<Entity> fixture() {
    std::vector<Entity> result{entity("property","property"),entity("building","building",{{"property_id","property"}}),
        entity("floor","floor",{{"building_id","building"}}),entity("layer","layer",{{"floor_id","floor"}})};
    rectangle(result,"",{0,0},{4,3}); return result;
}
std::vector<Entity> copied(const DocumentSnapshot& snapshot) {
    std::vector<Entity> result; for (const auto& [id,e]:snapshot.entities()) { (void)id; result.push_back(e); } return result;
}
Entity& find(std::vector<Entity>& values,const std::string& id) {
    for (auto& e:values) if (e.id==id) return e;
    throw std::runtime_error("missing test entity");
}
std::string create_room(Document& document,std::size_t index=0) {
    const auto command=prepare_physical_wall_rooms(document.snapshot(),"bottom",{index},"office");
    require(command.entity_changes.size()==1,"expected one new room");
    const auto id=command.entity_changes[0].entity.id;
    document.apply(command); return id;
}
void canonical_creation_and_idempotence() {
    auto d=Document::create(fixture()); const auto before=d.snapshot();
    const auto command=prepare_physical_wall_rooms(before,"bottom",{0},"  office  ");
    require(before.entities()==d.snapshot().entities(),"preparation mutated document");
    const auto& room=command.entity_changes.at(0).entity;
    require(room.type=="room_boundary" && is_physical_wall_room(room),"wrong room source owner");
    require(room.properties.at("classification")=="office" && room.properties.at("measurement_classification")=="office",
        "explicit room classification missing");
    require(!room.properties.contains("area_m2") && !room.properties.contains("appraisal_facts"),"creation fabricated area/GLA facts");
    require(room.properties.at("factor")==1.0 && room.properties.at("factor_numerator")==1,"room factor must be one");
    require(decode_identified_boundary_entity(room).segments.size()==4,"identified room outer missing");
    d.apply(command);
    const auto check=physical_wall_room_checks(d.snapshot()).at(room.id);
    require(check.current && check.diagnostic.empty(),"fresh room not current"); assert_near(check.area_square_metres,10.64);
    const auto repeat=prepare_physical_wall_rooms(d.snapshot(),"right",{0},"bedroom");
    require(repeat.entity_changes.empty(),"repeat created a duplicate or overwrote existing classification");
    require(d.snapshot().entities().at(room.id).properties.at("classification")=="office","repeat changed classification");
    rejects([&]{(void)prepare_physical_wall_rooms(d.snapshot(),"bottom",{0,0},"office");});
    rejects([&]{(void)prepare_physical_wall_rooms(d.snapshot(),"bottom",{1},"office");});
    rejects([&]{(void)prepare_physical_wall_rooms(d.snapshot(),"bottom",{0},"   ");});
}
void holes_obstacles_and_source_changes() {
    auto nested=fixture();
    for (auto& e:nested) if (e.type=="wall") {
        for (auto* key:{"start","end"}) {
            e.properties["baseline"][key][0]=e.properties["baseline"][key][0].get<double>()*2.5;
            e.properties["baseline"][key][1]=e.properties["baseline"][key][1].get<double>()*(10.0/3.0);
        }
    }
    rectangle(nested,"inner-",{3,3},{7,7});
    auto d=Document::create(nested);
    const auto detection=detect_physical_wall_spaces(d.snapshot(),"bottom");
    std::size_t parent=0;
    while (parent<detection.spaces.size() && detection.spaces[parent].holes.empty()) ++parent;
    const auto id=create_room(d,parent);
    const auto check=physical_wall_room_checks(d.snapshot()).at(id);
    require(check.current && check.holes.size()==1,"nested room lost inline island hole"); assert_near(check.area_square_metres,78.4);
    require(!d.snapshot().entities().at(id).properties.contains("deduction_ids"),"inline hole created independent deduction tools");
    for (const bool isolated:{false,true}) {
        auto input=fixture(); input.push_back(isolated ? wall("obstacle",{1,1},{3,1}) : wall("obstacle",{2,0},{2,1}));
        auto obstacle=Document::create(input); const auto room_id=create_room(obstacle);
        const auto room=physical_wall_room_checks(obstacle.snapshot()).at(room_id);
        require(room.current,"physical obstacle room not current"); assert_near(room.area_square_metres,isolated ? 10.24 : 10.46);
    }
    auto box=Document::create(fixture()); const auto box_id=create_room(box); const auto baseline=box.snapshot();
    auto added=copied(baseline); added.push_back(wall("new-wall",{1,1},{3,1}));
    auto changed=Document::create(added);
    require(!physical_wall_room_checks(changed.snapshot()).at(box_id).current,"wall addition left room current");
    require(prepare_physical_wall_rooms(changed.snapshot(),"bottom",{0},"office").entity_changes.size()==1,
        "stale owner was silently remapped to new clear geometry");
    auto thicker=copied(baseline); find(thicker,"bottom").properties["thickness_m"]=0.4;
    auto thick=Document::create(thicker); require(!physical_wall_room_checks(thick.snapshot()).at(box_id).current,"thickness edit left room current");
    auto shifted=copied(baseline); find(shifted,"bottom").properties["baseline"]["start"][1]=-0.1;
    find(shifted,"bottom").properties["baseline"]["end"][1]=-0.1;
    auto moved_source=Document::create(shifted);
    require(!physical_wall_room_checks(moved_source.snapshot()).at(box_id).current,"wall movement left room current");
    auto removed=copied(baseline);
    removed.erase(std::remove_if(removed.begin(),removed.end(),[](const auto& e){return e.id=="top";}),removed.end());
    auto open=Document::create(removed); require(!physical_wall_room_checks(open.snapshot()).at(box_id).current,"wall removal left room current");
}
void phase_visibility_and_tampering() {
    auto input=fixture(); input.push_back(wall("divider",{2,0},{2,3}));
    const auto phases=ModelPhases::create({"bottom","right","top","left","divider"},
        {"bottom","right","top","left","divider"},{{"remove","Remove divider",{"divider"},{}}});
    input.push_back(entity("phases","model_phases",{{"model",phases.to_json()}}));
    auto d=Document::create(input); const auto id=create_room(d); const auto baseline=d.snapshot();
    auto hidden=copied(baseline); find(hidden,"layer").properties["visible"]=false;
    auto invisible=Document::create(hidden); require(physical_wall_room_checks(invisible.snapshot()).at(id).current,"view visibility staled physical room");
    auto phased=copied(baseline); find(phased,"phases").properties["model"]=phases.with_active("remove").to_json();
    auto changed=Document::create(phased); require(!physical_wall_room_checks(changed.snapshot()).at(id).current,"semantic phase change left room current");
    auto lineage=copied(baseline); find(lineage,id).extensions["physical_wall_room"]["source_lineage"]["component_index"]=999;
    auto forged=Document::create(lineage); require(!physical_wall_room_checks(forged.snapshot()).at(id).current,"lineage tampering left room current");
    auto outer=copied(baseline); auto& owner=find(outer,id); auto decoded=decode_identified_boundary_entity(owner);
    for (auto& e:decoded.segments) { e.segment.start.x+=0.01; e.segment.end.x+=0.01; }
    owner=encode_identified_boundary_entity(decoded,&owner);
    auto moved=Document::create(outer); require(!physical_wall_room_checks(moved.snapshot()).at(id).current,"outer tampering left room current");
    auto claimed=copied(baseline); find(claimed,id).properties["area_m2"]=9999;
    auto false_area=Document::create(claimed); assert_near(physical_wall_room_checks(false_area.snapshot()).at(id).area_square_metres,5.04);
    auto future=copied(baseline); find(future,id).extensions["physical_wall_room"]["version"]=2;
    auto unsupported=Document::create(future); const auto check=physical_wall_room_checks(unsupported.snapshot()).at(id);
    require(!check.current && !check.diagnostic.empty() && check.boundary.empty(),"unknown descriptor was silently accepted");
    require(!unsupported.snapshot().is_editable(),"future source descriptor must make document read-only");
    auto inactive=copied(baseline);
    const auto room_phases=ModelPhases::create({id},{id},{{"remove-room","Demolish room",{id},{}}},"remove-room");
    inactive.push_back(entity("room-phases","model_phases",{{"model",room_phases.to_json()}}));
    auto inactive_room=Document::create(inactive);
    require(!physical_wall_room_checks(inactive_room.snapshot()).at(id).current,
        "room owner demolished in an independent phase registry remained current");
    const auto future_path=std::filesystem::temp_directory_path()/("vertex-future-room-"+make_stable_id()+".bldproj");
    (void)ProjectStore::save(future_path,unsupported.snapshot());
    auto future_reopened=ProjectStore::load(future_path); std::filesystem::remove(future_path);
    require(!future_reopened.document.snapshot().is_editable() &&
        !physical_wall_room_checks(future_reopened.document.snapshot()).at(id).current,
        "reopened future descriptor history lost read-only/noncurrent handling");
}
void descriptor_validation_and_hole_tampering() {
    auto input=fixture(); input.push_back(wall("obstacle",{1,1},{3,1}));
    auto d=Document::create(input); const auto id=create_room(d);
    auto changed=copied(d.snapshot()); auto& marker=find(changed,id).extensions["physical_wall_room"];
    for (auto& edge:marker["holes"][0]) { edge["start"][1]=edge["start"][1].get<double>()+0.01; edge["end"][1]=edge["end"][1].get<double>()+0.01; }
    auto modified=Document::create(changed); require(!physical_wall_room_checks(modified.snapshot()).at(id).current,"inline hole tampering left room current");
    auto malformed=d.snapshot().entities().at(id); malformed.extensions["physical_wall_room"]["holes"]="bad";
    rejects([&]{(void)validate_physical_wall_room_descriptor(malformed);});
    malformed=d.snapshot().entities().at(id); malformed.extensions["physical_wall_room"]["extra"]=true;
    rejects([&]{(void)validate_physical_wall_room_descriptor(malformed);});
    malformed=d.snapshot().entities().at(id); malformed.extensions["physical_wall_room"]["source_lineage"]["version"]=2;
    rejects([&]{(void)validate_physical_wall_room_descriptor(malformed);});
    auto stripped=d.snapshot().entities().at(id); stripped.extensions.erase("physical_wall_room");
    rejects_document([&]{d.apply(ApplyEntityChanges{d.revision(),{EntityChange::upsert(stripped)}, {},"Strip protected descriptor"});});
    auto replaced=d.snapshot().entities().at(id); replaced.extensions["physical_wall_room"]["source_lineage"]["component_index"]=999;
    rejects_document([&]{d.apply(ApplyEntityChanges{d.revision(),{EntityChange::upsert(replaced)}, {},"Replace protected descriptor"});});
    auto wrong_owner=d.snapshot().entities().at(id); wrong_owner.type="measurement_boundary";
    rejects_document([&]{(void)Document::create({wrong_owner});});
    rejects_document([&]{d.apply(TranslateBoundary{d.revision(),{id,{1,0}}});});
    auto deleted=Document::fork(d.snapshot());
    deleted.apply(ApplyEntityChanges{deleted.revision(),{EntityChange::erase(id)}, {},"Delete physical room"});
    require(ProjectStore::required_format_version(deleted.snapshot())==43,"deleted physical room lost retained reader floor");
    const auto path=std::filesystem::temp_directory_path()/("vertex-physical-room-"+make_stable_id()+".bldproj");
    require(ProjectStore::required_format_version(d.snapshot())==43,"physical room descriptor did not require format43");
    (void)ProjectStore::save(path,d.snapshot());
    auto reopened=ProjectStore::load(path); std::filesystem::remove(path);
    const auto loaded=physical_wall_room_checks(reopened.document.snapshot()).at(id);
    require(loaded.current && loaded.holes.size()==1,"save/reopen lost source-aware room hole");
    assert_near(loaded.area_square_metres,10.24);
}
void clear_room_exports() {
    auto input=fixture(); input.push_back(wall("obstacle",{1,1},{3,1}));
    auto d=Document::create(input); const auto id=create_room(d);
    const auto dxf=export_project_dxf(d.snapshot());
    require(dxf.drawing.polylines.size()==2,"DXF must retain clear outer and analytic island loop");
    require(std::any_of(dxf.drawing.polylines.begin(),dxf.drawing.polylines.end(),[](const auto& loop) {
        return loop.closed && std::any_of(loop.vertices.begin(),loop.vertices.end(),[](const auto& vertex) {
            return vertex.point.x==1 && std::abs(vertex.point.y-.9)<1e-12;
        });
    }),"DXF dropped the physical wall island outline");
    const auto ifc=export_project_ifc(d.snapshot());
    const auto footprint=ifc.step.find("'Footprint','Curve3D',(");
    require(footprint!=std::string::npos,"IFC lacks a current clear-room footprint");
    const auto close=ifc.step.find("));",footprint);
    require(ifc.step.substr(footprint,close-footprint).find(",#")!=std::string::npos,
        "IFC clear footprint must include its inner island loop");
    require(ifc.step.find("Pset_VertexExchange_v2")!=std::string::npos,"default IFC limits must retain large room source metadata in chunks");
    const auto imported=import_project_ifc(ifc.step);
    require(std::any_of(imported.entities.begin(),imported.entities.end(),[&](const auto& entity) {
        if (!entity.extensions.contains("ifc_vertex_properties")) return false;
        const auto& payload=entity.extensions.at("ifc_vertex_properties");
        return payload.contains("native_entity") && payload.at("native_entity").value("id",std::string{})==id &&
            payload.at("native_entity").at("extensions").at("physical_wall_room")==
                d.snapshot().entities().at(id).extensions.at("physical_wall_room");
    }),"IFC default-limit round trip lost the native room source descriptor");
    const auto second=import_project_ifc(export_project_ifc(Document::create(imported.entities).snapshot()).step);
    require(std::any_of(second.entities.begin(),second.entities.end(),[&](const auto& entity) {
        if (!entity.extensions.contains("ifc_vertex_properties")) return false;
        const auto& payload=entity.extensions.at("ifc_vertex_properties");
        return payload.contains("native_entity") && payload.at("native_entity").value("id",std::string{})==id &&
            payload.at("native_entity").at("extensions").at("physical_wall_room")==
                d.snapshot().entities().at(id).extensions.at("physical_wall_room");
    }),"Second IFC export/import lost the retained native source descriptor");
    const std::regex pset_pattern(R"(#(\d+)=IFCPROPERTYSET\([^\n]*Pset_VertexExchange_v2)");
    std::smatch pset_match; require(std::regex_search(ifc.step,pset_match,pset_pattern),"chunked source property set is identifiable");
    const std::regex relation_pattern("IFCRELDEFINESBYPROPERTIES\\([^\\n]*,\\((#\\d+)\\),#"+pset_match[1].str()+"\\)");
    std::smatch relation_match; require(std::regex_search(ifc.step,relation_match,relation_pattern),"chunked source relation has one owner");
    auto amplified=ifc.step;
    const auto single=relation_match[1].str();
    const auto target=static_cast<std::size_t>(relation_match.position(1));
    amplified.insert(target+single.size(),","+single);
    rejects([&]{(void)import_project_ifc(amplified);});
    auto forged_ifc=ifc.step;
    const auto digest=forged_ifc.find("\"sha256\":\"");
    require(digest!=std::string::npos,"chunked IFC metadata has an integrity digest");
    forged_ifc[digest+10]=forged_ifc[digest+10]=='a'?'b':'a';
    rejects([&]{(void)import_project_ifc(forged_ifc);});
    auto changed=d.snapshot().entities().at("bottom"); changed.properties["thickness_m"]=.4;
    d.apply(ApplyEntityChanges{d.revision(),{EntityChange::upsert(changed)}, {},"Thicken source wall"});
    const auto stale_dxf=export_project_dxf(d.snapshot());
    require(stale_dxf.drawing.polylines.empty() && std::any_of(stale_dxf.diagnostics.begin(),stale_dxf.diagnostics.end(),[&](const auto& issue) {
        return issue.source_id==id && issue.code=="physical_room_source_stale";
    }),"DXF exported stale room geometry");
    const auto stale_ifc=export_project_ifc(d.snapshot());
    require(std::any_of(stale_ifc.diagnostics.begin(),stale_ifc.diagnostics.end(),[&](const auto& issue) {
        return issue.source_id==id && issue.code=="physical_room_source_stale_or_runtime_unavailable";
    }),"IFC failed to withhold stale clear-room footprint");
}
}
int main() {
    sketch::runtime::configure_noninteractive_errors();
    try {
        const std::pair<const char*,void(*)()> cases[]{
            {"canonical_creation_and_idempotence",canonical_creation_and_idempotence},
            {"holes_obstacles_and_source_changes",holes_obstacles_and_source_changes},
            {"phase_visibility_and_tampering",phase_visibility_and_tampering},
            {"descriptor_validation_and_hole_tampering",descriptor_validation_and_hole_tampering},
            {"clear_room_exports",clear_room_exports}};
        for (const auto& [name,run]:cases) {
            try { run(); } catch (const std::exception& e) { throw std::runtime_error(std::string(name)+": "+e.what()); }
        }
        std::cout<<"physical wall room tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
