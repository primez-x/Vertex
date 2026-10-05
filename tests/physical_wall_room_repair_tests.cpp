#include "sketch/physical_wall_room.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/noninteractive_errors.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Operation> void refuses(Operation operation) {
    try { operation(); }
    catch (const DocumentError&) { return; }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("unreviewed or forged physical room repair was accepted");
}
Entity entity(std::string id,std::string type,Json properties=Json::object()) {
    return {std::move(id),std::move(type),std::move(properties)};
}
Entity wall(std::string id,Vec2 start,Vec2 end,double thickness=.2,double sweep=0) {
    return entity(std::move(id),"wall",{{"baseline",{{"start",{start.x,start.y}},
        {"end",{end.x,end.y}},{"sweep_radians",sweep}}},{"thickness_m",thickness},
        {"height_m",3},{"elevation_m",0},{"layer_id","layer"}});
}
Document fixture(bool curved=false) {
    return Document::create({entity("property","property"),entity("building","building",{{"property_id","property"}}),
        entity("floor","floor",{{"building_id","building"}}),entity("layer","layer",{{"floor_id","floor"}}),
        wall("bottom",{0,0},{4,0},.2,curved ? .4 : 0),wall("right",{4,0},{4,3}),
        wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0})});
}
std::string create_room(Document& document,std::size_t index=0) {
    auto command=prepare_physical_wall_rooms(document.snapshot(),"bottom",{index},"office");
    require(command.entity_changes.size()==1,"fixture must create one physical room");
    auto& room=command.entity_changes.front().entity;
    room.properties["name"]="Reviewed room";room.extensions["vendor"]={{"retain",true}};
    const auto id=room.id;(void)document.apply(command);return id;
}
void change_wall(Document& document,const char* id,double thickness) {
    auto changed=document.snapshot().entities().at(id);changed.properties["thickness_m"]=thickness;
    (void)document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(changed)}, {},"Change room sources"});
}
LegacyBoundaryIdentityOptions fresh_ids(std::size_t count) {
    LegacyBoundaryIdentityOptions result;
    for (std::size_t i=0;i<count;++i) {
        result.segment_ids.push_back("repair-edge-"+make_stable_id());
        result.vertex_ids.push_back("repair-vertex-"+make_stable_id());
    }
    return result;
}
EditBoundaryGeometry prepare(const DocumentSnapshot& source,const std::string& id,std::size_t destination=0,
    Vec2 witness={2,1.5},const PhysicalWallRoomRepairReferences& references={}) {
    const auto detected=detect_physical_wall_spaces(source,"bottom");
    const auto& space=detected.spaces.at(destination);
    return prepare_physical_wall_room_repair(source,id,"bottom",witness,space.source_lineage,
        physical_wall_room_descriptor_digest(source.entities().at(id)),fresh_ids(space.boundary.size()),references);
}
std::vector<Entity> copied(const DocumentSnapshot& snapshot) {
    std::vector<Entity> result;for (const auto& [id,value]:snapshot.entities()) {(void)id;result.push_back(value);}return result;
}
void unrelated_floor_rooms_do_not_consume_repair_budget() {
    auto initial=fixture(); const auto id=create_room(initial); change_wall(initial,"bottom",.4);
    const auto source=initial.snapshot(); auto values=copied(source);
    values.push_back(entity("other-floor","floor",{{"building_id","building"}}));
    values.push_back(entity("other-layer","layer",{{"floor_id","other-floor"}}));
    const auto& original=source.entities().at(id);
    for (std::size_t i=0;i<2049;++i) {
        auto metadata=original; metadata.id="unrelated-room-"+std::to_string(i); metadata.properties["layer_id"]="other-layer";
        metadata.properties["floor_id"]="other-floor";
        auto topology=decode_identified_boundary_entity(original); topology.id=metadata.id;
        const auto ids=fresh_ids(topology.segments.size());
        for (std::size_t j=0;j<topology.segments.size();++j) {
            topology.segments[j].segment_id=ids.segment_ids[j]; topology.segments[j].start_vertex_id=ids.vertex_ids[j];
            topology.segments[j].end_vertex_id=ids.vertex_ids[(j+1)%ids.vertex_ids.size()];
        }
        values.push_back(encode_identified_boundary_entity(topology,&metadata));
    }
    auto expanded=Document::create(values); const auto before=expanded.snapshot();
    const auto organization=organize_project(before); const auto selected_context=organization.drawing_context(id);
    for (const auto& [owner,value] : before.entities()) if (owner.starts_with("unrelated-room-")) {
        (void)value; const auto context=organization.drawing_context(owner);
        require(context && context->complete() && context!=selected_context,"regression needs genuinely resolved unrelated-floor owners");
    }
    const auto command=prepare(before,id);
    expanded.apply(command);
    const auto after=expanded.snapshot(); const auto& repaired=after.entities().at(id);
    const auto detected=detect_physical_wall_spaces(after,"bottom");
    require(decode_physical_wall_room_descriptor(repaired).source_lineage==detected.spaces.front().source_lineage,
        "unrelated-context owners blocked a valid current-context repair");
    for (const auto& [owner,value] : before.entities()) if (owner!=id)
        require(after.entities().at(owner)==value,"isolated repair changed an unrelated owner");
}
void same_id_repair_preserves_metadata_and_exact_history() {
    for (const bool curved:{false,true}) {
        auto document=fixture(curved);const auto id=create_room(document);
        change_wall(document,"bottom",.4);
        const auto before=document.snapshot();
        require(!physical_wall_room_checks(before).at(id).current,"source thickness change must make retained room stale");
        const auto command=prepare(before,id);
        require(command.edit.physical_wall_room_repair && encode_boundary_geometry_edit(command.edit).at("version")==7,
            "repair did not retain strict version-seven source authority");
        require(command_to_json(command_from_json(command_to_json(Command{command})))==command_to_json(Command{command}),
            "typed repair command did not round trip exactly");
        const auto candidate=Document::preview_command(before,Command{command});
        require(document.snapshot().entities()==before.entities(),"repair preparation or preview mutated captured document");
        const auto& old=before.entities().at(id);const auto& repaired=candidate.entities().at(id);
        require(repaired.id==old.id && repaired.type==old.type && repaired.properties.at("name")==old.properties.at("name") &&
            repaired.properties.at("classification")==old.properties.at("classification") &&
            repaired.properties.at("measurement_classification")==old.properties.at("measurement_classification") &&
            repaired.extensions.at("vendor")==old.extensions.at("vendor"),"same-ID repair overwrote retained room identity or facts");
        const auto current=physical_wall_room_checks(candidate).at(id);
        require(current.current,"independently replayed repair is not a current physical room");
        if (!curved) require(std::abs(current.area_square_metres-10.26)<1e-9,"thickened rectangular room has wrong known clear area");
        for (const auto* source:{"bottom","right","top","left"})
            require(candidate.entities().at(source)==before.entities().at(source),"room repair changed physical sources");
        require(candidate.history().back().boundary_geometry_edit==command.edit &&
            candidate.history().at(static_cast<std::size_t>(before.revision())).entities.at(id)==old,
            "repair history lost old descriptor evidence or reviewed intent");
        (void)document.apply(command);
        require(document.snapshot().entities()==candidate.entities() && document.revision()==before.revision()+1,
            "repair Apply did not commit one complete reviewed transaction");
        require(Document::fork(document.snapshot()).snapshot().entities()==candidate.entities(),"repair fork did not independently replay retained proof");
        document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"repair Undo lost exact retained stale room state");
        document.redo(document.revision());require(document.snapshot().entities()==candidate.entities(),"repair Redo lost exact reviewed destination");
        require(ProjectStore::required_format_version(document.snapshot())==44,"retained repair proof did not require native44");
        auto entity_only=Document::create(copied(candidate));
        require(ProjectStore::required_format_version(entity_only.snapshot())==44,"entity-only repair derivation carrier lost native44 reader floor");
        auto deleted=Document::fork(document.snapshot());
        deleted.apply(ApplyEntityChanges{deleted.revision(),{EntityChange::erase(id)}, {},"Delete repaired room"});
        require(ProjectStore::required_format_version(deleted.snapshot())==44,"deleted repair history lost its reader floor");
        const auto path=std::filesystem::temp_directory_path()/("physical-room-repair-"+make_stable_id()+".bldproj");
        (void)ProjectStore::save(path,document.snapshot());
        auto loaded=ProjectStore::load(path);std::filesystem::remove(path);
        require(loaded.document.snapshot().entities()==candidate.entities() && physical_wall_room_checks(loaded.document.snapshot()).at(id).current,
            "native reopen did not preserve reviewed source-room repair");
        const auto extraction=std::filesystem::temp_directory_path()/("physical-room-repair-extract-"+make_stable_id());
        extract_project(document.snapshot(),extraction);
        std::ifstream input(extraction/"project.json");const auto manifest=Json::parse(input);
        require(manifest.at("exchange_version")==42,"retained repair proof did not require extraction42");
        input.close();std::filesystem::remove_all(extraction);
    }
}
void forged_inputs_and_retained_payloads_are_refused() {
    auto document=fixture();const auto id=create_room(document);change_wall(document,"bottom",.4);
    const auto before=document.snapshot();const auto command=prepare(before,id);
    auto bad=command;bad.edit.physical_wall_room_repair->expected_descriptor_digest=std::string(64,'0');
    refuses([&]{(void)document.apply(bad);});
    bad=command;bad.edit.physical_wall_room_repair->interior_witness={0,0};
    refuses([&]{(void)document.apply(bad);});
    bad=command;bad.edit.physical_wall_room_repair->reviewed_source_lineage["forged"]=true;
    refuses([&]{(void)document.apply(bad);});
    bad=command;bad.edit.replacement_segments[0]["start"][0]=bad.edit.replacement_segments[0]["start"][0].get<double>()+.01;
    refuses([&]{(void)document.apply(bad);});
    bad=command;bad.edit.replacement_properties["classification"]="bedroom";
    refuses([&]{(void)command_to_json(Command{bad});});
    bad=command;bad.edit.replacement_wall_source_ids={"bottom","right","top","left"};
    refuses([&]{(void)command_to_json(Command{bad});});
    auto encoded=encode_boundary_geometry_edit(command.edit);encoded["physical_wall_room_repair"]["component_index"]=0;
    refuses([&]{(void)decode_boundary_geometry_edit(encoded);});
    encoded=encode_boundary_geometry_edit(command.edit);encoded["version"]=8;
    refuses([&]{(void)decode_boundary_geometry_edit(encoded);});
    require(document.snapshot().entities()==before.entities() && document.revision()==before.revision(),"repair refusal published partial state");
    (void)document.apply(command);const auto after=document.snapshot();
    auto tampered=after;
    auto& records=const_cast<std::vector<RevisionRecord>&>(tampered.history());
    records.back().boundary_geometry_edit->physical_wall_room_repair->reviewed_source_lineage["forged"]=true;
    refuses([&]{(void)Document::fork(tampered);});
    tampered=after;auto& holes=const_cast<std::vector<RevisionRecord>&>(tampered.history()).back().entities.at(id).extensions["physical_wall_room"]["holes"];
    holes.push_back(Json::array({{{"start",{1,1}},{"end",{1.2,1}},{"sweep_radians",0}},
        {{"start",{1.2,1}},{"end",{1.1,1.2}},{"sweep_radians",0}},{{"start",{1.1,1.2}},{"end",{1,1}},{"sweep_radians",0}}}));
    refuses([&]{(void)Document::fork(tampered);});
    auto unsupported=copied(after);
    for (auto& value:unsupported) if (value.id==id)
        value.extensions["boundary_geometry_derivation"]["operations"].back()["value"]["version"]=8;
    refuses([&]{(void)Document::create(unsupported);});
    const auto active=prepare(after,id);
    auto later=document.snapshot().entities().at("top");later.properties["thickness_m"]=.3;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(later)}, {},"Change after review"});
    refuses([&]{(void)document.apply(active);});
}
void holes_splits_merges_and_occupied_destinations_require_review() {
    auto document=fixture();const auto id=create_room(document);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(wall("island",{1,1},{3,1}))}, {},"Add island material"});
    const auto with_hole=document.snapshot();const auto hole_repair=prepare(with_hole,id,0,{2,2});
    const auto candidate=Document::preview_command(with_hole,Command{hole_repair});
    const auto checked=physical_wall_room_checks(candidate).at(id);
    require(checked.current && checked.holes.size()==1 && std::abs(checked.area_square_metres-10.24)<1e-9,
        "reviewed repair did not regenerate inline analytic wall-island deduction");
    auto in_hole=hole_repair;in_hole.edit.physical_wall_room_repair->interior_witness={2,1};
    refuses([&]{(void)Document::preview_command(with_hole,Command{in_hole});});
    auto split=fixture();const auto split_id=create_room(split);
    split.apply(ApplyEntityChanges{split.revision(),{EntityChange::upsert(wall("divider",{2,0},{2,3}))}, {},"Divide retained room"});
    const auto spaces=detect_physical_wall_spaces(split.snapshot(),"bottom");
    require(spaces.spaces.size()==2,"baseline divider must expose two explicit destinations");
    std::size_t left=0;while (left<spaces.spaces.size() && spaces.spaces[left].boundary.front().start.x>1) ++left;
    const auto reassigned=prepare(split.snapshot(),split_id,left,{1,1.5});split.apply(reassigned);
    const auto assignments=physical_wall_room_checks(split.snapshot());
    require(assignments.size()==1 && assignments.at(split_id).current && std::abs(assignments.at(split_id).area_square_metres-5.04)<1e-9,
        "split repair silently classified extra components or chose a different destination");
    const auto other=prepare_physical_wall_rooms(split.snapshot(),"bottom",{1-left},"bedroom");
    require(other.entity_changes.size()==1,"other split component must remain available and unclassified");
    split.apply(other);
    refuses([&]{(void)prepare(split.snapshot(),split_id,1-left,{3,1.5});});
    const auto merge_owner=other.entity_changes.front().entity.id;
    split.apply(ApplyEntityChanges{split.revision(),{EntityChange::erase("divider")}, {},"Merge source spaces"});
    const auto merge=prepare(split.snapshot(),split_id,0,{2,1.5});split.apply(merge);
    require(physical_wall_room_checks(split.snapshot()).at(split_id).current &&
        !physical_wall_room_checks(split.snapshot()).at(merge_owner).current &&
        split.snapshot().entities().at(merge_owner).properties.at("classification")=="bedroom",
        "reviewed merge acquired or rewrote another retained owner's facts");
}
void explicit_child_reference_decisions_and_lifetimes() {
    auto document=fixture();const auto id=create_room(document);
    const auto old=decode_identified_boundary_entity(document.snapshot().entities().at(id));
    PersistentConstraint pin;pin.id="room-pin";pin.relation=ConstraintRelationKind::fixed_anchor;
    pin.bindings={{id,WallEndpointRole::start,old.segments.front().segment_id,old.segments.front().start_vertex_id}};
    pin.anchor=old.segments.front().segment.start;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(encode_constraint_entity(pin))}, {},"Pin room vertex"});
    change_wall(document,"bottom",.4);const auto before=document.snapshot();
    refuses([&]{(void)prepare(before,id);});
    PhysicalWallRoomRepairReferences remove;remove.removed_reference_ids={pin.id};
    const auto command=prepare(before,id,0,{2,1.5},remove);document.apply(command);
    require(!document.snapshot().entities().contains(pin.id),"explicit affected constraint removal was not replayed atomically");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"repair Undo lost explicit dependency decisions");
    document.redo(document.revision());const auto repaired=document.snapshot();
    auto reuse=prepare(repaired,id);reuse.edit.replacement_segments=command.edit.replacement_segments;
    refuses([&]{(void)document.apply(reuse);});
    auto malformed=prepare(repaired,id);malformed.edit.replacement_removed_reference_ids={"bottom"};
    refuses([&]{(void)document.apply(malformed);});
    auto phase_values=copied(before);
    const auto phases=ModelPhases::create({id},{id},{{"remove","Remove retained room",{id},{}}},"remove");
    phase_values.push_back(entity("room-phases","model_phases",{{"model",phases.to_json()}}));
    const auto inactive=Document::create(phase_values);
    refuses([&]{(void)prepare(inactive.snapshot(),id,0,{2,1.5},remove);});
}

