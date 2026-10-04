#include "sketch/survey_boundary_update.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/survey_report.hpp"

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

void supported_source_version(const Json& source) {
    require(source.is_object() && source.contains("version") &&
        source.at("version").is_number_integer() &&
        (source.at("version") == 1 || source.at("version") == 2), "unsupported stored survey source version");
}

void validate_source_closure(const Json& source, const RebuiltSurveyReport& report) {
    for (const auto* key : {"added_closing_segment", "adjusted_final_endpoint"})
        require(source.contains(key) && source.at(key).is_boolean(), "stored closure choice is missing or malformed");
    require(source.at("version") == 2 || report.report().at("version") == 1,
        "curved calls require survey source version two");
    const auto geometry = make_survey_boundary(report, source.at("adjusted_final_endpoint").get<bool>() ?
        SurveyClosureMode::adjust_final_endpoint : SurveyClosureMode::retain_measured_calls);
    require(source.at("added_closing_segment") == geometry.added_closing_segment,
        "stored closure choice does not match original calls");
    const auto adjustment = geometry.endpoint_adjustment_m ?
        Json{{"east", geometry.endpoint_adjustment_m->x}, {"north", geometry.endpoint_adjustment_m->y}} : Json(nullptr);
    require(source.contains("endpoint_adjustment_m") && source.at("endpoint_adjustment_m") == adjustment,
        "stored endpoint adjustment does not match original calls");
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
    supported_source_version(existing_source);
    require(existing_source.contains("report"), "stored survey report is missing");
    const auto old_report = rebuild_survey_report(existing_source.at("report"));
    validate_source_closure(existing_source, old_report);
    const auto replacement = rebuild_survey_report(report);
    const auto geometry = make_survey_boundary(replacement, adjust_final_endpoint ?
        SurveyClosureMode::adjust_final_endpoint : SurveyClosureMode::retain_measured_calls);
    const auto current = decode_identified_boundary_entity(original);
    const auto anchor = current.segments.front().segment.start;
    const auto old_count = old_report.legs().size() + static_cast<std::size_t>(existing_source.at("added_closing_segment").get<bool>());
    const auto closing = geometry.added_closing_segment;
    const auto count = geometry.boundary.size();
    const auto reuse_rows = replacement.legs().size() == old_report.legs().size() &&
        has_original_row_ownership(source, current, existing_source, old_count);
    std::vector<std::string> vertex_ids(count);
    vertex_ids[0] = current.segments.front().start_vertex_id;
    for (std::size_t i = 1; i < count; ++i)
        vertex_ids[i] = reuse_rows && i < current.segments.size() ? current.segments[i].start_vertex_id : make_stable_id();
    const auto point = [&](Vec2 v) {
        const Vec2 result{anchor.x + v.x, anchor.y + v.y};
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
                (i < replacement.legs().size()) == (i < old_report.legs().size())) edge_id = old.segment_id;
        }
        rebuilt.segments.push_back({std::move(edge_id), vertex_ids[i], vertex_ids[next],
            {point(geometry.boundary[i].start), point(geometry.boundary[i].end), geometry.boundary[i].sweep_radians}});
    }
    auto updated = encode_identified_boundary_entity(rebuilt, &original);
    auto& new_source = updated.extensions.at("survey_source");
    if (!new_source.contains("original_report")) {
        new_source["original_report"] = existing_source.at("report");
        new_source["original_closure"] = {{"added_closing_segment", existing_source.at("added_closing_segment")},
            {"adjusted_final_endpoint", existing_source.at("adjusted_final_endpoint")},
            {"endpoint_adjustment_m", existing_source.at("endpoint_adjustment_m")}};
    }
    new_source["version"] = existing_source.at("version") == 2 || replacement.report().at("version") == 2 ? 2 : 1;
    new_source["report"] = replacement.report();
    new_source["added_closing_segment"] = closing;
    new_source["adjusted_final_endpoint"] = adjust_final_endpoint;
    new_source["endpoint_adjustment_m"] = geometry.endpoint_adjustment_m ?
        Json{{"east", geometry.endpoint_adjustment_m->x}, {"north", geometry.endpoint_adjustment_m->y}} : Json(nullptr);
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
