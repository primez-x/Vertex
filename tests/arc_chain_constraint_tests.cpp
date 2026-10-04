#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraints.hpp"
#include "sketch/document.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void expect_near(double actual,double expected,std::string_view message,double tolerance=1e-7) {
    require(std::isfinite(actual) && std::abs(actual-expected)<=tolerance,message);
}
template<class Action> void invalid(Action action,std::string_view message) {
    bool rejected=false;
    try { action(); } catch (const std::invalid_argument&) { rejected=true; }
    require(rejected,message);
}

Entity curved_wall(const char* id, double start, double end) {
    return Entity{id,"wall",{
        {"baseline",{{"start",{start,0}},{"end",{end,0}},{"sweep_radians",1.0}}},
        {"thickness_m",0.1},{"height_m",3.0},{"elevation_m",0.0}}};
}

Entity arc_chain_entity() {
    PersistentConstraint model;
    model.id="physical-total";
    model.relation=ConstraintRelationKind::fixed_arc_length;
    model.bindings={{"first",WallEndpointRole::start},{"first",WallEndpointRole::end}};
    // Each unit chord at a one-radian sweep measures 1/(2*sin(0.5)).
    model.length=parse_quantity("2.0858296429334882 m");
    auto entity=encode_constraint_entity(model);
    entity.properties["version"]=4;
    entity.properties["entity_ids"]={"first","second"};
    entity.properties["bindings"].push_back({{"owner_id","second"},{"feature","baseline"},{"role","start"}});
    entity.properties["bindings"].push_back({{"owner_id","second"},{"feature","baseline"},{"role","end"}});
    entity.properties["vendor_property"]={{"retain",17}};
    entity.properties["bindings"][2]["vendor_binding"]="retain";
    entity.properties["quantity_entries"]["/length_m"]["vendor_receipt"]="retain";
    entity.extensions["vendor_extension"]={{"retain",true}};
    return entity;
}

void test_current_valid_arc_chain_is_known_and_admitted() {
    const auto entity=arc_chain_entity();
    const auto decoded=decode_constraint_entity(entity);
    require(decoded.supported(),"a version-four physical arc chain must have known total-length semantics");
    require(decoded.constraint->bindings.size()==4 && decoded.constraint->length->original_expression=="2.0858296429334882 m",
        "arc-chain decoding must retain every endpoint and the original exact total quantity");
    require(encode_constraint_entity(*decoded.constraint,&entity)==entity,
        "arc-chain codec must preserve opaque envelope, binding, and quantity metadata");
    const auto document=Document::create({curved_wall("first",0,1),curved_wall("second",1,2),entity});
    require(document.is_editable(),"a physically satisfied arc-chain total must be admitted as editable");
    require(document.snapshot().entities().at(entity.id)==entity,
        "arc-chain admission must preserve its persistent identity and metadata");
}

void test_chain_structure_and_geometry_are_independently_validated() {
    const auto entity=arc_chain_entity();
    const auto model=*decode_constraint_entity(entity).constraint;
    const std::map<std::string,Entity,std::less<>> owners{
        {"first",curved_wall("first",0,1)},{"second",curved_wall("second",1,2)}};
    expect_near(resolve_constraint_arc_length(model,owners),2.0858296429334882,"chain measured an outer chord instead of physical arc total");
    invalid([&] { (void)resolve_constraint_arc_segment(model,owners); },"singular resolver silently ignored a chain piece");
    for (const auto version : {1,2,3}) {
        auto incompatible=entity; incompatible.properties["version"]=version;
        if (version==1) {
            incompatible.properties["wall_ids"]=incompatible.properties.at("entity_ids");
            incompatible.properties.erase("entity_ids");
        }
        if (version==3) invalid([&] { (void)decode_constraint_entity(incompatible); },"v3 silently admitted a four-binding chain");
        else require(!decode_constraint_entity(incompatible).supported(),"legacy version reinterpreted an arc chain");
    }
    auto other=entity; other.properties["relation"]="fixed_length";
    require(!decode_constraint_entity(other).supported(),"v4 reinterpreted endpoint distance as a chain total");
    auto odd=entity; odd.properties["bindings"].erase(3);
    invalid([&] { (void)decode_constraint_entity(odd); },"chain admitted an unpaired endpoint");
    auto bad=model; bad.bindings[2]=bad.bindings[0]; bad.bindings[3]=bad.bindings[1];
    invalid([&] { (void)encode_constraint_entity(bad); },"chain admitted duplicate segments");
    bad=model; bad.bindings[3].role=WallEndpointRole::start;
    invalid([&] { (void)encode_constraint_entity(bad); },"chain admitted a same-role segment pair");
    auto disconnected=owners; disconnected.at("second")=curved_wall("second",1.1,2.1);
    invalid([&] { (void)resolve_constraint_arc_length(model,disconnected); },"chain admitted disconnected geometry");
    auto straight=owners; straight.at("second").properties["baseline"]["sweep_radians"]=0;
    invalid([&] { (void)resolve_constraint_arc_length(model,straight); },"arc total admitted a straight segment");
    auto wrong_total=owners; wrong_total.emplace(entity.id,entity);
    wrong_total.at(entity.id).properties["length_m"]=2.0;
    wrong_total.at(entity.id).properties["quantity_entries"]["/length_m"]=encode_constraint_quantity_receipt(parse_quantity("2 m"));
    invalid([&] { (void)validate_constraint_integrity(wrong_total); },"persisted integrity accepted the outer chord as physical total");
    auto reverse=model;
    reverse.bindings={model.bindings[3],model.bindings[2],model.bindings[1],model.bindings[0]};
    const auto reversed=resolve_constraint_arc_segments(reverse,owners);
    require(reversed.size()==2 && reversed.front().start.x==2 && reversed.back().end.x==0 &&
        reversed.front().sweep_radians==-1 && reversed.back().sweep_radians==-1,
        "reverse chain changed traversal ordering or signed sweep");
    expect_near(resolve_constraint_arc_length(reverse,owners),2.0858296429334882,"reverse traversal changed physical total");
    for (const double sweep : {-1.0,4.0,-4.0}) {
        const Segment segment{{0,0},{1,0},sweep};
        expect_near(constraint_arc_length_coefficient(segment),std::abs(sweep/(2*std::sin(sweep/2))),
            "physical coefficient failed signed minor or major arc");
    }
}