void source_replacement_material_split_and_reviewed_mapping() {
    auto replacement=fixture();const auto id=create_room(replacement);
    auto bottom=replacement.snapshot().entities().at("bottom");bottom.id="replacement-bottom";
    replacement.apply(ApplyEntityChanges{replacement.revision(),{EntityChange::erase("bottom"),EntityChange::upsert(bottom)}, {},"Replace source wall identity"});
    const auto before=replacement.snapshot();const auto detected=detect_physical_wall_spaces(before,"replacement-bottom");
    const auto command=prepare_physical_wall_room_repair(before,id,"replacement-bottom",{2,1.5},detected.spaces.front().source_lineage,
        physical_wall_room_descriptor_digest(before.entities().at(id)),fresh_ids(detected.spaces.front().boundary.size()),{});
    replacement.apply(command);
    require(physical_wall_room_checks(replacement.snapshot()).at(id).current &&
        decode_physical_wall_room_descriptor(replacement.snapshot().entities().at(id)).selected_wall_id=="replacement-bottom",
        "reviewed same-ID repair did not reassign replaced physical source identity");
    BoundaryDimension unsupported{"room-length",id,decode_identified_boundary_entity(replacement.snapshot().entities().at(id)).segments.front().segment_id,{2,0}};
    refuses([&]{(void)unsupported.resolve(replacement.snapshot().entities().at(id));});
    auto material=fixture();const auto material_id=create_room(material);
    material.apply(ApplyEntityChanges{material.revision(),{EntityChange::upsert(wall("near-divider",{2,.05},{2,2.95},.3))}, {},
        "Split clear components with wall material"});
    const auto components=detect_physical_wall_spaces(material.snapshot(),"bottom");
    require(components.graph.faces.size()==1 && components.spaces.size()==2,"material-only split must retain one baseline face and expose two destinations");
    const auto left=std::find_if(components.spaces.begin(),components.spaces.end(),[](const auto& space){return space.boundary.front().start.x<1;});
    require(left!=components.spaces.end(),"material split omitted left clear component");
    const auto selected=static_cast<std::size_t>(left-components.spaces.begin());
    material.apply(prepare(material.snapshot(),material_id,selected,{1,1.5}));
    const auto material_checks=physical_wall_room_checks(material.snapshot());
    require(material_checks.size()==1 && material_checks.at(material_id).current &&
        std::abs(material_checks.at(material_id).area_square_metres-4.9)<1e-9,
        "material-only reviewed split invented extra room ownership");
    auto mapped=fixture();const auto mapped_id=create_room(mapped);const auto source=mapped.snapshot();
    const auto old=decode_identified_boundary_entity(source.entities().at(mapped_id));
    PersistentConstraint pin;pin.id="mapped-pin";pin.relation=ConstraintRelationKind::fixed_anchor;
    pin.bindings={{mapped_id,WallEndpointRole::start,old.segments.front().segment_id,old.segments.front().start_vertex_id}};
    pin.anchor=old.segments.front().segment.start;
    mapped.apply(ApplyEntityChanges{mapped.revision(),{EntityChange::upsert(encode_constraint_entity(pin))}, {},"Attach reviewed endpoint reference"});
    const auto current=mapped.snapshot();const auto rooms=detect_physical_wall_spaces(current,"bottom");
    const auto identities=fresh_ids(rooms.spaces.front().boundary.size());
    PhysicalWallRoomRepairReferences references;
    references.child_mapping={{"segments",{{old.segments.front().segment_id,identities.segment_ids.front()}}},
        {"vertices",{{old.segments.front().start_vertex_id,identities.vertex_ids.front()}}}};
    const auto remap=prepare_physical_wall_room_repair(current,mapped_id,"bottom",{2,1.5},rooms.spaces.front().source_lineage,
        physical_wall_room_descriptor_digest(current.entities().at(mapped_id)),identities,references);
    mapped.apply(remap);const auto constraint=decode_constraint_entity(mapped.snapshot().entities().at(pin.id));
    require(constraint.supported() && constraint.constraint->bindings.front().segment_id==identities.segment_ids.front() &&
        constraint.constraint->bindings.front().vertex_id==identities.vertex_ids.front(),"explicit reviewed child mapping did not preserve endpoint reference identity");
}

