#include "sketch/wall_measurement.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Function> void rejects(Function&& function, const char* message) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
Entity wall(std::string id, Vec2 start, Vec2 end, double sweep, double thickness) {
    Entity result{id, "wall", {{"baseline", {{"start", {start.x,start.y}}, {"end", {end.x,end.y}},
        {"sweep_radians", sweep}}}, {"thickness_m", thickness}, {"height_m", 3}, {"elevation_m", 0},
        {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}}};
    if (sweep != 0) {
        const auto angle = angle_from_radians(sweep);
        result.extensions["curve_input"] = {{"version",2}, {"construction","angle"},
            {"measure",angle.original_expression}, {"measure_value",sweep}, {"radians",sweep},
            {"clockwise",sweep < 0}, {"start",{start.x,start.y}}, {"end",{end.x,end.y}}, {"vendor","retain"}};
    }
    result.extensions["vendor"] = {{"number", 1.0}};
    return result;
}
Entities fixture(bool curved, bool unequal, bool contact = false, bool reverse_sources = false) {
    std::vector<Entity> entities{
        {"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}},
        {"floor","floor",{{"building_id","building"}}},
        {"layer","layer",{{"floor_id","floor"}}},
        wall("bottom",reverse_sources ? Vec2{4,0} : Vec2{0,0}, reverse_sources ? Vec2{0,0} : Vec2{4,0},
            curved ? (reverse_sources ? -0.6 : 0.6) : 0,0.2),
        wall("right",{4,0},{4,3},0,unequal ? 0.35 : 0.2),
        wall("top",reverse_sources ? Vec2{0,3} : Vec2{4,3}, reverse_sources ? Vec2{4,3} : Vec2{0,3},
            0,unequal ? 0.25 : 0.2),
        wall("left",{0,3},{0,0},0,unequal ? 0.15 : 0.2)};
    if (contact) entities.push_back(wall("partition",{4,0},{2,2},0,0.1));
    auto original = Document::create(entities).snapshot().entities();
    const auto measured = derive_exterior_wall_measurement(original,{"bottom","right","top","left"});
    IdentifiedBoundary outline{"area","measurement_boundary",{}};
    for (std::size_t i = 0; i < measured.boundary.size(); ++i)
        outline.segments.push_back({"edge-"+std::to_string(i),"vertex-"+std::to_string(i),
            "vertex-"+std::to_string((i+1)%measured.boundary.size()),measured.boundary[i]});
    auto owner = encode_identified_boundary_entity(outline);
    for (const auto* key : {"property_id","building_id","floor_id","layer_id"})
        owner.properties[key] = original.at("bottom").properties.at(key);
    owner.properties["wall_measurement_source"] = measured.source;
    original.emplace(owner.id, owner);
    return original;
}
std::string selected_id(const Entities& entities, bool curved) {
    const auto outline = decode_identified_boundary_entity(entities.at("area"));
    if (curved) {
        for (const auto& edge : outline.segments) if (edge.segment.sweep_radians != 0) return edge.segment_id;
    }
    const auto order = derive_exterior_wall_measurement(entities,{"bottom","right","top","left"}).ordered_wall_ids;
    const auto index = std::find(order.begin(),order.end(),"bottom") - order.begin();
    return outline.segments.at(static_cast<std::size_t>(index)).segment_id;
}
Entities complete_sources(const Entities& original, Entities physical) {
    for (const auto& edit : exterior_wall_measurement_source_updates(original, physical, false))
        physical = edited_boundary_entities(physical, edit);
    return physical;
}
void require_exact_source_cycle(const Entities& physical) {
    const auto source_ids = exterior_wall_measurement_source_ids(physical.at("area"));
    std::vector<Vec2> endpoints;
    for (const auto& id : source_ids) {
        const auto& baseline = physical.at(id).properties.at("baseline");
        for (const auto* key : {"start", "end"})
            endpoints.push_back({baseline.at(key)[0].get<double>(), baseline.at(key)[1].get<double>()});
    }
    for (const auto endpoint : endpoints) {
        const auto incident = std::count_if(endpoints.begin(),endpoints.end(), [&](Vec2 other) {
            return endpoint.x == other.x && endpoint.y == other.y;
        });
        require(incident == 2, "reconstructed physical cycle must share exact endpoint coordinates");
    }
}
void shared_physical_vertices_remain_exact_when_neighbor_moves() {
    for (const bool reversed : {false,true}) {
        const auto original = fixture(false,false,false,reversed);
        ExteriorSegmentResizeIntent intent{"area",selected_id(original,false),parse_quantity("16 ft 4 in"),
            BoundaryFixedEndpoint::start,false,false};
        const auto physical = exterior_segment_resize_physical_entities(original,intent);
        require(physical.at("left") == original.at("left") && physical.at("bottom") != original.at("bottom"),
            "an unchanged physical wall must retain its shared corner with a moved neighbor");
        require_exact_source_cycle(physical);
        validate_exterior_segment_resize_result(original,complete_sources(original,physical),intent);
    }
}
void inverse_resizes_requested_outline_and_preserves_inputs() {
    for (const bool curved : {false,true}) for (const bool unequal : {false,true})
        for (const auto anchor : {BoundaryFixedEndpoint::start,BoundaryFixedEndpoint::end})
            for (const bool chain : {false,true}) for (const bool reversed : {false,true}) {
                const auto case_name = std::string(curved ? "arc" : "line") +
                    (unequal ? "/unequal" : "/equal") +
                    (anchor == BoundaryFixedEndpoint::start ? "/start" : "/end") +
                    (chain ? "/chain" : "/local") + (reversed ? "/reversed-native" : "/native");
                const char* stage = "fixture";
                try {
                const auto original = fixture(curved,unequal,false,reversed);
                ExteriorSegmentResizeIntent intent{"area",selected_id(original,curved),parse_quantity("5 m"),anchor,chain,false};
                stage = "physical inverse";
                const auto physical = exterior_segment_resize_physical_entities(original,intent);
                require_exact_source_cycle(physical);
                require(physical.at("area") == original.at("area") && physical.at("bottom") != original.at("bottom"),
                    "inverse must move physical sources even with related movement disabled and leave consumers for completion");
                require(physical.at("bottom").extensions.at("vendor") == original.at("bottom").extensions.at("vendor"),
                    "inverse must preserve unrelated source metadata");
                if (curved) require(physical.at("bottom").extensions.at("curve_input_derivation").at("source_input") ==
                    original.at("bottom").extensions.at("curve_input"),
                    "inverse must preserve original physical curve input archive");
                const auto& bottom = physical.at("bottom").properties.at("baseline");
                const Segment physical_edge{{bottom.at("start")[0].get<double>(), bottom.at("start")[1].get<double>()},
                    {bottom.at("end")[0].get<double>(), bottom.at("end")[1].get<double>()}, bottom.at("sweep_radians").get<double>()};
                require(std::abs(segment_length(physical_edge) - intent.exact_length.metres) > 1e-6,
                    "requested exterior length must not be substituted into the physical source baseline");
                stage = "source completion";
                const auto final = complete_sources(original,physical);
                stage = "final validation";
                validate_exterior_segment_resize_result(original,final,intent);
                const auto outline = decode_identified_boundary_entity(final.at("area"));
                const auto edge = std::find_if(outline.segments.begin(),outline.segments.end(),
                    [&](const auto& value) { return value.segment_id == intent.segment_id; });
                require(edge != outline.segments.end() && std::abs(segment_length(edge->segment)-5) <= 1e-6,
                    "forward-derived selected measured length must satisfy exact requested length");
                validate_constraint_wall_host("bottom",physical);
                } catch (const std::exception& error) {
                    throw std::runtime_error(case_name + " [" + stage + "]: " + error.what());
                }
            }
}
void codec_retains_exact_receipt_and_rejects_malformed_fields() {
    ExteriorSegmentResizeIntent intent{"area","edge-0",parse_quantity("16 ft 4 1/2 in"),BoundaryFixedEndpoint::end,true,false};
    const auto encoded = encode_exterior_segment_resize(intent);
    const auto decoded = decode_exterior_segment_resize(encoded);
    require(decoded.exact_length.exact_metres == intent.exact_length.exact_metres &&
        decoded.exact_length.original_expression == intent.exact_length.original_expression &&
        decoded.fixed_endpoint == intent.fixed_endpoint && decoded.move_boundary_chain && !decoded.move_connected_objects &&
        encode_exterior_segment_resize(decoded) == encoded,"resize proof must round trip exact mixed-unit receipt and distinct flags");
    auto bad = encoded; bad["vendor"] = true;
    rejects([&] { (void)decode_exterior_segment_resize(bad); },"unknown resize proof fields must reject");
    bad = encoded; bad["fixed_endpoint"] = "middle";
    rejects([&] { (void)decode_exterior_segment_resize(bad); },"unknown resize anchor must reject");
    bad = encoded; bad["move_boundary_chain"] = 1;
    rejects([&] { (void)decode_exterior_segment_resize(bad); },"numeric movement flag must reject");
    bad = encoded; bad["exact_length"]["exact_metres"]["numerator"] = 2;
    rejects([&] { (void)decode_exterior_segment_resize(bad); },"tampered exact quantity must reject");
    bad = encoded; bad["exact_length"]["vendor"] = true;
    rejects([&] { (void)decode_exterior_segment_resize(bad); },"unknown exact receipt fields must reject");
    auto invalid = intent; invalid.exact_length = parse_quantity("0 m");
    rejects([&] { (void)encode_exterior_segment_resize(invalid); },"zero measured target must reject");
    invalid = intent; invalid.exact_length.metres = std::numeric_limits<double>::infinity();
    rejects([&] { (void)encode_exterior_segment_resize(invalid); },"nonfinite measured target must reject");
    invalid = intent; invalid.segment_id = "bad/id";
    rejects([&] { (void)encode_exterior_segment_resize(invalid); },"invalid stable edge ID must reject");
    invalid = intent; invalid.fixed_endpoint = static_cast<BoundaryFixedEndpoint>(99);
    rejects([&] { (void)encode_exterior_segment_resize(invalid); },"invalid typed resize anchor must reject");
}
void stale_sources_and_final_geometry_drift_reject() {
    const auto original = fixture(true,true);
    ExteriorSegmentResizeIntent intent{"area",selected_id(original,true),parse_quantity("5 m"),BoundaryFixedEndpoint::start,true,true};
    auto stale = original; stale.at("right").properties["thickness_m"] = 0.45;
    rejects([&] { (void)exterior_segment_resize_physical_entities(stale,intent); },"stale physical source cannot authorize resize inverse");
    auto ambiguous = original;
    auto& source_records = ambiguous.at("area").properties["wall_measurement_source"]["walls"];
    source_records.push_back(source_records.front());
    rejects([&] { (void)exterior_segment_resize_physical_entities(ambiguous,intent); },"duplicate physical source correspondence must reject");
    auto missing = intent; missing.segment_id = "missing";
    rejects([&] { (void)exterior_segment_resize_physical_entities(original,missing); },"missing selected stable edge must reject");
    auto final = complete_sources(original,exterior_segment_resize_physical_entities(original,intent));
    auto altered = intent; altered.exact_length = parse_quantity("5.1 m");
    rejects([&] { validate_exterior_segment_resize_result(original,final,altered); },"final forward length must reject an altered intent");
    altered = intent; altered.fixed_endpoint = BoundaryFixedEndpoint::end;
    rejects([&] { validate_exterior_segment_resize_result(original,final,altered); },"final forward anchor must reject an altered intent");
    final.at("area").properties["segments"][0]["start"][0] = 100;
    rejects([&] { validate_exterior_segment_resize_result(original,final,intent); },"consumer coordinates cannot fake final source-derived geometry");
}
void physical_contacts_move_or_refuse_frozen_related_objects() {
    const auto original = fixture(false,true,true);
    ExteriorSegmentResizeIntent intent{"area",selected_id(original,false),parse_quantity("5 m"),BoundaryFixedEndpoint::start,true,true};
    const auto physical = exterior_segment_resize_physical_entities(original,intent);
    require(physical.at("partition") != original.at("partition"),"attached related wall endpoint must follow inverse perimeter contacts");
    validate_exterior_corner_physical_contacts(original,physical);
    intent.move_connected_objects = false;
    rejects([&] { (void)exterior_segment_resize_physical_entities(original,intent); },"frozen related wall contact must refuse detachment");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    const char* stage = "shared vertex restoration";
    try {
        shared_physical_vertices_remain_exact_when_neighbor_moves();
        stage = "inverse matrix";
        inverse_resizes_requested_outline_and_preserves_inputs();
        stage = "exact codec";
        codec_retains_exact_receipt_and_rejects_malformed_fields();
        stage = "stale sources and final drift";
        stale_sources_and_final_geometry_drift_reject();
        stage = "physical contacts";
        physical_contacts_move_or_refuse_frozen_related_objects();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "exterior_segment_resize_inverse_tests [" << stage << "]: " << error.what() << '\n';
        return 1;
    }
}