void test_distinct_segments_in_one_boundary_form_a_directed_chain() {
    const auto owner=encode_identified_boundary_entity(IdentifiedBoundary{"outline","measurement_boundary",{
        {"ab","a","b",{{0,0},{1,0},1}},{"bc","b","c",{{1,0},{2,0},1}},
        {"cd","c","d",{{2,0},{2,2},0}},{"de","d","e",{{2,2},{0,2},0}},
        {"ea","e","a",{{0,2},{0,0},0}}}});
    auto model=*decode_constraint_entity(arc_chain_entity()).constraint;
    model.bindings={{"outline",WallEndpointRole::start,"ab","a"},{"outline",WallEndpointRole::end,"ab","b"},
        {"outline",WallEndpointRole::start,"bc","b"},{"outline",WallEndpointRole::end,"bc","c"}};
    const auto entity=encode_constraint_entity(model);
    require(entity.properties.at("entity_ids").size()==1,"same-owner chain duplicated declared owners");
    const auto document=Document::create({owner,entity});
    require(document.is_editable(),"distinct adjacent stable segments of one owner were rejected");
    expect_near(resolve_constraint_arc_length(model,document.snapshot().entities()),2.0858296429334882,
        "same-owner arc chain lost one segment");
    auto bad=model; bad.bindings[2].vertex_id="a";
    invalid([&] { (void)encode_constraint_entity(bad); },"same-owner chain admitted disconnected stable vertex identities");
}

const ConstraintPoint& point(const ConstraintPreview& preview,const char* id) {
    for (const auto& value : preview.points) if (value.id==id) return value;
    throw std::runtime_error("solver omitted a stable point identity");
}

void test_weighted_total_allows_length_redistribution_as_one_equation() {
    ConstraintSolveRequest request;
    request.points={{"a",0,0},{"b",2,0},{"c",4,0}};
    request.constraints={WeightedLengthSumConstraint{"total",{{"a","b",1.5},{"b","c",2.0}},7.0}};
    const auto diagnosed=diagnose_planar_constraints(request);
    require(diagnosed.accepted() && diagnosed.degrees_of_freedom==5,
        "weighted total locked individual pieces or omitted its scalar equation");
    request.constraints.push_back(FixedAnchorConstraint{"anchor-a","a",0,0});
    request.constraints.push_back(FixedAnchorConstraint{"move-b","b",3,0});
    request.constraints.push_back(HorizontalConstraint{"second-horizontal","b","c"});
    const auto before=request.points;
    const auto preview=solve_planar_constraints(request);
    require(preview.accepted(),"weighted total could not trade one piece length against another");
    expect_near(point(preview,"b").x,3,"requested first piece length was independently frozen");
    expect_near(point(preview,"c").x,4.25,"second piece did not compensate to preserve weighted total");
    expect_near(1.5*std::hypot(point(preview,"b").x-point(preview,"a").x,point(preview,"b").y-point(preview,"a").y)+
        2*std::hypot(point(preview,"c").x-point(preview,"b").x,point(preview,"c").y-point(preview,"b").y),7,
        "accepted solve did not preserve weighted physical total");
    require(request.points==before,"weighted solve mutated its source points");
}

