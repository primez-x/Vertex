#include "support/detached_document_snapshot.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/noninteractive_errors.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/project_store.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/ifc_project_exchange.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/vertical_levels.hpp"
#include <regex>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string_view>
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
void correspondence_noop_moves_and_thickness() {
    auto d=Document::create(fixture()); const auto id=create_room(d);
    const auto before=d.snapshot();
    const auto unchanged=physical_wall_room_correspondence(before,"bottom");
    require(unchanged.retained.size()==1 && unchanged.fresh.size()==1 && unchanged.overlaps.size()==1,
        "noop correspondence must keep one owner and one fresh space");
    require(unchanged.retained[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation &&
        unchanged.fresh[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation,
        "noop room must be an unambiguous continuation");
    require(unchanged.retained[0].room==before.entities().at(id) &&
        unchanged.retained[0].descriptor_digest==physical_wall_room_descriptor_digest(before.entities().at(id)),
        "report must retain the complete old room facts and descriptor identity");
    assert_near(unchanged.overlaps[0].area_square_metres.value(),10.64);
    require(!unchanged.overlaps[0].surviving_sources.empty() && unchanged.overlaps[0].reliable,
        "continuation needs surviving boundary lineage and reliable analytical overlap");
    require(physical_wall_room_correspondence_is_current(unchanged,before),"new report lost snapshot binding");
    const auto repeated=physical_wall_room_correspondence(before,"right");
    require(repeated.fresh[0].source_lineage==unchanged.fresh[0].source_lineage &&
        repeated.overlaps[0].area_square_metres==unchanged.overlaps[0].area_square_metres &&
        repeated.retained[0].room.id==id,"wall selection changed deterministic correspondence");
    require(document_snapshot_digest(before)==document_snapshot_digest(d.snapshot()),"query mutated source/history");
    auto branch_a=Document::fork(before),branch_b=Document::fork(before);
    auto first_wall=before.entities().at("bottom"),second_wall=first_wall;
    first_wall.properties["thickness_m"]=.3; second_wall.properties["thickness_m"]=.4;
    branch_a.apply(ApplyEntityChanges{branch_a.revision(),{EntityChange::upsert(first_wall)}, {},"First branch wall edit"});
    branch_b.apply(ApplyEntityChanges{branch_b.revision(),{EntityChange::upsert(second_wall)}, {},"Second branch wall edit"});
    const auto branch_report=physical_wall_room_correspondence(branch_a.snapshot(),"bottom");
    require(branch_a.snapshot().document_id()==branch_b.snapshot().document_id() && branch_a.revision()==branch_b.revision() &&
        !physical_wall_room_correspondence_is_current(branch_report,branch_b.snapshot()),
        "equal document ID/revision must not substitute for captured snapshot binding");
    auto bottom=d.snapshot().entities().at("bottom"); bottom.properties["thickness_m"]=.4;
    d.apply(ApplyEntityChanges{d.revision(),{EntityChange::upsert(bottom)}, {},"Thicken source wall"});
    const auto thick=physical_wall_room_correspondence(d.snapshot(),"bottom");
    require(thick.retained[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation,
        "wall thickening lost one-to-one overlap/lineage continuation");
    assert_near(thick.overlaps[0].area_square_metres.value(),10.26);
    require(!physical_wall_room_correspondence_is_current(unchanged,d.snapshot()),"wall edit did not stale captured report");
    auto moved=d.snapshot().entities().at("right");
    moved.properties["baseline"]["start"][0]=4.2; moved.properties["baseline"]["end"][0]=4.2;
    auto top=d.snapshot().entities().at("top"); top.properties["baseline"]["start"][0]=4.2;
    bottom=d.snapshot().entities().at("bottom"); bottom.properties["baseline"]["end"][0]=4.2;
    d.apply(ApplyEntityChanges{d.revision(),{EntityChange::upsert(moved),EntityChange::upsert(top),EntityChange::upsert(bottom)}, {},"Move right enclosure"});
    const auto shifted=physical_wall_room_correspondence(d.snapshot(),"bottom");
    require(shifted.retained[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation,
        "wall movement lost analytical continuation");
    assert_near(shifted.overlaps[0].area_square_metres.value(),10.26);
    require(d.snapshot().entities().at(id)==before.entities().at(id),"report replaced retained room facts or geometry");
}
void correspondence_partitions_split_merge_and_ambiguity() {
    auto d=Document::create(fixture()); const auto id=create_room(d);
    d.apply(ApplyEntityChanges{d.revision(),{EntityChange::upsert(wall("divider",{2,0},{2,3}))}, {},"Insert partition"});
    const auto split=physical_wall_room_correspondence(d.snapshot(),"bottom");
    require(split.retained[0].room.id==id && split.retained[0].kind==PhysicalWallRoomCorrespondenceKind::split &&
        split.retained[0].candidate_indices==std::vector<std::size_t>{0,1},"partition insertion must report a split with both candidates");
    require(split.fresh.size()==2 && split.fresh[0].kind==PhysicalWallRoomCorrespondenceKind::split &&
        split.fresh[1].kind==PhysicalWallRoomCorrespondenceKind::split,"split candidate dispositions disagree");
    for (const auto& link:split.overlaps) assert_near(link.area_square_metres.value(),5.04);
    auto input=fixture(); input.push_back(wall("divider",{2,0},{2,3}));
    auto two=Document::create(input);
    two.apply(prepare_physical_wall_rooms(two.snapshot(),"bottom",{0,1},"office"));
    two.apply(ApplyEntityChanges{two.revision(),{EntityChange::erase("divider")}, {},"Remove partition"});
    const auto merge=physical_wall_room_correspondence(two.snapshot(),"bottom");
    require(merge.retained.size()==2 && merge.fresh.size()==1 && merge.fresh[0].kind==PhysicalWallRoomCorrespondenceKind::merge,
        "partition removal must report both old owners merging into one candidate");
    for (const auto& old:merge.retained) require(old.kind==PhysicalWallRoomCorrespondenceKind::merge,"old merge owner was silently chosen");
    for (const auto& link:merge.overlaps) assert_near(link.area_square_metres.value(),5.04);
    auto cross=Document::create(input);
    cross.apply(prepare_physical_wall_rooms(cross.snapshot(),"bottom",{0,1},"office"));
    cross.apply(ApplyEntityChanges{cross.revision(),{EntityChange::erase("divider"),
        EntityChange::upsert(wall("horizontal",{0,1.5},{4,1.5}))}, {},"Replace vertical partition with horizontal"});
    const auto uncertain=physical_wall_room_correspondence(cross.snapshot(),"bottom");
    require(uncertain.retained.size()==2 && uncertain.fresh.size()==2 && uncertain.overlaps.size()==4,
        "many-to-many analytical overlap candidates were dropped");
    for (const auto& old:uncertain.retained) require(old.kind==PhysicalWallRoomCorrespondenceKind::ambiguous,"many-to-many owner must remain ambiguous");
    for (const auto& fresh:uncertain.fresh) require(fresh.kind==PhysicalWallRoomCorrespondenceKind::ambiguous,"many-to-many candidate must remain ambiguous");
}
void correspondence_holes_curves_new_retired_and_failures() {
    auto obstacle=Document::create(fixture()); (void)create_room(obstacle);
    obstacle.apply(ApplyEntityChanges{obstacle.revision(),{EntityChange::upsert(wall("island",{1,1},{3,1}))}, {},"Add physical obstacle"});
    const auto with_hole=physical_wall_room_correspondence(obstacle.snapshot(),"bottom");
    require(with_hole.fresh[0].holes.size()==1 && with_hole.retained[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation,
        "isolated obstacle must retain one-to-one correspondence and its hole");
    assert_near(with_hole.overlaps[0].area_square_metres.value(),10.24);
    // Nested rooms have overlapping bounding boxes but disjoint clear interiors.
    auto nested=fixture();
    for (auto& e:nested) if (e.type=="wall") for (const auto* key:{"start","end"}) {
        e.properties["baseline"][key][0]=e.properties["baseline"][key][0].get<double>()*2.5;
        e.properties["baseline"][key][1]=e.properties["baseline"][key][1].get<double>()*(10.0/3.0);
    }
    rectangle(nested,"inner-",{3,3},{7,7}); auto islands=Document::create(nested);
    islands.apply(prepare_physical_wall_rooms(islands.snapshot(),"bottom",{0,1},"office"));
    const auto nested_report=physical_wall_room_correspondence(islands.snapshot(),"bottom");
    require(nested_report.overlaps.size()==2,"nested clear regions were matched using gross outline or bounding box overlap");
    for (const auto& old:nested_report.retained) require(old.kind==PhysicalWallRoomCorrespondenceKind::unique_continuation,"hole exclusion lost nested room identity");
    auto capsule=fixture();
    find(capsule,"right").properties["baseline"]["sweep_radians"]=std::numbers::pi;
    find(capsule,"left").properties["baseline"]["sweep_radians"]=std::numbers::pi;
    auto curved=Document::create(capsule); (void)create_room(curved);
    const auto curved_source=curved.snapshot();
    std::vector<EntityChange> curved_changes;
    for (const auto& [key,e]:curved_source.entities()) if (e.type=="wall") {
        (void)key; auto changed=e; changed.properties["thickness_m"]=.4;
        curved_changes.push_back(EntityChange::upsert(std::move(changed)));
    }
    require(curved_changes.size()==4,"curved fixture must thicken all four physical walls");
    curved.apply(ApplyEntityChanges{curved.revision(),std::move(curved_changes), {},"Thicken curved enclosure"});
    const auto arcs=physical_wall_room_correspondence(curved.snapshot(),"bottom");
    require(arcs.retained[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation &&
        std::count_if(arcs.fresh[0].boundary.begin(),arcs.fresh[0].boundary.end(),[](const auto& s){return s.sweep_radians!=0;})==2,
        "curved correspondence must preserve analytical circular boundaries");
    assert_near(arcs.overlaps[0].area_square_metres.value(),arcs.fresh[0].area_square_metres);
    auto independent=fixture(); rectangle(independent,"other-",{10,0},{14,3});
    auto spaces=Document::create(independent); (void)create_room(spaces,0);
    const auto added=physical_wall_room_correspondence(spaces.snapshot(),"bottom");
    require(added.fresh.size()==2 && added.fresh[1].kind==PhysicalWallRoomCorrespondenceKind::new_space,
        "disjoint unowned enclosure must be new");
    spaces.apply(ApplyEntityChanges{spaces.revision(),{EntityChange::erase("top")}, {},"Open original enclosure"});
    const auto retired=physical_wall_room_correspondence(spaces.snapshot(),"bottom");
    require(retired.retained[0].kind==PhysicalWallRoomCorrespondenceKind::retired && retired.fresh[0].kind==PhysicalWallRoomCorrespondenceKind::new_space,
        "lost enclosure must retire without nearest-room assignment");
    auto forged=copied(obstacle.snapshot());
    for (auto& e:forged) if (is_physical_wall_room(e)) e.extensions["physical_wall_room"]["source_lineage"]["outer"]="bad";
    const auto invalid_source=Document::create(forged);
    const auto malformed=physical_wall_room_correspondence(invalid_source.snapshot(),"bottom");
    require(malformed.retained[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous &&
        !malformed.retained[0].diagnostic.empty() && malformed.fresh[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous,
        "malformed retained lineage must not become a new/retired claim");
    auto duplicate_input=copied(obstacle.snapshot());
    auto duplicate=*std::find_if(duplicate_input.begin(),duplicate_input.end(),[](const auto& e){return is_physical_wall_room(e);});
    duplicate.id="duplicate-room";
    const auto original_geometry=boundary_geometry(decode_identified_boundary_entity(duplicate));
    IdentifiedBoundary duplicate_geometry{duplicate.id,duplicate.type,{}};
    for (std::size_t i=0;i<original_geometry.size();++i) duplicate_geometry.segments.push_back({
        "duplicate-edge-"+std::to_string(i),"duplicate-vertex-"+std::to_string(i),
        "duplicate-vertex-"+std::to_string((i+1)%original_geometry.size()),original_geometry[i]});
    duplicate=encode_identified_boundary_entity(duplicate_geometry,&duplicate);
    duplicate_input.push_back(std::move(duplicate));
    const auto duplicates=Document::create(duplicate_input);
    const auto conflict=physical_wall_room_correspondence(duplicates.snapshot(),"bottom");
    require(conflict.retained.size()==2 && conflict.fresh.size()==1 &&
        conflict.fresh[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous,
        "overlapping retained owners must be ambiguous rather than a false merge");
    auto translated=Document::create(fixture()); const auto translated_room_id=create_room(translated);
    const auto translation_source=translated.snapshot();
    std::vector<EntityChange> translated_walls;
    for (const auto& [key,e]:translation_source.entities()) if (e.type=="wall") {
        (void)key; auto changed=e;
        for (const auto* endpoint:{"start","end"}) changed.properties["baseline"][endpoint][0]=
            changed.properties["baseline"][endpoint][0].get<double>()+20.0;
        translated_walls.push_back(EntityChange::upsert(std::move(changed)));
    }
    require(translated_walls.size()==4,"translation fixture must update all four physical walls");
    translated.apply(ApplyEntityChanges{translated.revision(),std::move(translated_walls), {},"Translate physical enclosure"});
    const auto translation_result=translated.snapshot();
    for (const auto& [key,e]:translation_source.entities()) if (e.type=="wall") {
        const auto& changed=translation_result.entities().at(key);
        for (const auto* endpoint:{"start","end"}) {
            assert_near(changed.properties.at("baseline").at(endpoint).at(0).get<double>(),
                e.properties.at("baseline").at(endpoint).at(0).get<double>()+20.0);
            assert_near(changed.properties.at("baseline").at(endpoint).at(1).get<double>(),
                e.properties.at("baseline").at(endpoint).at(1).get<double>());
        }
    }
    require(translation_result.entities().at(translated_room_id)==translation_source.entities().at(translated_room_id),
        "physical source translation must preserve retained room facts and geometry");
    const auto disjoint_identity=physical_wall_room_correspondence(translation_result,"bottom");
    require(disjoint_identity.retained.size()==1 && disjoint_identity.fresh.size()==1 &&
        disjoint_identity.retained[0].room==translation_source.entities().at(translated_room_id),
        "translated report must retain the original owner and one newly detected enclosure");
    assert_near(disjoint_identity.fresh[0].boundary[0].start.x-disjoint_identity.retained[0].boundary[0].start.x,20.0);
    if (!(disjoint_identity.overlaps.size()==1 && disjoint_identity.overlaps[0].area_square_metres==0.0 &&
        disjoint_identity.retained[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous)) {
        std::ostringstream detail; detail<<std::setprecision(17)<<"surviving identity without regional overlap must remain ambiguous: links="
            <<disjoint_identity.overlaps.size()<<", retained="<<disjoint_identity.retained.size();
        if (!disjoint_identity.retained.empty()) detail<<", kind="<<static_cast<int>(disjoint_identity.retained[0].kind)
            <<", owner diagnostic="<<disjoint_identity.retained[0].diagnostic;
        if (!disjoint_identity.overlaps.empty()) {
            const auto& link=disjoint_identity.overlaps[0]; detail<<", area=";
            if (link.area_square_metres) detail<<*link.area_square_metres; else detail<<"unresolved";
            detail<<", survivors="<<link.surviving_sources.size()<<", comparison diagnostic="<<link.diagnostic;
        }
        throw std::runtime_error(detail.str());
    }
    auto replaced=Document::create(fixture()); (void)create_room(replaced);
    const auto replacement_source=replaced.snapshot();
    std::vector<EntityChange> replaced_walls;
    for (const auto& [key,e]:replacement_source.entities()) if (e.type=="wall") {
        auto changed=e; changed.id="replacement-"+key;
        replaced_walls.push_back(EntityChange::erase(key)); replaced_walls.push_back(EntityChange::upsert(std::move(changed)));
    }
    require(replaced_walls.size()==8,"replacement fixture must erase and replace all four source identities");
    replaced.apply(ApplyEntityChanges{replaced.revision(),std::move(replaced_walls), {},"Replace physical source identities"});
    const auto missing_identity=physical_wall_room_correspondence(replaced.snapshot(),"replacement-bottom");
    require(missing_identity.overlaps.size()==1 && missing_identity.overlaps[0].surviving_sources.empty() &&
        missing_identity.retained[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous,
        "positive overlap without surviving physical identity must remain ambiguous");
    auto future_input=copied(obstacle.snapshot());
    for (auto& e:future_input) if (is_physical_wall_room(e)) e.extensions["physical_wall_room"]["version"]=2;
    const auto future=Document::create(future_input);
    require(!future.snapshot().is_editable() &&
        physical_wall_room_correspondence(future.snapshot(),"bottom").retained[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous,
        "future read-only owner descriptor must remain reviewable and ambiguous");
    rejects([&]{(void)physical_wall_room_correspondence(obstacle.snapshot(),"missing-wall");});
}
void correspondence_captured_evidence_and_budgets() {
    auto input=fixture();
    const auto phases=ModelPhases::create({"bottom","right","top","left"},{"bottom","right","top","left"},{});
    input.push_back(entity("phases","model_phases",{{"model",phases.to_json()}}));
    auto document=Document::create(input); const auto id=create_room(document); const auto source=document.snapshot();
    require(physical_wall_room_correspondence(source,"bottom").retained[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation,
        "valid captured phase evidence must support continuation");
    const auto ambiguous=[&](auto tamper) {
        auto values=copied(source); auto& lineage=find(values,id).extensions["physical_wall_room"]["source_lineage"];
        tamper(lineage); const auto forged=Document::create(values);
        const auto report=physical_wall_room_correspondence(forged.snapshot(),"bottom");
        require(report.retained.size()==1 && report.retained[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous &&
            !report.retained[0].diagnostic.empty() && report.fresh[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous &&
            std::none_of(report.overlaps.begin(),report.overlaps.end(),[](const auto& link){return link.reliable;}),
            "malformed captured source or phase evidence was accepted as reliable continuation");
    };
    for (const auto* key:{"baseline","thickness_m","source_elevation_m","effective_elevation_m","source_context","vertical_placement"}) {
        ambiguous([&](Json& lineage){lineage["physical_sources"][0].erase(key);});
        ambiguous([&](Json& lineage){lineage["physical_sources"][0][key]="corrupt";});
    }
    ambiguous([](Json& lineage){lineage["physical_sources"][0]["baseline"]["start"]={0.0};});
    ambiguous([](Json& lineage){lineage["physical_sources"][0]["thickness_m"]=-.2;});
    ambiguous([](Json& lineage){lineage["physical_sources"][0]["source_elevation_m"]=10.0;});
    ambiguous([](Json& lineage){lineage["physical_sources"][0]["source_context"]["layer_id"]="different-layer";});
    ambiguous([](Json& lineage){lineage["physical_sources"][0]["vertical_placement"]={{"version",1},{"mode","level"},{"offset_m",0.0}};});
    ambiguous([](Json& lineage){lineage.erase("semantic_phases");});
    ambiguous([](Json& lineage){lineage["semantic_phases"]="corrupt";});
    ambiguous([](Json& lineage){lineage["semantic_phases"][0].erase("active_alternative");});
    ambiguous([](Json& lineage){lineage["semantic_phases"][0]["active_alternative"]=false;});
    ambiguous([](Json& lineage){lineage["semantic_phases"][0]["owners"][0].erase("active_state");});
    ambiguous([](Json& lineage){lineage["semantic_phases"][0]["owners"][0]["active_state"]="unknown";});
    ambiguous([](Json& lineage){lineage["semantic_phases"][0]["owners"][0]["active_state"]="demolished";});
    ambiguous([](Json& lineage){auto& owners=lineage["semantic_phases"][0]["owners"]; owners.push_back(owners[0]);});
    const auto original_digest=document_snapshot_digest(source);
    const auto budget_error=[&](const DocumentSnapshot& excessive,std::string_view expected) {
        try { (void)physical_wall_room_correspondence(excessive,"bottom"); }
        catch (const std::invalid_argument& error) {
            require(std::string_view(error.what())==expected,"correspondence did not abort with the expected source-budget error");
            return;
        }
        throw std::runtime_error("excessive source evidence was swallowed as correspondence uncertainty");
    };
    auto values=copied(source); auto& inventory=find(values,id).extensions["physical_wall_room"]["source_lineage"]["physical_sources"];
    const auto captured_source=inventory[0]; inventory=Json::array();
    for (std::size_t i=0;i<2049;++i) inventory.push_back(captured_source);
    const auto excessive_inventory=Document::create(values);
    budget_error(excessive_inventory.snapshot(),"retained physical source inventory exceeds its budget");
    // The admitted Document limit rejects 65,537 use objects before query entry.
    // This nonconst, deeply copied snapshot is intentionally corrupted only in
    // the test to exercise the report's independent source-use budget boundary.
    sketch::test::DetachedDocumentSnapshotFixture excessive_uses(source);
    auto& uses=excessive_uses.entities().at(id).extensions["physical_wall_room"]["source_lineage"]["outer"]["edges"][0]["source_uses"];
    const auto captured_use=uses[0]; uses=Json::array();
    for (std::size_t i=0;i<65537;++i) uses.push_back(captured_use);
    budget_error(excessive_uses,"room source uses exceed the correspondence budget");
    require(original_digest==document_snapshot_digest(source) && original_digest==document_snapshot_digest(document.snapshot()),
        "captured-evidence queries or detached tamper fixture mutated their original source");
}
void correspondence_elevation_and_level_shifts() {
    const auto check_shift=[](Document& document,Entity changed) {
        const auto id=create_room(document); const auto retained=document.snapshot().entities().at(id);
        document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(std::move(changed))}, {},"Shift captured source plane"});
        const auto report=physical_wall_room_correspondence(document.snapshot(),"bottom");
        require(report.retained.size()==1 && report.retained[0].room==retained &&
            report.retained[0].kind==PhysicalWallRoomCorrespondenceKind::ambiguous &&
            !report.retained[0].diagnostic.empty(),"surviving source identity on a changed plane was dropped or classified reliable");
        for (const auto& fresh:report.fresh) require(fresh.kind==PhysicalWallRoomCorrespondenceKind::ambiguous,
            "changed-plane source identity was mislabeled as a new room");
        for (const auto& link:report.overlaps) require(!link.reliable,"changed-plane correspondence must not be reliable");
    };
    auto absolute=Document::create(fixture()); auto bottom=absolute.snapshot().entities().at("bottom");
    bottom.properties["elevation_m"]=3.0; check_shift(absolute,std::move(bottom));
    auto level_input=fixture();
    const auto graph=VerticalLevelGraph({{"ground",0.0},{"upper",3.0}},{{"storey","ground","upper"}});
    find(level_input,"floor").properties["vertical_level_binding"]={{"version",1},{"graph_id","levels"},{"level_id","ground"}};
    level_input.push_back(entity("levels","vertical_levels",{{"model",Json::parse(graph.serialize())}}));
    for (auto& value:level_input) if (value.type=="wall")
        value.properties["vertical_placement"]={{"version",1},{"mode","level"},{"offset_m",0.0}};
    auto level=Document::create(level_input); auto moved=level.snapshot().entities().at("levels");
    moved.properties["model"]=Json::parse(graph.with_elevation("ground",1.0).serialize()); check_shift(level,std::move(moved));
    // A different, unchanged plane with independent source identities remains outside the selected report.
    auto two_planes=fixture(); rectangle(two_planes,"upper-",{10,0},{14,3});
    for (auto& value:two_planes) if (value.type=="wall" && value.id.starts_with("upper-")) value.properties["elevation_m"]=3.0;
    auto unrelated=Document::create(two_planes); (void)create_room(unrelated);
    unrelated.apply(prepare_physical_wall_rooms(unrelated.snapshot(),"upper-bottom",{0},"office"));
    const auto report=physical_wall_room_correspondence(unrelated.snapshot(),"bottom");
    require(report.retained.size()==1 && report.retained[0].kind==PhysicalWallRoomCorrespondenceKind::unique_continuation,
        "unrelated plane owners must remain outside selected-plane correspondence");
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
            {"correspondence_noop_moves_and_thickness",correspondence_noop_moves_and_thickness},
            {"correspondence_partitions_split_merge_and_ambiguity",correspondence_partitions_split_merge_and_ambiguity},
            {"correspondence_holes_curves_new_retired_and_failures",correspondence_holes_curves_new_retired_and_failures},
            {"correspondence_captured_evidence_and_budgets",correspondence_captured_evidence_and_budgets},
            {"correspondence_elevation_and_level_shifts",correspondence_elevation_and_level_shifts},
            {"clear_room_exports",clear_room_exports}};
        for (const auto& [name,run]:cases) {
            try { run(); } catch (const std::exception& e) { throw std::runtime_error(std::string(name)+": "+e.what()); }
        }
        std::cout<<"physical wall room tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
