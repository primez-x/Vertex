#include "sketch/pinc_geometry_admission.hpp"
#include "sketch/measurement_linework.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void require_close(double actual,double expected){require(std::abs(actual-expected)<1e-10,"known analytical quantity changed");}
template<class Operation>void rejects(Operation operation){try{operation();}catch(const std::invalid_argument&){return;}throw std::runtime_error("unsafe admission accepted");}
Json edge(std::string id,double ax,double ay,double bx,double by,double height=0) {
    return {{"id",id},{"a",{{"x",ax},{"y",ay}}},{"b",{{"x",bx},{"y",by}}},
        {"kind",height?"arc":"line"},{"bulge",height}};
}
Json square(std::string prefix,double left=0,double size=8) {
    return Json::array({edge(prefix+"a",left,0,left+size,0),edge(prefix+"b",left+size,0,left+size,size),
        edge(prefix+"c",left+size,size,left,size),edge(prefix+"d",left,size,left,0)});
}
Json page(Json edges=square(""),std::string key="a|b|c|d") {
    return {{"calcWalls",edges},{"interiorWalls",Json::array()},
        {"assignments",{{key,{{"code","GLA1"},{"name","Source descriptive name"}}}}}};
}
PincImportProject parse(const Json& source) {
    const auto bytes=source.dump();return parse_pinc_project(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
}
PincImportProject modern(Json pg=page()){return parse({{"format","PincSketch"},{"version","4.2"},{"pages",{pg}}});}
std::vector<Entity> base_entities(std::size_t pages=1) {
    std::vector<Entity> result{{"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}}};
    for(std::size_t i=0;i<pages;++i) {
        const auto suffix=std::to_string(i);
        result.push_back({"floor"+suffix,"floor",{{"building_id","building"}}});
        result.push_back({"calc"+suffix,"layer",{{"floor_id","floor"+suffix}}});
        result.push_back({"interior"+suffix,"layer",{{"floor_id","floor"+suffix}}});
    }
    return result;
}
std::vector<PincPageGeometryContext> contexts(std::size_t pages=1) {
    std::vector<PincPageGeometryContext> result;
    for(std::size_t i=0;i<pages;++i) {
        const auto n=std::to_string(i);
        result.push_back({i,{"property","building","floor"+n,"calc"+n},
            {"property","building","floor"+n,"interior"+n}});
    }
    return result;
}
void exact_strokes_contexts_and_detached_area_handles() {
    auto pg=page();pg["interiorWalls"]=Json::array({edge("i",4,0,4,8)});
    auto source=modern(pg);auto base=Document::create(base_entities());const auto before=base.snapshot();
    const auto result=admit_pinc_geometry(source,base.snapshot(),contexts(),"import-one");
    require(base.snapshot().entities()==before.entities() && base.revision()==before.revision() &&
        base.snapshot().assets()==before.assets() && base.snapshot().history().size()==before.history().size() &&
        result.entities.size()==5,"preparation changed base or lost a stroke");
    require(result.pages[0].graph.faces.size()==1 && result.pages[0].assignments[0].face_index==0,
        "interior divider contaminated calculation context");
    require_close(result.pages[0].graph.faces[0].area_square_metres,5.94579456);
    std::set<std::string> ids;
    for(const auto& e:result.entities) {
        require(e.type=="measurement_linework" && e.required && !e.properties.contains("appraisal_facts") &&
            !e.properties.contains("thickness"),"source classifications became physical/appraisal facts");
        const auto model=decode_measurement_linework_model(e.properties.at("model"));
        const auto replay=replay_measurement_linework(*model.model);
        require(!model.model->closed && replay.edges.size()==1,"single-edge receipt topology changed");
        const auto& s=replay.edges[0];require(ids.insert(e.id).second && ids.insert(s.segment_id).second &&
            ids.insert(s.start_vertex_id).second && ids.insert(s.end_vertex_id).second,"global native identities collided");
    }
    require(result.source_mappings[0].stroke_id!=source.pages[0].calculation_segments[0].source.identity,
        "source identity became native authority");
    ApplyEntityChanges command{base.revision(),{}, {},"Detached Pinc geometry"};
    for(const auto& e:result.entities)command.entity_changes.push_back(EntityChange::upsert(e));
    const auto preview=Document::preview_command(base.snapshot(),command);
    require(preview.entities().size()==10,"private native candidate did not validate");
}
void legacy_attached_areas_and_partitions() {
    auto house=square("h");auto garage=Json::array({edge("ga",8,2,16,2),edge("gb",16,2,16,6),
        edge("gc",16,6,8,6),edge("gd",8,6,8,2)});
    auto source=parse({{"version","2.0"},{"areas",{{{"id",1},{"code","GLA1"},{"segments",house}},
        {{"id",2},{"code","GAR"},{"segments",garage}}}}});
    auto base=Document::create(base_entities());auto result=admit_pinc_geometry(source,base.snapshot(),contexts(),"attached-legacy");
    require(result.pages[0].graph.faces.size()==2 && result.pages[0].assignments[0].face_index &&
        result.pages[0].assignments[1].face_index && result.pages[0].assignments[0].face_index!=result.pages[0].assignments[1].face_index,
        "attached geometry refused an unchanged complete original source cycle");
    const auto house_face=*result.pages[0].assignments[0].face_index;
    require(source.pages[0].legacy_areas[0].segments.size()==4 && result.pages[0].graph.faces[house_face].boundary.size()==6,
        "attached-area fixture did not split the original house wall");
    require_close(result.pages[0].graph.faces[house_face].area_square_metres,5.94579456);
    require_close(result.pages[0].graph.faces[*result.pages[0].assignments[1].face_index].area_square_metres,2.97289728);
    for(auto& e:house)std::swap(e["a"],e["b"]);
    source=parse({{"version","2.0"},{"areas",{{{"id",1},{"code","GLA1"},{"segments",house}},
        {{"id",2},{"code","GAR"},{"segments",garage}}}}});
    result=admit_pinc_geometry(source,base.snapshot(),contexts(),"reversed-attached-legacy");
    require(result.pages[0].assignments[0].face_index && result.pages[0].assignments[1].face_index,
        "reversed original traversal lost exact split-interval coverage");
    // A second legacy area introduces a divider across the original house.
    // Neither resulting half covers the complete original 0..1 source cycle.
    // Keep the second area's height equal to the whole house, making its
    // right wall a complete partition rather than an attached stub.
    auto half=Json::array({edge("ga",0,0,4,0),edge("gb",4,0,4,8),edge("gc",4,8,0,8),edge("gd",0,8,0,0)});
    source=parse({{"version","2.0"},{"areas",{{{"id",1},{"code","GLA1"},{"segments",square("h")}},
        {{"id",2},{"code","GAR"},{"segments",half}}}}});
    result=admit_pinc_geometry(source,base.snapshot(),contexts(),"partitioned-legacy");
    require(result.pages[0].graph.faces.size()==2 && !result.pages[0].assignments[0].face_index &&
        result.pages[0].assignments[0].code=="assignment_no_exact_face" && result.pages[0].assignments[1].face_index,
        "actual partition inherited the original full-house assignment");
    PincGeometryAdmissionLimits limits;limits.max_correspondence_work=100;
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"limited-legacy",limits);});
}
void shared_edge_and_page_occurrences() {
    auto edges=square("");edges.erase(1);
    for(const auto& e:Json::array({edge("shared",8,0,8,8),edge("e",8,0,16,0),edge("f",16,0,16,8),edge("g",16,8,8,8)}))edges.push_back(e);
    auto pg=page(edges,"a|shared|c|d");pg["assignments"]["e|f|g|shared"]={{"code","GAR"}};
    auto source=parse({{"format","PincSketch"},{"version","4.2"},{"pages",{pg,pg}}});
    auto base=Document::create(base_entities(2));const auto result=admit_pinc_geometry(source,base.snapshot(),contexts(2),"shared-import");
    require(result.entities.size()==14 && result.pages[0].graph.faces.size()==2 && result.pages[1].graph.faces.size()==2,
        "shared source was duplicated or page graphs merged");
    for(const auto& p:result.pages)require(p.assignments[0].face_index && p.assignments[1].face_index &&
        p.assignments[0].face_index!=p.assignments[1].face_index,"adjacent assignments failed exact independent correspondence");
    require(result.source_mappings[0].stroke_id!=result.source_mappings[7].stroke_id,"duplicate page child IDs collided");
    auto values=base_entities();values.push_back({"calc1","layer",{{"floor_id","floor0"}}});
    values.push_back({"interior1","layer",{{"floor_id","floor0"}}});auto one_floor=Document::create(values);
    auto reviewed=contexts(2);reviewed[1].calculation.floor_id="floor0";reviewed[1].interior.floor_id="floor0";
    const auto same_floor=admit_pinc_geometry(source,one_floor.snapshot(),reviewed,"one-floor-import");
    require(same_floor.pages.size()==2 && same_floor.pages[0].graph.faces.size()==2 && same_floor.pages[1].graph.faces.size()==2,
        "reviewed pages sharing a floor lost independent calculation layers");
}
void arcs_and_legacy_alias_orientation() {
    auto pg=page();pg["calcWalls"][0]=edge("a",0,0,8,0,2);auto source=modern(pg);
    auto base=Document::create(base_entities());auto result=admit_pinc_geometry(source,base.snapshot(),contexts(),"arc-import");
    const auto model=decode_measurement_linework_model(result.entities[0].properties.at("model"));
    require(model.model->edges[0].receipt.kind==BoundaryConstructionKind::arc_chord_angle &&
        !model.model->edges[0].receipt.clockwise,"arc was reduced to linework or unsigned direction");
    require_close(replay_measurement_linework(*model.model).edges[0].segment.sweep_radians,1.8545904360032244);
    require(result.pages[0].assignments[0].face_index.has_value(),"exact curved assignment failed");
    auto first=square("");auto second=square("x");
    // Same complete source area in opposite orientation: retain it as a
    // conflicting second owner, never automatically choose a classification.
    for(auto& e:second){std::swap(e["a"],e["b"]);}
    source=parse({{"version","2.0"},{"areas",{{{"id",1},{"code","GLA1"},{"segments",first}},
        {{"id",2},{"code","GAR"},{"segments",second}}}}});
    result=admit_pinc_geometry(source,base.snapshot(),contexts(),"legacy-import");
    require(result.entities.size()==4 && result.source_mappings.size()==8,"legacy exact aliases expanded into native duplicates");
    std::size_t reversed=0;for(const auto& m:result.source_mappings)if(m.reversed)++reversed;
    require(reversed==4,"legacy reversed source mapping lost direction");
    for(const auto& a:result.pages[0].assignments)require(!a.face_index && a.code=="assignment_multiple_owners",
        "duplicate legacy owner was silently chosen");
}
void independent_nested_gross_and_unmatched_evidence() {
    auto pg=page();pg["assignments"]["a|b|c|d"]["_anchor"]={{"x",2},{"y",2}};
    pg["assignments"]["a|b|c|d"]["_area"]=999;
    for(const auto& e:Json::array({edge("ia",2,2,6,2),edge("ib",6,2,6,6),edge("ic",6,6,2,6),edge("id",2,6,2,2)}))pg["calcWalls"].push_back(e);
    pg["assignments"]["ia|ib|ic|id"]={{"code","GAR"}};
    auto source=modern(pg);auto base=Document::create(base_entities());
    auto result=admit_pinc_geometry(source,base.snapshot(),contexts(),"nested-import");
    require(result.pages[0].graph.faces.size()==2 && result.pages[0].assignments[0].face_index &&
        result.pages[0].assignments[1].face_index && result.pages[0].assignments[0].face_index!=result.pages[0].assignments[1].face_index,
        "independent nested gross outlines became implicit holes or deductions");
    require_close(result.pages[0].graph.faces[*result.pages[0].assignments[0].face_index].area_square_metres,5.94579456);
    require_close(result.pages[0].graph.faces[*result.pages[0].assignments[1].face_index].area_square_metres,1.48644864);
    require(result.entities.size()==8,"nested correspondence authored implicit area entities");
    pg=page();pg["calcWalls"][0]["a"]["x"]=.0001;source=modern(pg);
    result=admit_pinc_geometry(source,base.snapshot(),contexts(),"gap-import");
    require(!result.pages[0].assignments[0].face_index,"near endpoints silently established closure");
}
void split_source_junctions_and_ambiguous_keys() {
    // Both spaces use portions of the same long top/bottom sources. Whole
    // original walls cannot assemble either cycle; exact noding must preserve
    // the partial source intervals and distinguish left/right side sources.
    auto pg=page(Json::array({edge("top",0,0,16,0),edge("bottom",16,8,0,8),
        edge("left",0,8,0,0),edge("right",16,0,16,8),edge("divider",8,0,8,8)}),"bottom|divider|left|top");
    pg["assignments"]["bottom|divider|right|top"]={{"code","GAR"}};
    auto base=Document::create(base_entities());auto result=admit_pinc_geometry(modern(pg),base.snapshot(),contexts(),"junction-import");
    const auto& page_result=result.pages[0];
    require(result.entities.size()==5 && page_result.graph.faces.size()==2 && page_result.assignments[0].face_index &&
        page_result.assignments[1].face_index && page_result.assignments[0].face_index!=page_result.assignments[1].face_index,
        "long source walls lost uniquely reviewed T-junction spaces");
    for(const auto& face:page_result.graph.faces)require_close(face.area_square_metres,5.94579456);
    std::size_t partial_uses=0;
    for(const auto& e:page_result.graph.edges)for(const auto& use:e.source_uses)
        if(use.owner_id==result.source_mappings[0].stroke_id||use.owner_id==result.source_mappings[1].stroke_id) {
            require(use.parameter_end-use.parameter_start==.5,"split source lineage interval changed");++partial_uses;
        }
    require(partial_uses==4,"long wall lineage was replaced by fabricated source edges");

    // Three almost-complete circles leave their outward lobes open. Their
    // overlapping central regions still contain several bounded faces whose
    // boundaries all use the same three analytical arc sources. The key alone
    // cannot select one of those regions, even with a forged cached anchor.
    pg=page(Json::array({edge("a",-4.9,-1,-4.9,1,-9.9),edge("b",10.9,1,10.9,-1,-9.9),
        edge("c",2,9.9,4,9.9,-9.9)}),"a|b|c");
    pg["assignments"]["a|b|c"]["_anchor"]={{"x",3},{"y",2}};
    result=admit_pinc_geometry(modern(pg),base.snapshot(),contexts(),"ambiguous-import");
    require(result.pages[0].graph.faces.size()>1 && !result.pages[0].assignments[0].face_index &&
        result.pages[0].assignments[0].code=="assignment_ambiguous_source_faces",
        "ambiguous source key or cached anchor chose an analytical face");
}
void context_namespace_and_budget_refusals() {
    auto source=modern();auto base=Document::create(base_entities());auto mapping=contexts();
    mapping[0].interior=mapping[0].calculation;
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),mapping,"bad");});
    mapping=contexts();mapping[0].calculation.building_id="missing";
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),mapping,"bad");});
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),{},"bad");});
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"bad/namespace");});
    require(admit_pinc_geometry(source,base.snapshot(),contexts(),std::string(101,'n')).entities.size()==4,
        "maximum safe namespace refused");
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),std::string(102,'n'));});
    auto values=base_entities();values.push_back({"collision:p0:calc:0:segment","property",Json::object()});
    auto occupied=Document::create(values);
    rejects([&]{(void)admit_pinc_geometry(source,occupied.snapshot(),contexts(),"collision");});
    auto forged=source;forged.pages[0].calculation_segments[0].source.identity="forged";
    rejects([&]{(void)admit_pinc_geometry(forged,base.snapshot(),contexts(),"bad");});
    PincGeometryAdmissionLimits limits;limits.max_correspondence_work=0;
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"limited",limits);});
    limits={};limits.max_graph_edges=3;
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"limited",limits);});
    limits={};limits.source.max_calculation_pairs=5;
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"limited",limits);});
    limits={};limits.source.max_calculation_pairs=12;limits.max_graph_edges=8;limits.max_face_edge_uses=8;
    require(admit_pinc_geometry(source,base.snapshot(),contexts(),"exact-budget",limits).pages[0].assignments[0].face_index.has_value(),
        "exact native graph budget boundary refused");
    limits.max_face_edge_uses=7;rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"limited",limits);});
    limits={};limits.source.max_calculation_pairs=11;
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"limited",limits);});
    limits={};limits.max_correspondence_work=250001;
    rejects([&]{(void)admit_pinc_geometry(source,base.snapshot(),contexts(),"limited",limits);});
}
void admitted_arc_remains_editable_through_typed_document_json() {
    auto pg=page();pg["calcWalls"][0]=edge("a",0,0,8,0,2);const auto source=modern(pg);
    auto base=Document::create(base_entities());const auto admitted=admit_pinc_geometry(source,base.snapshot(),contexts(),"editable-import");
    auto values=base_entities();values.insert(values.end(),admitted.entities.begin(),admitted.entities.end());
    auto document=Document::create(values);const auto& imported=admitted.source_mappings[0];
    const auto original=decode_measurement_linework_model(document.snapshot().entities().at(imported.stroke_id).properties.at("model"));
    BoundaryGeometryEdit intent;intent.boundary_id=imported.stroke_id;intent.kind=BoundaryGeometryEditKind::move_vertex;
    intent.target_id=imported.end_vertex_id;intent.target_position={3,0};
    const auto expected=edited_measurement_linework(*original.model,intent);
    ApplyBoundaryConstraintChanges edit;edit.expected_revision=document.revision();edit.message="Edit imported analytical arc";
    edit.measured_source_completion=true;edit.measured_stroke_edits.push_back({imported.stroke_id,intent});
    // This is the actual Document command codec, including its retained typed
    // intent, not a replacement raw geometry payload or storage qualification.
    const auto wire=Json::parse(command_to_json(edit).dump());const auto command=command_from_json(wire);
    document.apply(command);
    const auto actual=decode_measurement_linework_model(document.snapshot().entities().at(imported.stroke_id).properties.at("model"));
    require(actual.supported() && encode_measurement_linework_model(*actual.model)==encode_measurement_linework_model(expected),
        "typed JSON command failed to preserve imported edit derivation");
    require(actual.model->edges[0].receipt==original.model->edges[0].receipt && actual.model->operations.size()==1,
        "editing rewrote imported authoring evidence");
    const auto replay=replay_measurement_linework(*actual.model);
    require_close(replay.edges[0].segment.end.x,3);require_close(replay.edges[0].segment.end.y,0);
    require_close(replay.edges[0].segment.sweep_radians,1.8545904360032244);
    document.undo(document.revision());require(document.snapshot().entities().at(imported.stroke_id).properties.at("model")==encode_measurement_linework_model(*original.model),
        "imported typed arc edit did not undo exactly");
    document.redo(document.revision());require(document.snapshot().entities().at(imported.stroke_id).properties.at("model")==encode_measurement_linework_model(expected),
        "imported typed arc edit did not redo exactly");
    // Revalidate retained history, then round-trip the native Document entity
    // command (including JSON receipt/derivation models) into a private document.
    // This exercises Document JSON without claiming the separate storage path.
    auto retained=Document::fork(document.snapshot());
    const auto retained_snapshot=retained.snapshot();
    ApplyEntityChanges capture{0,{}, {},"Round-trip admitted native entity models"};
    for(const auto& [id,e]:retained_snapshot.entities()){(void)id;capture.entity_changes.push_back(EntityChange::upsert(e));}
    auto reopened=Document::create();reopened.apply(command_from_json(Json::parse(command_to_json(capture).dump())));
    const auto restored=decode_measurement_linework_model(reopened.snapshot().entities().at(imported.stroke_id).properties.at("model"));
    require(restored.supported() && encode_measurement_linework_model(*restored.model)==encode_measurement_linework_model(expected),
        "native Document JSON lost editable imported receipt evidence");
    const auto restored_replay=replay_measurement_linework(*restored.model);
    require_close(restored_replay.edges[0].segment.end.x,3);
    require_close(restored_replay.edges[0].segment.sweep_radians,1.8545904360032244);
}
}
int main(){try{exact_strokes_contexts_and_detached_area_handles();shared_edge_and_page_occurrences();
    arcs_and_legacy_alias_orientation();legacy_attached_areas_and_partitions();independent_nested_gross_and_unmatched_evidence();split_source_junctions_and_ambiguous_keys();context_namespace_and_budget_refusals();
    admitted_arc_remains_editable_through_typed_document_json();
    std::cout<<"Pinc geometry admission tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