void test_invalid_terms_and_locked_conflict_reject_without_partial_results() {
    const WeightedLengthSumConstraint valid{"total",{{"a","b",1.5},{"b","c",2.0}},7.0};
    ConstraintSolveRequest request;
    request.points={{"a",0,0},{"b",2,0},{"c",4,0}};
    for (int corruption=0;corruption<8;++corruption) {
        auto bad=valid;
        if (corruption==0) bad.terms.clear();
        if (corruption==1) bad.total_metres=0;
        if (corruption==2) bad.terms[0].coefficient=-1;
        if (corruption==3) bad.terms[0].coefficient=std::numeric_limits<double>::infinity();
        if (corruption==4) bad.terms[0].first="missing";
        if (corruption==5) bad.terms[0].second="a";
        if (corruption==6) bad.terms[1]={"b","a",2};
        if (corruption==7) bad.terms[0].coefficient=std::numeric_limits<double>::max();
        request.constraints={bad};
        const auto preview=solve_planar_constraints(request);
        require(preview.status==ConstraintSolveStatus::rejected_invalid_input && preview.points==request.points,
            "malformed weighted term reached solver or published partial geometry");
    }
    request.constraints={valid};
    auto collapsed=request; collapsed.points[1].x=0;
    require(solve_planar_constraints(collapsed).status==ConstraintSolveStatus::rejected_invalid_input,
        "zero chord weighted term was admitted");
    auto inconsistent=valid; inconsistent.total_metres=8;
    request.constraints={inconsistent,FixedAnchorConstraint{"a-lock","a",0,0},
        FixedAnchorConstraint{"b-lock","b",2,0},FixedAnchorConstraint{"c-lock","c",4,0}};
    const auto before=request.points;
    const auto preview=solve_planar_constraints(request);
    require(!preview.accepted() && preview.points==before && request.points==before,
        "impossible anchored total published a partial solve or mutated source");
    require(preview.status==ConstraintSolveStatus::rejected_conflict || preview.status==ConstraintSolveStatus::rejected_residual,
        "anchored total conflict lacks independently diagnosed or verified rejection");
}

void test_persistent_authoring_redistributes_chain_and_reports_component_freedom() {
    const auto entity=arc_chain_entity();
    PersistentConstraint seam;
    seam.id="seam"; seam.relation=ConstraintRelationKind::coincident;
    seam.bindings={{"first",WallEndpointRole::end},{"second",WallEndpointRole::start}};
    const auto document=Document::create({curved_wall("first",0,1),curved_wall("second",1,2),entity,encode_constraint_entity(seam)});
    const auto before=document.snapshot();
    const auto analysis=analyze_persistent_constraint_component(before,{"first"});
    require(analysis.supported && analysis.owner_ids==std::vector<std::string>{"first","second"} &&
        analysis.point_count==4 && analysis.degrees_of_freedom==5,
        "chain graph omitted an owner or treated a total as individual locks");
    ConstraintAuthoringIntent intent;
    intent.wall_resize=WallResizeIntent{"first",parse_quantity("1.5643722322001162 m"),WallResizeAnchor::start,true};
    const auto preview=preview_constraint_authoring(before,intent);
    if (!preview.accepted()) for (const auto& message : preview.diagnostics()) std::cerr<<message<<'\n';
    require(preview.accepted(),"persisted physical arc-chain total prevented connected redistribution");
    const auto model=*decode_constraint_entity(entity).constraint;
    const auto arcs=resolve_constraint_arc_segments(model,preview.candidate_entities());
    expect_near(segment_length(arcs[0]),1.5643722322001162,"selected chain piece resize was not applied",1e-6);
    expect_near(segment_length(arcs[1]),0.521457410733372,"unselected chain piece was frozen instead of compensating",1e-6);
    expect_near(resolve_constraint_arc_length(model,preview.candidate_entities()),2.0858296429334882,
        "persistent authoring changed exact physical chain total",1e-6);
    require(preview.candidate_entities().at(entity.id)==entity && document.snapshot().entities()==before.entities(),
        "chain authoring rewrote its original exact constraint or source snapshot");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_current_valid_arc_chain_is_known_and_admitted();
        test_chain_structure_and_geometry_are_independently_validated();
        test_distinct_segments_in_one_boundary_form_a_directed_chain();
        test_weighted_total_allows_length_redistribution_as_one_equation();
        test_invalid_terms_and_locked_conflict_reject_without_partial_results();
        test_persistent_authoring_redistributes_chain_and_reports_component_freedom();
    } catch (const std::exception& error) {
        std::cerr << "arc_chain_constraint_tests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
