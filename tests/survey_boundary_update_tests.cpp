#include "sketch/survey_boundary_update.hpp"

#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/quantity.hpp"
#include "sketch/survey_contract.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
using json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

json report(double width = 10, bool gap = false, bool split = false) {
    std::vector<SurveyLeg> legs;
    auto receipts = json::array();
    std::string entered;
    const auto add = [&](std::string quadrant, BearingQuadrant q, int angle, std::string distance) {
        const auto quantity = parse_quantity(distance, Unit::metre);
        const auto id = "leg-" + std::to_string(legs.size() + 1);
        legs.push_back({id, q, static_cast<double>(angle), quantity.metres});
        entered += quadrant + ", " + std::to_string(angle) + ", " + distance + "\n";
        receipts.push_back({{"leg_id", id}, {"line_number", legs.size()},
            {"original_expression", quantity.original_expression},
            {"exact_metres", {{"numerator", quantity.exact_metres.numerator},
                              {"denominator", quantity.exact_metres.denominator}}}});
    };
    add("NE", BearingQuadrant::north_east, 0, "10 m");
    if (split) {
        add("NE", BearingQuadrant::north_east, 90, std::to_string(width / 2) + " m");
        add("NE", BearingQuadrant::north_east, 90, std::to_string(width / 2) + " m");
    } else add("NE", BearingQuadrant::north_east, 90, std::to_string(width) + " m");
    add("SE", BearingQuadrant::south_east, 0, "10 m");
    add("SW", BearingQuadrant::south_west, 90, std::to_string(width - (gap ? 0.001 : 0)) + " m");
    auto result = json::parse(SurveyTraverse("fixture", legs, 0.01).serialize());
    result["input_provenance"] = {{"version", 1}, {"default_unit", "m"},
        {"legs_text", entered}, {"source_text", "fixture"},
        {"closure_tolerance_expression", "0.01 m"}, {"distances", receipts}};
    return result;
}

Entity survey(const json& input = report(), bool adjust = false) {
    IdentifiedBoundary model{"survey-1", "measurement_boundary", {}};
    const auto& vertices = input.at("vertices");
    const auto point = [](const json& v) { return Vec2{100 + v.at("east_m").get<double>(),
                                                        200 + v.at("north_m").get<double>()}; };
    const auto end = point(vertices.back());
    const bool closing = !adjust && (end.x != 100 || end.y != 200);
    const auto count = vertices.size() - 1 + static_cast<std::size_t>(closing);
    for (std::size_t i = 0; i < count; ++i) {
        const auto start = point(vertices[i]);
        const auto next = i + 1 == count ? Vec2{100, 200} : point(vertices[i + 1]);
        model.segments.push_back({"s" + std::to_string(i), "v" + std::to_string(i),
            "v" + std::to_string((i + 1) % count), {start, next, 0}});
    }
    auto result = encode_identified_boundary_entity(model);
    result.required = true;
    result.properties["classification"] = "survey";
    result.properties["factor"] = 0.5;
    result.properties["vendor_property"] = {{"keep", 7}};
    result.properties["segments"][0]["vendor_edge"] = "keep";
    result.extensions = {{"vendor_extension", {1, 2}}, {"survey_source", {
        {"version", 1}, {"report", input}, {"added_closing_segment", closing},
        {"adjusted_final_endpoint", adjust}, {"endpoint_adjustment_m", adjust ?
            json{{"east", -input.at("vertices").back().at("east_m").get<double>()},
                 {"north", -input.at("vertices").back().at("north_m").get<double>()}} : json(nullptr)},
        {"vendor_source", "keep"}}}};
    return result;
}

template<class Function>
void rejected_unchanged(Document& document, Function operation, std::string_view reason) {
    const auto before = document.snapshot();
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    require(rejected, reason);
    const auto after = document.snapshot();
    require(after.entities() == before.entities() && after.revision() == before.revision() &&
        after.history().size() == before.history().size() &&
        after.saved_revision_optional() == before.saved_revision_optional(), "rejection mutated document");
}

