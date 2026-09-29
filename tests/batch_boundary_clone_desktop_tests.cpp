#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_transform.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>
#include <numbers>
#include <set>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_batch_clone() {
    const Vec2 points[]{{10, 0}, {14, 0}, {14, 4}, {10, 4}};
    const char* rises[]{"0 m", "4 m", "0 m", "-4 m"};
    const char* runs[]{"4 m", "0 m", "-4 m", "0 m"};
    BoundaryConstructionRecord record;
    record.boundary_id = "boundary";
    record.anchor = points[0];
    IdentifiedBoundary boundary{record.boundary_id, "measurement_boundary", {}};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto edge = "edge-" + std::to_string(i);
        const auto start = "vertex-" + std::to_string(i);
        const auto end = "vertex-" + std::to_string((i + 1) % 4);
        ConstructionReceipt receipt;
        receipt.segment_id = edge;
        receipt.kind = BoundaryConstructionKind::line_rise_run;
        receipt.start = points[i];
        receipt.rise = parse_quantity(rises[i]);
        receipt.run = parse_quantity(runs[i]);
        record.edges.push_back({edge, start, end, receipt});
        boundary.segments.push_back({edge, start, end, {points[i], points[(i + 1) % 4], 0}});
    }
    auto entity = encode_identified_boundary_entity(boundary);
    entity.properties["boundary_authoring"] = encode_boundary_receipt_envelope(record);
    const auto dimension = encode_boundary_dimension_entity(
        BoundaryDimension{"dimension", entity.id, "edge-0", {2, -1},
                          BoundaryDimensionPlacement::manual, {}});
    desktop::MainWindow window;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(entity), EntityChange::upsert(dimension)}, {}, "Receipt fixture"});
    ApplyBoundaryConstraintChanges batch{window.document().revision(), {}, {}, "Simultaneous translation"};
    for (std::size_t i = 0; i < 4; ++i)
        batch.boundary_edits.push_back({entity.id, BoundaryGeometryEditKind::move_vertex,
            "vertex-" + std::to_string(i), {points[i].x - 10, points[i].y}});
    // An intermediate sequential move crosses the still-unmoved opposite edge.
    auto sequential = window.document().snapshot().entities();
    bool rejected = false;
    for (const auto& edit : batch.boundary_edits) {
        try { sequential = edited_boundary_entities(sequential, edit); }
        catch (const std::exception&) { rejected = true; break; }
    }
    require(rejected, "fixture must require simultaneous vertex application");
    window.document().apply(batch);
    const auto before = window.document().snapshot();
    const auto& source = before.entities().at(entity.id);
    const auto& source_proof = source.extensions.at("boundary_geometry_derivation");
    require(source_proof.at("operations").size() == 1 &&
        source_proof.at("operations")[0].at("kind") == "vertex_batch",
        "constraint solve must retain a batch derivation");
    require(window.selectEntity(QString::fromStdString(entity.id)), "source must be selectable");
    if (!window.transformSelectedBoundary("90", false, false, "7 m", "3 m", true))
        throw std::runtime_error("batch clone failed: " + window.lastError().toStdString());
    const auto after = window.document().snapshot();
    const auto clone_id = window.selectedEntityId().toStdString();
    require(after.revision() == before.revision() + 1 && clone_id != entity.id &&
        after.entities().size() == before.entities().size() + 2,
        "clone and dimension must commit together in one revision");
    for (const auto& [id, original] : before.entities())
        require(after.entities().at(id) == original, "clone must preserve every source entity exactly");
    const auto& clone = after.entities().at(clone_id);
    const auto model = decode_identified_boundary_entity(clone);
    std::set<std::string> source_ids{entity.id};
    for (const auto& edge : boundary.segments) {
        source_ids.insert(edge.segment_id);
        source_ids.insert(edge.start_vertex_id);
    }
    const PlanarTransform transform{{2, 2}, std::numbers::pi / 2, false, false, {7, 3}};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& edge = model.segments[i];
        require(!source_ids.contains(edge.segment_id) && !source_ids.contains(edge.start_vertex_id) &&
            !source_ids.contains(edge.end_vertex_id), "clone must not reuse source child IDs");
        const auto expected = transform_point(batch.boundary_edits[i].target_position, transform);
        require(edge.segment.start.x == expected.x && edge.segment.start.y == expected.y,
            "clone geometry must exactly match batch followed by transform");
    }
    const auto& proof = clone.extensions.at("boundary_geometry_derivation");
    require(proof.at("operations").size() == 2 &&
        proof.at("operations")[0].at("kind") == "vertex_batch" &&
        proof.at("operations")[1].at("kind") == "transform",
        "clone must preserve batch grouping and append the requested transform");
    const auto clone_receipt = *decode_boundary_receipt_envelope(proof.at("source_boundary_authoring")).record;
    auto expected_receipt = record;
    expected_receipt.boundary_id = clone_id;
    // Identity remapping uses the existing schema-three identity frame.
    expected_receipt.schema_version = boundary_receipt_schema_version_v3;
    expected_receipt.transforms.push_back({});
    for (std::size_t i = 0; i < 4; ++i) {
        auto& edge = expected_receipt.edges[i];
        edge.segment_id = edge.receipt.segment_id = model.segments[i].segment_id;
        edge.start_vertex_id = model.segments[i].start_vertex_id;
        edge.end_vertex_id = model.segments[i].end_vertex_id;
        auto expected = batch.boundary_edits[i];
        expected.boundary_id = clone_id;
        expected.target_id = edge.start_vertex_id;
        require(proof.at("operations")[0].at("value")[i] == encode_boundary_geometry_edit(expected),
            "every batch intent must be remapped without changing its coordinates");
    }
    require(encode_boundary_receipt_envelope(clone_receipt) == encode_boundary_receipt_envelope(expected_receipt),
        "clone must retain exact construction evidence in its remapped identity frame");
    require(proof.at("operations")[1].at("value") ==
        encode_boundary_transform({clone_id, transform}), "clone transform proof must be exact");
    bool found_dimension = false;
    for (const auto& [id, candidate] : after.entities()) {
        if (before.entities().contains(id) || id == clone_id) continue;
        const auto decoded = decode_boundary_dimension_entity(candidate);
        require(decoded.supported(), "cloned companion must be a supported dimension");
        const auto& copied = *decoded.dimension;
        const auto expected = transform_point({2, -1}, transform);
        require(copied.boundary_id == clone_id && copied.segment_id == model.segments[0].segment_id &&
            copied.text_position.x == expected.x && copied.text_position.y == expected.y,
            "dimension must target clone identities and follow its transform");
        found_dimension = true;
    }
    require(found_dimension && !validate_boundary_integrity(after.entities()),
        "cloned geometry and proof must pass deterministic integrity replay");
    require(Document::fork(after).snapshot().entities() == after.entities(), "history replay must be exact");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(),
        "undo must restore exact pre-clone state");
    require(window.redoCommand() && window.document().snapshot().entities() == after.entities(),
        "redo must restore exact clone and dimension");
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary project directory required");
    const auto path = temporary.filePath("batch-clone.bldproj");
    require(window.saveProjectAs(path), "clone project must save");
    desktop::MainWindow reopened;
    require(reopened.openProject(path) && reopened.document().snapshot().entities() == after.entities(),
        "save and reopen must retain exact source, clone, dimensions and proofs");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    try {
        const auto font = QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        require(font >= 0, "bundled Inter font required");
        application.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 10));
        test_batch_clone();
        std::cout << "Batch boundary clone desktop tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
