#include "sketch/measurement_linework_source.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_area_graph.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/appraisal_document.hpp"
#include "support/noninteractive_errors.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Entity stroke(const std::string& id,const std::vector<Vec2>& points,bool closed) {
    MeasurementLinework model;model.stroke_id=id;model.anchor=points.front();model.closed=closed;
    for(std::size_t i=1;i<points.size();++i) {
        ConstructionReceipt receipt;receipt.segment_id=id+":e"+std::to_string(i);
        receipt.kind=BoundaryConstructionKind::line_to_point;receipt.start=points[i-1];receipt.chord_end=points[i];
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(i-1),
            closed&&i+1==points.size()?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"model",encode_measurement_linework_model(model)}},true};
}
Json use(const char* owner,const char* segment,double a,double b,bool reverse=false) {
    return Json::array({{{"owner_id",owner},{"segment_id",segment},{"parameter_start",a},
        {"parameter_end",b},{"reversed",reverse}}});
}
Entity area() {
    Boundary boundary{{{0,0},{2,0},0},{{2,0},{2,4},0},{{2,4},{0,4},0},{{0,4},{0,0},0}};
    Json geometry=Json::array();for(const auto& edge:boundary)
        geometry.push_back({{"start",{edge.start.x,edge.start.y}},{"end",{edge.end.x,edge.end.y}},{"sweep_radians",0}});
    Entity value{"area","measurement_boundary",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},
        {"layer_id","l"},{"classification","living"},{"segments",geometry}},false,
        {{"measurement_linework_sources",Json::array({use("outline","outline:e1",0,.5),
            use("separator","separator:e1",1.0/6,5.0/6),use("outline","outline:e3",.5,1),use("outline","outline:e4",0,1)})}}};
    return upgrade_legacy_boundary_entity(value);
}
std::map<std::string,Entity,std::less<>> fixture() {
    std::map<std::string,Entity,std::less<>> values;
    for(auto value:std::vector<Entity>{{"p","property",Json::object(),false},{"b","building",{{"property_id","p"}},false},
        {"f","floor",{{"building_id","b"}},false},{"l","layer",{{"floor_id","f"}},false},
        stroke("outline",{{0,0},{4,0},{4,4},{0,4},{0,0}},true),stroke("separator",{{2,-1},{2,5}},false),area()})
        values.emplace(value.id,std::move(value));
    return values;
}
void current_and_refresh() {
    auto values=fixture();const auto original=values;
    auto checks=measurement_linework_source_checks(values);
    require(checks.at("area").current,"unchanged analytical area and complete source lineage qualify as current");
    require(values==original,"freshness calculation never mutates source geometry or metadata");
    auto rotated=decode_identified_boundary_entity(values.at("area"));
    std::rotate(rotated.segments.begin(),rotated.segments.begin()+1,rotated.segments.end());
    values["area"]=encode_identified_boundary_entity(rotated,&values.at("area"));
    auto& lineage=values["area"].extensions["measurement_linework_sources"];
    std::rotate(lineage.begin(),lineage.begin()+1,lineage.end());
    require(measurement_linework_source_checks(values).at("area").current,"cyclic edge ordering retains exact source qualification");
    values=original;values["separator"]=stroke("separator",{{3,-1},{3,5}},false);
    checks=measurement_linework_source_checks(values);
    require(!checks.at("area").current && checks.at("area").proposed_boundary.has_value(),
        "moved separator marks old area stale and exposes an unambiguous same-topology refresh");
    require(std::abs(signed_area(*checks.at("area").proposed_boundary)-12)<1e-10,
        "refresh proposal measures the actual twelve-square-metre left face");
    auto refreshed=decode_identified_boundary_entity(values.at("area"));
    for(std::size_t i=0;i<refreshed.segments.size();++i)refreshed.segments[i].segment=checks.at("area").proposed_boundary->at(i);
    values["area"]=encode_identified_boundary_entity(refreshed,&values.at("area"));
    values["area"].extensions["measurement_linework_sources"]=checks.at("area").proposed_lineage;
    require(measurement_linework_source_checks(values).at("area").current,"accepted refresh restores actual source qualification");
}
void stale_inputs_and_visibility() {
    const auto original=fixture();
    auto values=original;values.erase("separator");
    require(!measurement_linework_source_checks(values).at("area").current,"deleted source never leaves a qualified area");
    values=original;values.at("separator").properties["model"]["version"]=999;
    require(!measurement_linework_source_checks(values).at("area").current,"future source geometry is not guessed into qualification");
    values=original;values.at("area").extensions["measurement_linework_sources"][0][0]["parameter_end"]=2;
    require(!measurement_linework_source_checks(values).at("area").current,"malformed lineage is explicitly stale");
    std::set<std::string,std::less<>> visible{"p","b","f","l","outline","area"};
    require(!measurement_linework_sources_visible(original.at("area"),&visible) &&
        !measurement_linework_source_checks(original,&visible).at("area").current,"semantic hidden source invalidates its area");
    auto plain=original.at("area");plain.extensions.erase("measurement_linework_sources");values=original;values["area"]=plain;
    require(measurement_linework_source_checks(values).empty() && measurement_linework_sources_visible(plain,&visible),
        "ordinary independent areas retain their existing calculation behavior");
}
Json lineage(const MeasurementAreaGraph& graph,const DerivedMeasurementFace& face) {
    Json result=Json::array();
    for(const auto& traversal:face.edge_uses) {
        Json edge=Json::array();
        for(const auto& source:graph.edges.at(traversal.edge_index).source_uses)
            edge.push_back({{"owner_id",source.owner_id},{"segment_id",source.segment_id},
                {"parameter_start",source.parameter_start},{"parameter_end",source.parameter_end},
                {"reversed",source.reversed!=traversal.reversed}});
        result.push_back(std::move(edge));
    }
    return result;
}
std::map<std::string,Entity,std::less<>> group_fixture(unsigned subdivisions=1) {
    auto values=fixture();
    if(subdivisions>1) {
        std::vector<Vec2> points{{0,0}};
        for(unsigned side=0;side<4;++side)for(unsigned step=1;step<=subdivisions;++step) {
            const double value=4.0*step/subdivisions;
            points.push_back(side==0 ? Vec2{value,0} : side==1 ? Vec2{4,value} :
                side==2 ? Vec2{4-value,4} : Vec2{0,4-value});
        }
        values["outline"]=stroke("outline",points,true);
    }
    std::vector<MeasurementGraphSource> sources;
    for(const auto id:{"outline","separator"})
        for(const auto& edge:replay_measurement_linework(*decode_measurement_linework_model(values.at(id).properties.at("model")).model).edges)
            sources.push_back({id,edge.segment_id,edge.segment});
    const auto graph=build_measurement_area_graph(sources);
    require(graph.faces.size()==2,"group fixture has two adjacent source faces");
    const auto combined=combine_measurement_faces(graph,{0,1});
    auto entity=values.at("area");
    entity.properties.erase("boundary_model_version");entity.properties.erase("boundary");
    Json segments=Json::array();
    for(const auto& edge:combined.boundary)segments.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    entity.properties["segments"]=segments;
    entity.extensions["measurement_linework_sources"]=lineage(graph,combined);
    entity.extensions["measurement_linework_group"]={{"version",1},{"members",Json::array({lineage(graph,graph.faces[0]),lineage(graph,graph.faces[1])})}};
    values["area"]=upgrade_legacy_boundary_entity(entity);
    return values;
}
void grouped_sources() {
    const auto original=group_fixture();auto values=original;
    auto checks=measurement_linework_source_checks(values);
    require(checks.at("area").current && checks.at("area").group_face_indices.size()==2,
        "combined area retains current provenance for both member faces and cancelled seam");
    const auto canonical_group=checks.at("area").proposed_group;
    require(values==original,"group checks do not mutate retained source evidence");
    std::set<std::string,std::less<>> visible{"p","b","f","l","outline","area"};
    require(!measurement_linework_sources_visible(values.at("area"),&visible) &&
        !measurement_linework_source_checks(values,&visible).at("area").current,
        "hidden cancelled seam source invalidates grouped area");
    values.erase("separator");
    require(!measurement_linework_source_checks(values).at("area").current,"deleted cancelled seam invalidates grouped area");
    values=original;values.at("separator")=stroke("separator",{{3,-1},{3,5}},false);
    checks=measurement_linework_source_checks(values);
    const auto& proposal=checks.at("area");
    require(!proposal.current && proposal.proposed_boundary && proposal.proposed_group.is_object(),
        "moving cancelled seam proposes atomic outer and member provenance refresh");
    require(std::abs(signed_area(*proposal.proposed_boundary)-16)<1e-10,"group refresh retains actual outer area16");
    auto updated=decode_identified_boundary_entity(values.at("area"));
    require(updated.segments.size()==proposal.proposed_boundary->size(),"same-topology group retains edge count");
    for(std::size_t i=0;i<updated.segments.size();++i)updated.segments[i].segment=proposal.proposed_boundary->at(i);
    values["area"]=encode_identified_boundary_entity(updated,&values.at("area"));
    values.at("area").extensions["measurement_linework_sources"]=proposal.proposed_lineage;
    values.at("area").extensions["measurement_linework_group"]=proposal.proposed_group;
    require(measurement_linework_source_checks(values).at("area").current,"atomic group refresh restores source qualification");
    values=original;
    auto& members=values.at("area").extensions["measurement_linework_group"]["members"];
    std::reverse(members.begin(),members.end());
    require(measurement_linework_source_checks(values).at("area").current &&
        measurement_linework_source_checks(values).at("area").proposed_group==canonical_group,
        "member input order retains current state and canonical proposal order");
    for(int fault=0;fault<7;++fault) {
        values=original;auto& group=values.at("area").extensions["measurement_linework_group"];
        if(fault==0)group["version"]=999;
        if(fault==1)group["extra"]=true;
        if(fault==2)group["members"]=Json::array({group["members"][0]});
        if(fault==3)group["members"][1]=group["members"][0];
        if(fault==4)group["members"][0][0][0]["parameter_end"]=2;
        if(fault==5)values.at("area").extensions.erase("measurement_linework_sources");
        if(fault==6)values.at("separator").properties["layer_id"]="another-layer";
        const auto failed=measurement_linework_source_checks(values);
        require(failed.contains("area") && !failed.at("area").current && !failed.at("area").proposed_boundary,
            "malformed unsupported duplicate missing-outer or wrong-context group fails closed");
        require(!measurement_linework_source_current(failed,values.at("area")),"group cannot bypass current-dependency recognition");
    }
    values=original;
    auto& duplicate=values.at("area").extensions["measurement_linework_group"]["members"];
    duplicate[1]=duplicate[0];std::rotate(duplicate[1].begin(),duplicate[1].begin()+1,duplicate[1].end());
    const auto injective=measurement_linework_source_checks(values).at("area");
    require(!injective.current && !injective.proposed_boundary,
        "rotated duplicate member cannot map two saved members to one current graph face");
    for(int budget=0;budget<3;++budget) {
        values=original;auto& group=values.at("area").extensions["measurement_linework_group"];
        if(budget==0)group["members"]=Json(std::vector<Json>(2049,group["members"][0]));
        if(budget==1)group["members"][0]=Json(std::vector<Json>(16385,group["members"][0][0]));
        if(budget==2)group["members"][0][0]=Json(std::vector<Json>(65537,group["members"][0][0][0]));
        const auto oversized=measurement_linework_source_checks(values).at("area");
        require(!oversized.current && !oversized.proposed_boundary &&
            !measurement_linework_sources_visible(values.at("area"),nullptr),
            "group member edge and source-use resource budgets fail before unbounded matching");
    }
    for(int budget=0;budget<2;++budget) {
        values=original;auto& outer=values.at("area").extensions["measurement_linework_sources"];
        if(budget==0)outer=Json(std::vector<Json>(16385,outer[0]));
        if(budget==1)outer[0]=Json(std::vector<Json>(65537,outer[0][0]));
        const auto oversized=measurement_linework_source_checks(values).at("area");
        require(!oversized.current && !oversized.proposed_boundary &&
            !measurement_linework_sources_visible(values.at("area"),nullptr),
            "outer lineage is independently bounded before source decoding or matching");
    }
    const auto many_edges=group_fixture(300);
    const auto exhausted=measurement_linework_source_checks(many_edges).at("area");
    require(!exhausted.current && !exhausted.proposed_boundary && exhausted.proposed_group.is_null() &&
        exhausted.diagnostic.find("matching work budget exhausted")!=std::string::npos,
        "symmetric many-edge valid group stops at explicit work limit without a partial refresh proposal");
}
void appraisal_never_uses_stale_sources() {
    auto values=fixture();
    values.at("p").properties={{"calculation_workflow","appraisal"},{"appraisal_policy",{
        {"policy_kind","residential_declared"},{"version",1},{"property_kind","detached_single_family"},{"measurement_basis","exterior"}}}};
    values.at("f").properties["appraisal_facts"]={{"grade","above"}};
    values.at("area").properties["appraisal_facts"]={{"finish","finished"},{"access","direct_interior"},
        {"ceiling_eligibility","standard"},{"area_use","dwelling"},{"boundary_role","measured_area"}};
    const auto report=[&](const std::set<std::string,std::less<>>* visible=nullptr) {
        std::vector<Entity> entities;for(const auto& [id,value]:values)entities.push_back(value);
        return build_appraisal_document_report(Document::create(std::move(entities)).snapshot(),"p",AreaUnit::square_metre,visible);
    };
    const auto current=report();
    require(current.qualified && current.calculation && current.boundaries.size()==1 && current.boundaries.front().measurement,
        "actual current source graph permits declared appraisal GLA");
    require(std::abs(current.calculation->property.gla().total.square_metres-8)<1e-10,"current half-outline contributes eight square metres");
    const auto facts=values.at("area").properties.at("appraisal_facts");
    values.at("separator")=stroke("separator",{{3,-1},{3,5}},false);
    const auto stale=report();
    require(!stale.qualified && !stale.calculation && !stale.boundaries.front().measurement && stale.boundaries.front().facts,
        "a moved source withholds outdated GLA and diagnostic geometry while retaining declared facts");
    require(values.at("area").properties.at("appraisal_facts")==facts,"freshness does not change classification facts");
    values.at("separator")=fixture().at("separator");
    std::set<std::string,std::less<>> phase{"p","b","f","l","outline","area"};
    require(!report(&phase).qualified,"phase-hidden separator cannot contribute stale appraisal totals");
    auto parent=values.at("area");parent.id="parent";parent.extensions.erase("measurement_linework_sources");
    parent.properties.erase("boundary_model_version");parent.properties.erase("boundary");
    Json outline=Json::array();for(const auto& edge:replay_measurement_linework(*decode_measurement_linework_model(
        values.at("outline").properties.at("model")).model).edges)
        outline.push_back({{"start",{edge.segment.start.x,edge.segment.start.y}},
            {"end",{edge.segment.end.x,edge.segment.end.y}},{"sweep_radians",0}});
    parent.properties["segments"]=outline;parent.properties["deduction_ids"]={"area"};
    values["parent"]=upgrade_legacy_boundary_entity(parent);
    values.at("area").properties["appraisal_facts"]["boundary_role"]="other_void";
    for(const auto ansi:{false,true}) {
        if(ansi) {
            values.at("p").properties["appraisal_policy"]["policy_kind"]="ansi_z765_2021";
            values.at("p").properties["appraisal_policy"]["ansi"]={{"interior_inspected",true},{"direct_measurement",true},{"acquisition_increment","inch"}};
            values.at("f").properties["appraisal_facts"]["ansi"]={{"any_part_below_grade",false}};
            for(const auto id:{"area","parent"}) {
                auto& facts=values.at(id).properties["appraisal_facts"];facts.erase("ceiling_eligibility");
                facts["ansi"]={{"year_round_suitable",true},{"finish_matches_dwelling",true},{"dwelling_identity","primary"},
                    {"ceiling",{{"kind","flat"},{"minimum_height_m",2.1336}}}};
            }
        }
        values.at("separator")=fixture().at("separator");
        require(report().qualified,"current source deduction permits qualified parent totals under either policy");
        values.at("separator")=stroke("separator",{{3,-1},{3,5}},false);
        const auto stale_parent=report();
        const auto status=std::find_if(stale_parent.boundaries.begin(),stale_parent.boundaries.end(),[](const auto& value){return value.boundary_id=="parent";});
        require(!stale_parent.qualified && !stale_parent.calculation && status!=stale_parent.boundaries.end() && !status->measurement,
            "stale derived deductions withhold their parent measurement and GLA under declared and ANSI policies");
    }
}
void ordinary_opaque_group_marker() {
    for(const auto type:{"boundary","room_boundary"}) {
        auto values=fixture();auto& ordinary=values.at("area");
        ordinary.type=type;ordinary.extensions.erase("measurement_linework_sources");
        const Json marker={{"version",999},{"members","vendor-opaque"},{"unrelated",{{"retain",true}}}};
        ordinary.extensions["measurement_linework_group"]=marker;
        values.at("p").properties={{"calculation_workflow","appraisal"},{"appraisal_policy",{
            {"policy_kind","residential_declared"},{"version",1},{"property_kind","detached_single_family"},{"measurement_basis","exterior"}}}};
        values.at("f").properties["appraisal_facts"]={{"grade","above"}};
        ordinary.properties["appraisal_facts"]={{"finish","finished"},{"access","direct_interior"},
            {"ceiling_eligibility","standard"},{"area_use","dwelling"},{"boundary_role","measured_area"}};
        std::set<std::string,std::less<>> visible{"p","b","f","l","area"};
        if(ordinary.type=="room_boundary") {
            // Architectural rooms do not implicitly become appraisal areas.
            // Keep a real independent measurement in the report to prove the
            // opaque room marker cannot withhold its qualified contribution.
            auto measured=area();measured.id="baseline-measurement";measured.type="boundary";
            measured.extensions.erase("measurement_linework_sources");
            measured.properties["appraisal_facts"]=ordinary.properties.at("appraisal_facts");
            values.emplace(measured.id,std::move(measured));visible.insert("baseline-measurement");
        }
        const auto checks=measurement_linework_source_checks(values,&visible);
        require(!checks.contains("area") && measurement_linework_source_current(checks,ordinary) &&
            measurement_linework_sources_visible(ordinary,&visible),
            "ordinary boundary and room vendor group markers have no measured source semantics");
        std::vector<Entity> entities;for(const auto& [id,value]:values)entities.push_back(value);
        const auto document=Document::create(entities);
        const auto report=build_appraisal_document_report(document.snapshot(),"p",AreaUnit::square_metre,&visible);
        require(report.qualified && report.calculation &&
            std::abs(report.calculation->property.gla().total.square_metres-8)<1e-10,
            "opaque vendor group marker cannot withhold an actual ordinary appraisal contribution");
        if(ordinary.type=="room_boundary")
            require(report.boundaries.size()==1 && report.boundaries.front().boundary_id=="baseline-measurement",
                "opaque vendor room metadata must not infer a new appraisal contribution");
        auto plain=ordinary;plain.extensions.erase("measurement_linework_group");
        auto without_marker=values;without_marker["area"]=plain;
        std::vector<Entity> plain_entities;for(const auto& [id,value]:without_marker)plain_entities.push_back(value);
        const auto plain_report=build_appraisal_document_report(Document::create(plain_entities).snapshot(),"p",AreaUnit::square_metre,&visible);
        require(plain_report.qualified && plain_report.calculation->property.gla().total.square_metres==
            report.calculation->property.gla().total.square_metres,
            "vendor group marker preserves the exact baseline total");
        const CalculationProfile physical{"ordinary-physical",1,AreaUnit::square_metre,2,{{"physical",{false,false}}}};
        const auto measure=[&](const Entity& entity) {
            return calculate_area({entity.id,"b","f","physical",boundary_geometry(decode_identified_boundary_entity(entity)),{}, {1,1}},physical);
        };
        const auto marked_measurement=measure(ordinary),plain_measurement=measure(plain);
        require(std::abs(marked_measurement.net_square_metres-8)<1e-10 &&
            marked_measurement.net_square_metres==plain_measurement.net_square_metres,
            "ordinary boundary and architectural room physical calculations preserve exact marker-free area");
        require(document.snapshot().entities().at("area").extensions.at("measurement_linework_group").dump()==marker.dump(),
            "ordinary vendor group bytes survive document admission unchanged");
        const auto wire=command_to_json(ApplyEntityChanges{0,{EntityChange::upsert(ordinary)}, {},"Opaque vendor group"});
        require(wire.dump().find(marker.dump())!=std::string::npos &&
            command_to_json(command_from_json(wire))==wire,
            "ordinary boundary and room vendor group markers survive command serialization exactly");
    }
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try{current_and_refresh();stale_inputs_and_visibility();grouped_sources();appraisal_never_uses_stale_sources();ordinary_opaque_group_marker();std::cout<<"measured linework source checks passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
