#include "sketch/dxf_exchange.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "invalid input accepted");
}
std::string file(std::string entities) {
    return "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1027\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n" + entities + "0\nENDSEC\n0\nEOF\n";
}
void run() {
    using namespace sketch;
    DxfDrawing d;
    d.insertion_units = 2;
    d.lines.push_back({{1.25, -2}, {10, 3}, "Walls"});
    d.arcs.push_back({{4, 5}, 3.5, 300, 30, "Curves"});
    d.polylines.push_back({{{{0, 0}, 0.5}, {{3, 4}, -1}, {{8, 0}, 0}}, true, "Boundary"});
    d.labels.push_back({{5, 7}, 2, 45, "  Room A  ", "Labels"});
    d.dimensions.push_back({{1, 2}, {8, 2}, {4.5, 3}, {4.5, 3.5}, 0, "7 m", "Dimensions"});
    d.hatches.push_back({{{0, 0}, {4, 0}, {4, 3}, {0, 3}}, true, "Fill"});
    d.blocks.push_back({"Window", {0, 0}, {{{0, 0}, {1.2, 0}, "Frame"}}, {}, {}, {}});
    d.inserts.push_back({"Window", {12, 4}, 1.0, 1.5, 90, "Openings"});
    const auto encoded = export_dxf_ascii(d);
    const auto imported = parse_dxf_ascii(encoded);
    check(imported.diagnostics.empty(), "own export loses fidelity");
    check(imported.drawing.insertion_units == 2, "units lost");
    check(imported.drawing.lines.at(0).start.x == 1.25, "line coordinate lost");
    check(imported.drawing.arcs.at(0).start_degrees == 300, "arc sweep lost");
    check(imported.drawing.polylines.at(0).vertices.at(1).bulge == -1, "bulge lost");
    check(imported.drawing.polylines.at(0).closed, "closure lost");
    check(imported.drawing.labels.at(0).text == "  Room A  ", "text whitespace lost");
    check(imported.drawing.dimensions.at(0).extension_end.x == 8 &&
              imported.drawing.dimensions.at(0).text_position.y == 3.5 &&
              imported.drawing.dimensions.at(0).text == "7 m",
          "linear dimension lost");
    check(imported.drawing.hatches.at(0).boundary.size() == 4 &&
              imported.drawing.hatches.at(0).solid,
          "solid hatch lost");
    check(encoded.find("92\n3\n72\n0\n73\n1\n") != std::string::npos,
          "exported HATCH boundary does not declare an external polyline path");
    const std::string polygon_hatch =
        "0\nHATCH\n10\n0\n20\n0\n30\n0\n2\nSOLID\n70\n1\n71\n0\n"
        "91\n1\n92\n2\n72\n0\n73\n1\n93\n3\n10\n0\n20\n0\n"
        "10\n3\n20\n0\n10\n0\n20\n2\n97\n0\n75\n0\n76\n1\n";
    const auto polygon_import = parse_dxf_ascii(file(polygon_hatch));
    check(polygon_import.diagnostics.empty() && polygon_import.drawing.hatches.size() == 1 &&
              polygon_import.drawing.hatches[0].boundary[1].x == 3,
          "standard polyline HATCH boundary rejected");
    auto external_polygon_hatch = polygon_hatch;
    external_polygon_hatch.replace(external_polygon_hatch.find("92\n2\n"), 5, "92\n3\n");
    const auto external_polygon_import = parse_dxf_ascii(file(external_polygon_hatch));
    check(external_polygon_import.diagnostics.empty() && external_polygon_import.drawing.hatches.size() == 1,
          "standard external polyline HATCH boundary rejected");
    check(export_dxf_ascii(polygon_import.drawing) == export_dxf_ascii(external_polygon_import.drawing),
          "single-loop polygon HATCH boundary flags did not canonicalize");
    auto wrong_polygon_hatch = polygon_hatch;
    wrong_polygon_hatch.replace(wrong_polygon_hatch.find("92\n2\n"), 5, "92\n1\n");
    const auto wrong_polygon_import = parse_dxf_ascii(file(wrong_polygon_hatch));
    check(wrong_polygon_import.drawing.hatches.empty() && wrong_polygon_import.diagnostics.size() == 1 &&
              wrong_polygon_import.diagnostics[0].code == "unsupported_feature",
          "non-polyline HATCH path interpreted as polygon vertices");
    check(imported.drawing.blocks.at(0).name == "Window" &&
              imported.drawing.blocks.at(0).lines.size() == 1 &&
              imported.drawing.inserts.at(0).block_name == "Window" &&
              imported.drawing.inserts.at(0).scale_y == 1.5,
          "block definition or insert lost");
    check(export_dxf_ascii(imported.drawing) == encoded, "round trip not deterministic");
    auto metadata = d;
    metadata.blocks[0].vertex_entity_json = "{\"opaque\":\"" + std::string(500, 'x') + "\xc3\xa9\"}";
    const auto metadata_bytes = export_dxf_ascii(metadata);
    check(metadata_bytes.find("2\nAPPID\n") != std::string::npos &&
          metadata_bytes.find("1001\nVERTEX_ENTITY_V1\n") != std::string::npos,
          "native XDATA must register its application");
    const auto metadata_import = parse_dxf_ascii(metadata_bytes);
    check(metadata_import.diagnostics.empty() && metadata_import.drawing.blocks[0].vertex_entity_json == metadata.blocks[0].vertex_entity_json,
          "bounded UTF-8 XDATA chunks must round trip exactly");
    auto foreign_metadata = metadata_bytes;
    const auto native_marker = foreign_metadata.find("1001\nVERTEX_ENTITY_V1\n");
    foreign_metadata.insert(native_marker, "1001\nFOREIGN_APP\n1000\nforeign opaque data\n");
    const auto foreign_import = parse_dxf_ascii(foreign_metadata);
    check(foreign_import.drawing.blocks[0].vertex_entity_json == metadata.blocks[0].vertex_entity_json &&
          foreign_import.drawing.blocks[0].lines.size() == 1,
          "foreign XDATA application strings must not contaminate native payload or geometry");
    auto duplicate_metadata = metadata_bytes;
    duplicate_metadata.insert(duplicate_metadata.find("0\nLINE\n", native_marker), "1001\nVERTEX_ENTITY_V1\n1000\n{}\n");
    const auto duplicate_import = parse_dxf_ascii(duplicate_metadata);
    check(!duplicate_import.diagnostics.empty() && duplicate_import.drawing.blocks[0].lines.size() == 1 &&
          duplicate_import.drawing.blocks[0].vertex_entity_json == "!invalid_vertex_xdata",
          "duplicate native XDATA must be inert while block geometry survives");
    auto unregistered = metadata_bytes;
    const auto table_start = unregistered.find("0\nSECTION\n2\nTABLES\n");
    const auto table_end = unregistered.find("0\nENDSEC\n", table_start) + std::string("0\nENDSEC\n").size();
    unregistered.erase(table_start, table_end - table_start);
    const auto unregistered_import = parse_dxf_ascii(unregistered);
    check(!unregistered_import.diagnostics.empty() && unregistered_import.drawing.blocks[0].lines.size() == 1,
          "unregistered native XDATA must remain inert without losing visual block geometry");
    auto invalid_utf8 = metadata_bytes;
    invalid_utf8[invalid_utf8.find("1000\n") + 5] = static_cast<char>(0xff);
    const auto invalid_utf8_import = parse_dxf_ascii(invalid_utf8);
    check(!invalid_utf8_import.diagnostics.empty() && invalid_utf8_import.drawing.blocks[0].lines.size() == 1,
          "malformed native UTF-8 must preserve readable block geometry");
    auto unsupported_native = metadata_bytes;
    unsupported_native.insert(unsupported_native.find("0\nLINE\n", native_marker), "0\nCIRCLE\n10\n0\n20\n0\n40\n1\n");
    const auto unsupported_native_import = parse_dxf_ascii(unsupported_native);
    check(!unsupported_native_import.diagnostics.empty() &&
          unsupported_native_import.drawing.blocks[0].vertex_entity_json == "!unsupported_vertex_block",
          "omitted unsupported block entities must prevent native metadata activation");
    auto oversized = metadata;
    oversized.blocks[0].vertex_entity_json = std::string(16 * 1024, 'x');
    rejects([&] { (void)export_dxf_ascii(oversized); });
    auto aggregate_metadata = metadata_bytes;
    const auto foreign_position = aggregate_metadata.find("1001\nVERTEX_ENTITY_V1\n");
    std::string oversized_foreign = "1001\nFOREIGN_APP\n";
    for (int i = 0; i < 64; ++i) oversized_foreign += "1000\n" + std::string(255, 'x') + "\n";
    aggregate_metadata.insert(foreign_position, oversized_foreign);
    rejects([&] { (void)parse_dxf_ascii(aggregate_metadata); });
    auto crlf = encoded;
    for (std::size_t i = 0; i < crlf.size(); ++i) if (crlf[i] == '\n') crlf.insert(i++, 1, '\r');
    check(export_dxf_ascii(parse_dxf_ascii(crlf).drawing) == encoded, "CRLF differs");
    const auto skipped = parse_dxf_ascii(file("0\nCIRCLE\n10\n0\n20\n0\n40\n2\n0\n3DFACE\n"));
    check(skipped.diagnostics.size() == 2 && skipped.diagnostics[1].entity_index == 2,
        "unsupported entities not reported");
    check(skipped.diagnostics[0].code == "unsupported_entity", "unstable diagnostic");
    rejects([&] { (void)parse_dxf_ascii(file(
        "0\nINSERT\n2\nMissing\n10\n0\n20\n0\n")); });
    const auto nonplanar = parse_dxf_ascii(file("0\nLINE\n10\n0\n20\n0\n11\n1\n21\n1\n30\n1\n"));
    check(nonplanar.drawing.lines.empty() && !nonplanar.diagnostics.empty(), "3D silently flattened");
    const auto aligned = parse_dxf_ascii(file("0\nTEXT\n10\n0\n20\n0\n40\n1\n1\nHi\n72\n1\n"));
    check(aligned.drawing.labels.empty() && !aligned.diagnostics.empty(), "aligned text silently changed");
    const auto weighted = parse_dxf_ascii(file("0\nLWPOLYLINE\n90\n2\n10\n0\n20\n0\n40\n2\n10\n1\n20\n1\n"));
    check(weighted.drawing.polylines.empty() && !weighted.diagnostics.empty(), "width silently discarded");
    const auto mirrored = parse_dxf_ascii(file("0\nARC\n10\n0\n20\n0\n40\n2\n50\n0\n51\n90\n230\n-1\n"));
    check(mirrored.drawing.arcs.empty() && !mirrored.diagnostics.empty(), "mirrored OCS silently changed");
    const auto formatted = parse_dxf_ascii(file("0\nTEXT\n10\n0\n20\n0\n40\n1\n1\n%%d\n"));
    check(formatted.drawing.labels.empty() && !formatted.diagnostics.empty(), "text control syntax interpreted as literal");
    const auto dimension_style = parse_dxf_ascii(file(
        "0\nDIMENSION\n10\n0\n20\n2\n11\n1\n21\n3\n13\n0\n23\n0\n14\n2\n24\n0\n70\n1\n"));
    check(dimension_style.drawing.dimensions.empty() && !dimension_style.diagnostics.empty(),
          "nonlinear dimension type silently changed");
    const auto hatch_style = parse_dxf_ascii(file(
        "0\nHATCH\n10\n0\n20\n0\n30\n1\n70\n0\n71\n0\n91\n1\n92\n1\n72\n0\n73\n1\n93\n3\n10\n0\n20\n0\n10\n1\n20\n0\n10\n0\n20\n1\n"));
    check(hatch_style.drawing.hatches.empty() && !hatch_style.diagnostics.empty(),
          "non-solid hatch silently changed");
    const auto unknown_group = parse_dxf_ascii(file("0\nLINE\n10\n0\n20\n0\n11\n1\n21\n1\n1000\nexternal-reference\n"));
    check(unknown_group.drawing.lines.empty() && !unknown_group.diagnostics.empty(), "extension data silently discarded");
    auto wrong_version = encoded; wrong_version.replace(wrong_version.find("AC1027"), 6, "AC1015");
    rejects([&] { (void)parse_dxf_ascii(wrong_version); });
    rejects([&] { (void)parse_dxf_ascii("0\nSECTION\n2\nENTITIES\n0\nENDSEC\n0\nEOF\n"); });
    rejects([&] { (void)parse_dxf_ascii(encoded.substr(0, encoded.size() - 6)); });
    rejects([&] { (void)parse_dxf_ascii(encoded + "0\nEOF\n"); });
    rejects([&] { (void)parse_dxf_ascii(file("0\nLINE\n10\nNaN\n20\n0\n11\n1\n21\n1\n")); });
    rejects([&] { (void)parse_dxf_ascii(file("0\nLINE\n10\n0\n10\n1\n20\n0\n11\n1\n21\n1\n")); });
    rejects([&] { (void)parse_dxf_ascii(file("0\nLWPOLYLINE\n90\n3\n10\n0\n20\n0\n10\n1\n20\n1\n")); });
    rejects([&] { (void)parse_dxf_ascii(file("0\nLINE\n10\n0\n20\n0\n11\n1\n")); });
    auto bounds = DxfExchangeLimits{};
    bounds.max_bytes = 10;
    rejects([&] { (void)parse_dxf_ascii(encoded, bounds); });
    rejects([&] { (void)export_dxf_ascii(d, bounds); });
    bounds = {}; bounds.max_entities = 1;
    rejects([&] { (void)parse_dxf_ascii(encoded, bounds); });
    bounds = {}; bounds.max_vertices = 2;
    rejects([&] { (void)parse_dxf_ascii(encoded, bounds); });
    rejects([&] { (void)export_dxf_ascii(d, bounds); });
    bounds = {}; bounds.max_pairs = 2;
    rejects([&] { (void)parse_dxf_ascii(encoded, bounds); });
    rejects([&] { (void)export_dxf_ascii(d, bounds); });
    bounds = {}; bounds.max_string_bytes = 8;
    rejects([&] { (void)parse_dxf_ascii(encoded, bounds); });
    bounds = {}; bounds.max_bytes = 0;
    rejects([&] { (void)parse_dxf_ascii(encoded, bounds); });
    bounds = {}; ++bounds.max_entities;
    rejects([&] { (void)parse_dxf_ascii(encoded, bounds); });
    auto invalid_units = d; invalid_units.insertion_units = 21;
    rejects([&] { (void)export_dxf_ascii(invalid_units); });
    auto unicode_text = encoded; unicode_text[unicode_text.find("Room")] = static_cast<char>(0xff);
    rejects([&] { (void)parse_dxf_ascii(unicode_text); });
    d.lines[0].start.x = std::numeric_limits<double>::infinity();
    rejects([&] { (void)export_dxf_ascii(d); });
    d.lines.clear(); d.labels[0].text = "injected\n0\nEOF";
    rejects([&] { (void)export_dxf_ascii(d); });
}
}
int main() { try { run(); std::cout << "DXF exchange tests passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
