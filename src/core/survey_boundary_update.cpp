#include "sketch/survey_boundary_update.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/quantity.hpp"
#include "sketch/survey_contract.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace sketch {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument("Survey correction: " + std::move(message));
}

void require(bool condition, const char* message) {
    if (!condition) invalid(message);
}

std::string_view trim(std::string_view text) {
    // Match QString::trimmed() for the UTF-8 entered text captured by the UI.
    const auto space_width = [](std::string_view value) -> std::size_t {
        if (value.empty()) return 0;
        const auto first = static_cast<unsigned char>(value[0]);
        if (first < 128) return std::isspace(first) ? 1 : 0;
        if (value.starts_with("\xc2\x85") || value.starts_with("\xc2\xa0")) return 2;
        if (value.starts_with("\xe1\x9a\x80") || value.starts_with("\xe2\x80\xa8") ||
            value.starts_with("\xe2\x80\xa9") || value.starts_with("\xe2\x80\xaf") ||
            value.starts_with("\xe2\x81\x9f") || value.starts_with("\xe3\x80\x80")) return 3;
        if (value.size() >= 3 && first == 0xe2 && static_cast<unsigned char>(value[1]) == 0x80 &&
            static_cast<unsigned char>(value[2]) >= 0x80 && static_cast<unsigned char>(value[2]) <= 0x8a) return 3;
        return 0;
    };
    while (const auto width = space_width(text)) text.remove_prefix(width);
    while (!text.empty()) {
        auto start = text.size() - 1;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
        const auto width = space_width(text.substr(start));
        if (width == 0 || width != text.size() - start) break;
        text.remove_suffix(width);
    }
    return text;
}

std::string text_field(const Json& object, const char* name, std::size_t limit) {
    require(object.contains(name) && object.at(name).is_string(), "missing entered text field");
    auto result = object.at(name).get<std::string>();
    require(result.size() <= limit && result.find('\0') == std::string::npos, "entered text exceeds limits or contains NUL");
    return result;
}

void version_one(const Json& object, const char* message) {
    require(object.is_object() && object.contains("version") &&
        object.at("version").is_number_integer() && object.at("version") == 1, message);
}

struct RebuiltReport {
    Json report;
    std::vector<SurveyVertex> vertices;
    std::size_t leg_count{};
};

RebuiltReport rebuild_report(const Json& report) {
    version_one(report, "unsupported report version");
    require(report.dump().size() <= 4 * 1024 * 1024, "report exceeds 4 MiB");
    require(report.contains("input_provenance"), "report has no editable entered calls");
    const auto& input = report.at("input_provenance");
    version_one(input, "unsupported entered-input version");
    const auto unit_text = text_field(input, "default_unit", 2);
    require(unit_text == "m" || unit_text == "ft", "unsupported default input unit");
    const auto unit = unit_text == "m" ? Unit::metre : Unit::foot;
    const auto entered = text_field(input, "legs_text", 1024 * 1024);
    const auto source_text = text_field(input, "source_text", 4096);
    const auto tolerance_text = text_field(input, "closure_tolerance_expression", 4096);
    std::vector<SurveyLeg> legs;
    auto receipts = Json::array();
    std::size_t line_number = 0;
    std::string_view remaining = entered;
    do {
        ++line_number;
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        if (!trim(line).empty()) {
            try {
                require(legs.size() < SurveyTraverse::maximum_legs, "too many entered legs");
                std::array<std::string_view, 3> fields;
                auto rest = line;
                for (std::size_t i = 0; i < 2; ++i) {
                    const auto comma = rest.find(',');
                    require(comma != std::string_view::npos, "use quadrant, angle, distance");
                    fields[i] = trim(rest.substr(0, comma));
                    rest.remove_prefix(comma + 1);
                }
                fields[2] = trim(rest);
                require(fields[2].find(',') == std::string_view::npos, "use exactly three fields per leg");
                std::string quadrant(fields[0]);
                std::transform(quadrant.begin(), quadrant.end(), quadrant.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                BearingQuadrant bearing;
                if (quadrant == "NE") bearing = BearingQuadrant::north_east;
                else if (quadrant == "SE") bearing = BearingQuadrant::south_east;
                else if (quadrant == "SW") bearing = BearingQuadrant::south_west;
                else if (quadrant == "NW") bearing = BearingQuadrant::north_west;
                else invalid("use NE, SE, SW, or NW");
                const auto angle = parse_survey_angle(fields[1]);
                const auto quantity = parse_quantity(fields[2], unit);
                require(quantity.metres > 0, "distance must be positive");
                const auto id = "leg-" + std::to_string(legs.size() + 1);
                legs.push_back({id, bearing, angle, quantity.metres});
                receipts.push_back({{"leg_id", id}, {"line_number", line_number},
                    {"original_expression", quantity.original_expression},
                    {"exact_metres", {{"numerator", quantity.exact_metres.numerator},
                                      {"denominator", quantity.exact_metres.denominator}}}});
            } catch (const std::exception& error) {
                invalid("line " + std::to_string(line_number) + ": " + error.what());
            }
        }
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1);
    } while (true);
    require(input.contains("distances") && input.at("distances").is_array() &&
        input.at("distances").size() == receipts.size(), "entered distance receipts are missing or inconsistent");
    for (std::size_t i = 0; i < receipts.size(); ++i) {
        const auto& actual = input.at("distances")[i];
        const auto& expected = receipts[i];
        require(actual.is_object() && actual.contains("line_number") && actual.at("line_number").is_number_integer() &&
            actual.contains("exact_metres") && actual.at("exact_metres").is_object() &&
            actual.at("exact_metres").contains("numerator") && actual.at("exact_metres").at("numerator").is_number_integer() &&
            actual.at("exact_metres").contains("denominator") && actual.at("exact_metres").at("denominator").is_number_integer(),
            "malformed exact distance receipt");
        for (const auto* key : {"leg_id", "line_number", "original_expression", "exact_metres"})
            require(actual.contains(key) && actual.at(key) == expected.at(key), "distance receipt does not match entered calls");
    }
    const SurveyTraverse traverse(std::string(trim(source_text)), legs, parse_quantity(trim(tolerance_text), unit).metres);
    auto canonical = Json::parse(traverse.serialize());
    // Legs and tolerance are normalized input receipts. Displayed vertices and
    // diagnostics are deliberately never consulted, including their shapes.
    for (const auto* key : {"provenance", "legs", "closure_tolerance_m"})
        require(report.contains(key) && report.at(key) == canonical.at(key), "normalized report inputs do not match entered calls");
    canonical["input_provenance"] = input;
    canonical["input_provenance"]["distances"] = std::move(receipts);
    return {std::move(canonical), traverse.vertices(), legs.size()};
}

