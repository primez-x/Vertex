#include "sketch/measurement_linework_source.hpp"
#include "sketch/measurement_linework.hpp"
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
}
int main() {
    sketch::testing::noninteractive_errors();
    try{current_and_refresh();stale_inputs_and_visibility();appraisal_never_uses_stale_sources();std::cout<<"measured linework source checks passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