void moved_sources_and_context_phase_changes_are_revalidated() {
    auto document=fixture();const auto id=create_room(document);
    std::vector<EntityChange> moves;
    for (const auto* key:{"bottom","right","top","left"}) {
        auto value=document.snapshot().entities().at(key);
        for (const auto* endpoint:{"start","end"}) {
            value.properties["baseline"][endpoint][0]=value.properties["baseline"][endpoint][0].get<double>()+10;
            value.properties["baseline"][endpoint][1]=value.properties["baseline"][endpoint][1].get<double>()+5;
        }
        moves.push_back(EntityChange::upsert(value));
    }
    document.apply(ApplyEntityChanges{document.revision(),moves,{},"Move physical source loop"});
    const auto before=document.snapshot();const auto repair=prepare(before,id,0,{12,6.5});
    document.apply(repair);
    require(physical_wall_room_checks(document.snapshot()).at(id).current &&
        std::abs(physical_wall_room_checks(document.snapshot()).at(id).area_square_metres-10.64)<1e-9,
        "reviewed repair did not follow moved physical sources with unchanged analytical area");
    auto reordered=copied(before);std::reverse(reordered.begin(),reordered.end());
    const auto source=Document::create(reordered);
    const auto detection=detect_physical_wall_spaces(source.snapshot(),"bottom");
    require(detection.spaces.front().source_lineage==repair.edit.physical_wall_room_repair->reviewed_source_lineage,
        "source entity permutation changed exact reviewed destination lineage");
    auto wrong=repair;wrong.expected_revision=source.revision();
    wrong.edit.physical_wall_room_repair->selected_wall_id="absent-source";
    refuses([&]{(void)Document::preview_command(source.snapshot(),Command{wrong});});
    auto phased=copied(before);
    const auto phases=ModelPhases::create({"bottom"},{"bottom"},{{"demolish","Demolish selected source",{"bottom"},{}}},"demolish");
    phased.push_back(entity("source-phases","model_phases",{{"model",phases.to_json()}}));
    const auto inactive=Document::create(phased);
    refuses([&]{(void)prepare(inactive.snapshot(),id,0,{12,6.5});});
    auto wrong_context=copied(before);
    wrong_context.push_back(entity("other-layer","layer",{{"floor_id","floor"}}));
    for (auto& value:wrong_context) if (value.id==id) value.properties["layer_id"]="other-layer";
    const auto foreign=Document::create(wrong_context);
    refuses([&]{(void)prepare(foreign.snapshot(),id,0,{12,6.5});});
}
}
int main() {
    sketch::runtime::configure_noninteractive_errors();
    try {
        same_id_repair_preserves_metadata_and_exact_history();
        unrelated_floor_rooms_do_not_consume_repair_budget();
        forged_inputs_and_retained_payloads_are_refused();
        holes_splits_merges_and_occupied_destinations_require_review();
        explicit_child_reference_decisions_and_lifetimes();
        source_replacement_material_split_and_reviewed_mapping();
        moved_sources_and_context_phase_changes_are_revalidated();
    } catch (const std::exception& error) {
        std::cerr<<"physical_wall_room_repair_tests: "<<error.what()<<'\n';return 1;
    }
    std::cout<<"Typed reviewed physical room repair passed\n";
}
