#include "sketch/pinc_import_candidate_codec.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::span<const std::byte> bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}
PincImportProject parse(const Json& value) { const auto text = value.dump(); return parse_pinc_project(bytes(text)); }
Json wire(const PincImportProject& project) {
    const auto value = encode_pinc_import_candidate(project);
    return Json::parse(reinterpret_cast<const char*>(value.data()), reinterpret_cast<const char*>(value.data() + value.size()));
}
PincImportProject decode(const Json& value) { const auto text = value.dump(); return decode_pinc_import_candidate(bytes(text)); }
template<class F> void rejects(F action, std::string_view diagnostic = {}) {
    try { action(); } catch (const std::invalid_argument& error) {
        require(diagnostic.empty() || std::string_view(error.what()).find(diagnostic) != std::string_view::npos,
                "codec refusal lost expected failure diagnostic"); return;
    }
    throw std::runtime_error("Pinc candidate codec admitted forged, malformed or over-budget reply");
}
Json edge(std::string id, double bulge = 0) {
    return {{"id", id}, {"a", {{"x", 0}, {"y", 2}}}, {"b", {{"x", 8}, {"y", 2}}},
            {"kind", bulge == 0 ? "line" : "arc"}, {"bulge", bulge}};
}
Json page() {
    return {{"id", "page-source-id"}, {"name", "Ground"}, {"calcWalls", {edge("straight"), edge("curve", 2), edge("tiny", .000999)}},
        {"interiorWalls", {edge("inside")}}, {"assignments", {{"straight|curve", {{"code", "GLA1"}, {"name", "Descriptive only"}}},
            {"missing", {{"code", "CUSTOM"}, {"_anchor", {{"x", 2}, {"y", 2}}}, {"_area", 12}}}}},
        {"symbols", {{{"id", "symbol-source-id"}, {"kind", "Door"}, {"x", 4}, {"y", 2}, {"w", 3}, {"h", 3}, {"rot", 90},
            {"wallRef", {{"type", "calc"}, {"id", "straight"}, {"t", .5}}}}}},
        {"texts", {{{"id", "text-source-id"}, {"text", "Retained note"}, {"x", 2}, {"y", 3}, {"rot", 90}, {"align", "right"}}}},
        {"underlay", {{"data", "data:image/png;base64,aGVsbG8="}, {"x", 2}, {"y", 3}, {"width", 40}, {"opacity", .28}}}};
}
Json modern() {
    auto pg = page(); pg["privateUnknownPayload"] = "DO_NOT_COPY_RAW_VALUE";
    return {{"format", "PincSketch"}, {"version", "4.2"}, {"pages", {pg, pg}}, {"currentPage", 1}};
}
PincImportProject roundtrip(const PincImportProject& project) {
    const auto encoded = encode_pinc_import_candidate(project);
    const auto decoded = decode_pinc_import_candidate(encoded);
    require(encode_pinc_import_candidate(decoded) == encoded, "candidate roundtrip lost typed fields or original record order");
    return decoded;
}
void modern_and_legacy_known_answers() {
    const auto candidate = parse(modern());
    const auto restored = roundtrip(candidate);
    require(restored.pages.size() == 2 && restored.current_page == 1 &&
        restored.pages[0].calculation_segments[0].source.identity != restored.pages[1].calculation_segments[0].source.identity,
        "equal source IDs across page occurrences must remain distinct provenance");
    require(restored.pages[0].calculation_segments[0].source.scalar_id_json == "\"straight\"" &&
        restored.pages[0].calculation_segments[0].source.json_pointer == "/pages/0/calcWalls/0",
        "source IDs and pointers remain source evidence");
    require(restored.pages[0].calculation_segments[1].geometry.sweep_radians > 0 &&
        restored.pages[0].calculation_segments[2].geometry.sweep_radians == 0 &&
        restored.pages[0].texts[0].rotation_radians < 0,
        "codec must preserve normalized sweep threshold and already converted angle");
    require(restored.pages[0].underlay->data_url == "data:image/png;base64,aGVsbG8=",
        "known raster descriptor is retained without image decode");
    require(wire(candidate).dump().find("DO_NOT_COPY_RAW_VALUE") == std::string::npos, "unknown source content cannot become candidate native properties");
    require(wire(candidate).at("format") == "VertexPincCandidate" && !wire(candidate).contains("entities"),
        "Pinc candidate is distinct from native/DXF/IFC entity protocol");
    auto reversed = edge("reverse", -2); reversed["a"] = {{"x", 8}, {"y", 2}}; reversed["b"] = {{"x", 0}, {"y", 2}};
    const Json legacy{{"version", "2.1"}, {"areas", {{{"id", 1}, {"code", "GLA1"}, {"segments", {edge("first", 2)}}},
        {{"id", 2}, {"code", "GAR"}, {"segments", {reversed, edge("last")}}}}}};
    const auto old = roundtrip(parse(legacy));
    require(old.pages[0].legacy_areas.size() == 2 && old.pages[0].legacy_areas[1].segments.size() == 2 &&
        old.pages[0].calculation_segments.size() == 2 && old.pages[0].calculation_segments[0].equivalent_sources.size() == 1,
        "legacy originals, order, exact duplicates and normalized edges must all survive");
    require(old.pages[0].assignments[1].legacy_area_index == 1 && old.pages[0].assignments[1].source_segment_references.size() == 2,
        "legacy assignments retain exact original references rather than centroid authority");
    auto forged = wire(old); forged["project"]["pages"][0]["calculation_segments"][0]["equivalent_sources"][0] =
        forged["project"]["pages"][0]["legacy_areas"][0]["segments"][0]["source"];
    rejects([&] { (void)decode(forged); });
    auto unsupported = modern(); unsupported["pages"][0]["underlay"]["data"] = "https://example.invalid/unsafe.svg";
    const auto opaque = roundtrip(parse(unsupported));
    require(!opaque.pages[0].underlay->supported_raster_descriptor && !opaque.pages[0].underlay->data_url &&
        std::any_of(opaque.diagnostics.begin(), opaque.diagnostics.end(), [](const auto& d) { return d.code == "underlay_unsupported_descriptor"; }),
        "unsupported underlay retains typed descriptor and diagnostic without fetching or decode");
}
void schema_geometry_and_reference_failures() {
    const auto base = wire(parse(modern()));
    const auto mutate = [&](auto action) { auto value = base; action(value); rejects([&] { (void)decode(value); }); };
    mutate([](Json& j) { j["version"] = 2; });
    mutate([](Json& j) { j["format"] = "PSIP0001"; });
    mutate([](Json& j) { j["broker_attestation"] = "forged"; });
    mutate([](Json& j) { j["project"]["entities"] = Json::array(); });
    mutate([](Json& j) { j["project"]["pages"][0]["source"]["native_id"] = "forged-native-authority"; });
    mutate([](Json& j) { j["project"]["current_page"] = 2; });
    mutate([](Json& j) { j["project"]["current_page"] = .5; });
    mutate([](Json& j) { j["project"]["pages"][0]["ghost_previous"] = 1; });
    mutate([](Json& j) { j["project"]["pages"][0]["dimension"]["color"] = "javascript:red"; });
    mutate([](Json& j) { j["project"]["pages"][0]["calculation_segments"][0]["geometry"]["sweep_radians"] = .5; });
    mutate([](Json& j) { j["project"]["pages"][0]["calculation_segments"][1]["geometry"]["sweep_radians"] = -1.8545904360032244; });
    mutate([](Json& j) { j["project"]["pages"][0]["calculation_segments"][0]["geometry"]["start"]["x"] = 1'000'001; });
    mutate([](Json& j) { j["project"]["pages"][0]["texts"][0]["alignment"] = "justify"; });
    mutate([](Json& j) { j["project"]["pages"][0]["symbols"][0]["width_metres"] = 0; });
    mutate([](Json& j) { j["project"]["pages"][0]["symbols"][0]["wall_reference"]["target"] = j["project"]["pages"][1]["calculation_segments"][0]["source"]; });
    mutate([](Json& j) { j["project"]["pages"][0]["symbols"][0]["wall_reference"]["target"] = j["project"]["pages"][0]["interior_segments"][0]["source"]; });
    mutate([](Json& j) { j["project"]["pages"][0]["assignments"][1]["source_segment_references"][0] = j["project"]["pages"][0]["interior_segments"][0]["source"]; });
    mutate([](Json& j) { j["project"]["pages"][0]["assignments"][0]["known_category"] = true; });
    mutate([](Json& j) { j["project"]["pages"][0]["underlay"]["data_url"] = "data:image/jpeg;base64,aGVsbG8="; });
    mutate([](Json& j) { j["project"]["pages"][0]["calculation_segments"][0]["source"]["identity"] = "native-wall-id"; });
    mutate([](Json& j) { j["project"]["pages"][0]["calculation_segments"][0]["source"]["scalar_id_json"] = "{}"; });
    mutate([](Json& j) { auto& edge = j["project"]["pages"][0]["calculation_segments"][1]; edge["source"] = j["project"]["pages"][0]["calculation_segments"][0]["source"]; });
    auto typed = parse(modern()); typed.pages[0].texts[0].size_metres = std::numeric_limits<double>::infinity();
    rejects([&] { (void)encode_pinc_import_candidate(typed); });
    const auto text = base.dump();
    rejects([&] { (void)decode_pinc_import_candidate(bytes(text.substr(0, text.size() - 1))); });
    rejects([&] { (void)decode_pinc_import_candidate(bytes("{\"format\":\"VertexPincCandidate\",\"format\":\"VertexPincCandidate\"}")); }, "duplicate");
    rejects([&] { (void)decode_pinc_import_candidate(bytes("{\"n\":1e999}")); });
}
void rotations_outside_one_turn() {
    for (const double degrees : {270.0, -450.0, 540.0}) {
        auto input = modern();
        input["pages"][0]["symbols"][0]["rot"] = degrees;
        input["pages"][0]["texts"][0]["rot"] = degrees;
        const auto result = roundtrip(parse(input));
        const auto expected = -degrees / 180 * std::numbers::pi;
        require(std::abs(result.pages[0].symbols[0].rotation_radians - expected) < 1e-12 &&
            std::abs(result.pages[0].texts[0].rotation_radians - expected) < 1e-12,
            "multi-turn modern orientation was rejected or changed");
        const Json legacy{{"version", "2.1"}, {"areas", Json::array()},
            {"symbols", {{{"id", "s"}, {"kind", "Door"}, {"x", 0}, {"y", 0}, {"rot", degrees}}}},
            {"texts", {{{"id", "t"}, {"text", "Note"}, {"x", 0}, {"y", 0}, {"rot", degrees}}}}};
        const auto old = roundtrip(parse(legacy));
        require(std::abs(old.pages[0].symbols[0].rotation_radians - expected) < 1e-12 &&
            std::abs(old.pages[0].texts[0].rotation_radians - expected) < 1e-12,
            "multi-turn legacy orientation was rejected or changed");
    }
    auto oversized = modern(); oversized["pages"][0]["symbols"][0]["rot"] = 1e9;
    rejects([&] { (void)parse(oversized); }, "/rot");
    oversized = modern(); oversized["pages"][0]["texts"][0]["rot"] = 1e9;
    rejects([&] { (void)parse(oversized); }, "/rot");
    auto forged = wire(parse(modern()));
    forged["project"]["pages"][0]["symbols"][0]["rotation_radians"] = 1'000'001;
    rejects([&] { (void)decode(forged); });
    forged = wire(parse(modern()));
    forged["project"]["pages"][0]["texts"][0]["rotation_radians"] = -1'000'001;
    rejects([&] { (void)decode(forged); });
}
void legacy_duplicate_visual_reference() {
    auto reversed = edge("duplicate"); reversed["a"] = {{"x",8},{"y",2}};
    reversed["b"] = {{"x",0},{"y",2}};
    const Json input{{"version","2.1"},{"areas",{{{"segments",{edge("primary")}}},
        {{"segments",{reversed}}}}},{"symbols",{{{"kind","Door"},{"x",4},{"y",2},
        {"wallRef",{{"type","calc"},{"id","duplicate"},{"t",.5}}}}}}};
    const auto restored = roundtrip(parse(input));
    require(restored.pages[0].calculation_segments.size() == 1 &&
        restored.pages[0].symbols[0].wall_reference->target.scalar_id_json == "\"duplicate\"",
        "legacy duplicate visual reference was lost during normalization");
    auto forged = wire(restored);
    forged["project"]["pages"][0]["symbols"][0]["wall_reference"]["target"] =
        forged["project"]["pages"][0]["legacy_areas"][1]["source"];
    rejects([&] { (void)decode(forged); });
    forged = wire(restored);
    forged["project"]["pages"][0]["symbols"][0]["wall_reference"]["type"] = "interior";
    rejects([&] { (void)decode(forged); });
}
void budgets_precede_dom_and_reconstruction() {
    { std::vector<std::byte> oversized(64 * 1024 * 1024 + 1); rejects([&] { (void)decode_pinc_import_candidate(oversized); }, "byte"); }
    { std::string deep(65, '['); deep += '0'; deep += std::string(65, ']'); rejects([&] { (void)decode_pinc_import_candidate(bytes(deep)); }, "depth"); }
    { std::string nodes = "["; for (int i = 0; i < 200001; ++i) nodes += i ? ",null" : "null"; nodes += ']';
      rejects([&] { (void)decode_pinc_import_candidate(bytes(nodes)); }, "node"); }
    { std::string strings = "\"" + std::string(32 * 1024 * 1024 + 1, 'x') + "\"";
      rejects([&] { (void)decode_pinc_import_candidate(bytes(strings)); }, "string"); }
    auto candidate = parse(modern());
    candidate.pages.resize(129, candidate.pages.front()); rejects([&] { (void)encode_pinc_import_candidate(candidate); }, "page");
    auto pair_budget = wire(parse(modern()));
    auto& segments = pair_budget["project"]["pages"][0]["calculation_segments"];
    const auto prototype = segments[0]; segments = Json::array(); for (int i = 0; i < 708; ++i) segments.push_back(prototype);
    rejects([&] { (void)decode(pair_budget); }, "pair");
    auto records = parse(modern()); records.diagnostics.resize(100001, {"/", "fixture", "record"});
    rejects([&] { (void)encode_pinc_import_candidate(records); }, "record");
    auto edges = parse(modern()); const auto template_page = edges.pages[0]; edges.pages.clear(); edges.current_page = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        auto pg = template_page; pg.calculation_segments.clear(); pg.assignments.clear(); pg.symbols.clear(); pg.texts.clear(); pg.underlay.reset();
        pg.source.page_index = i; pg.source.json_pointer = "/pages/" + std::to_string(i); pg.source.identity = pinc_source_identity(pg.source);
        pg.interior_segments.resize(2048, template_page.interior_segments[0]);
        for (std::size_t k = 0; k < pg.interior_segments.size(); ++k) {
            auto& source = pg.interior_segments[k].source; source.page_index = i; source.scalar_id_json = Json(k).dump();
            source.json_pointer = pg.source.json_pointer + "/interiorWalls/" + std::to_string(k); source.identity = pinc_source_identity(source);
        }
        edges.pages.push_back(std::move(pg));
    }
    rejects([&] { (void)encode_pinc_import_candidate(edges); }, "edge");
}
} // namespace
int main() {
    try { modern_and_legacy_known_answers(); rotations_outside_one_turn(); legacy_duplicate_visual_reference(); schema_geometry_and_reference_failures(); budgets_precede_dom_and_reconstruction();
        std::cout << "Pinc candidate codec checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "pinc_import_candidate_codec_tests: " << error.what() << '\n'; return 1; }
}