bool needs_closing_segment(const RebuiltReport& report, bool adjust) {
    const auto end = report.vertices.back();
    return !adjust && (end.east_m != 0 || end.north_m != 0);
}

void validate_source_closure(const Json& source, const RebuiltReport& report) {
    for (const auto* key : {"added_closing_segment", "adjusted_final_endpoint"})
        require(source.contains(key) && source.at(key).is_boolean(), "stored closure choice is missing or malformed");
    const auto adjust = source.at("adjusted_final_endpoint").get<bool>();
    require(source.at("added_closing_segment") == needs_closing_segment(report, adjust),
        "stored closure choice does not match original calls");
    const auto end = report.vertices.back();
    const auto adjustment = adjust ? Json{{"east", -end.east_m}, {"north", -end.north_m}} : Json(nullptr);
    require(source.contains("endpoint_adjustment_m") && source.at("endpoint_adjustment_m") == adjustment,
        "stored endpoint adjustment does not match original calls");
    require(!report.report.at("diagnostics").at("area_m2").is_null(), "stored traverse cannot form an area boundary");
}

bool has_original_row_ownership(const DocumentSnapshot& source, const IdentifiedBoundary& current,
                                const Json& survey_source, std::size_t expected_count) {
    if (current.segments.size() != expected_count) return false;
    // Current geometry can be translated, rotated, or edited since creation.
    // The earliest retained state of these calls establishes row ownership;
    // a reordered or rebuilt cycle must not relabel old dependent targets.
    for (const auto& revision : source.history()) {
        const auto found = revision.entities.find(current.id);
        if (found == revision.entities.end() || !found->second.extensions.contains("survey_source")) continue;
        const auto& previous_source = found->second.extensions.at("survey_source");
        if (!previous_source.is_object() || !previous_source.contains("report") ||
            previous_source.at("report") != survey_source.at("report") ||
            previous_source.value("added_closing_segment", Json()) != survey_source.at("added_closing_segment") ||
            previous_source.value("adjusted_final_endpoint", Json()) != survey_source.at("adjusted_final_endpoint")) continue;
        if (inspect_boundary_entity_version(found->second).format != BoundaryEntityFormat::identified_v1) return false;
        const auto previous = decode_identified_boundary_entity(found->second);
        if (previous.segments.size() != current.segments.size()) return false;
        for (std::size_t i = 0; i < current.segments.size(); ++i) {
            const auto& a = current.segments[i];
            const auto& b = previous.segments[i];
            if (a.segment_id != b.segment_id || a.start_vertex_id != b.start_vertex_id || a.end_vertex_id != b.end_vertex_id)
                return false;
        }
        return true;
    }
    return false;
}
} // namespace

