#include "sketch/measurement_area_definition.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/calculations.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& action, const char* message) {
    bool rejected=false; try { action(); } catch(const std::exception&) { rejected=true; }
    require(rejected,message);
}
Entity stroke(std::string id, double lo, double hi) {
    const std::vector<Vec2> points{{lo,lo},{hi,lo},{hi,hi},{lo,hi},{lo,lo}};
    MeasurementLinework model; model.stroke_id=id; model.anchor=points.front(); model.closed=true;
    for(std::size_t i=1;i<points.size();++i) {
        ConstructionReceipt receipt; receipt.segment_id=id+":e"+std::to_string(i);
        receipt.kind=BoundaryConstructionKind::line_to_point;receipt.start=points[i-1];receipt.chord_end=points[i];
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(i-1),
            i==4?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},
        {"floor_id","f"},{"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
}
Document fixture(bool inner=true,bool appraisal=false) {
    std::vector<Entity> values{{"p","property",Json::object()},
        {"b","building",{{"property_id","p"}}},{"f","floor",{{"building_id","b"}}},
        {"l","layer",{{"floor_id","f"}}},stroke("outer",0,10)};
    if(inner)values.push_back(stroke("inner",3,7));
    if(appraisal) {
        values[0].properties={{"calculation_workflow","appraisal"},{"appraisal_policy",{
            {"policy_kind","residential_declared"},{"version",1},
            {"property_kind","detached_single_family"},{"measurement_basis","exterior"}}}};
        values[2].properties["appraisal_facts"]={{"grade","above"}};
    }
    return Document::create(std::move(values));
}
std::size_t outer_index(const DetectedMeasurementAreas& result) {
    for(std::size_t i=0;i<result.graph.faces.size();++i)if(!result.graph.faces[i].parent_face_index)return i;
    throw std::runtime_error("outer outline missing");
}
std::vector<MeasurementAreaChoice> decisions(const DetectedMeasurementAreas& result, bool deduct,
    std::string root="living",std::string inner="garage") {
    std::vector<MeasurementAreaChoice> choices(result.graph.faces.size());
    choices[outer_index(result)]={MeasurementAreaDisposition::define_area,std::move(root)};
    if(deduct)for(std::size_t i=0;i<choices.size();++i)if(result.graph.faces[i].parent_face_index)
        choices[i]={MeasurementAreaDisposition::deduct_from_parent,inner};
    return choices;
}
template<class Choice> Choice combined_choice(std::size_t group,const std::string& classification) {
    Choice choice{MeasurementAreaDisposition::define_area,classification};
    if constexpr(requires { choice.combine_group=group; })choice.combine_group=group;
    else throw std::runtime_error("detected regions must support a single classified combined area");
    return choice;
}
void combine_adjacent_detected_areas() {
    auto document=fixture(false);auto divider=stroke("divider",0,10);
    MeasurementLinework model;model.stroke_id="divider";model.anchor={5,0};
    ConstructionReceipt input;input.segment_id="divider:side";input.kind=BoundaryConstructionKind::line_to_point;
    input.start={5,0};input.chord_end={5,10};model.edges.push_back({input.segment_id,"divider:a","divider:b",input});
    divider.properties["model"]=encode_measurement_linework_model(model);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(divider)},{},"Measured separator"});
    const auto source=document.snapshot();const auto detected=detect_measurement_areas(source,"outer");
    require(detected.graph.faces.size()==2,"separator creates two real adjacent detected regions");
    std::vector<MeasurementAreaChoice> choices;
    for(std::size_t i=0;i<detected.graph.faces.size();++i)choices.push_back(combined_choice<MeasurementAreaChoice>(7,"living"));
    const auto definition=prepare_measurement_area_definition(source,"outer",choices);
    require(definition.area_ids.size()==1 && definition.command.entity_changes.size()==1,"combined regions publish one classified area in one command");
    const auto preview=Document::preview_command(source,definition.command);
    const auto& area=preview.entities().at(definition.area_ids.front());
    const auto boundary=boundary_geometry(decode_identified_boundary_entity(area));
    require(std::abs(signed_area(boundary)-100)<1e-10 && std::abs(perimeter(boundary)-40)<1e-10,
        "combination preserves analytical area and removes the shared separator from its perimeter");
    require(area.extensions.contains("measurement_linework_group") &&
        area.extensions.at("measurement_linework_group").at("members").size()==2,"all member source lineages persist including the cancelled separator");
    require(measurement_linework_source_current(measurement_linework_source_checks(preview.entities()),area),
        "combined region is current against the complete measured graph");
    require(definition.face_area_ids.size()==2 && definition.face_area_ids[0]==definition.area_ids.front() &&
        definition.face_area_ids[1]==definition.area_ids.front(),"combined preview rows resolve the same retained owner");
    BoundaryGeometryEdit replacement;replacement.kind=BoundaryGeometryEditKind::redefine_boundary;
    replacement.boundary_id=area.id;replacement.target_id=area.id;replacement.fresh_topology=true;
    auto new_topology=decode_identified_boundary_entity(area);
    for(std::size_t i=0;i<new_topology.segments.size();++i) {
        auto& edge=new_topology.segments[i];edge.segment_id=area.id+":replacement:e"+std::to_string(i);
        edge.start_vertex_id=area.id+":replacement:v"+std::to_string(i);
        edge.end_vertex_id=area.id+":replacement:v"+std::to_string((i+1)%new_topology.segments.size());
    }
    replacement.replacement_segments=encode_identified_boundary_entity(new_topology).properties.at("segments");
    replacement.replacement_linework_sources=area.extensions.at("measurement_linework_sources");
    bool complete_group_refused=false;
    try{(void)Document::preview_command(preview,EditBoundaryGeometry{preview.revision(),replacement});}
    catch(const std::exception& error){complete_group_refused=std::string(error.what()).find("complete group refresh")!=std::string::npos;}
    require(complete_group_refused,"a single-face replacement cannot silently discard combined membership provenance");
    require(document.snapshot().entities()==source.entities(),"combination preview preserves the source document");
    document.apply(definition.command);const auto committed=document.snapshot();
    require(ProjectStore::required_format_version(committed)==33,"combined semantics require a reader that understands all member sources");
    document.undo(document.revision());require(document.snapshot().entities()==source.entities(),"one Undo removes the complete combined definition");
    require(ProjectStore::required_format_version(document.snapshot())==33,"retained combined-area history keeps its reader floor after Undo");
    document.redo(document.revision());require(document.snapshot().entities()==committed.entities(),"Redo restores exact combined identities and source metadata");
    const auto repeated=prepare_measurement_area_definition(document.snapshot(),"outer",choices);
    require(repeated.command.entity_changes.empty() && repeated.area_ids==definition.area_ids,"re-detection preserves the existing combined identity without duplicating area totals");
    std::vector<MeasurementAreaChoice> keep(choices.size());
    const auto kept=prepare_measurement_area_definition(document.snapshot(),"outer",keep);
    require(kept.command.entity_changes.empty() && kept.area_ids.empty() && kept.face_area_ids==definition.face_area_ids,
        "reference-only re-detection retains the whole combined definition and its preview rows");
    auto partial=choices;partial.back()={};
    rejects([&]{(void)prepare_measurement_area_definition(document.snapshot(),"outer",partial);},
        "partial member choices cannot break a retained combined area");
    auto single_definitions=choices;for(auto& choice:single_definitions)choice.combine_group.reset();
    auto individually_defined=Document::fork(source);individually_defined.apply(prepare_measurement_area_definition(source,"outer",single_definitions).command);
    rejects([&]{(void)prepare_measurement_area_definition(individually_defined.snapshot(),"outer",choices);},
        "existing individual definitions must not be implicitly consumed by a new combined owner");
    auto inconsistent=choices;inconsistent.back().classification="garage";
    rejects([&]{(void)prepare_measurement_area_definition(source,"outer",inconsistent);},"a new combined group cannot silently mix classifications");
    // An orthogonal separator forms four real cells, not four duplicate
    // outlines. Combining all of them removes both interior separators.
    auto crossing=divider;crossing.id="horizontal";model.stroke_id=crossing.id;model.anchor={0,5};model.edges.clear();
    input.segment_id="horizontal:side";input.start={0,5};input.chord_end={10,5};
    model.edges.push_back({input.segment_id,"horizontal:a","horizontal:b",input});
    crossing.properties["model"]=encode_measurement_linework_model(model);
    document=Document::fork(source);document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(crossing)},{},"Crossing separator"});
    const auto four=detect_measurement_areas(document.snapshot(),"outer");
    require(four.graph.faces.size()==4,"two separators create four detected regions");
    choices.clear();for(std::size_t i=0;i<4;++i)choices.push_back(combined_choice<MeasurementAreaChoice>(11,"living"));
    const auto combined_four=prepare_measurement_area_definition(document.snapshot(),"outer",choices);
    const auto four_preview=Document::preview_command(document.snapshot(),combined_four.command);
    const auto& four_area=four_preview.entities().at(combined_four.area_ids.front());
    require(combined_four.area_ids.size()==1 && four_area.extensions.at("measurement_linework_group").at("members").size()==4 &&
        std::abs(perimeter(boundary_geometry(decode_identified_boundary_entity(four_area)))-40)<1e-10 &&
        measurement_linework_source_current(measurement_linework_source_checks(four_preview.entities()),four_area),
        "four cells produce one current analytical exterior with all original member sources");
}
void reference_and_deduction() {
    auto document=fixture();const auto before=document.snapshot();
    const auto detected=detect_measurement_areas(before,"outer");
    require(detected.graph.faces.size()==2,"nested closed measured outlines are inspectable");
    const auto reference=prepare_measurement_area_definition(before,"outer",decisions(detected,false));
    require(reference.area_ids.size()==1 && reference.command.entity_changes.size()==1,"reference inner outline creates no area");
    const auto reference_preview=Document::preview_command(before,reference.command);
    require(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(
        reference_preview.entities().at(reference.area_ids[0]))))-100)<1e-10,"reference decision preserves gross100");
    const auto defined=prepare_measurement_area_definition(before,"outer",decisions(detected,true));
    require(defined.area_ids.size()==2,"explicit subtraction retains two identified boundaries");
    const auto preview=Document::preview_command(before,defined.command);
    const auto root=defined.area_ids[outer_index(detected)];
    const auto& parent=preview.entities().at(root);
    require(parent.properties.at("deduction_ids").size()==1,"explicit deduction is linked to its parent");
    const auto child=parent.properties.at("deduction_ids").at(0).get<std::string>();
    CalculationProfile physical{"physical",1,AreaUnit::square_metre,2,{{"living",{true,true}},{"garage",{true,false}}}};
    const auto trace=calculate_area({root,"b","f","living",boundary_geometry(decode_identified_boundary_entity(parent)),
        {{child,boundary_geometry(decode_identified_boundary_entity(preview.entities().at(child)))}},{1,1}},physical);
    require(std::abs(trace.net_square_metres-84)<1e-10,"explicit100minus16 is exactly84");
    require(document.snapshot().entities()==before.entities(),"previews never modify source document");
    document.apply(defined.command);const auto committed=document.snapshot();
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"oneundo restores source strokes only");
    document.redo(document.revision());require(document.snapshot().entities()==committed.entities(),"redo restores boundaries and links");
    for(const auto& [id,check]:measurement_linework_source_checks(committed.entities()))
        require(check.current,"nested areas retain current source lineage");
    const auto repeated=prepare_measurement_area_definition(committed,"outer",decisions(detected,true));
    require(repeated.command.entity_changes.empty() && repeated.area_ids==defined.area_ids,"re-detection reuses exact IDs and deduction links");
    const auto path=std::filesystem::temp_directory_path()/("vertex-nested-"+make_stable_id()+".bldproj");
    (void)ProjectStore::save(path,document.snapshot());auto reopened=ProjectStore::load(path);std::filesystem::remove(path);
    require(reopened.document.snapshot().entities()==committed.entities(),"native save/reopen preserves exact boundaries and deduction links");
}
void previous_outer_remains_current() {
    auto document=fixture(false);const auto first=detect_measurement_areas(document.snapshot(),"outer");
    const auto initial=prepare_measurement_area_definition(document.snapshot(),"outer",decisions(first,false));
    document.apply(initial.command);auto outer=document.snapshot().entities().at(initial.area_ids[0]);
    outer.properties["name"]="Keep my name";outer.extensions["vendor"]={{"style",17}};
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(outer),EntityChange::upsert(stroke("inner",3,7))},{},"Add inner outline"});
    const auto source=document.snapshot();
    require(measurement_linework_source_checks(source.entities()).at(outer.id).current,"adding inner loop does not stale unchanged outer source");
    const auto detected=detect_measurement_areas(source,"inner");
    require(detected.existing_area_ids[outer_index(detected)]==outer.id,"existing outer definition assigned to exact outline");
    const auto proposed=prepare_measurement_area_definition(source,"inner",decisions(detected,true));
    const auto preview=Document::preview_command(source,proposed.command);
    auto expected=outer;expected.properties["deduction_ids"]=preview.entities().at(outer.id).properties.at("deduction_ids");
    require(preview.entities().at(outer.id)==expected,"deduction admission preserves existing IDs/style/name/facts");
    auto rotated=decode_identified_boundary_entity(outer);
    std::rotate(rotated.segments.begin(),rotated.segments.begin()+1,rotated.segments.end());
    auto rotated_entity=encode_identified_boundary_entity(rotated,&outer);
    auto& lineage=rotated_entity.extensions["measurement_linework_sources"];
    std::rotate(lineage.begin(),lineage.begin()+1,lineage.end());
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(rotated_entity)},{},"Cyclic ordering"});
    require(detect_measurement_areas(document.snapshot(),"outer").existing_area_ids[outer_index(detected)]==outer.id,"cyclic existing edge order reuses identity");
}
void invalid_and_appraisal() {
    auto document=fixture();const auto source=document.snapshot();const auto detected=detect_measurement_areas(source,"outer");
    rejects([&]{(void)prepare_measurement_area_definition(source,"outer",{});},"wrong choices count rejected");
    auto choices=decisions(detected,true,"living","living");
    rejects([&]{(void)prepare_measurement_area_definition(source,"outer",choices);},"sameTYPE deduction rejected");
    choices[outer_index(detected)].disposition=MeasurementAreaDisposition::reference_only;
    rejects([&]{(void)prepare_measurement_area_definition(source,"outer",choices);},"undefined parent cannot receive deduction");
    choices=decisions(detected,false);choices[outer_index(detected)].disposition=MeasurementAreaDisposition::deduct_from_parent;
    rejects([&]{(void)prepare_measurement_area_definition(source,"outer",choices);},"root cannot deduct without parent");
    choices.assign(2,{});require(prepare_measurement_area_definition(source,"outer",choices).command.entity_changes.empty(),"allreference review is no-op");
    const auto command=prepare_measurement_area_definition(source,"outer",decisions(detected,true)).command;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(stroke("other",12,14))},{},"Other work"});
    rejects([&]{document.apply(command);},"stale exact preview rejected atomically");

    auto app=fixture(true,true);const auto graph=detect_measurement_areas(app.snapshot(),"outer");
    const auto proposed=prepare_measurement_area_definition(app.snapshot(),"outer",decisions(graph,true,"above_grade_finished","role:other_void"));
    app.apply(proposed.command);auto snapshot=app.snapshot();
    const auto root=proposed.area_ids[outer_index(graph)];
    const auto child=snapshot.entities().at(root).properties.at("deduction_ids").at(0).get<std::string>();
    require(snapshot.entities().at(child).properties.at("appraisal_facts").at("boundary_role")=="other_void","explicit void choice records only declared role");
    auto report=build_appraisal_document_report(snapshot,"p",AreaUnit::square_metre);
    require(!report.qualified && !report.calculation,"definition does not invent missing GLA facts");
    auto parent=snapshot.entities().at(root);parent.properties["appraisal_facts"]={{"finish","finished"},{"access","direct_interior"},
        {"ceiling_eligibility","standard"},{"area_use","dwelling"},{"boundary_role","measured_area"}};
    app.apply(ApplyEntityChanges{app.revision(),{EntityChange::upsert(parent)},{},"Declare observed facts"});
    report=build_appraisal_document_report(app.snapshot(),"p",AreaUnit::square_metre);
    require(report.qualified && report.calculation && std::abs(report.calculation->property.gla().total.square_metres-84)<1e-10,"declared appraisal GLA excludes16 once");

    // A separately classified garage fills the deducted footprint once.
    auto garage=app.snapshot().entities().at(child);
    garage.properties["appraisal_facts"]={{"area_use","garage"},{"boundary_role","measured_area"},
        {"finish","finished"},{"access","direct_interior"},{"ceiling_eligibility","standard"}};
    garage.properties["appraisal_category"]="garage";
    app.apply(ApplyEntityChanges{app.revision(),{EntityChange::upsert(garage)},{},"Declare garage use"});
    report=build_appraisal_document_report(app.snapshot(),"p",AreaUnit::square_metre);
    require(report.qualified && report.calculation && std::abs(report.calculation->property.gla().total.square_metres-84)<1e-10 &&
        std::abs(report.calculation->property.by_category.at(AppraisalAreaCategory::garage).total.square_metres-16)<1e-10,
        "garage16 and GLA84 contribute once without overlapping totals");

    auto moved=stroke("inner",4,8);
    app.apply(ApplyEntityChanges{app.revision(),{EntityChange::upsert(moved)},{},"Move inner source"});
    const auto current=measurement_linework_source_checks(app.snapshot().entities());
    require(current.at(root).current && !current.at(child).current,"inner movement preserves outer gross source but invalidates deduction geometry");
    report=build_appraisal_document_report(app.snapshot(),"p",AreaUnit::square_metre);
    require(!report.qualified && !report.calculation,"stale inner source withholds parent GLA despite unchanged gross boundary");
    rejects([&]{(void)detect_measurement_areas(app.snapshot(),"outer");},"stale area needs refresh rather than duplicate definition");
}
void deeper_and_sibling_decisions() {
    auto document=fixture();
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(stroke("deep",4,6))},{},"Add third outline"});
    const auto source=document.snapshot();const auto detected=detect_measurement_areas(source,"outer");
    require(detected.graph.faces.size()==3,"deeper outlines are detectable");
    auto choices=decisions(detected,true,"living","garage");
    rejects([&]{(void)prepare_measurement_area_definition(source,"outer",choices);},"nested deduction chains fail with no commit");
    for(std::size_t i=0;i<choices.size();++i)if(detected.graph.faces[i].area_square_metres==4)
        choices[i]={MeasurementAreaDisposition::reference_only,{}};
    const auto proposed=prepare_measurement_area_definition(source,"outer",choices);
    require(proposed.area_ids.size()==2,"deep reference does not block explicit parent deduction");
    require(document.snapshot().entities()==source.entities(),"rejected and prepared nested decisions never mutate source");

    auto sibling=fixture();
    auto second=stroke("second",1,2);
    sibling.apply(ApplyEntityChanges{sibling.revision(),{EntityChange::upsert(second)},{},"Add sibling outline"});
    const auto sibling_graph=detect_measurement_areas(sibling.snapshot(),"outer");
    const auto siblings=prepare_measurement_area_definition(sibling.snapshot(),"outer",decisions(sibling_graph,true));
    const auto preview=Document::preview_command(sibling.snapshot(),siblings.command);
    const auto root=siblings.area_ids[outer_index(sibling_graph)];
    const auto& parent=preview.entities().at(root);
    require(parent.properties.at("deduction_ids").size()==2,"multiple siblings are combined in one parent transaction");
    std::vector<AreaDeduction> tools;
    for(const auto& id:parent.properties.at("deduction_ids")) {
        const auto value=id.get<std::string>();tools.push_back({value,boundary_geometry(decode_identified_boundary_entity(preview.entities().at(value)))});
    }
    const CalculationProfile physical{"physical",1,AreaUnit::square_metre,2,{{"living",{true,true}}}};
    const auto trace=calculate_area({root,"b","f","living",boundary_geometry(decode_identified_boundary_entity(parent)),tools,{1,1}},physical);
    require(std::abs(trace.net_square_metres-83)<1e-10,"both explicit sibling deductions count once:100minus16minus1");
}
void active_phase_sources() {
    auto document=fixture();
    auto phases=ModelPhases::create({"outer","inner"},{"outer"},{{"future","Future",{}, {"inner"}}});
    Entity registry{"phases","model_phases",{{"model",phases.to_json()}}};
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(registry)},{},"Add future alternative"});
    const auto baseline=detect_measurement_areas(document.snapshot(),"outer");
    require(baseline.graph.faces.size()==1,"inactive proposed inner source is excluded from baseline graph");
    rejects([&]{(void)detect_measurement_areas(document.snapshot(),"inner");},"inactive selected source cannot define active areas");
    phases=phases.with_active("future");registry.properties["model"]=phases.to_json();
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(registry)},{},"Select future"});
    const auto active=detect_measurement_areas(document.snapshot(),"outer");
    require(active.graph.faces.size()==2,"active alternative includes proposed inner source");
    const auto definition=prepare_measurement_area_definition(document.snapshot(),"outer",decisions(active,true));
    document.apply(definition.command);
    const auto root=definition.area_ids[outer_index(active)];
    const auto child=document.snapshot().entities().at(root).properties.at("deduction_ids").at(0).get<std::string>();
    // Hide an existing stale area with its proposed source. An inactive
    // definition must not block a valid active outer outline review.
    phases=ModelPhases::create({"outer","inner",child},{"outer"},{{"future","Future",{}, {"inner",child}}});
    registry.properties["model"]=phases.to_json();
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(registry)},{},"Return to baseline"});
    const auto returned=detect_measurement_areas(document.snapshot(),"outer");
    require(returned.graph.faces.size()==1 && returned.existing_area_ids[0]==root,"inactive stale child area does not block active detection");
    phases=ModelPhases::create({"outer","inner",child},{"outer","inner",child},
        {{"remove","Remove",{"inner",child},{}}},"remove");
    registry.properties["model"]=phases.to_json();
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(registry)},{},"Demolish inner source and area"});
    require(detect_measurement_areas(document.snapshot(),"outer").graph.faces.size()==1,"demolished nested source and identity remain excluded");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {combine_adjacent_detected_areas();reference_and_deduction();previous_outer_remains_current();invalid_and_appraisal();deeper_and_sibling_decisions();active_phase_sources();
        std::cout<<"Nested measurement area definition checks passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
