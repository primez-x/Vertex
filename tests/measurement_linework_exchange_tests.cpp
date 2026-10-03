#include "sketch/measurement_linework.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Document fixture(bool closed = false, bool future = false) {
    MeasurementLinework model;
    model.stroke_id = "stroke"; model.anchor = {0, 0}; model.closed = closed;
    const auto edge = [&](std::string id, std::string a, std::string b, Vec2 start, Vec2 end) {
        ConstructionReceipt receipt;
        receipt.segment_id = id; receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.start = start; receipt.chord_end = end;
        model.edges.push_back({id, a, b, receipt});
    };
    edge("e0", "v0", "v1", {0,0}, {3,0});
    edge("e1", "v1", "v2", {3,0}, {3,2});
    if (closed) edge("e2", "v2", "v0", {3,2}, {0,0});
    else {
        auto& receipt = model.edges.back().receipt;
        receipt.kind = BoundaryConstructionKind::arc_chord_angle;
        receipt.angle = parse_angle("-90 deg");
    }
    auto encoded = encode_measurement_linework_model(model);
    if (future) encoded["replay_version"] = 2;
    return Document::create({
        {"p", "property", {{"name", "Property"}}, false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}}, false},
        {"l", "layer", {{"floor_id", "f"}, {"name", "Measured lines"}}, false},
        {"stroke", "measurement_linework", {{"property_id", "p"}, {"building_id", "b"},
            {"floor_id", "f"}, {"layer_id", "l"}, {"model", encoded}}, true}
    });
}
bool has_diagnostic(const DxfProjectExportResult& result, const char* code) {
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.source_id == "stroke" && diagnostic.code == code) return true;
    return false;
}
void test_open_curve() {
    const auto document = fixture(); const auto source = document.snapshot().entities();
    const auto result = export_project_dxf(document.snapshot());
    require(result.drawing.polylines.size() == 1, "open linework must export a connected DXF polyline");
    const auto& polyline = result.drawing.polylines.front();
    require(!polyline.closed && polyline.vertices.size() == 3, "open linework must not gain a false closing edge");
    require(polyline.layer == "Measured lines", "linework exports to its actual layer name");
    require(polyline.vertices[0].point.x == 0 && polyline.vertices[1].point.x == 3 &&
        polyline.vertices[2].point.y == 2, "polyline retains authored endpoints");
    require(std::abs(polyline.vertices[1].bulge - std::tan(-std::numbers::pi / 8)) < 1e-12,
        "negative analytical arc keeps its signed bulge");
    require(has_diagnostic(result, "measurement_linework_inputs_not_representable"),
        "plain DXF export must disclose loss of exact receipt expressions and identities");
    require(!has_diagnostic(result, "open_boundary"), "legitimate open linework is not an invalid area");
    const auto transport = parse_dxf_ascii(export_dxf_ascii(result.drawing));
    require(transport.diagnostics.empty() && transport.drawing.polylines.size() == 1 &&
        !transport.drawing.polylines.front().closed, "serialized DXF retains open curve geometry");
    require(document.snapshot().entities() == source, "export never modifies source receipts");
}
void test_closed_and_future() {
    const auto closed = export_project_dxf(fixture(true).snapshot());
    require(closed.drawing.polylines.size() == 1 && closed.drawing.polylines.front().closed &&
        closed.drawing.polylines.front().vertices.size() == 3, "closed linework retains its explicit closure");
    const auto future_document = fixture(false, true);
    require(!future_document.snapshot().is_editable(), "future linework is read only");
    const auto future = export_project_dxf(future_document.snapshot());
    require(future.drawing.polylines.empty() && future.drawing.lines.empty() && future.drawing.arcs.empty() &&
        has_diagnostic(future, "measurement_linework_model_unsupported"), "future receipt geometry is never guessed for export");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { test_open_curve(); test_closed_and_future(); std::cout << "measurement linework DXF checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
