#include "sketch/appraisal_area_partition.hpp"
#include "sketch/area_subtraction.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/calculations.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> void refuses(const DocumentSnapshot& source,F&& action,const char* message) {
    const auto before=source.entities();
    bool rejected=false;
    try { action(); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,message);
    require(source.entities()==before,"refused partition preparation never changes the candidate");
}
Json square(double lo,double hi) {
    return Json::array({{{"start",{lo,lo}},{"end",{hi,lo}},{"sweep_radians",0}},
        {{"start",{hi,lo}},{"end",{hi,hi}},{"sweep_radians",0}},
        {{"start",{hi,hi}},{"end",{lo,hi}},{"sweep_radians",0}},
        {{"start",{lo,hi}},{"end",{lo,lo}},{"sweep_radians",0}}});
}
Entity area(std::string id,double lo,double hi) {
    return upgrade_legacy_boundary_entity({id,"measurement_boundary",{{"property_id","p"},{"building_id","b"},
        {"floor_id","f"},{"layer_id","l"},{"boundary",square(lo,hi)},{"appraisal_category","above_grade_finished"},
        {"classification","measurement"},{"name","Keep my name"},{"factor",1}},false,{{"vendor",{{"style",17}}}}});
}
std::vector<Entity> fixture() {
    return {{"p","property",{{"calculation_workflow","appraisal"},{"appraisal_policy",{
        {"policy_kind","ansi_z765_2021"},{"version",1}}}}},
        {"b","building",{{"property_id","p"}}},{"f","floor",{{"building_id","b"}}},
        {"l","layer",{{"floor_id","f"}}},area("floor",0,10),area("room",2,8),area("void",4,6)};
}
Entity& find(std::vector<Entity>& entities,const std::string& id) {
    for(auto& entity:entities)if(entity.id==id)return entity;
    throw std::runtime_error("fixture identity missing");
}
void overlay_and_policy() {
    auto document=Document::create(fixture());const auto source=document.snapshot();
    require(ansi_appraisal_partition_context(source,"floor"),"minimal explicit ANSI policy supports geometry without invented observations");
    const auto proposed=prepare_ansi_appraisal_partition_targets(source,{{"floor",{"room"}},{"room",{"void"}}});
    require(proposed.size()==2,"whole-graph overlay returns every requested parent");
    for(std::size_t i=0;i<proposed.size();++i) {
        auto expected=source.entities().at(i==0?"floor":"room");
        expected.properties["deduction_ids"]=Json::array({i==0?"room":"void"});
        require(proposed[i]==expected,"preparation changes only deduction IDs and retains all identities geometry facts and metadata");
    }
    require(source.entities()==document.snapshot().entities(),"successful preparation is pure");
    for(const auto& version:std::vector<Json>{nullptr,"1",0,3,1.0}) {
        auto entities=fixture();entities.front().properties["appraisal_policy"]["version"]=version;
        const auto invalid=Document::create(entities).snapshot();
        refuses(invalid,[&]{(void)ansi_appraisal_partition_context(invalid,"floor");},"malformed or future ANSI version is refused by routing predicate");
        refuses(invalid,[&]{(void)prepare_ansi_appraisal_partition_targets(invalid,{{"floor",{"room"}}});},"malformed ANSI policy cannot bypass helper validation");
    }
    for(const auto& policy:std::vector<Json>{Json{{"policy_kind","ansi_z765_2021"}},
        Json{{"policy_kind","ansi_z765_2021"},{"version",1},{"ansi",{{"direct_measurement","yes"}}}},
        Json{{"policy_kind","ansi_z765_2021"},{"version",1},{"measurement_basis","invented"}},
        Json{{"policy_kind","ansi_z765_2021"},{"version",1},{"extra",true}}}) {
        auto entities=fixture();entities.front().properties["appraisal_policy"]=policy;
        const auto invalid=Document::create(entities).snapshot();
        refuses(invalid,[&]{(void)ansi_appraisal_partition_context(invalid,"floor");},"malformed ANSI policy declaration is refused");
    }
    auto entities=fixture();entities.front().properties["appraisal_policy"]["version"]=2;
    const auto v2=Document::create(entities).snapshot();
    require(ansi_appraisal_partition_context(v2,"room") &&
        prepare_ansi_appraisal_partition_targets(v2,{{"floor",{"room"}},{"room",{"void"}}}).size()==2,
        "ANSI v2 partition authoring also permits missing eligibility observations");
    entities.front().properties["appraisal_policy"]["policy_kind"]="residential_declared";
    entities.front().properties["appraisal_policy"]["version"]=1;
    const auto legacy=Document::create(entities).snapshot();
    require(!ansi_appraisal_partition_context(legacy,"floor"),"non-ANSI routing remains legacy");
    refuses(legacy,[&]{(void)prepare_area_subtraction_target(legacy,legacy.entities().at("room"),"floor");},
        "legacy Auto-Subtract still refuses same TYPE areas");
    refuses(legacy,[&]{(void)prepare_ansi_appraisal_partition_targets(legacy,{{"floor",{"room"}}});},
        "ANSI helper does not widen non-ANSI authoring semantics");
}
void graph_failures_and_repairs() {
    const auto source=Document::create(fixture()).snapshot();
    for(const auto& assignments:std::vector<std::vector<AppraisalPartitionAssignment>>{
        {{"floor",{"room"}},{"floor",{"void"}}},{{"floor",{"room","room"}}},{{"floor",{""}}},
        {{"floor",{"missing"}}},{{"floor",{"floor"}}},{{"floor",{"room"}},{"room",{"floor"}}},
        {{"room",{"floor"}}}})
        refuses(source,[&]{(void)prepare_ansi_appraisal_partition_targets(source,assignments);},
            "duplicate IDs unavailable geometry cycles and noncontained deductions refuse atomically");
    for(const auto* mismatch:{"floor","site","room","unsupported"}) {
        auto entities=fixture();auto& child=find(entities,"void");
        if(std::string(mismatch)=="floor") {
            entities.push_back({"other-floor","floor",{{"building_id","b"}}});
            // Vector growth invalidates child; find it again.
            auto& moved=find(entities,"void");moved.properties["floor_id"]="other-floor";moved.properties.erase("layer_id");
        } else if(std::string(mismatch)=="site")child.properties["calculation_scope"]="site";
        else if(std::string(mismatch)=="room")child.type="room_boundary";
        else child.properties["boundary_model_version"]=99;
        const auto invalid=Document::create(entities).snapshot();
        refuses(invalid,[&]{(void)prepare_ansi_appraisal_partition_targets(invalid,{{"floor",{"room"}},{"room",{"void"}}});},
            "deep descendant context site architectural room and unsupported geometry cannot be hidden behind a valid parent");
    }
    auto entities=fixture();find(entities,"floor").properties["deduction_ids"]={"missing"};
    const auto broken=Document::create(entities).snapshot();
    const auto repaired=prepare_ansi_appraisal_partition_targets(broken,{{"floor",{}}});
    auto expected=broken.entities().at("floor");expected.properties.erase("deduction_ids");
    require(repaired.size()==1 && repaired.front()==expected,"replacement-empty repairs an unavailable removed child without validating it");
    entities=fixture();find(entities,"floor").properties["deduction_ids"]={"room"};
    find(entities,"room").properties["deduction_ids"]={"void"};
    const auto retained=Document::create(entities).snapshot();
    require(prepare_ansi_appraisal_partition_targets(retained,{{"floor",{"room"}}}).front()==retained.entities().at("floor"),
        "retained deductions and complete existing descendant graph are preserved exactly");
}
void layerless_saved_boundaries() {
    auto entities=fixture();
    for(const auto& id:{"floor","room","void"})find(entities,id).properties.erase("layer_id");
    const auto source=Document::create(entities).snapshot();
    require(ansi_appraisal_partition_context(source,"floor"),"saved ANSI boundary without a drawing layer still resolves its owning policy");
    const auto proposed=prepare_ansi_appraisal_partition_targets(source,{{"floor",{"room"}},{"room",{"void"}}});
    require(proposed.size()==2,"source-free layerless ANSI boundaries support complete nested partition preparation");
    for(std::size_t i=0;i<proposed.size();++i) {
        auto expected=source.entities().at(i==0?"floor":"room");
        expected.properties["deduction_ids"]=Json::array({i==0?"room":"void"});
        require(proposed[i]==expected && !proposed[i].properties.contains("layer_id"),
            "nested preparation preserves absent layer IDs and all unrelated saved boundary data");
    }
    find(entities,"floor").properties["deduction_ids"]={"room"};
    find(entities,"room").properties["deduction_ids"]={"missing"};
    const auto broken=Document::create(entities).snapshot();
    const auto repaired=prepare_ansi_appraisal_partition_targets(broken,{{"floor",{"room"}},{"room",{}}});
    auto expected=broken.entities().at("room");expected.properties.erase("deduction_ids");
    require(repaired.size()==2 && repaired[0]==broken.entities().at("floor") && repaired[1]==expected &&
        !repaired[1].properties.contains("layer_id"),"layerless saved boundary can clear an invalid link without inventing a layer");
    auto mismatch_entities=entities;
    mismatch_entities.push_back({"other-property","property",Json::object()});
    mismatch_entities.push_back({"other-building","building",{{"property_id","other-property"}}});
    mismatch_entities.push_back({"other-floor","floor",{{"building_id","other-building"}}});
    mismatch_entities.push_back({"other-layer","layer",{{"floor_id","other-floor"}}});
    mismatch_entities.push_back(area("same-floor-area",1,9));
    for(const auto& [key,id]:std::vector<std::pair<const char*,const char*>>{
        {"property_id","other-property"},{"building_id","other-building"},
        {"layer_id","other-layer"}}) {
        auto mismatched=mismatch_entities;find(mismatched,"room").properties[key]=id;
        const auto invalid=Document::create(mismatched).snapshot();
        refuses(invalid,[&]{(void)ansi_appraisal_partition_context(invalid,"room");},
            "existing contradictory owner or layer IDs refuse policy routing");
        refuses(invalid,[&]{(void)prepare_ansi_appraisal_partition_targets(invalid,{{"floor",{"room"}},{"room",{}}});},
            "layerless compatibility cannot hide existing contradictory owners or layers while repairing links");
    }
    // DocumentSnapshot's entity state is private. Native admission validates
    // typed references before a malformed non-layer reference can reach the
    // partition helper, so exercise that boundary through an atomic command.
    auto document=Document::create(mismatch_entities);
    const auto before=document.snapshot();
    auto invalid_room=before.entities().at("room");invalid_room.properties["layer_id"]="same-floor-area";
    bool rejected=false;
    try {
        document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(invalid_room)},{},"Invalid layer reference"});
    } catch(const DocumentError& error) {
        require(error.code()==DocumentErrorCode::invalid_entity && std::string(error.what()).find(
            "reference same-floor-area has type measurement_boundary, expected layer")!=std::string::npos,
            "native admission must identify the non-layer reference with its invalid-entity diagnostic");
        rejected=true;
    }
    require(rejected,"native admission refuses a same-floor measurement boundary as layer_id before partition preparation");
    const auto after=document.snapshot();
    require(after.entities()==before.entities() && after.revision()==before.revision() &&
        after.history().size()==before.history().size(),"invalid layer admission preserves all entities revision and history");
}
void overlapping_voids_and_shared_descendants() {
    auto entities=fixture();entities.push_back(area("second-void",5,7));
    const auto source=Document::create(entities).snapshot();
    const auto proposed=prepare_ansi_appraisal_partition_targets(source,{{"room",{"void","second-void"}}});
    std::vector<AreaDeduction> tools;
    for(const auto& id:{"void","second-void"})tools.push_back({id,boundary_geometry(decode_identified_boundary_entity(source.entities().at(id)))});
    const CalculationProfile physical{"physical",1,AreaUnit::square_metre,2,{{"physical",{false,false}}}};
    const auto trace=calculate_area({"room","b","f","physical",boundary_geometry(decode_identified_boundary_entity(proposed.front())),tools,{1,1}},physical);
    require(std::abs(trace.net_square_metres-29)<1e-10,"overlapping4+4 void footprints retain union7 rather than double subtraction8");
    entities=fixture();entities.push_back(area("second-room",3,7));
    const auto shared=Document::create(entities).snapshot();
    require(prepare_ansi_appraisal_partition_targets(shared,{{"floor",{"room","second-room"}},
        {"room",{"void"}},{"second-room",{"void"}}}).size()==3,
        "valid shared descendant graphs retain geometric authoring compatibility; report owns measured contribution overlap qualification");
}
Entity measured_stroke(std::string id,double lo,double hi) {
    const std::vector<Vec2> points{{lo,lo},{hi,lo},{hi,hi},{lo,hi},{lo,lo}};
    MeasurementLinework model;model.stroke_id=id;model.anchor=points.front();model.closed=true;
    for(std::size_t i=1;i<points.size();++i) {
        ConstructionReceipt receipt;receipt.segment_id=id+":e"+std::to_string(i);
        receipt.kind=BoundaryConstructionKind::line_to_point;receipt.start=points[i-1];receipt.chord_end=points[i];
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(i-1),i==4?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"model",encode_measurement_linework_model(model)}},true};
}
void stale_and_phase_hidden_sources() {
    auto entities=fixture();entities.push_back(measured_stroke("void-source",4,6));
    auto lineage=Json::array();
    for(int i=1;i<=4;++i)lineage.push_back(Json::array({{{"owner_id","void-source"},{"segment_id","void-source:e"+std::to_string(i)},
        {"parameter_start",0},{"parameter_end",1},{"reversed",false}}}));
    find(entities,"void").extensions["measurement_linework_sources"]=lineage;
    const auto current=Document::create(entities).snapshot();
    require(prepare_ansi_appraisal_partition_targets(current,{{"floor",{"room"}},{"room",{"void"}}}).size()==2,
        "deep measured-line source fixture is current before failure mutations");
    find(entities,"void-source")=measured_stroke("void-source",5,7);
    const auto stale=Document::create(entities).snapshot();
    refuses(stale,[&]{(void)prepare_ansi_appraisal_partition_targets(stale,{{"floor",{"room"}},{"room",{"void"}}});},
        "stale grandchild linework invalidates the complete proposed partition");
    const auto removed=prepare_ansi_appraisal_partition_targets(stale,{{"floor",{"room"}},{"room",{}}});
    require(!removed.back().properties.contains("deduction_ids"),"removing stale descendant repairs only retained reachable geometry");
    find(entities,"void-source")=measured_stroke("void-source",4,6);
    auto phases=ModelPhases::create({"void-source"},{},{{"future","Future",{}, {"void-source"}}});
    entities.push_back({"phases","model_phases",{{"model",phases.to_json()}}});
    const auto hidden=Document::create(entities).snapshot();
    refuses(hidden,[&]{(void)prepare_ansi_appraisal_partition_targets(hidden,{{"floor",{"room"}},{"room",{"void"}}});},
        "active boundary with a semantic phase-hidden grandchild source refuses atomically");
    find(entities,"phases").properties["model"]=phases.with_active("future").to_json();
    require(prepare_ansi_appraisal_partition_targets(Document::create(entities).snapshot(),{{"floor",{"room"}},{"room",{"void"}}}).size()==2,
        "making the exact source semantically active restores geometry preparation");
}
void exterior_sources() {
    auto entities=fixture();
    const std::vector<Vec2> points{{4,4},{6,4},{6,6},{4,6},{4,4}};
    std::vector<std::string> wall_ids;
    for(std::size_t i=1;i<points.size();++i) {
        const auto id="wall"+std::to_string(i);wall_ids.push_back(id);
        entities.push_back({id,"wall",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
            {"baseline",{{"start",{points[i-1].x,points[i-1].y}},{"end",{points[i].x,points[i].y}},{"sweep_radians",0}}},
            {"thickness_m",0.2},{"height_m",3},{"elevation_m",0}}});
    }
    const auto derived=derive_exterior_wall_measurement(Document::create(entities).snapshot(),wall_ids);
    auto exterior=area("exterior-void",4,6);auto model=decode_identified_boundary_entity(exterior);
    for(std::size_t i=0;i<model.segments.size();++i)model.segments[i].segment=derived.boundary.at(i);
    exterior=encode_identified_boundary_entity(model,&exterior);exterior.properties["wall_measurement_source"]=derived.source;
    entities.push_back(exterior);
    const auto current=Document::create(entities).snapshot();
    require(prepare_ansi_appraisal_partition_targets(current,{{"room",{"exterior-void"}}}).size()==1,
        "current exterior descendant fixture prepares before failure mutations");
    find(entities,"wall1").properties["thickness_m"]=0.3;
    const auto stale=Document::create(entities).snapshot();
    refuses(stale,[&]{(void)prepare_ansi_appraisal_partition_targets(stale,{{"room",{"exterior-void"}}});},
        "changed exterior source geometry refuses partition preparation");
    find(entities,"wall1").properties["thickness_m"]=0.2;
    auto phases=ModelPhases::create({"wall1"},{},{{"future","Future",{}, {"wall1"}}});
    entities.push_back({"phases","model_phases",{{"model",phases.to_json()}}});
    const auto hidden=Document::create(entities).snapshot();
    refuses(hidden,[&]{(void)prepare_ansi_appraisal_partition_targets(hidden,{{"room",{"exterior-void"}}});},
        "current exterior shape with semantic phase-hidden source wall refuses preparation");
}
} // namespace
int main() {
    sketch::testing::noninteractive_errors();
    try {overlay_and_policy();graph_failures_and_repairs();layerless_saved_boundaries();overlapping_voids_and_shared_descendants();
        stale_and_phase_hidden_sources();exterior_sources();
        std::cout<<"ANSI appraisal partition checks passed\n";return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
