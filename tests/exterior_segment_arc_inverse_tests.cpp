#include "sketch/wall_measurement.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/geometry_operations.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& f, const char* message) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
Segment baseline(const Entity& wall) {
    const auto& b=wall.properties.at("baseline");
    return {{b.at("start")[0].get<double>(),b.at("start")[1].get<double>()},
        {b.at("end")[0].get<double>(),b.at("end")[1].get<double>()},b.at("sweep_radians").get<double>()};
}
bool same_point(Vec2 a,Vec2 b) {return a.x==b.x && a.y==b.y;}
Vec2 split_seam(const Segment& old,double fraction) {
    const auto dx=old.end.x-old.start.x,dy=old.end.y-old.start.y,k=0.5/std::tan(old.sweep_radians/2);
    const Vec2 center{old.start.x+dx/2-dy*k,old.start.y+dy/2+dx*k};
    const auto angle=old.sweep_radians*fraction,x=old.start.x-center.x,y=old.start.y-center.y;
    return {center.x+x*std::cos(angle)-y*std::sin(angle),center.y+x*std::sin(angle)+y*std::cos(angle)};
}
Entity wall(std::string id, Vec2 a, Vec2 b, double sweep, double thickness) {
    Entity result{id,"wall",{{"baseline",{{"start",{a.x,a.y}},{"end",{b.x,b.y}},{"sweep_radians",sweep}}},
        {"thickness_m",thickness},{"height_m",3},{"elevation_m",0},{"property_id","property"},
        {"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}}};
    if (sweep!=0) {
        const auto angle=angle_from_radians(sweep);
        result.extensions["curve_input"]={{"version",2},{"construction","angle"},
            {"measure",angle.original_expression},{"measure_value",sweep},{"radians",sweep},
            {"clockwise",sweep<0},{"start",{a.x,a.y}},{"end",{b.x,b.y}},{"vendor","retain"}};
    }
    result.extensions["vendor"]="retain";
    return result;
}
Entities fixture(bool curved, bool reverse_sources, bool reverse_owner, double translation, bool contact=false, bool equal_thickness=false) {
    const auto p=[&](double x,double y){return Vec2{x+translation,y-translation};};
    std::vector<Entity> values{{"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}},{"floor","floor",{{"building_id","building"}}},
        {"layer","layer",{{"floor_id","floor"}}},
        wall("bottom",reverse_sources?p(4,0):p(0,0),reverse_sources?p(0,0):p(4,0),
            curved?(reverse_sources?-0.6:0.6):0,0.2),
        wall("right",p(4,0),p(4,4),0,equal_thickness?0.2:0.35),wall("top",p(4,4),p(0,4),0,equal_thickness?0.2:0.25),
        wall("left",p(0,4),p(0,0),0,equal_thickness?0.2:0.15)};
    if(contact) values.push_back(wall("partition",p(4,0),p(2,2),0,0.1));
    auto entities=Document::create(values).snapshot().entities();
    const auto measured=derive_exterior_wall_measurement(entities,{"bottom","right","top","left"});
    IdentifiedBoundary outline{"area","measurement_boundary",{}};
    for(std::size_t i=0;i<measured.boundary.size();++i)
        outline.segments.push_back({"edge-"+std::to_string(i),"vertex-"+std::to_string(i),
            "vertex-"+std::to_string((i+1)%measured.boundary.size()),measured.boundary[i]});
    auto owner=encode_identified_boundary_entity(outline);
    for(const auto* key:{"property_id","building_id","floor_id","layer_id"})
        owner.properties[key]=entities.at("bottom").properties.at(key);
    owner.properties["wall_measurement_source"]=measured.source;
    auto consumer=owner; consumer.id="consumer";
    auto consumer_outline=decode_identified_boundary_entity(consumer);
    std::reverse(consumer_outline.segments.begin(),consumer_outline.segments.end());
    for(auto& edge:consumer_outline.segments) {
        std::swap(edge.start_vertex_id,edge.end_vertex_id); std::swap(edge.segment.start,edge.segment.end);
        edge.segment.sweep_radians=-edge.segment.sweep_radians;
    }
    consumer=encode_identified_boundary_entity(consumer_outline,&consumer);
    if(reverse_owner) {
        std::reverse(outline.segments.begin(),outline.segments.end());
        for(auto& edge:outline.segments) {
            std::swap(edge.start_vertex_id,edge.end_vertex_id); std::swap(edge.segment.start,edge.segment.end);
            edge.segment.sweep_radians=-edge.segment.sweep_radians;
        }
        owner=encode_identified_boundary_entity(outline,&owner);
    }
    entities.emplace("area",owner); entities.emplace("consumer",consumer);
    return entities;
}
ExteriorSegmentArcIntent intent_for(const Entities& entities, BoundaryConstructionKind kind, bool negative=false) {
    const auto outline=decode_identified_boundary_entity(entities.at("area"));
    const auto order=derive_exterior_wall_measurement(entities,{"bottom","right","top","left"}).ordered_wall_ids;
    const auto measured=derive_exterior_wall_measurement(entities,{"bottom","right","top","left"});
    const auto index=static_cast<std::size_t>(std::find(order.begin(),order.end(),"bottom")-order.begin());
    const auto source=measured.boundary[index];
    const auto selected=std::find_if(outline.segments.begin(),outline.segments.end(),[&](const auto& edge) {
        return (same_point(edge.segment.start,source.start) && same_point(edge.segment.end,source.end)) ||
            (same_point(edge.segment.start,source.end) && same_point(edge.segment.end,source.start));
    });
    require(selected!=outline.segments.end(),"selected measured bottom edge must exist");
    ConstructionReceipt receipt; receipt.segment_id=selected->segment_id; receipt.kind=kind;
    receipt.start=selected->segment.start; receipt.chord_end=selected->segment.end;
    if(kind==BoundaryConstructionKind::arc_chord_angle) receipt.angle=parse_angle(negative?"-60 deg":"60 deg");
    else if(kind==BoundaryConstructionKind::arc_chord_height) receipt.height=parse_quantity(negative?"-0.5 m":"0.5 m");
    else {receipt.arc_length=parse_quantity("4.7 m");receipt.clockwise=negative;}
    return {"area",selected->segment_id,receipt,true};
}
Entities complete(const Entities& before,Entities physical) {
    for(const auto& edit:exterior_wall_measurement_source_updates(before,physical,false))
        physical=edited_boundary_entities(physical,edit);
    return physical;
}
void matrix() {
    for(const bool curved:{false,true}) for(const bool reverse_sources:{false,true})
    for(const bool reverse_owner:{false,true}) for(const double translation:{0.0,125000.0})
    for(const bool negative:{false,true}) for(const auto kind:{BoundaryConstructionKind::arc_chord_angle,
        BoundaryConstructionKind::arc_chord_height,BoundaryConstructionKind::arc_chord_length}) {
        const auto original=fixture(curved,reverse_sources,reverse_owner,translation);
        const auto intent=intent_for(original,kind,negative);
        const auto physical=exterior_segment_arc_physical_entities(original,intent);
        require(physical.at("area")==original.at("area"),"inverse must leave measured owners for source completion");
        const auto final=complete(original,physical);
        validate_exterior_segment_arc_result(original,final,intent);
        require(wall_measurement_source_current(final,final.at("consumer")),"every reversed consumer must be current");
        for(const auto* id:{"bottom","right","top","left"}) {
            require(physical.at(id).properties.at("thickness_m")==original.at(id).properties.at("thickness_m"),
                "inverse must retain unequal physical thicknesses");
            require(physical.at(id).extensions.at("vendor")==original.at(id).extensions.at("vendor"),
                "inverse must retain unrelated metadata");
            validate_wall_curve_input(physical.at(id));
        }
        const auto& proof=physical.at("bottom").extensions.at("curve_input_derivation");
        require(proof.at("source_baseline")==original.at("bottom").properties.at("baseline"),
            "curve provenance must retain exact original physical baseline");
        require(curved?proof.at("source_input")==original.at("bottom").extensions.at("curve_input"):
            (proof.at("version")==3 && proof.at("source_input").is_null()),"line origins must not invent original curve receipts");
        require(physical.at("bottom").extensions.at("curve_input").at("construction")=="angle",
            "physical curve input must be its derived angle");
        validate_constraint_wall_geometry_transition(original,physical,true,true);
        if(!curved) rejects([&]{validate_constraint_wall_geometry_transition(original,physical,true);},
            "historical qualified corner/resize authority must not authorize a line-origin curve");
    }
}
void semicircle_and_major_branch() {
    for(const bool curved:{false,true})for(const auto* expression:{"180 deg","240 deg"}) {
        const auto original=fixture(curved,false,false,0,false,true);
        auto intent=intent_for(original,BoundaryConstructionKind::arc_chord_angle);
        intent.arc_construction.angle=parse_angle(expression);
        const auto physical=exterior_segment_arc_physical_entities(original,intent);
        const auto final=complete(original,physical);
        validate_exterior_segment_arc_result(original,final,intent);
        const auto owner=decode_identified_boundary_entity(final.at("area"));
        const auto edge=std::find_if(owner.segments.begin(),owner.segments.end(),[&](const auto& value){return value.segment_id==intent.segment_id;});
        require(edge!=owner.segments.end() && std::abs(edge->segment.sweep_radians-intent.arc_construction.angle->radians)<1e-10,
            "Exact semicircle or outward major arc must retain its requested branch");
    }
}
void provenance_continues_and_rejects_tampering() {
    const auto original=fixture(false,false,false,0);
    const auto intent=intent_for(original,BoundaryConstructionKind::arc_chord_height);
    const auto physical=exterior_segment_arc_physical_entities(original,intent);
    const auto source=physical.at("bottom");
    const auto old=baseline(source);
    const auto completed=complete(original,physical);
    const auto second_intent=intent_for(completed,BoundaryConstructionKind::arc_chord_angle,true);
    const auto second=exterior_segment_arc_physical_entities(completed,second_intent);
    validate_exterior_segment_arc_result(completed,complete(completed,second),second_intent);
    const auto& previous=source.extensions.at("curve_input_derivation");
    const auto& history=second.at("bottom").extensions.at("curve_input_derivation");
    require(history.at("version")==3 && history.at("source_baseline")==previous.at("source_baseline") &&
        history.at("operations")[0]==previous.at("operations")[0],"subsequent arc edits must preserve the straight-origin prefix");
    auto moved=source; const Segment next{{old.start.x+0.25,old.start.y},{old.end.x+0.5,old.end.y},old.sweep_radians};
    rebase_wall_curve_input(moved,next);
    moved.properties["baseline"]["start"]={next.start.x,next.start.y};
    moved.properties["baseline"]["end"]={next.end.x,next.end.y};
    validate_wall_curve_input(moved);
    require(moved.extensions.at("curve_input_derivation").at("version")==3,"endpoint rebase must preserve v3");
    PlanarTransform transform{{0,0},0.25,true,false,{2,1}};
    auto rotated=source; transform_wall_curve_input(rotated,transform);
    const auto transformed=transform_segment(old,transform);
    rotated.properties["baseline"]={{"start",{transformed.start.x,transformed.start.y}},
        {"end",{transformed.end.x,transformed.end.y}},{"sweep_radians",transformed.sweep_radians}};
    validate_wall_curve_input(rotated);
    require(rotated.extensions.at("curve_input_derivation").at("version")==3,"rigid transform must preserve v3");
    Entities before{{source.id,source}},after{{rotated.id,rotated}};
    validate_constraint_wall_geometry_transition(before,after);
    const auto seam=split_seam(old,0.4);
    const auto split=reconstruct_split_wall(source,{old.start,seam,old.sweep_radians*0.4},0.4,false);
    validate_wall_curve_input(split); validate_wall_split_archive(split);
    require(split.extensions.at("curve_input_derivation").at("version")==3,"split must preserve straight-origin prefix");
    auto bad=source; bad.extensions["curve_input_derivation"]["source_input"]=source.extensions.at("curve_input");
    rejects([&]{validate_wall_curve_input(bad);},"v3 must reject a fabricated curve source input");
    bad=source; bad.extensions["curve_input_derivation"]["operations"][0]["input"]["measure"]="1 deg";
    rejects([&]{validate_wall_curve_input(bad);},"tampered first physical construction must reject");
    rejects([&]{(void)reconstruct_exterior_corner_wall(original.at("bottom"),baseline(source));},
        "historical corner helper must retain kind-change rejection");
}
void codec_and_failure_paths() {
    const auto original=fixture(false,false,false,0);
    const auto intent=intent_for(original,BoundaryConstructionKind::arc_chord_height);
    const auto encoded=encode_exterior_segment_arc(intent);
    const auto decoded=decode_exterior_segment_arc(encoded);
    require(decoded.arc_construction==intent.arc_construction && encode_exterior_segment_arc(decoded)==encoded,
        "codec must retain exact measured construction receipt");
    auto bad=encoded;bad["vendor"]=true;
    rejects([&]{(void)decode_exterior_segment_arc(bad);},"unknown proof members must reject");
    bad=encoded;bad["arc_construction"]["vendor"]=true;
    rejects([&]{(void)decode_exterior_segment_arc(bad);},"noncanonical nested receipt must reject");
    bad=encoded;bad["move_connected_objects"]=1;
    rejects([&]{(void)decode_exterior_segment_arc(bad);},"numeric movement flag must reject");
    auto stale=original;stale.at("right").properties["thickness_m"]=0.4;
    rejects([&]{(void)exterior_segment_arc_physical_entities(stale,intent);},"stale source must reject");
    auto ambiguous=original;auto& records=ambiguous.at("area").properties["wall_measurement_source"]["walls"];
    records.push_back(records.front());
    rejects([&]{(void)exterior_segment_arc_physical_entities(ambiguous,intent);},"ambiguous source identity must reject");
    auto physical=exterior_segment_arc_physical_entities(original,intent);
    auto drift=physical;
    // Change the far top/left corner while keeping the selected curve intact,
    // then independently derive a current consumer: selected-edge-only checks miss this.
    drift.at("top").properties["baseline"]["end"][1]=4.0+1e-8;
    drift.at("left").properties["baseline"]["start"][1]=4.0+1e-8;
    const auto final=complete(original,drift);
    require(wall_measurement_source_current(final,final.at("area")),"drift fixture must itself be source current");
    rejects([&]{validate_exterior_segment_arc_result(original,final,intent);},"full final outline drift must reject");
    const auto attached=fixture(false,false,false,0,true);
    auto attached_intent=intent_for(attached,BoundaryConstructionKind::arc_chord_height);
    const auto moving=exterior_segment_arc_physical_entities(attached,attached_intent);
    require(moving.at("partition")!=attached.at("partition"),"attached physical endpoint must follow sources");
    attached_intent.move_connected_objects=false;
    rejects([&]{(void)exterior_segment_arc_physical_entities(attached,attached_intent);},"frozen physical contact must reject");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    const char* stage="inverse matrix";
    try {matrix();stage="semicircle and major arc";semicircle_and_major_branch();stage="provenance";provenance_continues_and_rejects_tampering();stage="failure paths";
        codec_and_failure_paths();return 0;
    } catch(const std::exception& e) {std::cerr<<"exterior_segment_arc_inverse_tests ["<<stage<<"]: "<<e.what()<<'\n';return 1;}
}
