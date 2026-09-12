#include "sketch/room_relationship_geometry_commit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/document_digest.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sketch;
using K = RoomReferenceKind;
using R = RoomRelationKind;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Segment line(Vec2 start, Vec2 end) { return {start, end, 0.0}; }

IdentifiedBoundary boundary(std::string id, std::string type, double left, double bottom,
                            double right, double top) {
    const std::vector<Vec2> points{{left, bottom}, {right, bottom}, {right, top}, {left, top}};
    IdentifiedBoundary result{std::move(id), std::move(type), {}};
    for (std::size_t index = 0; index < points.size(); ++index) {
        result.segments.push_back({"edge-" + std::to_string(index),
                                   "vertex-" + std::to_string(index),
                                   "vertex-" + std::to_string((index + 1) % points.size()),
                                   line(points[index], points[(index + 1) % points.size()])});
    }
    return result;
}

Entity receipt_boundary_entity(std::string id) {
    const auto model = boundary(id, "room_boundary", 0.0, 0.0, 4.0, 3.0);
    BoundaryConstructionRecord record;
    record.boundary_id = id;
    record.anchor = {0.0, 0.0};
    const auto points = std::vector<Vec2>{{0.0, 0.0}, {4.0, 0.0}, {4.0, 3.0}, {0.0, 3.0}};
    const auto rises = std::vector<std::string>{"0 m", "3 m", "0 m", "-3 m"};
    const auto runs = std::vector<std::string>{"4 m", "0 m", "-4 m", "0 m"};
    for (std::size_t index = 0; index < points.size(); ++index) {
        ConstructionReceipt receipt;
        receipt.segment_id = model.segments[index].segment_id;
        receipt.kind = BoundaryConstructionKind::line_rise_run;
        receipt.start = points[index];
        receipt.rise = parse_quantity(rises[index]);
        receipt.run = parse_quantity(runs[index]);
        record.edges.push_back({model.segments[index].segment_id,
                                model.segments[index].start_vertex_id,
                                model.segments[index].end_vertex_id, receipt});
    }
    auto result = encode_identified_boundary_entity(model);
    result.properties["boundary_authoring"] = encode_boundary_receipt_envelope(record);
    return result;
}

RoomRelationshipSnapshot relationship_model() {
    return RoomRelationshipSnapshot::create(
        {{"room", K::room_boundary}, {"wall", K::architectural_wall}},
        {{"room", "wall", R::follows}});
}

Document make_document() {
    const auto room = encode_identified_boundary_entity(
        boundary("room", "room_boundary", 0.0, 0.0, 4.0, 3.0));
    const auto relationships = Entity{"relationships", "room_relationships",
                                      {{"model", relationship_model().to_json()},}, false,
                                      nlohmann::json::object()};
    const auto wall = Entity{"wall", "wall",
                             {{"baseline", {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
                                             {"sweep_radians", 0.0}}},
                              {"thickness_m", 0.2}, {"height_m", 2.7}, {"elevation_m", 0.0}},
                             false, nlohmann::json::object()};
    return Document::create({room, wall, relationships});
}

Document make_receipt_document() {
    const auto room = receipt_boundary_entity("room");
    const auto relationships = Entity{"relationships", "room_relationships",
                                      {{"model", relationship_model().to_json()},}, false,
                                      nlohmann::json::object()};
    const auto wall = Entity{"wall", "wall",
                             {{"baseline", {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
                                             {"sweep_radians", 0.0}}},
                              {"thickness_m", 0.2}, {"height_m", 2.7}, {"elevation_m", 0.0}},
                             false, nlohmann::json::object()};
    return Document::create({room, wall, relationships});
}

void require_contains(const std::vector<std::string>& diagnostics, const char* needle) {
    require(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& value) {
                return value.find(needle) != std::string::npos;
            }),
            needle);
}

