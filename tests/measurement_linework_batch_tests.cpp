#include "sketch/measurement_linework.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool same(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
void expect_invalid(const std::function<void()>& fn, const char* message) {
    try { fn(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
BoundaryGeometryEdit move(const char* id, Vec2 position) {
    BoundaryGeometryEdit edit; edit.boundary_id = "stroke";
    edit.target_id = id; edit.target_position = position; return edit;
}
MeasurementLinework stroke(const std::vector<Vec2>& points, const std::vector<std::string>& ids,
                           bool closed = false) {
    MeasurementLinework model; model.stroke_id = "stroke"; model.anchor = points.front(); model.closed = closed;
    model.extensions = {{"literal", "a"}, {"vendor", Json::array({7, "keep"})}};
    for (std::size_t i = 1; i < points.size(); ++i) {
        ConstructionReceipt receipt; receipt.segment_id = "e" + std::to_string(i);
        receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.start = points[i-1]; receipt.chord_end = points[i];
        model.edges.push_back({receipt.segment_id, ids[i-1], ids[i], receipt});
    }
    return model;
}
void roundtrip(const MeasurementLinework& model, const MeasurementLinework& source) {
    require(model.edges == source.edges && same(model.anchor, source.anchor) &&
        model.extensions == source.extensions && model.stroke_id == source.stroke_id && model.closed == source.closed,
        "operations must retain original topology, typed receipts, anchor and metadata");
    const auto encoded = encode_measurement_linework_model(model);
    const auto decoded = decode_measurement_linework_model(Json::parse(encoded.dump()));
    require(decoded.supported() && encode_measurement_linework_model(*decoded.model) == encoded,
        "saved batch must decode and encode exactly");
}
// Sequential moves collapse the sole edge, although swapping both stable
// vertices simultaneously gives a valid reversed edge with unchanged evidence.
void simultaneous_swap_replays() {
    MeasurementLinework source; source.stroke_id = "stroke";
    ConstructionReceipt receipt; receipt.segment_id = "edge";
    receipt.kind = BoundaryConstructionKind::line_heading;
    receipt.distance = parse_quantity("6 ft 6 3/4 in"); receipt.heading = parse_angle("0 deg");
    source.edges.push_back({"edge", "a", "b", receipt});
    source.extensions = {{"vendor", {{"literal", "a"}, {"values", {7, "keep"}}}}};
    const auto before = encode_measurement_linework_model(source);
    const auto end = replay_measurement_linework(source).edges.front().segment.end;
    bool sequential_rejected = false;
    try { (void)edited_measurement_linework(source, move("a", end)); }
    catch (const std::invalid_argument&) { sequential_rejected = true; }
    require(sequential_rejected, "fixture must expose intermediate collapsed-edge admission");
    auto wire = before; wire["version"] = 5; wire["replay_version"] = 5;
    wire["operations"] = nlohmann::json::array({{{"type", "vertex_batch"},
        {"edits", nlohmann::json::array({encode_boundary_geometry_edit(move("a", end)),
                                       encode_boundary_geometry_edit(move("b", {0,0}))})}}});
    const auto decoded = decode_measurement_linework_model(wire);
    require(decoded.supported(), "valid simultaneous stable-vertex batch must be supported");
    const auto replay = replay_measurement_linework(*decoded.model);
    require(replay.anchor.x == end.x && replay.anchor.y == end.y &&
        replay.edges.front().segment.end.x == 0 && replay.edges.front().segment.end.y == 0,
        "simultaneous swap must validate its final geometry rather than intermediate collapse");
    require(decoded.model->edges == source.edges && decoded.model->extensions == source.extensions,
        "batch must preserve immutable typed receipts and opaque metadata");
    require(encode_measurement_linework_model(*decoded.model) == wire,
        "batch save/reopen must retain exact intent and receipt evidence");
    require(encode_measurement_linework_model(source) == before, "batch must not mutate source");
    const auto authored = edited_measurement_linework_vertices(source, {move("a", end), move("b", {0,0})});
    require(encode_measurement_linework_model(authored) == wire,
        "public batch authoring must record the same simultaneous durable intent");
}

// Moving each vertex to its successor collapses an intermediate edge if the
// implementation accidentally sequences the edits rather than admitting once.
void translated_shared_and_revisited_vertices() {
    const auto source = stroke({{0,0},{2,0},{2,2},{2,0},{4,0}}, {"a","b","c","b","d"});
    const auto moved = edited_measurement_linework_vertices(source,
        {move("a",{2,0}), move("b",{4,0}), move("c",{4,2}), move("d",{6,0})});
    const auto replay = replay_measurement_linework(moved);
    require(same(replay.anchor,{2,0}) && same(replay.edges.back().segment.end,{6,0}) &&
        same(replay.edges[0].segment.end,{4,0}) && same(replay.edges[1].segment.start,{4,0}) &&
        same(replay.edges[2].segment.end,{4,0}) && same(replay.edges[3].segment.start,{4,0}),
        "open terminal and all revisited shared vertices must move together");
    require(moved.operations.size() == 1 && std::holds_alternative<MeasurementLineworkVertexBatch>(moved.operations[0]),
        "one simultaneous batch must retain one replay operation");
    roundtrip(moved,source);
    const auto crossing = stroke({{0,0},{2,2},{0,2},{2,0},{0,0}}, {"a","b","c","d","a"},true);
    const auto closed = edited_measurement_linework_vertices(crossing,
        {move("a",{2,2}),move("b",{4,4}),move("c",{2,4}),move("d",{4,2})});
    const auto world = replay_measurement_linework(closed);
    require(world.closed && same(world.anchor,{2,2}) && same(world.edges.back().segment.end,world.anchor),
        "crossing closed linework must retain shared closing identity");
    roundtrip(closed,crossing);
    const auto translated = transformed_measurement_linework(source, {{},0,false,false,{10,0}});
    const auto swapped = edited_measurement_linework_vertices(translated,
        {move("a",{12,0}),move("b",{10,0}),move("c",{10,2}),move("d",{14,0})});
    const auto after = replay_measurement_linework(swapped);
    require(swapped.transforms.empty() && swapped.operations.size()==2 &&
        std::holds_alternative<PlanarTransform>(swapped.operations[0]) && same(after.anchor,{12,0}) &&
        same(after.edges[0].segment.end,{10,0}),
        "v2 promotion must retain prior transforms and interpret batch targets in the current world frame");
    roundtrip(swapped,source);
}

void typed_arc_and_ordered_derivations() {
    auto source = stroke({{0,0},{2,0}}, {"a","b"});
    source = promoted_measurement_linework_for_typed_chord(source);
    auto& receipt = source.edges[0].receipt;
    receipt.kind = BoundaryConstructionKind::arc_chord_angle; receipt.chord_end.reset();
    receipt.chord_input = ChordInput{parse_quantity("2 m"),parse_angle("0 deg")};
    receipt.angle = parse_angle("-90 deg");
    auto world = transformed_measurement_linework(source, {{},0,false,false,{5,0}});
    world = edited_measurement_linework_vertices(world,{move("a",{7,0}),move("b",{5,0})});
    require(world.schema_version == 5 && world.replay_version == 5 && world.transforms.empty(),
        "batch must promote typed arc into a single ordered v5 stream");
    require(encode_measurement_linework_model(promoted_measurement_linework_for_typed_chord(world)) ==
        encode_measurement_linework_model(world), "typed chord promotion must not downgrade v5 batch evidence");
    world = transformed_measurement_linework(world, {{},0,true,false,{10,0}});
    world = edited_measurement_linework(world,move("b",{5,2}));
    const auto replay = replay_measurement_linework(world);
    require(world.schema_version == 5 && world.operations.size() == 4 && same(replay.anchor,{3,0}) &&
        same(replay.edges[0].segment.end,{5,2}) && replay.edges[0].segment.sweep_radians == std::numbers::pi/2,
        "post-batch reflection/edit must replay in world order and preserve analytical signed sweep");
    BoundaryGeometryEdit resize; resize.boundary_id="stroke"; resize.kind=BoundaryGeometryEditKind::resize_segment;
    resize.target_id="e1"; resize.target_length_metres=4;
    world=edited_measurement_linework(world,resize,parse_quantity("4 m"));
    require(world.schema_version==5 && std::abs(segment_length(replay_measurement_linework(world).edges[0].segment)-4)<1e-9,
        "typed length edits after batch must retain v5 and requested arc length");
    roundtrip(world,source);
}

void noop_invalid_and_historical_dialects() {
    const auto source=stroke({{0.1,0},{2.1,0},{2.1,3}}, {"a","b","c"});
    const auto v2=transformed_measurement_linework(source,{{},0,false,false,{1,2}});
    const auto v3=edited_measurement_linework(source,move("c",{3,4}));
    const auto v4=promoted_measurement_linework_for_typed_chord(v3);
    const auto v5=edited_measurement_linework_vertices(v4,{move("c",{4,4})});
    for (const auto& model : {source,v2,v3,v4,v5}) {
        const auto before=encode_measurement_linework_model(model);
        const auto position=replay_measurement_linework(model).edges.front().segment.start;
        require(encode_measurement_linework_model(edited_measurement_linework_vertices(model,{}))==before &&
            encode_measurement_linework_model(edited_measurement_linework_vertices(model,{move("a",position)}))==before,
            "empty/all-noop batches must preserve historical and current exact dialects");
        require(encode_measurement_linework_model(*decode_measurement_linework_model(before).model)==before,
            "historical dialects must remain byte-exact through save/reopen");
    }
    const auto before=encode_measurement_linework_model(source);
    auto wrong_owner=move("b",{3,0}); wrong_owner.boundary_id="other";
    auto extra=move("b",{3,0}); extra.move_connected=true;
    BoundaryGeometryEdit resize; resize.boundary_id="stroke"; resize.kind=BoundaryGeometryEditKind::resize_segment;
    resize.target_id="e1"; resize.target_length_metres=3;
    const std::vector<std::vector<BoundaryGeometryEdit>> invalid{
        {move("missing",{3,0})},{move("e1",{3,0})},{wrong_owner},{extra},{resize},
        {move("b",{3,0}),move("b",{4,0})},{move("a",{3,0}),move("b",{3,0})},
        {move("b",{std::numeric_limits<double>::infinity(),0})},{move("b",{1e16,0})}};
    for(const auto& edits:invalid) expect_invalid([&]{(void)edited_measurement_linework_vertices(source,edits);},
        "invalid batch targets, duplicate intent, final degeneration or precision loss must reject atomically");
    require(encode_measurement_linework_model(source)==before,"all rejected batches must leave source unchanged");
}

void strict_batches_and_future_opaque() {
    const auto source=stroke({{0,0},{2,0}}, {"a","b"});
    const auto valid=encode_measurement_linework_model(edited_measurement_linework_vertices(source,
        {move("a",{2,0}),move("b",{0,0})}));
    require(inspect_measurement_linework_model(valid).format==MeasurementLineworkFormat::supported_v5,
        "v5 inspection must admit saved simultaneous evidence");
    std::vector<Json> invalid;
    auto bad=valid; bad["operations"][0]["extra"]=true; invalid.push_back(bad);
    bad=valid; bad["operations"][0]["edits"]=Json::object(); invalid.push_back(bad);
    bad=valid; bad["operations"][0]["edits"]=Json::array(); invalid.push_back(bad);
    bad=valid; bad["operations"][0]["edits"][1]=bad["operations"][0]["edits"][0]; invalid.push_back(bad);
    bad=valid; bad["operations"][0]["edits"][0]["boundary_id"]="other"; invalid.push_back(bad);
    bad=valid; bad["operations"][0]["edits"][0]["vertex_id"]="missing"; invalid.push_back(bad);
    bad=valid; bad["operations"][0]["edits"][0]["extra"]=true; invalid.push_back(bad);
    bad=valid; bad["operations"][0]["edits"]=Json::array({encode_boundary_geometry_edit(move("a",{0,0}))}); invalid.push_back(bad);
    bad=valid; bad["transforms"]=Json::array(); invalid.push_back(bad);
    for(int dialect:{3,4}) { bad=valid; bad["version"]=dialect; bad["replay_version"]=dialect; invalid.push_back(bad); }
    for(const auto& wire:invalid) expect_invalid([&]{(void)decode_measurement_linework_model(wire);},
        "malformed known batches and batches in historical dialects must fail closed");
    for(const auto& future:std::vector<Json>{{{"version",6},{"opaque",{{"keep",true}}}},
        {{"version",5},{"replay_version",99},{"opaque",Json::array({1,2})}}}) {
        const auto decoded=decode_measurement_linework_model(future);
        require(!decoded.supported() && decoded.original_model==std::optional<Json>{future},
            "future schema or replay must remain exact opaque payload");
    }
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { simultaneous_swap_replays(); translated_shared_and_revisited_vertices();
        typed_arc_and_ordered_derivations(); noop_invalid_and_historical_dialects(); strict_batches_and_future_opaque(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Measurement linework batch tests passed\n";
    return 0;
}