void test_correction_preserves_identity_origin_metadata_and_recomputes_geometry() {
    auto original = survey();
    original.properties["layer_id"] = "layer-1";
    auto document = Document::create({original, Entity{"layer-1", "layer"}});
    auto changed_report = report(20);
    changed_report["vertices"] = "forged";
    changed_report["diagnostics"] = {{"area_m2", -123}};
    const auto command = survey_boundary_update_command(document.snapshot(), original.id, changed_report, false);
    require(command.expected_revision == document.revision() && command.entity_changes.size() == 1 &&
        command.asset_changes.empty(), "correction must be a single revision-fenced upsert");
    require(document.snapshot().entities().at(original.id) == original, "builder mutated live document");
    document.apply(command);
    const auto updated = document.snapshot().entities().at(original.id);
    const auto model = decode_identified_boundary_entity(updated);
    require(std::abs(signed_area(boundary_geometry(model))) == 200 && perimeter(boundary_geometry(model)) == 60,
        "corrected calls did not change area and perimeter");
    require(model.segments.front().segment.start.x == 100 && model.segments.front().segment.start.y == 200,
        "correction lost current placement anchor");
    for (std::size_t i = 0; i < model.segments.size(); ++i) {
        require(model.segments[i].segment_id == "s" + std::to_string(i) &&
            model.segments[i].start_vertex_id == "v" + std::to_string(i), "unchanged ownership lost child IDs");
    }
    require(updated.required && updated.properties.at("factor") == 0.5 &&
        updated.properties.at("classification") == "survey" &&
        updated.properties.at("layer_id") == "layer-1" &&
        updated.properties.at("vendor_property") == original.properties.at("vendor_property") &&
        updated.properties.at("segments")[0].at("vendor_edge") == "keep" &&
        updated.extensions.at("vendor_extension") == original.extensions.at("vendor_extension"),
        "correction lost unrelated metadata");
    const auto& source = updated.extensions.at("survey_source");
    require(source.at("original_report") == original.extensions.at("survey_source").at("report") &&
        source.at("report").at("diagnostics").at("area_m2") == 200 &&
        source.at("report").at("vertices").is_array(), "source archive or recomputation failed");
    document.apply(survey_boundary_update_command(document.snapshot(), original.id, report(30), false));
    require(document.snapshot().entities().at(original.id).extensions.at("survey_source").at("original_report") ==
        original.extensions.at("survey_source").at("report"), "second update rewrote original report");
}

void test_called_bearings_replace_drawing_edits_at_current_origin() {
    auto original = survey();
    auto document = Document::create({original});
    auto model = decode_identified_boundary_entity(original);
    for (auto& edge : model.segments) {
        const auto rotate = [](Vec2 point) { return Vec2{point.y + 50, -point.x + 70}; };
        edge.segment.start = rotate(edge.segment.start);
        edge.segment.end = rotate(edge.segment.end);
    }
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(encode_identified_boundary_entity(model, &original))}, {}, "rotate placement"});
    document.apply(survey_boundary_update_command(document.snapshot(), "survey-1", report(20), false));
    const auto corrected = decode_identified_boundary_entity(document.snapshot().entities().at("survey-1"));
    require(corrected.segments[0].segment.start.x == 250 && corrected.segments[0].segment.start.y == -30 &&
        corrected.segments[0].segment.end.x == 250 && corrected.segments[0].segment.end.y == -20,
        "correction did not replace drawing orientation at current origin");
    require(corrected.segments[0].segment_id == "s0", "drawing edit lost unchanged call ownership");

    const auto prior = document.snapshot().entities().at("survey-1");
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(reverse_identified_boundary_entity(prior))}, {}, "reverse cycle"});
    const auto reversed = decode_identified_boundary_entity(document.snapshot().entities().at("survey-1"));
    const auto command = survey_boundary_update_command(document.snapshot(), "survey-1", report(30), false);
    const auto rebuilt = decode_identified_boundary_entity(command.entity_changes.front().entity);
    require(rebuilt.segments.front().start_vertex_id == reversed.segments.front().start_vertex_id,
        "reordered correction lost current origin identity");
    for (const auto& edge : rebuilt.segments)
        for (const auto& old : reversed.segments)
            require(edge.segment_id != old.segment_id, "reordered cycle aliased an unrelated called row");
}

