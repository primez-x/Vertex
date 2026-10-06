#include "sketch/pinc_measurement_admission.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/measurement_linework_source.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void close(double actual,double expected){require(std::abs(actual-expected)<1e-10,"analytical area changed");}
template<class F>void rejects(F operation){try{operation();}catch(const std::invalid_argument&){return;}throw std::runtime_error("unsafe content accepted");}
Json edge(std::string id,double ax,double ay,double bx,double by,double h=0) {
    return {{"id",id},{"a",{{"x",ax},{"y",ay}}},{"b",{{"x",bx},{"y",by}}},{"kind",h?"arc":"line"},{"bulge",h}};
}
Json square(std::string prefix="",double l=0,double b=0,double size=8) {
    return Json::array({edge(prefix+"a",l,b,l+size,b),edge(prefix+"b",l+size,b,l+size,b+size),
        edge(prefix+"c",l+size,b+size,l,b+size),edge(prefix+"d",l,b+size,l,b)});
}
Json page(Json edges=square(),std::string key="a|b|c|d") {
    return {{"calcWalls",edges},{"interiorWalls",Json::array()},
        {"assignments",{{key,{{"code","GLA1"},{"name","Original descriptive name"},{"_area",999}}}}}};
}
PincImportProject parse(const Json& source) {
    const auto bytes=source.dump();return parse_pinc_project(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
}
PincImportProject modern(Json pg=page()){return parse({{"format","PincSketch"},{"version","4.2"},{"pages",{pg}}});}
std::vector<Entity> base_entities() {
    return {{"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}},{"floor","floor",{{"building_id","building"}}},
        {"calc","layer",{{"floor_id","floor"}}},{"interior","layer",{{"floor_id","floor"}}}};
}
std::vector<PincPageGeometryContext> contexts(){return {{0,{"property","building","floor","calc"},{"property","building","floor","interior"}}};}
std::vector<PincAreaReview> reviews(const PincImportProject& source) {
    std::vector<PincAreaReview> result;
    for(std::size_t p=0;p<source.pages.size();++p)for(std::size_t a=0;a<source.pages[p].assignments.size();++a)
        result.push_back({p,a,true,source.pages[p].assignments[a].code=="GAR"?"garage":"first_floor"});
    return result;
}
ApplyEntityChanges command(const DocumentSnapshot& base,const PincMeasurementAdmission& content) {
    ApplyEntityChanges result{base.revision(),{}, {},"Import reviewed Pinc content"};
    for(const auto& e:content.entities)result.entity_changes.push_back(EntityChange::upsert(e));
    return result;
}
void known_area_name_native_lineage_and_one_history_entry() {
    auto source=modern();auto base=Document::create(base_entities());const auto before=base.snapshot();
    const auto result=prepare_pinc_measurement_admission(source,before,contexts(),"content",reviews(source));
    require(result.entities.size()==5 && result.area_mappings.size()==1,"known assignment not authored once");
    require(base.snapshot().entities()==before.entities() && base.revision()==before.revision() &&
        base.snapshot().history().size()==before.history().size(),"private preparation mutated base");
    auto document=Document::fork(before);const auto wire=command_to_json(command(before,result));
    document.apply(command_from_json(Json::parse(wire.dump())));const auto imported=document.snapshot();
    require(imported.history().size()==before.history().size()+1,"content required multiple active commands");
    const auto& area=imported.entities().at(result.area_mappings[0].area_id);
    require(area.properties.at("name")=="Original descriptive name" && area.properties.at("classification")=="first_floor",
        "source name or explicit native classification lost");
    close(signed_area(boundary_geometry(decode_identified_boundary_entity(area))),5.94579456);
    require(area.extensions.contains("measurement_linework_sources") && !area.properties.contains("appraisal_facts") &&
        !area.properties.contains("appraisal_category") && !area.properties.contains("deduction_ids") &&
        !area.properties.contains("wall_measurement_source"),"content invented appraisal/deduction/physical authority");
    const auto checks=measurement_linework_source_checks(imported.entities());
    require(checks.at(area.id).current,"native area has stale or fabricated lineage");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"one import did not undo exactly");
    document.redo(document.revision());require(document.snapshot().entities()==imported.entities(),"one import did not redo exactly");
}
void shared_split_curved_and_nested_gross() {
    auto pg=page(Json::array({edge("top",0,0,16,0),edge("bottom",16,8,0,8),edge("left",0,8,0,0),
        edge("right",16,0,16,8),edge("divider",8,0,8,8)}),"bottom|divider|left|top");
    pg["assignments"]["bottom|divider|right|top"]={{"code","GAR"},{"name","Garage"}};
    auto source=modern(pg);auto base=Document::create(base_entities());
    auto result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"split",reviews(source));
    require(result.area_mappings.size()==2 && result.entities.size()==7,"shared source pieces lost area ownership");
    auto preview=Document::preview_command(base.snapshot(),command(base.snapshot(),result));
    for(const auto& m:result.area_mappings)close(signed_area(boundary_geometry(decode_identified_boundary_entity(preview.entities().at(m.area_id)))),5.94579456);
    pg=page();pg["calcWalls"][0]=edge("a",0,0,8,0,2);source=modern(pg);
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"curve",reviews(source));
    require(result.area_mappings.size()==1,"curved cycle lost correspondence");
    const auto& curved=result.entities.back();
    close(signed_area(boundary_geometry(decode_identified_boundary_entity(curved))),result.geometry.pages[0].graph.faces[0].area_square_metres);
    pg=page();for(const auto& e:square("i",2,2,4))pg["calcWalls"].push_back(e);
    pg["assignments"]["ia|ib|ic|id"]={{"code","GAR"}};source=modern(pg);
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"nested",reviews(source));
    require(result.area_mappings.size()==2,"nested independent gross areas lost");
    double total=0;for(const auto& e:result.entities)if(e.type=="measurement_boundary") {
        require(!e.properties.contains("deduction_ids"),"containment inferred a deduction");
        total+=signed_area(boundary_geometry(decode_identified_boundary_entity(e)));
    }
    close(total,7.4322432);
}
void legacy_aliases_ambiguity_unknown_unmatched_and_decline() {
    auto source=parse({{"version","2.0"},{"areas",{{{"id",1},{"code","GLA1"},{"name","Legacy"},{"segments",square("h")}},
        {{"id",2},{"code","GAR"},{"segments",Json::array({edge("ga",8,2,16,2),edge("gb",16,2,16,6),edge("gc",16,6,8,6),edge("gd",8,6,8,2)})}}}}});
    auto base=Document::create(base_entities());auto result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"legacy",reviews(source));
    require(result.area_mappings.size()==2,"legacy split intervals failed content admission");
    auto reversed=square("x");for(auto& e:reversed)std::swap(e["a"],e["b"]);
    source=parse({{"version","2.0"},{"areas",{{{"id",1},{"code","GLA1"},{"segments",square()}},{{"id",2},{"code","GAR"},{"segments",reversed}}}}});
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"duplicate",reviews(source));
    require(result.area_mappings.empty() && result.geometry.source_mappings.size()==8 && !result.diagnostics.empty(),"ambiguous legacy owner silently imported");
    auto pg=page();pg["assignments"]["a|b|c|d"]["code"]="UNRECOGNIZED";source=modern(pg);
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"unknown",reviews(source));
    require(result.area_mappings.empty() && !result.diagnostics.empty(),"unknown source category silently imported");
    pg["assignments"]["a|b|c|d"]["code"]="UND";source=modern(pg);auto undefined_review=reviews(source);
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"undefined-reference",undefined_review);
    require(result.area_mappings.empty(),"undefined category received an implicit area classification");
    undefined_review[0].classification="non_calculated";
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"undefined-reviewed",undefined_review);
    require(result.area_mappings.size()==1,"explicit non-calculated undefined area was refused");
    source=modern();auto review=reviews(source);review[0].import_area=false;review[0].classification.clear();
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"declined",review);
    require(result.area_mappings.empty() && result.entities.size()==4 && !result.diagnostics.empty(),"declined review claimed area import");
    pg=page();pg["calcWalls"][0]["a"]["x"]=.0001;source=modern(pg);
    result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"gap",reviews(source));
    require(result.area_mappings.empty() && !result.diagnostics.empty(),"unmatched gap borrowed cached area authority");
    pg=page(Json::array({edge("a",-4.9,-1,-4.9,1,-9.9),edge("b",10.9,1,10.9,-1,-9.9),edge("c",2,9.9,4,9.9,-9.9)}),"a|b|c");
    source=modern(pg);result=prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"ambiguous",reviews(source));
    require(result.area_mappings.empty(),"ambiguous analytic source membership selected a face");
}
void hostile_reviews_workflows_and_aggregate_limits() {
    const auto source=modern();auto base=Document::create(base_entities());const auto review=reviews(source);
    rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"bad",{});});
    auto bad=review;bad.push_back(bad[0]);rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"bad",bad);});
    bad=review;bad[0].assignment_index=1;rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"bad",bad);});
    for(const auto* classification:{""," ","role:other_void","gla","first_floor ","invented"}) {
        bad=review;bad[0].classification=classification;
        rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"bad",bad);});
    }
    auto values=base_entities();values[0].properties["calculation_workflow"]="appraisal";auto appraisal=Document::create(values);
    rejects([&]{(void)prepare_pinc_measurement_admission(source,appraisal.snapshot(),contexts(),"bad",review);});
    PincGeometryAdmissionLimits limit;limit.source.max_calculation_pairs=12;
    rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"budget",review,limit);});
    limit={};limit.max_correspondence_work=10;rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"budget",review,limit);});
    limit={};limit.source.max_string_bytes=8;rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"budget",review,limit);});
    limit={};limit.max_face_edge_uses=7;rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"budget",review,limit);});
    limit={};limit.max_graph_edges=65'537;rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"budget",review,limit);});
    auto forged=source;forged.pages[0].assignments.push_back(forged.pages[0].assignments[0]);
    rejects([&]{(void)prepare_pinc_measurement_admission(forged,base.snapshot(),contexts(),"bad",reviews(forged));});
    forged=source;forged.pages[0].assignments[0].name=std::string("name\0injection",14);
    rejects([&]{(void)prepare_pinc_measurement_admission(forged,base.snapshot(),contexts(),"bad",review);});
    bad=review;bad[0].page_index=1;rejects([&]{(void)prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"bad",bad);});
    bad=review;bad[0].classification="non_calculated";
    require(prepare_pinc_measurement_admission(source,base.snapshot(),contexts(),"noncalculated",bad).area_mappings.size()==1,
        "explicit descriptive non-calculated classification refused");
}
void separate_pages_on_one_floor_preserve_scaffold() {
    const auto source=parse({{"format","PincSketch"},{"version","4.2"},{"pages",{page(),page()}}});
    auto values=base_entities();values.push_back({"calc2","layer",{{"floor_id","floor"}}});
    values.push_back({"interior2","layer",{{"floor_id","floor"}}});
    values.push_back({"untouched","property",{{"name","Keep scaffold"}}});
    auto base=Document::create(values);auto reviewed=contexts();
    reviewed.push_back({1,{"property","building","floor","calc2"},{"property","building","floor","interior2"}});
    const auto result=prepare_pinc_measurement_admission(source,base.snapshot(),reviewed,"separate-pages",reviews(source));
    require(result.area_mappings.size()==2 && result.entities.size()==10,"same-floor pages merged or lost area identities");
    const auto preview=Document::preview_command(base.snapshot(),command(base.snapshot(),result));
    require(preview.entities().at("untouched")==base.snapshot().entities().at("untouched"),"unrelated scaffold changed");
    require(preview.entities().at(result.area_mappings[0].area_id).properties.at("layer_id")!=
        preview.entities().at(result.area_mappings[1].area_id).properties.at("layer_id"),"same-floor pages merged calculation contexts");
}
}
int main(){try{known_area_name_native_lineage_and_one_history_entry();shared_split_curved_and_nested_gross();
    legacy_aliases_ambiguity_unknown_unmatched_and_decline();hostile_reviews_workflows_and_aggregate_limits();separate_pages_on_one_floor_preserve_scaffold();
    std::cout<<"Pinc measurement admission tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
