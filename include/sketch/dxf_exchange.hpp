#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// In-memory, unit-preserving 2D exchange records; no project or filesystem access.
struct DxfPoint { double x{}, y{}; };
struct DxfLine { DxfPoint start, end; std::string layer{"0"}; };
struct DxfArc {
    DxfPoint center;
    double radius{}, start_degrees{}, end_degrees{}; // Counterclockwise in XY.
    std::string layer{"0"};
};
struct DxfVertex { DxfPoint point; double bulge{}; }; // tan(signed sweep / 4).
struct DxfPolyline {
    std::vector<DxfVertex> vertices;
    bool closed{};
    std::string layer{"0"};
};
struct DxfLabel {
    DxfPoint position;
    double height{1}, rotation_degrees{};
    std::string text;
    std::string layer{"0"};
};
// A bounded linear dimension retains the two extension points, the dimension
// line location, rotation, and an optional text override. Associative blocks,
// tolerances, alternate units, and annotative styles are outside this subset.
struct DxfDimension {
    DxfPoint extension_start;
    DxfPoint extension_end;
    DxfPoint dimension_line;
    DxfPoint text_position;
    double rotation_degrees{};
    std::string text;
    std::string layer{"0"};
};
// A solid hatch is represented by one closed planar polygon. Patterned,
// multi-loop, associative, and edge-defined hatches are reported as
// unsupported rather than flattened into a misleading fill.
struct DxfHatch {
    std::vector<DxfPoint> boundary;
    bool solid{true};
    std::string layer{"0"};
};
// A block definition contains the same bounded 2D primitive families as the
// main drawing. Nested blocks, dimensions, hatches, and attributes are not
// representable in this first block subset.
struct DxfBlock {
    std::string name;
    DxfPoint base;
    std::vector<DxfLine> lines;
    std::vector<DxfArc> arcs;
    std::vector<DxfPolyline> polylines;
    std::vector<DxfLabel> labels;
};
struct DxfInsert {
    std::string block_name;
    DxfPoint insertion;
    double scale_x{1};
    double scale_y{1};
    double rotation_degrees{};
    std::string layer{"0"};
};
struct DxfDrawing {
    int insertion_units{}; // R2013 $INSUNITS 0..20, 0 = unspecified. No conversion.
    std::vector<DxfLine> lines;
    std::vector<DxfArc> arcs;
    std::vector<DxfPolyline> polylines;
    std::vector<DxfLabel> labels;
    std::vector<DxfDimension> dimensions;
    std::vector<DxfHatch> hatches;
    std::vector<DxfBlock> blocks;
    std::vector<DxfInsert> inserts;
};
struct DxfDiagnostic {
    std::size_t entity_index{}; // One-based ENTITIES ordinal; zero for a section.
    std::string entity_type;
    std::string code;
};
struct DxfImportResult {
    DxfDrawing drawing;
    std::vector<DxfDiagnostic> diagnostics;
};
struct DxfExchangeLimits {
    std::size_t max_bytes{16 * 1024 * 1024};
    std::size_t max_pairs{500'000};
    std::size_t max_entities{50'000};
    std::size_t max_vertices{100'000}; // Aggregate across all polylines.
    std::size_t max_string_bytes{255};
};

// Strict AC1027 ASCII subset. Malformed data/limits throw std::invalid_argument.
// Unsupported entities/features are omitted with stable diagnostics, never executed.
[[nodiscard]] DxfImportResult parse_dxf_ascii(
    std::string_view bytes, const DxfExchangeLimits& limits = {});
// Canonical entity order: block definitions, lines, arcs, polylines,
// dimensions, hatches, labels, then INSERT references (vector order retained
// within each family).
// Unsupported style/3D information is not representable and is never synthesized.
[[nodiscard]] std::string export_dxf_ascii(
    const DxfDrawing& drawing, const DxfExchangeLimits& limits = {});

} // namespace sketch
