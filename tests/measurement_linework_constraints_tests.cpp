#include "sketch/constraint_authoring.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void accepted(const ConstraintAuthoringPreview& preview) {
    if (preview.accepted()) return;
    for (const auto& error : preview.diagnostics()) std::cerr << error << '\n';
    throw std::runtime_error("measured constraint preview rejected");
}
Entity stroke(std::string id, std::vector<Vec2> points) {
    MeasurementLinework model; model.stroke_id = id; model.anchor = points.front();
    model.extensions = {{"vendor", "retained"}};
    for (std::size_t i = 1; i < points.size(); ++i) {
        ConstructionReceipt receipt; receipt.segment_id = id + ":e" + std::to_string(i);
        receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.start = points[i - 1]; receipt.chord_end = points[i];
        model.edges.push_back({receipt.segment_id, id + ":v" + std::to_string(i - 1), id + ":v" + std::to_string(i), receipt});
    }
    return {id, "measurement_linework", {{"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"},
        {"layer_id", "l"}, {"model", encode_measurement_linework_model(model)}, {"stroke_color", "#123456"}},
        true, {{"vendor_entity", Json::array({1,2})}}};
}
Document document(std::vector<Entity> strokes) {
    std::vector<Entity> values{{"p", "property", Json::object(), false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}}, false},
        {"l", "layer", {{"floor_id", "f"}}, false}};
    for (auto& value : strokes) values.push_back(std::move(value));
    return Document::create(std::move(values));
}
WallEndpointBinding binding(const std::string& id, int edge, WallEndpointRole role) {
    return {id, role, id + ":e" + std::to_string(edge), id + ":v" + std::to_string(role == WallEndpointRole::start ? edge-1 : edge)};
}
MeasurementLinework model(const Entity& entity) { return *decode_measurement_linework_model(entity.properties.at("model")).model; }
PersistentConstraint relation(std::string id, ConstraintRelationKind kind, std::vector<WallEndpointBinding> endpoints) {
    PersistentConstraint result; result.id = std::move(id); result.relation = kind; result.bindings = std::move(endpoints); return result;
}
void terminal_fixed_length_and_history() {
    auto doc = document({stroke("s", {{0,0},{2,0},{2,3}})});
    const auto before = doc.snapshot();
    auto lock = relation("terminal-lock", ConstraintRelationKind::fixed_length,
        {binding("s",2,WallEndpointRole::start), binding("s",2,WallEndpointRole::end)});
    lock.length = parse_quantity("4 m");
    ConstraintAuthoringIntent intent;
    intent.relation_mutations = {ConstraintRelationMutation::upsert(lock)};
    intent.relation_anchor = lock.bindings.front();
    const auto preview = preview_constraint_authoring(before,intent); accepted(preview);
    require(preview.changed_measured_strokes().size() == 1, "open final endpoint movement must be shown");
    const auto candidate = preview_constraint_authoring_snapshot(before,preview);
    const auto replay = replay_measurement_linework(model(candidate.entities().at("s")));
    require(std::abs(segment_length(replay.edges.back().segment)-4) < 1e-7, "fixed terminal length must solve");
    require(model(candidate.entities().at("s")).edges == model(before.entities().at("s")).edges &&
        candidate.entities().at("s").extensions == before.entities().at("s").extensions,
        "solver must preserve original receipts and metadata");
    auto original_metadata = before.entities().at("s").properties;
    auto candidate_metadata = candidate.entities().at("s").properties;
    original_metadata.erase("model"); candidate_metadata.erase("model");
    require(original_metadata == candidate_metadata && candidate.entities().at("s").required == before.entities().at("s").required,
        "solver must retain drawing context, style, and required geometry status");
    const auto command = constraint_authoring_verified_command(before,preview,nullptr);
    require(std::holds_alternative<ApplyBoundaryConstraintChanges>(command) &&
        std::get<ApplyBoundaryConstraintChanges>(command).measured_source_completion,
        "stroke constraints require typed envelope eleven");
    const auto wire = command_to_json(command);
    require(wire.at("version") == 11 &&
        Document::preview_command(before,command_from_json(wire)).entities() == candidate.entities(),
        "envelope eleven must round-trip and independently reproduce admitted stroke geometry");
    apply_constraint_authoring(doc,preview);
    const auto after = doc.snapshot();
    doc.undo(doc.revision());
    require(doc.snapshot().entities() == before.entities(), "Undo restores relation and derivation atomically");
    doc.redo(doc.revision());
    require(doc.snapshot().entities() == after.entities(), "Redo restores relation and derivation atomically");
    bool stale = false; try { apply_constraint_authoring(doc,preview); } catch (const std::exception&) { stale = true; }
    require(stale && doc.snapshot().entities() == after.entities(), "stale preview must leave document unchanged");
    const auto path = std::filesystem::temp_directory_path() / ("measured-constraints-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bldproj");
    (void)ProjectStore::save(path,doc.snapshot());
    auto reopened = ProjectStore::load(path);
    require(reopened.document.snapshot().entities() == after.entities(), "saved constraints and measured operations must reopen exactly");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == before.entities(), "reopened Undo must restore original measured input");
    std::filesystem::remove(path);
}
void all_relations_and_analysis() {
    for (const auto kind : {ConstraintRelationKind::horizontal,ConstraintRelationKind::vertical,
            ConstraintRelationKind::coincident,ConstraintRelationKind::fixed_length,
            ConstraintRelationKind::parallel,ConstraintRelationKind::perpendicular,ConstraintRelationKind::fixed_anchor}) {
        const bool vertical = kind == ConstraintRelationKind::vertical;
        auto doc = document({stroke("s", {{0,0},{4,0}}), stroke("t", vertical || kind == ConstraintRelationKind::perpendicular
            ? std::vector<Vec2>{{4,0},{4,3}} : std::vector<Vec2>{{4,0},{8,0}})});
        std::vector<WallEndpointBinding> endpoints{binding(vertical ? "t" : "s",1,WallEndpointRole::start),
            binding(vertical ? "t" : "s",1,WallEndpointRole::end)};
        if (kind == ConstraintRelationKind::parallel || kind == ConstraintRelationKind::perpendicular) {
            endpoints.push_back(binding("t",1,WallEndpointRole::start)); endpoints.push_back(binding("t",1,WallEndpointRole::end));
        } else if (kind == ConstraintRelationKind::coincident)
            endpoints = {binding("s",1,WallEndpointRole::end),binding("t",1,WallEndpointRole::start)};
        else if (kind == ConstraintRelationKind::fixed_anchor) endpoints.resize(1);
        auto value = relation("relation",kind,endpoints);
        if (kind == ConstraintRelationKind::fixed_length) value.length = parse_quantity("4000 mm");
        if (kind == ConstraintRelationKind::fixed_anchor) value.anchor = Vec2{0,0};
        ConstraintAuthoringIntent intent; intent.relation_mutations = {ConstraintRelationMutation::upsert(value)};
        const auto preview = preview_constraint_authoring(doc.snapshot(),intent); accepted(preview);
        require(preview.changed_measured_strokes().empty(), "satisfied relation-only admission must preserve exact geometry");
        const auto command = constraint_authoring_verified_command(doc.snapshot(),preview,nullptr);
        require(std::get<ApplyBoundaryConstraintChanges>(command).measured_source_completion &&
            std::get<ApplyBoundaryConstraintChanges>(command).measured_stroke_edits.empty(),
            "relation-only measured participation must retain envelope eleven without geometry edits");
        apply_constraint_authoring(doc,preview);
        require(!validate_constraint_integrity(doc.snapshot().entities()), "persisted measured relation must validate");
        const auto analysis = analyze_persistent_constraint_component(doc.snapshot(),{"s","t"});
        require(analysis.supported && analysis.point_count == 4 && analysis.degrees_of_freedom >= 0,
            "analysis must enumerate both endpoints of each open stroke");
    }
    auto repeated = stroke("r", {{0,0},{2,0},{2,2},{2,0},{3,0}});
    auto repeated_model = model(repeated);
    repeated_model.edges[2].end_vertex_id = "r:v1"; repeated_model.edges[3].start_vertex_id = "r:v1";
    repeated.properties["model"] = encode_measurement_linework_model(repeated_model);
    auto doc = document({repeated});
    require(analyze_persistent_constraint_component(doc.snapshot(),{"r"}).point_count == 4,
        "revisited and retraced vertices must have one solver variable per stable identity");
    auto crossing = stroke("cross",{{0,0},{2,2},{0,2},{2,0},{0,0}});
    auto crossing_model = model(crossing); crossing_model.closed = true;
    crossing_model.edges.back().end_vertex_id = "cross:v0";
    crossing.properties["model"] = encode_measurement_linework_model(crossing_model);
    auto crossing_doc = document({crossing});
    auto horizontal = relation("cross-horizontal",ConstraintRelationKind::horizontal,
        {binding("cross",2,WallEndpointRole::start),binding("cross",2,WallEndpointRole::end)});
    ConstraintAuthoringIntent crossing_intent;
    crossing_intent.relation_mutations = {ConstraintRelationMutation::upsert(horizontal)};
    accepted(preview_constraint_authoring(crossing_doc.snapshot(),crossing_intent));
    require(analyze_persistent_constraint_component(crossing_doc.snapshot(),{"cross"}).point_count == 4,
        "self-crossing closed linework must not acquire boundary winding or area invariants");
}
void authored_resize_and_incompatible_lock() {
    auto doc = document({stroke("s", {{0,0},{2,0},{2,3}})});
    const auto original = model(doc.snapshot().entities().at("s"));
    BoundaryGeometryEdit edit; edit.boundary_id = "s"; edit.kind = BoundaryGeometryEditKind::resize_segment;
    edit.target_id = "s:e1"; edit.target_length_metres = 4;
    ConstraintAuthoringIntent intent; intent.measured_stroke_resize = MeasuredStrokeResizeIntent{edit,parse_quantity("4000 mm"),true};
    auto preview = preview_constraint_authoring(doc.snapshot(),intent); accepted(preview);
    const auto command = std::get<ApplyBoundaryConstraintChanges>(constraint_authoring_verified_command(doc.snapshot(),preview,nullptr));
    require(command.measured_stroke_edits.front().authored_length->original_expression == "4000 mm" &&
        command.measured_stroke_edits.front().authored_edit->kind == BoundaryGeometryEditKind::resize_segment,
        "selected resize must retain exact authored quantity and operation");
    apply_constraint_authoring(doc,preview);
    require(model(doc.snapshot().entities().at("s")).edges == original.edges, "typed resize must preserve original receipts");
    auto lock = relation("fixed",ConstraintRelationKind::fixed_length,
        {binding("s",1,WallEndpointRole::start),binding("s",1,WallEndpointRole::end)});
    lock.length = parse_quantity("4 m");
    intent = {}; intent.relation_mutations = {ConstraintRelationMutation::upsert(lock)};
    preview = preview_constraint_authoring(doc.snapshot(),intent); accepted(preview); apply_constraint_authoring(doc,preview);
    const auto before = doc.snapshot();
    edit.target_length_metres = 5;
    intent = {}; intent.measured_stroke_resize = MeasuredStrokeResizeIntent{edit,parse_quantity("5 m"),true};
    preview = preview_constraint_authoring(before,intent);
    require(!preview.accepted() && preview.candidate_entities() == before.entities() && doc.snapshot().entities() == before.entities(),
        "conflicting fixed length must reject without changing source or candidate");
    auto malformed = lock; malformed.id = "wrong-child"; malformed.bindings.back().vertex_id = "s:v0";
    intent = {}; intent.relation_mutations = {ConstraintRelationMutation::upsert(malformed)};
    require(!preview_constraint_authoring(before,intent).accepted(), "binding role must match its segment's stable endpoint identity");
    malformed = lock; malformed.id = "missing-child"; malformed.bindings.back().segment_id = "absent";
    intent.relation_mutations = {ConstraintRelationMutation::upsert(malformed)};
    require(!preview_constraint_authoring(before,intent).accepted(), "missing measured edge must reject without coordinate fallback");
}
void mixed_terminal_owners() {
    Entity wall{"w","wall",{{"baseline",{{"start",{2,3}},{"end",{6,3}},{"sweep_radians",0}}},
        {"thickness_m",0.14},{"height_m",2.4},{"elevation_m",0}},false};
    Entity boundary{"area","measurement_boundary",{{"segments",Json::array({
        {{"start",{2,3}},{"end",{6,3}},{"sweep_radians",0}},
        {{"start",{6,3}},{"end",{6,6}},{"sweep_radians",0}},
        {{"start",{6,6}},{"end",{2,6}},{"sweep_radians",0}},
        {{"start",{2,6}},{"end",{2,3}},{"sweep_radians",0}}})}},false};
    boundary = upgrade_legacy_boundary_entity(boundary);
    auto doc = document({stroke("s",{{0,0},{2,0},{2,3}}),wall,boundary});
    const auto edge = decode_identified_boundary_entity(boundary).segments.front();
    auto join_wall = relation("wall-join",ConstraintRelationKind::coincident,
        {binding("s",2,WallEndpointRole::end),{"w",WallEndpointRole::start}});
    auto join_area = relation("area-join",ConstraintRelationKind::coincident,
        {binding("s",2,WallEndpointRole::end),{"area",WallEndpointRole::start,edge.segment_id,edge.start_vertex_id}});
    ConstraintAuthoringIntent intent; intent.relation_mutations = {ConstraintRelationMutation::upsert(join_wall),ConstraintRelationMutation::upsert(join_area)};
    auto preview = preview_constraint_authoring(doc.snapshot(),intent); accepted(preview); apply_constraint_authoring(doc,preview);
    const auto before = doc.snapshot();
    BoundaryGeometryEdit edit; edit.boundary_id = "s"; edit.target_id = "s:v2"; edit.target_position = {3,4};
    intent = {}; intent.measured_stroke_vertex_move = MeasuredStrokeVertexMoveIntent{edit,true};
    preview = preview_constraint_authoring(before,intent); accepted(preview);
    require(!preview.changed_walls().empty() && !preview.changed_boundaries().empty(), "terminal stroke move must propagate through explicit mixed-owner relations");
    const auto candidate = preview_constraint_authoring_snapshot(before,preview);
    const auto actual = candidate.entities().at("w").properties.at("baseline").at("start");
    require(std::abs(actual[0].get<double>()-3)<1e-7 && std::abs(actual[1].get<double>()-4)<1e-7,
        "wall endpoint must follow moved terminal stable vertex");
    intent.measured_stroke_vertex_move->move_related_objects = false;
    require(!preview_constraint_authoring(before,intent).accepted(), "frozen mixed owners must reject incompatible stroke movement");
}
void analytical_arc_reflection() {
    auto curve = stroke("s",{{0,0},{2,0}}); auto source = model(curve);
    source.edges.front().receipt.kind = BoundaryConstructionKind::arc_chord_angle;
    source.edges.front().receipt.angle = parse_angle("90 deg");
    curve.properties["model"] = encode_measurement_linework_model(source);
    auto doc = document({curve});
    constexpr double physical = 2.221441469079183;
    auto lock = relation("arc",ConstraintRelationKind::fixed_arc_length,
        {binding("s",1,WallEndpointRole::start),binding("s",1,WallEndpointRole::end)});
    // Keep a decimal receipt accurate enough for the persisted linear tolerance.
    lock.length = parse_quantity("2.221441469079183 m");
    ConstraintAuthoringIntent intent; intent.relation_mutations = {ConstraintRelationMutation::upsert(lock)};
    auto preview = preview_constraint_authoring(doc.snapshot(),intent); accepted(preview); apply_constraint_authoring(doc,preview);
    intent = {}; intent.measured_stroke_transform = MeasuredStrokeTransformIntent{{{"s",{{},0,true,false,{5,0}}}},true};
    preview = preview_constraint_authoring(doc.snapshot(),intent); accepted(preview);
    const auto candidate = preview_constraint_authoring_snapshot(doc.snapshot(),preview);
    const auto edited = model(candidate.entities().at("s"));
    const auto segment = replay_measurement_linework(edited).edges.front().segment;
    require(segment.sweep_radians == -std::numbers::pi/2 && std::abs(segment_length(segment)-physical)<1e-9 &&
        edited.edges == source.edges, "reflection must retain analytical arc length, reverse signed sweep, and preserve original receipts");
    const auto command = std::get<ApplyBoundaryConstraintChanges>(constraint_authoring_verified_command(doc.snapshot(),preview,nullptr));
    require(command.measured_stroke_edits.front().rigid_transform.has_value(), "reflection must retain exact rigid transform intent");
}
void opaque_owner_and_batch_failure_paths() {
    auto future = stroke("future",{{0,0},{2,0}});
    future.properties["model"]["version"] = 99;
    auto doc = document({future,stroke("known",{{0,2},{2,2}})});
    require(!doc.is_editable(), "future measured model must remain opaque and make document read-only");
    auto values = doc.snapshot().entities();
    auto future_lock = relation("future-lock",ConstraintRelationKind::horizontal,
        {binding("future",1,WallEndpointRole::start),binding("future",1,WallEndpointRole::end)});
    values.emplace(future_lock.id,encode_constraint_entity(future_lock));
    require(validate_constraint_integrity(values).has_value(), "known relation on opaque stroke must preserve read-only status");
    auto invalid = relation("known-invalid",ConstraintRelationKind::vertical,
        {binding("known",1,WallEndpointRole::start),binding("known",1,WallEndpointRole::end)});
    values.emplace(invalid.id,encode_constraint_entity(invalid));
    bool rejected = false; try { (void)validate_constraint_integrity(values); } catch (const std::exception&) { rejected = true; }
    require(rejected, "opaque owner must not hide an unrelated unsatisfied known constraint");
    const auto original = model(stroke("s",{{0,0},{2,0}}));
    BoundaryGeometryEdit first; first.boundary_id = "s"; first.target_id = "s:v0"; first.target_position = {2,0};
    auto last = first; last.target_id = "s:v1"; last.target_position = {4,0};
    rejected = false; try { (void)edited_measurement_linework(original,first); } catch (const std::exception&) { rejected = true; }
    require(rejected, "single intermediate collapsed edge must reject");
    const auto simultaneous = edited_measurement_linework_vertices(original,{first,last});
    const auto geometry = replay_measurement_linework(simultaneous).edges.front().segment;
    require(geometry.start.x == 2 && geometry.end.x == 4 && simultaneous.edges == original.edges,
        "simultaneous final-valid batch must bypass intermediate collapse without rewriting receipts");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        terminal_fixed_length_and_history(); all_relations_and_analysis(); authored_resize_and_incompatible_lock();
        mixed_terminal_owners(); analytical_arc_reflection(); opaque_owner_and_batch_failure_paths();
        std::cout << "Measured linework constraints tests passed\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
