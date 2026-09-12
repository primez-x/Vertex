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
struct DxfDrawing {
    int insertion_units{}; // R2013 $INSUNITS 0..20, 0 = unspecified. No conversion.
    std::vector<DxfLine> lines;
    std::vector<DxfArc> arcs;
    std::vector<DxfPolyline> polylines;
    std::vector<DxfLabel> labels;
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
// Canonical entity order: lines, arcs, polylines, labels (vector order retained).
// Unsupported style/3D information is not representable and is never synthesized.
[[nodiscard]] std::string export_dxf_ascii(
    const DxfDrawing& drawing, const DxfExchangeLimits& limits = {});

} // namespace sketch