void test_unsuffixed_feet_blank_lines_and_unicode_trim_preserve_receipts() {
    std::vector<SurveyLeg> legs{{"leg-1", BearingQuadrant::north_east, 0, 3.048},
        {"leg-2", BearingQuadrant::north_east, 90, 3.048},
        {"leg-3", BearingQuadrant::south_east, 0, 3.048},
        {"leg-4", BearingQuadrant::south_west, 90, 3.048}};
    auto feet = json::parse(SurveyTraverse("fixture", legs, 0.003048).serialize());
    auto receipts = json::array();
    const auto quantity = parse_quantity("10", Unit::foot);
    for (std::size_t i = 0; i < 4; ++i) receipts.push_back({{"leg_id", "leg-" + std::to_string(i + 1)},
        {"line_number", i + 2}, {"original_expression", "10"},
        {"exact_metres", {{"numerator", quantity.exact_metres.numerator}, {"denominator", quantity.exact_metres.denominator}}}});
    feet["input_provenance"] = {{"version", 1}, {"default_unit", "ft"},
        {"legs_text", "\n NE, 0:0:0, 10\nNE, 90, 10\nSE, 0, 10\nSW, 90, 10\n"},
        {"source_text", "\xc2\xa0" "fixture" "\xc2\xa0"}, {"closure_tolerance_expression", "0.01"}, {"distances", receipts}};
    auto document = Document::create({survey()});
    const auto command = survey_boundary_update_command(document.snapshot(), "survey-1", feet, false);
    const auto& source = command.entity_changes.front().entity.extensions.at("survey_source").at("report");
    require(source.at("input_provenance") == feet.at("input_provenance"), "exact entered feet receipts changed");
    require(std::abs(source.at("diagnostics").at("area_m2").get<double>() - 9.290304) < 1e-10,
        "unsuffixed feet did not recompute in entered units");
}

void test_topology_and_closure_changes_do_not_alias_children() {
    auto document = Document::create({survey()});
    const auto command = survey_boundary_update_command(document.snapshot(), "survey-1", report(10, false, true), false);
    const auto changed = decode_identified_boundary_entity(command.entity_changes.front().entity);
    require(changed.segments.size() == 5 && changed.segments.front().start_vertex_id == "v0",
        "topology correction must retain origin only");
    for (const auto& edge : changed.segments) {
        require(edge.segment_id != "s0" && edge.segment_id != "s1" && edge.segment_id != "s2" && edge.segment_id != "s3",
            "topology change aliased different calls");
        require(edge.start_vertex_id == "v0" || (edge.start_vertex_id != "v1" &&
            edge.start_vertex_id != "v2" && edge.start_vertex_id != "v3"), "topology change reused a retired vertex");
    }
    document.apply(command);
    auto gap_document = Document::create({survey(report(10, true))});
    const auto gap_update = survey_boundary_update_command(gap_document.snapshot(), "survey-1", report(20, true), true);
    const auto adjusted = decode_identified_boundary_entity(gap_update.entity_changes.front().entity);
    require(adjusted.segments.size() == 4 && adjusted.segments[2].segment_id == "s2" &&
        adjusted.segments[3].segment_id != "s3", "changed final endpoint ownership retained old segment ID");
    const auto& source = gap_update.entity_changes.front().entity.extensions.at("survey_source");
    require(source.at("adjusted_final_endpoint") == true && source.at("added_closing_segment") == false &&
        std::abs(source.at("endpoint_adjustment_m").at("east").get<double>() + 0.001) < 1e-12,
        "closure choice or adjustment evidence lost");
}