ApplyEntityChanges survey_boundary_update_command(const DocumentSnapshot& source,
    std::string_view boundary_id, const Json& report, bool adjust_final_endpoint) {
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, "Survey correction: document is read-only: " + source.read_only_reason());
    const auto found = source.entities().find(boundary_id);
    require(found != source.entities().end() && found->second.type == "measurement_boundary",
        "select an existing survey measurement boundary");
    const auto& original = found->second;
    require(!original.properties.contains("boundary_authoring") &&
        !original.extensions.contains("boundary_authoring") &&
        !original.extensions.contains("boundary_geometry_derivation"),
        "construction receipt or derived survey boundaries require a supported receipt-preserving correction workflow");
    require(original.extensions.contains("survey_source"), "selected boundary has no survey source");
    const auto& existing_source = original.extensions.at("survey_source");
    version_one(existing_source, "unsupported stored survey source version");
    require(existing_source.contains("report"), "stored survey report is missing");
    const auto old_report = rebuild_report(existing_source.at("report"));
    validate_source_closure(existing_source, old_report);
    const auto replacement = rebuild_report(report);
    require(!replacement.report.at("diagnostics").at("area_m2").is_null(),
        "an open or degenerate traverse cannot become an area boundary");
    const auto current = decode_identified_boundary_entity(original);
    const auto anchor = current.segments.front().segment.start;
    const auto old_count = old_report.leg_count + static_cast<std::size_t>(existing_source.at("added_closing_segment").get<bool>());
    const auto closing = needs_closing_segment(replacement, adjust_final_endpoint);
    const auto count = replacement.leg_count + static_cast<std::size_t>(closing);
    const auto reuse_rows = replacement.leg_count == old_report.leg_count &&
        has_original_row_ownership(source, current, existing_source, old_count);
    std::vector<std::string> vertex_ids(count);
    vertex_ids[0] = current.segments.front().start_vertex_id;
    for (std::size_t i = 1; i < count; ++i)
        vertex_ids[i] = reuse_rows && i < current.segments.size() ? current.segments[i].start_vertex_id : make_stable_id();
    const auto point = [&](std::size_t index) {
        const auto v = replacement.vertices[index];
        const Vec2 result{anchor.x + v.east_m, anchor.y + v.north_m};
        require(std::isfinite(result.x) && std::isfinite(result.y), "placed survey coordinates are nonfinite");
        return result;
    };
    IdentifiedBoundary rebuilt{current.id, current.type, {}};
    for (std::size_t i = 0; i < count; ++i) {
        const auto next = (i + 1) % count;
        auto edge_id = make_stable_id();
        if (reuse_rows && i < current.segments.size()) {
            const auto& old = current.segments[i];
            if (old.start_vertex_id == vertex_ids[i] && old.end_vertex_id == vertex_ids[next] &&
                (i < replacement.leg_count) == (i < old_report.leg_count)) edge_id = old.segment_id;
        }
        rebuilt.segments.push_back({std::move(edge_id), vertex_ids[i], vertex_ids[next],
            {point(i), next == 0 ? anchor : point(i + 1), 0.0}});
    }
    auto updated = encode_identified_boundary_entity(rebuilt, &original);
    auto& new_source = updated.extensions.at("survey_source");
    if (!new_source.contains("original_report")) {
        new_source["original_report"] = existing_source.at("report");
        new_source["original_closure"] = {{"added_closing_segment", existing_source.at("added_closing_segment")},
            {"adjusted_final_endpoint", existing_source.at("adjusted_final_endpoint")},
            {"endpoint_adjustment_m", existing_source.at("endpoint_adjustment_m")}};
    }
    new_source["report"] = replacement.report;
    new_source["added_closing_segment"] = closing;
    new_source["adjusted_final_endpoint"] = adjust_final_endpoint;
    const auto end = replacement.vertices.back();
    new_source["endpoint_adjustment_m"] = adjust_final_endpoint ?
        Json{{"east", -end.east_m}, {"north", -end.north_m}} : Json(nullptr);
    new_source["placement"] = {{"version", 1}, {"anchor_m", {anchor.x, anchor.y}},
        {"orientation", "called_north_bearings"}};
    ApplyEntityChanges command{source.revision(), {EntityChange::upsert(std::move(updated))}, {}, "Correct survey boundary calls"};
    const auto preview = Document::preview_command(source, command);
    for (const auto& [id, entity] : preview.entities()) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported() || decoded.dimension->boundary_id != current.id) continue;
        try { (void)decoded.dimension->resolve(preview.entities().at(current.id)); }
        catch (const std::exception& error) {
            invalid("dimension " + id + " requires target repair before correction: " + error.what());
        }
    }
    return command;
}
} // namespace sketch