void test_preview_and_apply() {
    auto document = make_document();
    const auto source = document.snapshot();
    const auto model = relationship_model();
    auto after = std::vector<RelationshipGeometry>{
        {"room", K::room_boundary,
         {{ {0.0, 0.0}, {4.0, 0.0}, 0.0}, {{4.0, 0.0}, {4.0, 3.0}, 0.0},
           {{4.0, 3.0}, {0.0, 3.0}, 0.0}, {{0.0, 3.0}, {0.0, 0.0}, 0.0}}},
        {"wall", K::architectural_wall, {{{5.0, 2.0}, {9.0, 2.0}, 0.0}}}};
    const auto preview = preview_room_relationship_geometry(source, model, after);
    require(preview.accepted(), "valid relationship geometry should be accepted");
    require(preview.changes().size() == 1 && preview.changes().front().source_id == "room",
            "preview should contain the moved room");
    require(preview.changes().front().source_kind == K::room_boundary,
            "preview should retain the source semantic role");
    require(document_snapshot_digest(source) == preview.source_snapshot_digest(),
            "preview source digest should bind to the complete source");
    auto workspace_copy = Document::fork(source);
    const auto workspace_command = make_room_relationship_geometry_command(
        workspace_copy.snapshot(), preview);
    require(workspace_copy.apply(Command{workspace_command}) == 1,
            "workspace hosts should apply the ordinary relationship command");
    const auto workspace_room = decode_identified_boundary_entity(
        workspace_copy.snapshot().entities().at("room"));
    require(workspace_room.segments.front().segment.start.x == 5.0 &&
                workspace_room.segments.front().segment.start.y == 2.0,
            "workspace command should produce the propagated entity");
    const auto revision = apply_room_relationship_geometry(document, preview);
    require(revision == 1, "relationship geometry should commit as one revision");
    const auto committed = decode_identified_boundary_entity(
        document.snapshot().entities().at("room"));
    require(committed.segments.front().segment.start.x == 5.0 &&
                committed.segments.front().segment.start.y == 2.0,
            "committed room geometry did not follow the moved wall");
    require(document.can_undo(), "relationship geometry should be undoable");
    document.undo(document.revision());
    const auto undone = decode_identified_boundary_entity(document.snapshot().entities().at("room"));
    require(undone.segments.front().segment.start.x == 0.0 &&
                undone.segments.front().segment.start.y == 0.0,
            "undo should restore the original room geometry");
    document.redo(document.revision());
    const auto redone = decode_identified_boundary_entity(document.snapshot().entities().at("room"));
    require(redone.segments.front().segment.start.x == 5.0 &&
                redone.segments.front().segment.start.y == 2.0,
            "redo should restore propagated room geometry");
}

void test_stale_and_rejected_previews() {
    auto document = make_document();
    const auto source = document.snapshot();
    auto after = std::vector<RelationshipGeometry>{
        {"room", K::room_boundary,
         {{{0.0, 0.0}, {4.0, 0.0}, 0.0}, {{4.0, 0.0}, {4.0, 3.0}, 0.0},
          {{4.0, 3.0}, {0.0, 3.0}, 0.0}, {{0.0, 3.0}, {0.0, 0.0}, 0.0}}},
        {"wall", K::architectural_wall, {{{5.0, 2.0}, {9.0, 2.0}, 0.0}}}};
    const auto preview = preview_room_relationship_geometry(source, relationship_model(), after);
    require(preview.accepted(), "stale preview fixture should be accepted before intervening edit");
    document.apply(NameRevision{document.revision(), "before relationship commit"});
    try {
        (void)apply_room_relationship_geometry(document, preview);
        throw std::runtime_error("stale relationship preview was accepted");
    } catch (const DocumentError& error) {
        require(error.code() == DocumentErrorCode::stale_revision,
                "stale relationship preview should report stale revision");
    }

    auto unsafe = after;
    unsafe.back().geometry = {{{0.0, 0.0}, {8.0, 0.0}, 0.0}};
    const auto rejected = preview_room_relationship_geometry(source, relationship_model(), unsafe);
    require(!rejected.accepted() && rejected.changes().empty(),
            "non-rigid relationship edit should be rejected");
    require_contains(rejected.diagnostics(), "non-rigid");
}

void test_receipt_backed_commit_and_restore() {
    auto document = make_receipt_document();
    const auto source = document.snapshot();
    auto after = std::vector<RelationshipGeometry>{
        {"room", K::room_boundary,
         {{{0.0, 0.0}, {4.0, 0.0}, 0.0}, {{4.0, 0.0}, {4.0, 3.0}, 0.0},
          {{4.0, 3.0}, {0.0, 3.0}, 0.0}, {{0.0, 3.0}, {0.0, 0.0}, 0.0}}},
        {"wall", K::architectural_wall, {{{5.0, 2.0}, {9.0, 2.0}, 0.0}}}};
    const auto preview = preview_room_relationship_geometry(source, relationship_model(), after);
    require(preview.accepted(), "receipt-backed relationship preview should be accepted");
    require(apply_room_relationship_geometry(document, preview) == 1,
            "receipt-backed relationship should commit as one revision");
    const auto committed = document.snapshot().entities().at("room");
    const auto receipt = decode_boundary_receipt_envelope(
        committed.properties.at("boundary_authoring"));
    require(receipt.supported() && receipt.record->schema_version == boundary_receipt_schema_version_v3,
            "relationship commit should retain a transformed construction receipt");
    const auto replay = replay_boundary_construction(*receipt.record);
    require(replay.edges.front().segment.start.x == 5.0 &&
                replay.edges.front().segment.start.y == 2.0,
            "transformed construction receipt did not replay committed geometry");
    auto reopened = Document::fork(document.snapshot());
    require(reopened.snapshot().entities().at("room") == committed,
            "reopened document changed receipt-backed relationship geometry");
    reopened.undo(reopened.revision());
    const auto undone = decode_identified_boundary_entity(reopened.snapshot().entities().at("room"));
    require(undone.segments.front().segment.start.x == 0.0 &&
                undone.segments.front().segment.start.y == 0.0,
            "receipt-backed relationship undo did not restore original geometry");
}

void run() {
    test_preview_and_apply();
    test_stale_and_rejected_previews();
    test_receipt_backed_commit_and_restore();
}
} // namespace

int main() {
    try {
        run();
        std::cout << "Room relationship geometry commit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
