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
    check(export_dxf_ascii(imported.drawing) == encoded, "round trip not deterministic");
    auto crlf = encoded;
    for (std::size_t i = 0; i < crlf.size(); ++i) if (crlf[i] == '\n') crlf.insert(i++, 1, '\r');
    check(export_dxf_ascii(parse_dxf_ascii(crlf).drawing) == encoded, "CRLF differs");
    const auto skipped = parse_dxf_ascii(file("0\nINSERT\n2\n../../external.dwg\n0\nCIRCLE\n10\n0\n20\n0\n40\n2\n"));
    check(skipped.diagnostics.size() == 2 && skipped.diagnostics[1].entity_index == 2,
        "unsupported entities not reported");
    check(skipped.diagnostics[0].code == "unsupported_entity", "unstable diagnostic");
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