void test_dependents_and_constraints_reject_atomically() {
    auto dimension = encode_boundary_dimension_entity(BoundaryDimension{
        .id = "dimension-1", .boundary_id = "survey-1", .segment_id = "s1", .text_position = {105, 211}});
    auto angle = BoundaryDimension{.id = "angle-1", .boundary_id = "survey-1", .segment_id = "s1",
        .text_position = {109, 209}};
    angle.kind = BoundaryDimensionKind::angle;
    angle.vertex_id = "v2";
    angle.secondary_segment_id = "s2";
    const auto angle_entity = encode_boundary_dimension_entity(angle);
    auto document = Document::create({survey(), dimension, angle_entity});
    rejected_unchanged(document, [&] {
        (void)survey_boundary_update_command(document.snapshot(), "survey-1", report(10, false, true), false);
    }, "topology correction silently removed a dimension target");
    document.apply(survey_boundary_update_command(document.snapshot(), "survey-1", report(20), false));
    require(document.snapshot().entities().at("dimension-1") == dimension, "valid dependent was rewritten");
    require(decode_boundary_dimension_entity(dimension).dimension->resolve(
        document.snapshot().entities().at("survey-1")).segment_length() == 20, "dimension did not follow stable corrected edge");
    require(document.snapshot().entities().at("angle-1") == angle_entity &&
        std::isfinite(angle.resolve(document.snapshot().entities().at("survey-1")).angle()),
        "angle dimension did not retain its valid endpoint ownership");

    PersistentConstraint fixed{"fixed-1", ConstraintRelationKind::fixed_length,
        {{"survey-1", WallEndpointRole::start, "s1", "v1"},
         {"survey-1", WallEndpointRole::end, "s1", "v2"}}, parse_quantity("10 m"), std::nullopt};
    auto locked = Document::create({survey(), encode_constraint_entity(fixed)});
    rejected_unchanged(locked, [&] {
        (void)survey_boundary_update_command(locked.snapshot(), "survey-1", report(20), false);
    }, "correction bypassed a fixed-length constraint");
}

void test_forged_receipts_versions_read_only_and_stale_reject() {
    auto document = Document::create({survey()});
    auto forged_display = survey();
    forged_display.extensions["survey_source"]["report"]["vertices"] = "forged display";
    forged_display.extensions["survey_source"]["report"]["diagnostics"] = nullptr;
    auto display_document = Document::create({forged_display});
    const auto rebuilt = survey_boundary_update_command(display_document.snapshot(), "survey-1", report(20), false);
    require(rebuilt.entity_changes.front().entity.extensions.at("survey_source").at("report").at("diagnostics").at("area_m2") == 200,
        "stored derived display values were trusted");
    for (int change = 0; change < 5; ++change) {
        auto forged = report(20);
        if (change == 0) forged["input_provenance"]["distances"][0]["exact_metres"]["numerator"] = 11;
        if (change == 1) forged["legs"][0]["angle_degrees"] = 45;
        if (change == 2) forged["input_provenance"]["default_unit"] = "cm";
        if (change == 3) forged["version"] = 2;
        if (change == 4) forged["input_provenance"]["distances"][0]["line_number"] = 2;
        rejected_unchanged(document, [&] { (void)survey_boundary_update_command(document.snapshot(), "survey-1", forged, false); },
            "forged or malformed input accepted");
    }
    auto forged_source = survey();
    forged_source.extensions["survey_source"]["report"]["input_provenance"]["distances"][0]["exact_metres"]["denominator"] = 2;
    auto bad_source = Document::create({forged_source});
    rejected_unchanged(bad_source, [&] { (void)survey_boundary_update_command(bad_source.snapshot(), "survey-1", report(20), false); },
        "forged stored source receipt accepted");
    const auto stale = survey_boundary_update_command(document.snapshot(), "survey-1", report(20), false);
    document.apply(NameRevision{document.revision(), "advance"});
    rejected_unchanged(document, [&] { document.apply(stale); }, "stale correction applied");
    document.mark_read_only("test ownership");
    rejected_unchanged(document, [&] { (void)survey_boundary_update_command(document.snapshot(), "survey-1", report(20), false); },
        "read-only correction accepted");
}

void test_receipt_backed_open_and_malformed_source_reject() {
    auto receipt_entity = survey();
    const auto model = decode_identified_boundary_entity(receipt_entity);
    BoundaryConstructionRecord record;
    record.schema_version = 2;
    record.boundary_id = model.id;
    record.anchor = model.segments.front().segment.start;
    for (const auto& edge : model.segments) {
        ConstructionReceipt receipt;
        receipt.segment_id = edge.segment_id;
        receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.start = edge.segment.start;
        receipt.chord_end = edge.segment.end;
        record.edges.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id, receipt});
    }
    receipt_entity.properties["boundary_authoring"] = encode_boundary_receipt_envelope(record);
    auto receipt_document = Document::create({receipt_entity});
    rejected_unchanged(receipt_document, [&] {
        (void)survey_boundary_update_command(receipt_document.snapshot(), "survey-1", report(20), false);
    }, "receipt-backed correction discarded construction evidence");
    auto document = Document::create({survey()});
    auto open = report(20, true);
    open["closure_tolerance_m"] = 0;
    open["input_provenance"]["closure_tolerance_expression"] = "0 m";
    rejected_unchanged(document, [&] { (void)survey_boundary_update_command(document.snapshot(), "survey-1", open, true); },
        "out-of-tolerance traverse became an adjusted area boundary");
    auto bad = survey();
    bad.extensions["survey_source"]["adjusted_final_endpoint"] = "yes";
    auto malformed = Document::create({bad});
    rejected_unchanged(malformed, [&] { (void)survey_boundary_update_command(malformed.snapshot(), "survey-1", report(20), false); },
        "malformed stored closure choice accepted");
}

void test_atomic_undo_redo_save_reopen() {
    auto document = Document::create({survey()});
    const auto original = document.snapshot().entities();
    document.apply(survey_boundary_update_command(document.snapshot(), "survey-1", report(20), false));
    const auto corrected = document.snapshot().entities();
    require(document.snapshot().history().size() == 2, "correction made more than one history event");
    document.undo(document.revision());
    require(document.snapshot().entities() == original, "undo did not restore exact original geometry and source");
    document.redo(document.revision());
    require(document.snapshot().entities() == corrected, "redo did not restore exact corrected geometry and source");
    const auto folder = std::filesystem::temp_directory_path() / ("survey-update-" + make_stable_id());
    std::filesystem::create_directory(folder);
    try {
        const auto path = folder / "survey.sketch";
        (void)ProjectStore::save(path, document.snapshot());
        auto loaded = ProjectStore::load(path);
        require(loaded.document.snapshot().entities() == corrected, "save/reopen changed correction");
        loaded.document.undo(loaded.document.revision());
        require(loaded.document.snapshot().entities() == original, "reopened undo lost original report");
    } catch (...) { std::filesystem::remove_all(folder); throw; }
    std::filesystem::remove_all(folder);
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_correction_preserves_identity_origin_metadata_and_recomputes_geometry();
        test_called_bearings_replace_drawing_edits_at_current_origin();
        test_unsuffixed_feet_blank_lines_and_unicode_trim_preserve_receipts();
        test_topology_and_closure_changes_do_not_alias_children();
        test_dependents_and_constraints_reject_atomically();
        test_forged_receipts_versions_read_only_and_stale_reject();
        test_receipt_backed_open_and_malformed_source_reject();
        test_atomic_undo_redo_save_reopen();
        std::cout << "survey boundary update tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
