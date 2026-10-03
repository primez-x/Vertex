#include "sketch/constraint_authoring.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/project_store.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/model_phases.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sketch;
using json = nlohmann::json;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "constraint_authoring_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void require_accepted(const ConstraintAuthoringPreview& preview, std::string_view message) {
    if (preview.accepted()) {
        return;
    }
    std::cerr << "constraint_authoring_tests: " << message << '\n';
    for (const auto& diagnostic : preview.diagnostics()) {
        std::cerr << "  diagnostic: " << diagnostic << '\n';
    }
    std::exit(1);
}

bool has_diagnostic(const ConstraintAuthoringPreview& preview, std::string_view fragment) {
    return std::any_of(preview.diagnostics().begin(), preview.diagnostics().end(),
                       [&](const std::string& diagnostic) {
                           return diagnostic.find(fragment) != std::string::npos;
                       });
}

void require_near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << "constraint_authoring_tests: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        std::exit(1);
    }
}

json segment_json(Vec2 start, Vec2 end, double sweep = 0.0) {
    return {{"start", {start.x, start.y}}, {"end", {end.x, end.y}},
            {"sweep_radians", sweep}};
}

Entity wall(std::string id, Vec2 start, Vec2 end, double sweep = 0.0) {
    return {std::move(id), "wall",
            {{"baseline", segment_json(start, end, sweep)}, {"thickness_m", 0.14},
             {"height_m", 2.4}, {"elevation_m", 0.0}, {"classification", "existing"},
             {"future_wall_metadata", {{"retain", true}}}},
            false, {{"future_extension", {1, 2, 3}}}};
}

Entity opening(std::string id, std::string wall_id, double offset, double width) {
    return {std::move(id), "opening",
            {{"wall_id", std::move(wall_id)}, {"offset_m", offset}, {"width_m", width},
             {"sill_m", 0.0}, {"height_m", 2.0}},
            false, json::object()};
}

WallEndpointBinding endpoint(std::string owner, WallEndpointRole role) {
    return {std::move(owner), role};
}

PersistentConstraint relation(std::string id, ConstraintRelationKind kind,
                              std::vector<WallEndpointBinding> bindings) {
    PersistentConstraint value;
    value.id = std::move(id);
    value.relation = kind;
    value.bindings = std::move(bindings);
    return value;
}

Segment baseline(const Entity& entity) {
    const auto& value = entity.properties.at("baseline");
    return {{value.at("start").at(0).get<double>(), value.at("start").at(1).get<double>()},
            {value.at("end").at(0).get<double>(), value.at("end").at(1).get<double>()},
            value.at("sweep_radians").get<double>()};
}

double length(const Segment& value) {
    return std::hypot(value.end.x - value.start.x, value.end.y - value.start.y);
}

bool moved(const Segment& before, const Segment& after) {
    return before.start.x != after.start.x || before.start.y != after.start.y ||
           before.end.x != after.end.x || before.end.y != after.end.y;
}

const ConstraintWallChange& changed_wall(const ConstraintAuthoringPreview& preview,
                                         std::string_view id) {
    for (const auto& change : preview.changed_walls()) {
        if (change.wall_id == id) {
            return change;
        }
    }
    fail("preview omitted an expected changed wall");
}

void require_rejected_unchanged(Document& document, const ConstraintAuthoringPreview& preview,
                                std::string_view message) {
    const auto before = document.snapshot();
    bool rejected = false;
    try {
        (void)apply_constraint_authoring(document, preview);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
    const auto after = document.snapshot();
    require(after.revision() == before.revision() && after.entities() == before.entities(),
            "rejected Apply mutated the document");
}

void test_resize_twelve_to_fourteen_feet_with_either_anchor_and_exact_receipt() {
    constexpr double twelve_feet = 12.0 * 0.3048;
    constexpr double fourteen_feet = 14.0 * 0.3048;
    for (const auto anchor : {WallResizeAnchor::start, WallResizeAnchor::end}) {
        auto document = Document::create({wall("wall-a", {2.0, 3.0}, {2.0 + twelve_feet, 3.0})});
        const auto before = document.snapshot();
        ConstraintAuthoringIntent intent;
        intent.wall_resize = WallResizeIntent{
            "wall-a", parse_quantity("14 ft"), anchor, false};
        intent.message = "resize exact wall";
        const auto preview = preview_constraint_authoring(before, intent);
        require_accepted(preview, "a valid isolated resize was rejected");
        require(preview.document_id() == before.document_id() &&
                    preview.expected_revision() == before.revision() &&
                    !preview.source_snapshot_digest().empty() && !preview.candidate_digest().empty(),
                "preview is not bound to its complete source snapshot and candidate");
        const auto& change = changed_wall(preview, "wall-a");
        require_near(length(change.old_baseline), twelve_feet, 1e-12, "old wall length");
        require_near(length(change.proposed_baseline), fourteen_feet, 1e-9, "proposed wall length");
        if (anchor == WallResizeAnchor::start) {
            require_near(change.proposed_baseline.start.x, change.old_baseline.start.x, 1e-12,
                 "start anchor moved");
            require_near(change.proposed_baseline.start.y, change.old_baseline.start.y, 1e-12,
                 "start anchor moved vertically");
        } else {
            require_near(change.proposed_baseline.end.x, change.old_baseline.end.x, 1e-12,
                 "end anchor moved");
            require_near(change.proposed_baseline.end.y, change.old_baseline.end.y, 1e-12,
                 "end anchor moved vertically");
        }
        const auto revision = apply_constraint_authoring(document, preview);
        require(revision == before.revision() + 1, "resize did not create one history revision");
        const auto stored = document.snapshot().entities().at("wall-a");
        require_near(length(baseline(stored)), fourteen_feet, 1e-9, "stored wall length");
        require(stored.properties.at("future_wall_metadata").at("retain") == true &&
                    stored.extensions.at("future_extension") == json({1, 2, 3}),
                "wall resize discarded unrelated metadata");
        const auto& receipt = stored.extensions.at("constraint_authoring")
                                  .at("last_length_entry");
        require(receipt.at("original_expression") == "14 ft" &&
                    receipt.at("entered_unit") == "ft" &&
                    receipt.at("exact_metres").at("numerator") == 2667 &&
                    receipt.at("exact_metres").at("denominator") == 625 &&
                    receipt.at("baseline").at("start") == stored.properties.at("baseline").at("start") &&
                    receipt.at("baseline").at("end") == stored.properties.at("baseline").at("end") &&
                    receipt.at("baseline").at("sweep_radians") == 0.0,
                "wall resize did not preserve the exact entered quantity");
    }
}

void test_nested_metadata_and_receipt_validation() {
    auto original = wall("wall-a", {0, 0}, {4, 0});
    original.properties["baseline"]["future_key"] = {{"retain", true}};
    original.extensions["constraint_authoring"] =
        {{"version", 1}, {"last_length_entry",
            {{"version", 1}, {"original_expression", "4 m"}, {"entered_unit", "m"},
             {"exact_metres", {{"numerator", 4}, {"denominator", 1}, {"future_exact", true}}},
             {"baseline", segment_json({0, 0}, {4, 0})}, {"future_receipt", true}}}};
    original.extensions["constraint_authoring"]["last_length_entry"]["baseline"]["future_baseline"] = true;
    auto document = Document::create({original});
    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("5 m"), WallResizeAnchor::start, false};
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require_accepted(preview, "valid receipt with opaque metadata rejected");
    const auto check = [&](const Entity& value) {
        require(value.properties.at("baseline").contains("future_key"),
                "resize discarded nested baseline metadata");
        const auto& receipt = value.extensions.at("constraint_authoring").at("last_length_entry");
        require(receipt.at("future_receipt") == true &&
                    receipt.at("exact_metres").at("future_exact") == true &&
                    receipt.at("baseline").at("future_baseline") == true &&
                    receipt.at("original_expression") == "5 m" &&
                    receipt.at("exact_metres").at("numerator") == 5,
                "resize discarded nested receipt metadata or failed to update known values");
    };
    check(preview.candidate_entities().at("wall-a"));
    apply_constraint_authoring(document, preview);
    check(document.snapshot().entities().at("wall-a"));
    document.undo(document.revision());
    require(document.snapshot().entities().at("wall-a") == original,
            "undo lost original nested metadata");
    document.redo(document.revision());
    const auto path = std::filesystem::temp_directory_path() /
        ("constraint-nested-" + make_stable_id() + ".bldproj");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{path};
    (void)ProjectStore::save(path, document.snapshot());
    auto reopened = ProjectStore::load(path);
    check(reopened.document.snapshot().entities().at("wall-a"));
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities().at("wall-a") == original,
            "reopened undo lost original nested metadata");

    const auto good_receipt = original.extensions.at("constraint_authoring").at("last_length_entry");
    std::vector<json> invalid_receipts;
    invalid_receipts.push_back({{"version", 1}, {"future_receipt", true}});
    auto bad = good_receipt;
    bad["exact_metres"]["numerator"] = 5;
    invalid_receipts.push_back(bad);
    bad = good_receipt;
    bad["exact_metres"]["denominator"] = 0;
    invalid_receipts.push_back(bad);
    bad = good_receipt;
    bad["entered_unit"] = "ft";
    invalid_receipts.push_back(bad);
    bad = good_receipt;
    bad["baseline"]["end"] = {3, 0};
    invalid_receipts.push_back(bad);
    bad = good_receipt;
    bad["baseline"]["sweep_radians"] = 0.1;
    invalid_receipts.push_back(bad);
    bad = good_receipt;
    bad["exact_metres"]["numerator"] = 4.0;
    invalid_receipts.push_back(bad);
    for (const auto& receipt : invalid_receipts) {
        auto malformed = original;
        malformed.extensions["constraint_authoring"]["last_length_entry"] = receipt;
        bool creation_rejected=false;
        try { (void)Document::create({malformed}); }
        catch (const DocumentError& error) {
            creation_rejected=error.code()==DocumentErrorCode::invalid_entity &&
                std::string_view(error.what()).find("Wall length receipt")!=std::string_view::npos;
        }
        require(creation_rejected,"known malformed receipt bypassed independent document admission");
        auto admitted=Document::create({original});
        admitted.mark_saved(admitted.revision());
        const auto before=admitted.snapshot();
        bool edit_rejected=false;
        try { admitted.apply(ApplyEntityChanges{admitted.revision(),{EntityChange::upsert(malformed)},{},
            "Forge a known receipt"}); }
        catch (const DocumentError& error) { edit_rejected=error.code()==DocumentErrorCode::invalid_entity; }
        require(edit_rejected && admitted.snapshot().entities()==before.entities() &&
            admitted.revision()==before.revision() && admitted.snapshot().history().size()==before.history().size() &&
            admitted.snapshot().saved_revision_optional()==before.saved_revision_optional(),
            "known malformed receipt edit was accepted or mutated admitted state");
    }

    ConstraintAuthoringIntent straighten;
    straighten.relation_anchor = endpoint("wall-a", WallEndpointRole::start);
    straighten.relation_mutations.push_back(ConstraintRelationMutation::upsert(
        relation("horizontal", ConstraintRelationKind::horizontal,
                 {endpoint("wall-a", WallEndpointRole::start), endpoint("wall-a", WallEndpointRole::end)})));
    for (const bool opaque : {false, true}) {
        auto diagonal = wall("wall-a", {0, 0}, {3, 4});
        diagonal.extensions["constraint_authoring"] =
            {{"version", 1}, {"last_length_entry",
                {{"version", 1}, {"original_expression", "5 m"}, {"entered_unit", "m"},
                 {"exact_metres", {{"numerator", 5}, {"denominator", 1}}},
                 {"baseline", segment_json({0, 0}, {3, 4})}}}};
        if (opaque) diagonal.extensions["constraint_authoring"]["last_length_entry"]["future_key"] = true;
        auto changed = Document::create({diagonal});
        const auto result = preview_constraint_authoring(changed.snapshot(), straighten);
        if (opaque) {
            require(!result.accepted() && has_diagnostic(result, "would discard"),
                    "relation edit silently erased opaque receipt metadata");
            require_rejected_unchanged(changed, result, "opaque invalidated receipt Apply accepted");
        } else {
            require_accepted(result, "recognized receipt could not be invalidated by relation edit");
            require(!result.candidate_entities().at("wall-a").extensions.at("constraint_authoring")
                        .contains("last_length_entry"),
                    "relation edit retained a stale receipt");
        }
    }
}

void test_rigid_transform_rebases_length_receipt_without_losing_metadata() {
    auto original = wall("wall-a", {0, 0}, {3, 4});
    original.extensions["constraint_authoring"] =
        {{"version", 1}, {"future_section", {1, 2}}, {"last_length_entry",
            {{"version", 1}, {"original_expression", "500 cm"}, {"entered_unit", "cm"},
             {"exact_metres", {{"numerator", 5}, {"denominator", 1}, {"opaque", true}}},
             {"baseline", segment_json({0, 0}, {3, 4})}, {"future_receipt", "retain"}}}};
    original.extensions["constraint_authoring"]["last_length_entry"]["baseline"]["opaque"] = true;
    for (const auto& transformed : std::vector<Segment>{
             {{10, -2}, {13, 2}, 0}, {{10, -2}, {6, 1}, 0}}) {
        auto actual = original;
        auto expected = original;
        auto& expected_baseline = expected.extensions["constraint_authoring"]
                                     ["last_length_entry"]["baseline"];
        expected_baseline["start"] = {transformed.start.x, transformed.start.y};
        expected_baseline["end"] = {transformed.end.x, transformed.end.y};
        expected_baseline["sweep_radians"] = transformed.sweep_radians;
        rebase_wall_length_receipt(actual, transformed);
        require(actual == expected,
                "rigid receipt transform changed properties, exact input, or opaque metadata");
        actual.properties["baseline"] = segment_json(transformed.start, transformed.end);
        rebase_wall_length_receipt(actual, transformed);
    }

    const Segment valid{{10, -2}, {6, 1}, 0};
    const auto reject = [&](Entity value, const Segment& transformed) {
        const auto before = value;
        bool rejected = false;
        try { rebase_wall_length_receipt(value, transformed); }
        catch (const std::exception&) { rejected = true; }
        require(rejected && value == before,
                "invalid receipt transform was accepted or mutated its input");
    };
    auto bad = original;
    bad.extensions["constraint_authoring"]["version"] = 2;
    reject(bad, valid);
    bad.extensions["constraint_authoring"] = "malformed";
    reject(bad, valid);
    bad = original;
    bad.extensions["constraint_authoring"]["last_length_entry"]["version"] = 2;
    reject(bad, valid);
    bad = original;
    bad.extensions["constraint_authoring"]["last_length_entry"].erase("exact_metres");
    reject(bad, valid);
    bad = original;
    bad.extensions["constraint_authoring"]["last_length_entry"]["baseline"]["end"] = {4, 3};
    reject(bad, valid);
    reject(original, {{0, 0}, {6, 0}, 0});
    reject(original, {{0, 0}, {3, 4}, 0.1});
    reject(original, {{0, 0}, {std::numeric_limits<double>::infinity(), 4}, 0});
    reject(original, {{0, 0}, {3, 4}, std::numeric_limits<double>::quiet_NaN()});

    auto absent = wall("wall-a", {0, 0}, {3, 4});
    auto before = absent;
    rebase_wall_length_receipt(absent, valid);
    require(absent == before, "missing receipt was not a no-op");
    absent.extensions["constraint_authoring"] = {{"version", 1}, {"opaque", true}};
    before = absent;
    rebase_wall_length_receipt(absent, valid);
    require(absent == before, "empty supported receipt envelope was not a no-op");
}

void test_connected_resize_moves_only_an_explicit_component() {
    constexpr double twelve_feet = 12.0 * 0.3048;
    auto joined = relation("join", ConstraintRelationKind::coincident,
                           {endpoint("wall-a", WallEndpointRole::end),
                            endpoint("wall-b", WallEndpointRole::start)});
    auto document = Document::create({
        wall("wall-a", {0.0, 0.0}, {twelve_feet, 0.0}),
        wall("wall-b", {twelve_feet, 0.0}, {twelve_feet, 2.0}),
        wall("wall-c", {0.0, 0.0}, {-1.0, 1.0}),
        encode_constraint_entity(joined),
    });
    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("14 ft"), WallResizeAnchor::start, true};
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require_accepted(preview, "connected resize should solve its explicit component");
    const auto& first = changed_wall(preview, "wall-a");
    const auto& second = changed_wall(preview, "wall-b");
    require_near(first.proposed_baseline.end.x, 14.0 * 0.3048, 1e-8,
         "selected connected endpoint");
    require_near(second.proposed_baseline.start.x, first.proposed_baseline.end.x, 1e-8,
         "explicitly coincident endpoint did not propagate");
    require(!preview.candidate_entities().at("wall-c").properties.at("baseline").is_null() &&
                baseline(preview.candidate_entities().at("wall-c")).start.x == 0.0 &&
                baseline(preview.candidate_entities().at("wall-c")).end.x == -1.0,
            "coincident coordinates created an implicit component connection");
    (void)apply_constraint_authoring(document, preview);
}

void test_disabled_connected_movement_freezes_other_walls_and_rejects_conflict() {
    constexpr double twelve_feet = 12.0 * 0.3048;
    auto joined = relation("join", ConstraintRelationKind::coincident,
                           {endpoint("wall-a", WallEndpointRole::end),
                            endpoint("wall-b", WallEndpointRole::start)});
    auto document = Document::create({
        wall("wall-a", {0.0, 0.0}, {twelve_feet, 0.0}),
        wall("wall-b", {twelve_feet, 0.0}, {twelve_feet, 2.0}),
        encode_constraint_entity(joined),
    });
    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("14 ft"), WallResizeAnchor::start, false};
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require(!preview.accepted() && !preview.diagnostics().empty(),
            "disabled propagation ignored a fixed neighbor conflict");
    require_rejected_unchanged(document, preview, "a rejected connected preview was applied");
}

void test_solver_conflicts_use_semantic_wall_diagnostics() {
    auto named_wall = wall("wall-a", {0.0, 0.0}, {12.0 * 0.3048, 0.0});
    named_wall.properties["name"] = "Wall A";
    auto locked_length = relation("length-lock", ConstraintRelationKind::fixed_length,
                                  {endpoint("wall-a", WallEndpointRole::start),
                                   endpoint("wall-a", WallEndpointRole::end)});
    locked_length.length = parse_quantity("12 ft");
    auto document = Document::create({named_wall, encode_constraint_entity(locked_length)});

    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("14 ft"), WallResizeAnchor::start, false};
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);

    require(!preview.accepted(), "a locked resize unexpectedly solved");
    require(has_diagnostic(preview, "Conflicting fixed length (12 ft) on Wall A"),
            "locked resize omitted its readable fixed-length conflict");
    require(has_diagnostic(preview, "Conflicting fixed endpoint: Wall A"),
            "locked resize omitted its readable fixed-endpoint conflict");
    require(std::none_of(preview.diagnostics().begin(), preview.diagnostics().end(),
                         [](const std::string& diagnostic) {
                             return diagnostic.find("__constraint_authoring_") != std::string::npos ||
                                 diagnostic.find("length-lock") != std::string::npos;
                         }),
            "solver implementation identifiers leaked into user-facing diagnostics");
}

void test_overlap_and_endpoint_reversal_are_rejected_after_solve() {
    constexpr double twelve_feet = 12.0 * 0.3048;
    auto overlap_document = Document::create({
        wall("wall-a", {0.0, 0.0}, {twelve_feet, 0.0}),
        wall("wall-b", {twelve_feet, 0.0}, {twelve_feet + 2.0, 0.0}),
    });
    ConstraintAuthoringIntent overlap;
    overlap.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("14 ft"), WallResizeAnchor::start, true};
    const auto overlap_preview = preview_constraint_authoring(overlap_document.snapshot(), overlap);
    require(!overlap_preview.accepted() && has_diagnostic(overlap_preview, "overlap"),
            "wall extension created an unreported overlap branch");

    auto orthogonal_document = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0})});
    auto orthogonal = relation("move-end", ConstraintRelationKind::fixed_anchor,
                               {endpoint("wall-a", WallEndpointRole::end)});
    orthogonal.anchor = Vec2{0.0, 4.0};
    ConstraintAuthoringIntent rotate;
    rotate.relation_mutations.push_back(ConstraintRelationMutation::upsert(orthogonal));
    rotate.relation_anchor = endpoint("wall-a", WallEndpointRole::start);
    const auto rotate_preview =
        preview_constraint_authoring(orthogonal_document.snapshot(), rotate);
    require_accepted(rotate_preview, "a valid 90-degree relation solve was rejected");
    const auto rotated = changed_wall(rotate_preview, "wall-a").proposed_baseline;
    require_near(rotated.end.x, 0.0, 1e-10, "orthogonal solve end x");
    require_near(rotated.end.y, 4.0, 1e-10, "orthogonal solve end y");

    auto joined = relation("join", ConstraintRelationKind::coincident,
                           {endpoint("wall-a", WallEndpointRole::start),
                            endpoint("wall-b", WallEndpointRole::start)});
    auto reversal_document = Document::create({
        wall("wall-a", {0.0, 0.0}, {4.0, 0.0}),
        wall("wall-b", {0.0, 0.0}, {0.0, 2.0}),
        encode_constraint_entity(joined),
    });
    auto move_start = relation("move-start", ConstraintRelationKind::fixed_anchor,
                               {endpoint("wall-a", WallEndpointRole::start)});
    move_start.anchor = Vec2{4.0, 0.0};
    auto move_end = relation("move-end", ConstraintRelationKind::fixed_anchor,
                             {endpoint("wall-a", WallEndpointRole::end)});
    move_end.anchor = Vec2{0.0, 0.0};
    ConstraintAuthoringIntent reverse;
    reverse.relation_mutations.push_back(ConstraintRelationMutation::upsert(move_start));
    reverse.relation_mutations.push_back(ConstraintRelationMutation::upsert(move_end));
    reverse.relation_anchor = endpoint("wall-b", WallEndpointRole::end);
    const auto reverse_preview =
        preview_constraint_authoring(reversal_document.snapshot(), reverse);
    require(!reverse_preview.accepted() && has_diagnostic(reverse_preview, "reverse"),
            "relation solve silently reversed stable wall endpoint identity");
}

void test_topology_is_scale_translation_and_drawing_plane_aware() {
    struct TopologyCase {
        Vec2 origin;
        double original_length;
        std::string resize_expression;
    };
    for (const auto& item : std::vector<TopologyCase>{
             {{0.0, 0.0}, 1.0, "2 m"},
             {{1'000'000.0, -1'000'000.0}, 0.001, "2 mm"},
             {{20.0, 30.0}, 0.0001, "0.2 mm"},
         }) {
        const auto crossing_x = item.origin.x + item.original_length * 1.5;
        auto first = wall("wall-a", item.origin,
                          {item.origin.x + item.original_length, item.origin.y});
        auto second = wall("wall-b", {crossing_x, item.origin.y - item.original_length},
                           {crossing_x, item.origin.y + item.original_length});
        auto document = Document::create({first, second});
        ConstraintAuthoringIntent intent;
        intent.wall_resize = WallResizeIntent{
            "wall-a", parse_quantity(item.resize_expression), WallResizeAnchor::start, false};
        const auto preview = preview_constraint_authoring(document.snapshot(), intent);
        require(!preview.accepted() && has_diagnostic(preview, "crossing"),
                "topology crossing result changed with scale or translation");
    }

    auto ground = wall("wall-a", {0.0, 0.0}, {1.0, 0.0});
    ground.properties["layer_id"] = "ground-layer";
    auto upper = wall("wall-b", {1.5, -1.0}, {1.5, 1.0});
    upper.properties["layer_id"] = "upper-layer";
    auto document = Document::create({
        Entity{"property", "property"},
        Entity{"building", "building", {{"property_id", "property"}}},
        Entity{"ground", "floor", {{"building_id", "building"}}},
        Entity{"upper", "floor", {{"building_id", "building"}}},
        Entity{"ground-layer", "layer", {{"floor_id", "ground"}}},
        Entity{"upper-layer", "layer", {{"floor_id", "upper"}}},
        ground,
        upper,
    });
    ConstraintAuthoringIntent resize;
    resize.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("2 m"), WallResizeAnchor::start, false};
    const auto preview = preview_constraint_authoring(document.snapshot(), resize);
    require_accepted(preview, "an upper-floor projection blocked a ground-floor resize");

    auto incomplete = wall("wall-a", {0.0, 0.0}, {1.0, 0.0});
    incomplete.properties["floor_id"] = "ground";
    auto incomplete_document = Document::create({
        Entity{"property", "property"},
        Entity{"building", "building", {{"property_id", "property"}}},
        Entity{"ground", "floor", {{"building_id", "building"}}},
        incomplete,
    });
    const auto incomplete_preview =
        preview_constraint_authoring(incomplete_document.snapshot(), resize);
    require(!incomplete_preview.accepted() &&
                has_diagnostic(incomplete_preview, "unresolved explicit drawing context"),
            "affected wall with an incomplete explicit drawing context was solved");
}

void test_explicit_wall_cycle_preserves_winding_branch() {
    auto join_ab = relation("join-ab", ConstraintRelationKind::coincident,
                            {endpoint("wall-a", WallEndpointRole::end),
                             endpoint("wall-b", WallEndpointRole::start)});
    auto join_bc = relation("join-bc", ConstraintRelationKind::coincident,
                            {endpoint("wall-b", WallEndpointRole::end),
                             endpoint("wall-c", WallEndpointRole::start)});
    auto join_ca = relation("join-ca", ConstraintRelationKind::coincident,
                            {endpoint("wall-c", WallEndpointRole::end),
                             endpoint("wall-a", WallEndpointRole::start)});
    auto document = Document::create({
        wall("wall-a", {0.0, 0.0}, {4.0, 0.0}),
        wall("wall-b", {4.0, 0.0}, {0.0, 3.0}),
        wall("wall-c", {0.0, 3.0}, {0.0, 0.0}),
        encode_constraint_entity(join_ab),
        encode_constraint_entity(join_bc),
        encode_constraint_entity(join_ca),
    });
    auto force_flip = relation("force-flip", ConstraintRelationKind::fixed_anchor,
                               {endpoint("wall-b", WallEndpointRole::end)});
    force_flip.anchor = Vec2{0.0, -3.0};
    ConstraintAuthoringIntent intent;
    intent.relation_mutations.push_back(ConstraintRelationMutation::upsert(force_flip));
    intent.relation_anchor = endpoint("wall-a", WallEndpointRole::start);
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require(!preview.accepted() && has_diagnostic(preview, "winding"),
            "explicitly connected wall cycle changed winding branch");

    const auto boundary = encode_identified_boundary_entity(IdentifiedBoundary{"area","measurement_boundary",{
        {"ab","a","b",{{0,0},{-2,0},0}}, {"bc","b","c",{{-2,0},{-2,-2},0}},
        {"cd","c","d",{{-2,-2},{0,-2},0}}, {"da","d","a",{{0,-2},{0,0},0}}}});
    const WallEndpointBinding shared{"area",WallEndpointRole::start,"ab","a"};
    for (const bool boundary_first : {true,false}) {
        auto join_boundary_a = relation("join-boundary-a",ConstraintRelationKind::coincident,
            {shared,endpoint("wall-a",WallEndpointRole::start)});
        auto join_boundary_c = relation("join-boundary-c",ConstraintRelationKind::coincident,
            {shared,endpoint("wall-c",WallEndpointRole::end)});
        if (!boundary_first) {
            std::reverse(join_boundary_a.bindings.begin(),join_boundary_a.bindings.end());
            std::reverse(join_boundary_c.bindings.begin(),join_boundary_c.bindings.end());
        }
        auto mixed = Document::create({boundary,
            wall("wall-a",{0,0},{4,0}),wall("wall-b",{4,0},{0,3}),wall("wall-c",{0,3},{0,0}),
            encode_constraint_entity(join_ab),encode_constraint_entity(join_bc),
            encode_constraint_entity(join_boundary_a),encode_constraint_entity(join_boundary_c)});
        const auto source = mixed.snapshot();
        const auto proposed = preview_constraint_authoring(source,intent);
        require(!proposed.accepted() && has_diagnostic(proposed,"winding"),
            "a wall cycle closed through a boundary vertex must protect winding in either binding order");
        require_rejected_unchanged(mixed,proposed,"mixed winding failure committed");
        require(mixed.snapshot().entities() == source.entities(),"mixed winding refusal moved geometry");
    }

    for (const bool boundary_first : {true,false}) {
        auto first = relation("join-first",ConstraintRelationKind::coincident,
            {shared,endpoint("wall-a",WallEndpointRole::end)});
        auto second = relation("join-second",ConstraintRelationKind::coincident,
            {shared,endpoint("wall-b",WallEndpointRole::start)});
        if (!boundary_first) {
            std::reverse(first.bindings.begin(),first.bindings.end());
            std::reverse(second.bindings.begin(),second.bindings.end());
        }
        auto separated = Document::create({boundary,wall("wall-a",{-2,0},{-1,0}),
            wall("wall-b",{1,0},{2,0})});
        ConstraintAuthoringIntent unite;
        unite.relation_anchor = shared;
        unite.relation_mutations = {ConstraintRelationMutation::upsert(first),ConstraintRelationMutation::upsert(second)};
        const auto connected = preview_constraint_authoring(separated.snapshot(),unite);
        require_accepted(connected,"transitive declared coincidence through a boundary vertex must permit wall contact");
        require_near(baseline(connected.candidate_entities().at("wall-a")).end.x,0,1e-7,"first transitive endpoint");
        require_near(baseline(connected.candidate_entities().at("wall-b")).start.x,0,1e-7,"second transitive endpoint");
        (void)apply_constraint_authoring(separated,connected);

        // The same coordinates without the second point-equivalence are an
        // implicit wall connection and must still fail the topology guard.
        auto incidental = Document::create({boundary,wall("wall-a",{-2,0},{-1,0}),
            wall("wall-b",{1,0},{2,0})});
        auto pin = relation("pin-second",ConstraintRelationKind::fixed_anchor,
            {endpoint("wall-b",WallEndpointRole::start)});
        pin.anchor = Vec2{0,0};
        auto level = relation("level-second",ConstraintRelationKind::horizontal,
            {shared,endpoint("wall-b",WallEndpointRole::start)});
        unite.relation_mutations = {ConstraintRelationMutation::upsert(first),
            ConstraintRelationMutation::upsert(pin),ConstraintRelationMutation::upsert(level)};
        const auto rejected = preview_constraint_authoring(incidental.snapshot(),unite);
        require(!rejected.accepted() && has_diagnostic(rejected,"implicit"),
            "incidental wall contact must not become a declared transitive join");
        require_rejected_unchanged(incidental,rejected,"incidental mixed contact applied");
    }
}

void test_all_seven_relations_add_edit_remove_and_relation_solves_move_geometry() {
    struct Case {
        ConstraintRelationKind kind;
        std::vector<WallEndpointBinding> bindings;
        std::optional<Quantity> length;
        std::optional<Vec2> fixed_anchor;
        WallEndpointBinding authoring_anchor;
    };
    const std::vector<Case> cases{
        {ConstraintRelationKind::horizontal,
         {endpoint("wall-a", WallEndpointRole::start), endpoint("wall-a", WallEndpointRole::end)},
         std::nullopt, std::nullopt, endpoint("wall-a", WallEndpointRole::start)},
        {ConstraintRelationKind::vertical,
         {endpoint("wall-b", WallEndpointRole::start), endpoint("wall-b", WallEndpointRole::end)},
         std::nullopt, std::nullopt, endpoint("wall-b", WallEndpointRole::start)},
        {ConstraintRelationKind::coincident,
         {endpoint("wall-a", WallEndpointRole::end), endpoint("wall-b", WallEndpointRole::start)},
         std::nullopt, std::nullopt, endpoint("wall-a", WallEndpointRole::end)},
        {ConstraintRelationKind::fixed_length,
         {endpoint("wall-a", WallEndpointRole::start), endpoint("wall-a", WallEndpointRole::end)},
         parse_quantity("5 m"), std::nullopt, endpoint("wall-a", WallEndpointRole::start)},
        {ConstraintRelationKind::parallel,
         {endpoint("wall-a", WallEndpointRole::start), endpoint("wall-a", WallEndpointRole::end),
          endpoint("wall-c", WallEndpointRole::start), endpoint("wall-c", WallEndpointRole::end)},
         std::nullopt, std::nullopt, endpoint("wall-a", WallEndpointRole::start)},
        {ConstraintRelationKind::perpendicular,
         {endpoint("wall-a", WallEndpointRole::start), endpoint("wall-a", WallEndpointRole::end),
          endpoint("wall-d", WallEndpointRole::start), endpoint("wall-d", WallEndpointRole::end)},
         std::nullopt, std::nullopt, endpoint("wall-a", WallEndpointRole::start)},
        {ConstraintRelationKind::fixed_anchor,
         {endpoint("wall-b", WallEndpointRole::start)}, std::nullopt, Vec2{1.0, 1.0},
         endpoint("wall-b", WallEndpointRole::end)},
    };

    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto document = Document::create({
            wall("wall-a", {0.0, 0.0}, {4.0, 1.0}),
            wall("wall-b", {10.0, 0.0}, {11.0, 4.0}),
            wall("wall-c", {0.0, 10.0}, {3.0, 11.0}),
            wall("wall-d", {10.0, 10.0}, {11.0, 13.0}),
        });
        auto value = relation("relation", cases[index].kind, cases[index].bindings);
        value.length = cases[index].length;
        value.anchor = cases[index].fixed_anchor;
        ConstraintAuthoringIntent add;
        add.relation_mutations.push_back(ConstraintRelationMutation::upsert(value));
        add.relation_anchor = cases[index].authoring_anchor;
        add.relation_move_connected_walls = true;
        const auto relation_name = std::string(constraint_relation_name(cases[index].kind));
        const auto add_preview = preview_constraint_authoring(document.snapshot(), add);
        require_accepted(add_preview, "relation addition was rejected: " + relation_name);
        require(!add_preview.changed_walls().empty(),
                "relation solve did not expose its genuinely changed geometry");
        (void)apply_constraint_authoring(document, add_preview);
        require(document.snapshot().entities().contains("relation"),
                "relation addition was not committed atomically");

        auto edited = value;
        auto edit_authoring_anchor = cases[index].authoring_anchor;
        if (edited.relation == ConstraintRelationKind::fixed_length) {
            edited.length = parse_quantity("6 m");
        } else if (edited.relation == ConstraintRelationKind::fixed_anchor) {
            edited.anchor = Vec2{2.0, 1.0};
        } else if (edited.relation == ConstraintRelationKind::horizontal) {
            edited.bindings = {endpoint("wall-c", WallEndpointRole::start),
                               endpoint("wall-c", WallEndpointRole::end)};
            edit_authoring_anchor = endpoint("wall-c", WallEndpointRole::start);
        } else if (edited.relation == ConstraintRelationKind::vertical) {
            edited.bindings = {endpoint("wall-d", WallEndpointRole::start),
                               endpoint("wall-d", WallEndpointRole::end)};
            edit_authoring_anchor = endpoint("wall-d", WallEndpointRole::start);
        } else if (edited.relation == ConstraintRelationKind::coincident) {
            edited.bindings = {endpoint("wall-a", WallEndpointRole::start),
                               endpoint("wall-b", WallEndpointRole::start)};
            edit_authoring_anchor = endpoint("wall-a", WallEndpointRole::start);
        } else if (edited.relation == ConstraintRelationKind::parallel) {
            edited.bindings = {endpoint("wall-a", WallEndpointRole::start),
                               endpoint("wall-a", WallEndpointRole::end),
                               endpoint("wall-d", WallEndpointRole::start),
                               endpoint("wall-d", WallEndpointRole::end)};
        } else if (edited.relation == ConstraintRelationKind::perpendicular) {
            edited.bindings = {endpoint("wall-a", WallEndpointRole::start),
                               endpoint("wall-a", WallEndpointRole::end),
                               endpoint("wall-c", WallEndpointRole::start),
                               endpoint("wall-c", WallEndpointRole::end)};
        }
        ConstraintAuthoringIntent edit;
        edit.relation_mutations.push_back(ConstraintRelationMutation::upsert(edited));
        edit.relation_anchor = edit_authoring_anchor;
        edit.relation_move_connected_walls = true;
        const auto edit_preview = preview_constraint_authoring(document.snapshot(), edit);
        require_accepted(edit_preview, "relation edit was rejected: " + relation_name);
        require(!edit_preview.changed_walls().empty(),
                "one of the seven relation edits did not re-solve geometry");
        (void)apply_constraint_authoring(document, edit_preview);

        ConstraintAuthoringIntent remove;
        remove.relation_mutations.push_back(ConstraintRelationMutation::remove("relation"));
        const auto before_remove = document.snapshot();
        const auto remove_preview = preview_constraint_authoring(before_remove, remove);
        require_accepted(remove_preview, "relation removal was rejected: " + relation_name);
        require(remove_preview.changed_walls().empty(), "relation removal should preserve geometry");
        (void)apply_constraint_authoring(document, remove_preview);
        require(!document.snapshot().entities().contains("relation") &&
                    baseline(document.snapshot().entities().at("wall-a")).start.x ==
                        baseline(before_remove.entities().at("wall-a")).start.x,
                "relation removal did not preserve geometry or erase the relation");
    }
}

void test_stale_foreign_same_revision_head_and_mutated_preview_are_rejected() {
    auto document = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0})});
    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("5 m"), WallResizeAnchor::start, false};
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require_accepted(preview, "security fixture preview was rejected");

    auto foreign = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0})});
    require_rejected_unchanged(foreign, preview, "foreign document accepted another preview");

    auto stale = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0})});
    const auto stale_preview = preview_constraint_authoring(stale.snapshot(), intent);
    auto renamed = stale.snapshot().entities().at("wall-a");
    renamed.properties["classification"] = "new head";
    stale.apply(ApplyEntityChanges{stale.revision(), {EntityChange::upsert(renamed)}, {}, "advance"});
    require_rejected_unchanged(stale, stale_preview, "stale preview was applied to a newer head");

    auto altered_snapshot = document.snapshot();
    auto& altered_history = const_cast<std::vector<RevisionRecord>&>(altered_snapshot.history());
    altered_history.at(static_cast<std::size_t>(altered_snapshot.revision()))
        .entities.at("wall-a").properties["classification"] = "forged same revision head";
    const auto altered_preview = preview_constraint_authoring(altered_snapshot, intent);
    require_accepted(altered_preview, "same-revision digest fixture was rejected early");
    require_rejected_unchanged(document, altered_preview,
                               "same identity/revision with a changed head bypassed source digest");

    auto tampered = preview;
    auto& candidate = const_cast<std::map<std::string, Entity, std::less<>>&>(
        tampered.candidate_entities());
    candidate.at("wall-a").properties["classification"] = "forged candidate";
    require_rejected_unchanged(document, tampered, "mutated candidate preview was applied");

    auto remapped = preview;
    auto& remapped_entities = const_cast<std::map<std::string, Entity, std::less<>>&>(
        remapped.candidate_entities());
    auto node = remapped_entities.extract("wall-a");
    node.key() = "forged-map-key";
    remapped_entities.insert(std::move(node));
    require_rejected_unchanged(document, remapped, "candidate map identity mismatch was applied");

    auto tampered_changes = preview;
    auto& changes = const_cast<std::vector<ConstraintWallChange>&>(tampered_changes.changed_walls());
    changes.front().proposed_baseline.end.x += 99.0;
    require_rejected_unchanged(document, tampered_changes, "mutated shown result was applied");
}

void test_history_reopen_and_host_failures() {
    auto locked = relation("length-lock", ConstraintRelationKind::fixed_length,
                           {endpoint("wall-a", WallEndpointRole::start),
                            endpoint("wall-a", WallEndpointRole::end)});
    locked.length = parse_quantity("5 m");
    auto document = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0})});
    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("5 m"), WallResizeAnchor::start, false};
    intent.relation_mutations.push_back(ConstraintRelationMutation::upsert(locked));
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require_accepted(preview, "compound resize and relation preview was rejected");
    const auto applied_revision = apply_constraint_authoring(document, preview);
    document.undo(applied_revision);
    require(!document.snapshot().entities().contains("length-lock"),
            "undo did not restore the atomic pre-authoring state");
    document.redo(document.revision());
    require(document.snapshot().entities().contains("length-lock"),
            "redo did not restore the persistent relation");

    const auto directory = std::filesystem::temp_directory_path() /
        ("constraint-authoring-" + make_stable_id());
    require(std::filesystem::create_directory(directory), "could not create reopen fixture");
    const auto path = directory / "authoring.psketch";
    struct Cleanup {
        std::filesystem::path file;
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{path, directory};
    (void)ProjectStore::save(path, document.snapshot());
    auto reopened = ProjectStore::load(path);
    require(reopened.document.snapshot().entities().contains("length-lock") &&
                std::abs(length(baseline(reopened.document.snapshot().entities().at("wall-a"))) -
                         5.0) < 1e-8,
            "save/reopen lost authored geometry or relation");

    auto hosted = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0}),
                                    opening("opening-a", "wall-a", 3.5, 0.4)});
    ConstraintAuthoringIntent shrink;
    shrink.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("3.6 m"), WallResizeAnchor::start, false};
    const auto host_preview = preview_constraint_authoring(hosted.snapshot(), shrink);
    require(!host_preview.accepted(), "resize stranded a hosted opening");

    auto curved = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0}, 0.4)});
    const auto curve_preview = preview_constraint_authoring(curved.snapshot(), intent);
    require(!curve_preview.accepted(), "curve was flattened by line constraint authoring");

    auto future_wall = wall("wall-a", {0.0, 0.0}, {4.0, 0.0});
    future_wall.extensions["constraint_authoring"] =
        {{"version", 99}, {"future_length_semantics", true}};
    auto future = Document::create({future_wall});
    const auto future_preview = preview_constraint_authoring(future.snapshot(), intent);
    require(!future_preview.accepted(),
            "resize overwrote unsupported wall length receipt semantics");

    auto future_entry_wall = wall("wall-a", {0.0, 0.0}, {4.0, 0.0});
    future_entry_wall.extensions["constraint_authoring"] =
        {{"version", 1},
         {"last_length_entry", {{"version", 99}, {"future_receipt", true}}},
         {"opaque_section_metadata", "retain"}};
    auto future_entry = Document::create({future_entry_wall});
    const auto future_entry_preview =
        preview_constraint_authoring(future_entry.snapshot(), intent);
    require(!future_entry_preview.accepted() &&
                has_diagnostic(future_entry_preview, "unsupported last_length_entry"),
            "resize overwrote unsupported nested length receipt semantics");

    auto missing = relation("missing", ConstraintRelationKind::horizontal,
                            {endpoint("absent", WallEndpointRole::start),
                             endpoint("absent", WallEndpointRole::end)});
    ConstraintAuthoringIntent missing_intent;
    missing_intent.relation_mutations.push_back(ConstraintRelationMutation::upsert(missing));
    const auto missing_preview = preview_constraint_authoring(document.snapshot(), missing_intent);
    require(!missing_preview.accepted(), "missing relation owner was accepted");
}

void test_straight_wall_only_authoring_retains_guarded_typed_intent() {
    auto document = Document::create({wall("straight", {0, 0}, {4, 0})});
    const auto initial = document.snapshot().entities();
    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{"straight", parse_quantity("5 m"), WallResizeAnchor::start, false};
    intent.message = "Resize straight wall";
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require_accepted(preview, "straight wall-only resize must remain authorable");
    (void)apply_constraint_authoring(document, preview);
    const auto edited = document.snapshot().entities();
    const auto snapshot = document.snapshot();
    const auto& retained = snapshot.history().back().boundary_constraint_changes;
    require(retained.has_value(), "straight wall-only authoring must retain typed endpoint intent");
    const auto encoded = command_to_json(*retained);
    require(encoded.at("version") == 4 && encoded.at("boundary_edits").empty() &&
                encoded.at("wall_edits").size() == 1 && !encoded.at("wall_edits")[0].contains("version") &&
                encoded.at("wall_edits")[0].at("length_entry").at("original_expression") == "5 m",
            "straight wall-only proof must use version4 with unchanged historical inner receipt encoding");
    require(command_to_json(command_from_json(encoded)) == encoded,
            "straight wall-only command codec must preserve exact endpoint and length evidence");
    for (const auto version : {1, 2, 3}) {
        auto downgraded = encoded;
        downgraded["version"] = version;
        bool rejected = false;
        try { (void)command_from_json(downgraded); } catch (const DocumentError&) { rejected = true; }
        require(rejected, "old command envelopes must not accept straight wall-only typed proof");
    }
    auto with_boundary = encoded;
    with_boundary["boundary_edits"] = json::array({{{"version", 1}, {"kind", "move_vertex"},
        {"boundary_id", "area"}, {"vertex_id", "corner"}, {"position", {0, 0}}}});
    bool rejected = false;
    try { (void)command_from_json(with_boundary); } catch (const DocumentError&) { rejected = true; }
    require(rejected, "version4 must be reserved for straight wall-only transactions");
    auto empty_walls = encoded;
    empty_walls["wall_edits"] = json::array();
    rejected = false;
    try { (void)command_from_json(empty_walls); } catch (const DocumentError&) { rejected = true; }
    require(rejected, "version4 must contain a nonempty straight wall proof");
    auto curved_proof = encoded;
    curved_proof["wall_edits"][0]["version"] = 2;
    curved_proof["wall_edits"][0]["baseline"]["sweep_radians"] = 0.4;
    curved_proof["wall_edits"][0]["length_entry"] = nullptr;
    rejected = false;
    try { (void)command_from_json(curved_proof); } catch (const DocumentError&) { rejected = true; }
    require(rejected, "version4 cannot substitute for the exact version3 curved proof envelope");
    auto restored = Document::fork(document.snapshot());
    restored.undo(restored.revision());
    require(restored.snapshot().entities() == initial, "restored typed straight authoring must undo exactly");
    restored.redo(restored.revision());
    require(restored.snapshot().entities() == edited, "restored typed straight authoring must redo exactly");

    // Generic explicit construction/transform commands retain their policy.
    auto transformed = Document::create({wall("straight", {0, 0}, {4, 0})});
    auto moved = transformed.snapshot().entities().at("straight");
    moved.properties["baseline"]["start"] = {2, 3};
    moved.properties["baseline"]["end"] = {6, 3};
    transformed.apply(ApplyEntityChanges{0, {EntityChange::upsert(moved)}, {}, "Explicit translation"});
    require(!transformed.snapshot().history().back().boundary_constraint_changes &&
                Document::fork(transformed.snapshot()).snapshot().entities().at("straight") == moved,
            "valid generic explicit straight wall transforms must retain their original admission policy");
}

void test_noop_and_cancel_leave_revision_saved_state_and_history_unchanged() {
    auto document = Document::create({wall("wall-a", {0.0, 0.0}, {4.0, 0.0})});
    document.mark_saved(document.revision());
    const auto initial = document.snapshot();
    const auto empty = preview_constraint_authoring(initial, ConstraintAuthoringIntent{});
    require(!empty.accepted(), "empty authoring intent should reject as a no-op");

    ConstraintAuthoringIntent same_length;
    same_length.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("4 m"), WallResizeAnchor::start, false};
    const auto same = preview_constraint_authoring(initial, same_length);
    require(!same.accepted(), "unchanged wall length should reject as a no-op");

    ConstraintAuthoringIntent cancel_intent;
    cancel_intent.wall_resize = WallResizeIntent{
        "wall-a", parse_quantity("5 m"), WallResizeAnchor::start, false};
    const auto cancel_preview = preview_constraint_authoring(initial, cancel_intent);
    require_accepted(cancel_preview, "cancel fixture preview was rejected");
    const auto after_cancel = document.snapshot();
    require(after_cancel.revision() == initial.revision() &&
                after_cancel.saved_revision_optional() == initial.saved_revision_optional() &&
                after_cancel.history().size() == initial.history().size() &&
                after_cancel.history().back().revision == initial.history().back().revision &&
                after_cancel.history().back().action == initial.history().back().action &&
                after_cancel.entities() == initial.entities(),
            "preview/cancel changed revision, saved state, history, or entities");
}

}  // namespace

void test_boundary_horizontal_authoring() {
    IdentifiedBoundary boundary{"measure", "measurement_boundary", {
        {"ab", "a", "b", {{0, 0}, {4, 1}, 0}},
        {"bc", "b", "c", {{4, 1}, {4, 4}, 0}},
        {"cd", "c", "d", {{4, 4}, {0, 4}, 0}},
        {"da", "d", "a", {{0, 4}, {0, 0}, 0}}}};
    auto document = Document::create({encode_identified_boundary_entity(boundary)});
    const auto before = document.snapshot();
    ConstraintAuthoringIntent intent;
    intent.relation_mutations.push_back(ConstraintRelationMutation::upsert(relation(
        "level", ConstraintRelationKind::horizontal,
        {{"measure", WallEndpointRole::start, "ab", "a"},
         {"measure", WallEndpointRole::end, "ab", "b"}})));
    intent.relation_anchor = WallEndpointBinding{"measure", WallEndpointRole::start, "ab", "a"};
    const auto preview = preview_constraint_authoring(before, intent);
    require_accepted(preview, "boundary horizontal solve rejected");
    require(preview.degrees_of_freedom() == 5, "boundary shared vertices have incorrect degrees of freedom");
    require(preview.changed_boundaries().size() == 1, "boundary preview omitted changed geometry");
    require(document.snapshot().entities() == before.entities(), "boundary preview mutated source");
    (void)apply_constraint_authoring(document, preview);
    const auto solved = decode_identified_boundary_entity(document.snapshot().entities().at("measure"));
    require_near(solved.segments[0].segment.end.y, 0, 1e-7, "boundary endpoint not horizontal");
    require(solved.segments[0].end_vertex_id == "b" && solved.segments[1].start_vertex_id == "b",
            "boundary vertex identity lost");
    require_near(solved.segments[1].segment.start.y, 0, 1e-7, "shared vertex not moved together");
    const auto committed = document.snapshot().entities();
    require_rejected_unchanged(document, preview, "stale boundary preview accepted");
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(), "boundary undo lost original state");
    document.redo(document.revision());
    require(document.snapshot().entities() == committed, "boundary redo lost solved state");
    const auto path = std::filesystem::temp_directory_path() / ("constraint-boundary-" + make_stable_id() + ".bldproj");
    (void)ProjectStore::save(path, document.snapshot());
    auto reopened = ProjectStore::load(path);
    std::filesystem::remove(path);
    require(reopened.document.snapshot().entities() == committed, "boundary locks lost on reopen");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == before.entities(), "reopened boundary undo lost original state");

    auto contradictory = intent;
    auto pin = relation("pin-b", ConstraintRelationKind::fixed_anchor,
                       {{"measure", WallEndpointRole::end, "ab", "b"}});
    pin.anchor = Vec2{4, 1};
    contradictory.relation_mutations.push_back(ConstraintRelationMutation::upsert(pin));
    const auto conflict = preview_constraint_authoring(before, contradictory);
    require(!conflict.accepted() && conflict.candidate_entities() == before.entities(),
            "contradictory boundary relation leaked candidate changes");
    auto bad_binding = intent;
    bad_binding.relation_mutations[0].constraint.bindings[1].vertex_id = "c";
    require(!preview_constraint_authoring(before, bad_binding).accepted(), "incorrect stable vertex binding accepted");
    boundary.segments[0].segment.sweep_radians = 0.2;
    auto curved = Document::create({encode_identified_boundary_entity(boundary)});
    require_accepted(preview_constraint_authoring(curved.snapshot(), intent), "curved boundary chord constraint rejected");
}

void test_boundary_cross_relations_and_fixed_length() {
    const auto rectangle = [](std::string id, double x, double tilt) {
        return encode_identified_boundary_entity(IdentifiedBoundary{std::move(id), "measurement_boundary", {
            {"ab", "a", "b", {{x, 0}, {x + 4, tilt}, 0}},
            {"bc", "b", "c", {{x + 4, tilt}, {x + 4, 4}, 0}},
            {"cd", "c", "d", {{x + 4, 4}, {x, 4}, 0}},
            {"da", "d", "a", {{x, 4}, {x, 0}, 0}}}});
    };
    for (const auto kind : {ConstraintRelationKind::parallel, ConstraintRelationKind::perpendicular,
                            ConstraintRelationKind::coincident, ConstraintRelationKind::fixed_length,
                            ConstraintRelationKind::vertical}) {
        auto document = Document::create({rectangle("first", 0, 0.5), rectangle("second", 10, 0)});
        auto relation_value = relation("cross", kind,
            {{"first", WallEndpointRole::start, "ab", "a"},
             {"first", WallEndpointRole::end, "ab", "b"}});
        if (kind == ConstraintRelationKind::parallel || kind == ConstraintRelationKind::perpendicular) {
            // A perpendicular target is a vertical side, avoiding a branch-flipping solve.
            const bool perpendicular = kind == ConstraintRelationKind::perpendicular;
            relation_value.bindings.push_back({"second", WallEndpointRole::start,
                perpendicular ? "bc" : "ab", perpendicular ? "b" : "a"});
            relation_value.bindings.push_back({"second", WallEndpointRole::end,
                perpendicular ? "bc" : "ab", perpendicular ? "c" : "b"});
        } else if (kind == ConstraintRelationKind::coincident) {
            relation_value.bindings[0] = {"first", WallEndpointRole::end, "ab", "b"};
            relation_value.bindings[1] = {"second", WallEndpointRole::start, "ab", "a"};
        } else if (kind == ConstraintRelationKind::fixed_length) {
            relation_value.length = parse_quantity("5 m");
        } else {
            relation_value.bindings = {{"first", WallEndpointRole::start, "bc", "b"},
                                       {"first", WallEndpointRole::end, "bc", "c"}};
        }
        ConstraintAuthoringIntent intent;
        intent.relation_anchor = WallEndpointBinding{"first", WallEndpointRole::start, "ab", "a"};
        intent.relation_mutations.push_back(ConstraintRelationMutation::upsert(relation_value));
        const auto preview = preview_constraint_authoring(document.snapshot(), intent);
        require_accepted(preview, "boundary relation fixture rejected");
        (void)apply_constraint_authoring(document, preview);
        const auto lock = decode_constraint_entity(document.snapshot().entities().at("cross"));
        require(lock.supported() && lock.constraint->bindings == relation_value.bindings,
                "cross-boundary relation identity changed");
    }
}

void test_boundary_receipt_and_dimension_preview() {
    IdentifiedBoundary boundary{"receipt-boundary", "measurement_boundary", {}};
    BoundaryConstructionRecord record;
    record.boundary_id = boundary.id;
    record.anchor = {0, 0};
    const Vec2 points[]{{0, 0}, {4, 1}, {4, 4}, {0, 4}};
    const char* rises[]{"1 m", "3 m", "0 m", "-4 m"};
    const char* runs[]{"4 m", "0 m", "-4 m", "0 m"};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto segment = "edge-" + std::to_string(i);
        const auto start = "vertex-" + std::to_string(i);
        const auto end = "vertex-" + std::to_string((i + 1) % 4);
        ConstructionReceipt receipt;
        receipt.segment_id = segment;
        receipt.kind = BoundaryConstructionKind::line_rise_run;
        receipt.start = points[i];
        receipt.rise = parse_quantity(rises[i]);
        receipt.run = parse_quantity(runs[i]);
        record.edges.push_back({segment, start, end, receipt});
        boundary.segments.push_back({segment, start, end, {points[i], points[(i + 1) % 4], 0}});
    }
    auto owner = encode_identified_boundary_entity(boundary);
    const auto receipt = encode_boundary_receipt_envelope(record);
    owner.properties["boundary_authoring"] = receipt;
    BoundaryDimension dimension;
    dimension.id = "dimension";
    dimension.boundary_id = boundary.id;
    dimension.segment_id = "edge-0";
    const auto dimension_entity = encode_boundary_dimension_entity(dimension);
    auto document = Document::create({owner, dimension_entity});
    ConstraintAuthoringIntent resize_receipt;
    resize_receipt.boundary_resize = BoundaryResizeIntent{
        {boundary.id,BoundaryGeometryEditKind::resize_segment,"edge-0",{},5},true};
    const auto resized_receipt = preview_constraint_authoring(document.snapshot(),resize_receipt);
    require_accepted(resized_receipt,"receipt-backed boundary resize rejected");
    const auto& resized_owner = resized_receipt.candidate_entities().at(boundary.id);
    require(resized_owner.extensions.at("boundary_geometry_derivation").at("source_boundary_authoring") == receipt,
        "boundary resize rewrote exact original construction input");
    require(resized_receipt.candidate_entities().at("dimension") == dimension_entity,
        "boundary resize changed attached dimension identity or presentation");
    require_near(dimension.resolve(resized_owner).segment_length(),5,1e-7,
        "dimension did not resolve resized analytical length");
    require(!validate_boundary_integrity(resized_receipt.candidate_entities()),
        "boundary resize receipt failed independent replay");
    auto resized_document = Document::fork(document.snapshot());
    (void)apply_constraint_authoring(resized_document,resized_receipt);
    const auto resized_state = resized_document.snapshot();
    const auto resized_path = std::filesystem::temp_directory_path() / ("boundary-resize-receipt-"+make_stable_id()+".bldproj");
    (void)ProjectStore::save(resized_path,resized_state);
    auto resized_reopened = ProjectStore::load(resized_path);
    std::filesystem::remove(resized_path);
    require(resized_reopened.document.snapshot().entities() == resized_state.entities(),
        "boundary resize save/reopen changed exact archived receipts");
    resized_reopened.document.undo(resized_reopened.document.revision());
    require(resized_reopened.document.snapshot().entities() == document.snapshot().entities(),
        "boundary resize undo did not restore exact construction receipt");
    resized_reopened.document.redo(resized_reopened.document.revision());
    require(resized_reopened.document.snapshot().entities() == resized_state.entities(),
        "boundary resize redo changed exact derivation evidence");
    ConstraintAuthoringIntent receipt_vertex_move;
    const BoundaryGeometryEdit receipt_move{boundary.id,BoundaryGeometryEditKind::move_vertex,"vertex-1",{5,1}};
    receipt_vertex_move.boundary_vertex_move = BoundaryVertexMoveIntent{receipt_move,true};
    const auto moved_receipt = preview_constraint_authoring(document.snapshot(),receipt_vertex_move);
    require_accepted(moved_receipt,"receipt-backed canonical vertex move rejected");
    require(moved_receipt.boundary_edits().size() == 1 && moved_receipt.boundary_edits().front() == receipt_move,
        "receipt-backed vertex move proof differs from requested semantic edit");
    require(moved_receipt.candidate_entities().at(boundary.id).extensions.at("boundary_geometry_derivation")
        .at("source_boundary_authoring") == receipt,
        "vertex move changed archived exact construction input");
    require(moved_receipt.candidate_entities().at("dimension") == dimension_entity,
        "vertex move changed attached dimension identity or presentation");
    auto moved_document = Document::fork(document.snapshot());
    (void)apply_constraint_authoring(moved_document,moved_receipt);
    const auto moved_state = moved_document.snapshot();
    const auto moved_path = std::filesystem::temp_directory_path() / ("boundary-vertex-receipt-"+make_stable_id()+".bldproj");
    (void)ProjectStore::save(moved_path,moved_state);
    auto moved_reopened = ProjectStore::load(moved_path);
    std::filesystem::remove(moved_path);
    require(moved_reopened.document.snapshot().entities() == moved_state.entities(),
        "vertex move reopen changed exact receipt or typed evidence");
    moved_reopened.document.undo(moved_reopened.document.revision());
    require(moved_reopened.document.snapshot().entities() == document.snapshot().entities(),
        "vertex move undo changed exact construction source");
    moved_reopened.document.redo(moved_reopened.document.revision());
    require(moved_reopened.document.snapshot().entities() == moved_state.entities(),
        "vertex move redo changed exact derived source");
    ConstraintAuthoringIntent intent;
    intent.relation_anchor = WallEndpointBinding{boundary.id, WallEndpointRole::start, "edge-0", "vertex-0"};
    intent.relation_mutations.push_back(ConstraintRelationMutation::upsert(relation(
        "level", ConstraintRelationKind::horizontal,
        {*intent.relation_anchor, {boundary.id, WallEndpointRole::end, "edge-0", "vertex-1"}})));
    const auto preview = preview_constraint_authoring(document.snapshot(), intent);
    require_accepted(preview, "receipt-backed boundary preview rejected");
    const auto& candidate = preview.candidate_entities().at(boundary.id);
    require(candidate.extensions.at("boundary_geometry_derivation").at("source_boundary_authoring") == receipt,
            "constraint solve rewrote original construction receipt");
    require(preview.candidate_entities().at("dimension") == dimension_entity,
            "constraint solve changed attached dimension identity or presentation");
    require_near(dimension.resolve(candidate).segment_length(), 4, 1e-7,
                 "dimension did not resolve solved geometry");
    require(!validate_boundary_integrity(preview.candidate_entities()), "derived receipt failed independent replay");
    require(!preview.boundary_edits().empty(), "preview omitted replayable boundary edit intents");

    // Move the entire receipt-backed boundary beyond its width. Moving vertex-0
    // first self-intersects; only the simultaneous solved result is valid.
    auto remote = boundary;
    remote.id = "remote";
    for (auto& edge : remote.segments) {
        edge.segment.start.x += 10;
        edge.segment.end.x += 10;
    }
    auto translated = Document::create({owner, dimension_entity, encode_identified_boundary_entity(remote)});
    const auto initial = translated.snapshot();
    ConstraintAuthoringIntent translate;
    translate.relation_anchor = WallEndpointBinding{"remote", WallEndpointRole::start, "edge-0", "vertex-0"};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto edge = "edge-" + std::to_string(i);
        const auto vertex = "vertex-" + std::to_string(i);
        const WallEndpointBinding destination{"remote", WallEndpointRole::start, edge, vertex};
        auto pin = relation("pin-" + std::to_string(i), ConstraintRelationKind::fixed_anchor, {destination});
        pin.anchor = Vec2{points[i].x + 10, points[i].y};
        translate.relation_mutations.push_back(ConstraintRelationMutation::upsert(pin));
        translate.relation_mutations.push_back(ConstraintRelationMutation::upsert(relation(
            "match-" + std::to_string(i), ConstraintRelationKind::coincident,
            {{boundary.id, WallEndpointRole::start, edge, vertex}, destination})));
    }
    const auto moved_preview = preview_constraint_authoring(initial, translate);
    require_accepted(moved_preview, "simultaneous boundary translation preview rejected");
    (void)apply_constraint_authoring(translated, moved_preview);
    const auto moved = translated.snapshot();
    require(moved.revision() == 1 && moved.entities() == moved_preview.candidate_entities(),
            "simultaneous authoring did not commit the preview in one revision");
    const auto geometry = decode_identified_boundary_entity(moved.entities().at(boundary.id));
    for (std::size_t i = 0; i < 4; ++i)
        require_near(geometry.segments[i].segment.start.x, points[i].x + 10, 1e-7,
                     "simultaneous solve lost a vertex destination");
    require(!validate_boundary_integrity(moved.entities()), "batch receipt replay failed");
    const auto path = std::filesystem::temp_directory_path() / ("constraint-batch-" + make_stable_id() + ".bldproj");
    (void)ProjectStore::save(path, moved);
    auto reopened = ProjectStore::load(path);
    std::filesystem::remove(path);
    require(reopened.document.snapshot().entities() == moved.entities(), "batch reopen differs");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == initial.entities(), "batch reopened undo differs");
    reopened.document.redo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == moved.entities(), "batch reopened redo differs");
}

void test_persisted_component_analysis_excludes_edit_pins() {
    const auto rectangle = [](std::string id) {
        return IdentifiedBoundary{std::move(id), "measurement_boundary", {
            {"ab","a","b",{{0,0},{4,0},0}}, {"bc","b","c",{{4,0},{4,3},0}},
            {"cd","c","d",{{4,3},{0,3},0}}, {"da","d","a",{{0,3},{0,0},0}}}};
    };
    const auto boundary = rectangle("measure");
    const auto owner = encode_identified_boundary_entity(boundary);
    auto document = Document::create({owner});
    document.mark_saved(document.revision());
    const auto before = document.snapshot();
    const auto free = analyze_persistent_constraint_component(before, {"measure"});
    require(free.supported && free.degrees_of_freedom == 8 && free.point_count == 4 &&
        free.owner_ids == std::vector<std::string>{"measure"} && free.constraint_ids.empty(),
        "unconstrained boundary analysis must report eight persisted endpoint freedoms");
    BoundaryGeometryEdit split;
    split.kind = BoundaryGeometryEditKind::insert_vertex;
    split.boundary_id = "measure"; split.target_id = "ab"; split.fraction = 0.5;
    split.new_vertex_id = "inserted"; split.new_segment_id = "inserted-edge";
    const auto inserted = Document::preview_command(before, EditBoundaryGeometry{before.revision(), split});
    const auto inserted_analysis = analyze_persistent_constraint_component(inserted, {"measure"});
    require(inserted_analysis.supported && inserted_analysis.degrees_of_freedom == 10 && inserted_analysis.point_count == 5,
        "an unconstrained inserted vertex must add two actual persistent freedoms");
    const auto bindings = std::vector<WallEndpointBinding>{{"measure",WallEndpointRole::start,"ab","a"},
        {"measure",WallEndpointRole::end,"ab","b"}};
    const auto level = encode_constraint_entity(relation("level", ConstraintRelationKind::horizontal, bindings));
    auto candidate = before.entities(); candidate.emplace(level.id, level);
    const auto level_analysis = analyze_persistent_constraint_component(candidate, {"measure"}, before.revision());
    require(level_analysis.supported && level_analysis.degrees_of_freedom == 7 && level_analysis.constraint_ids == std::vector<std::string>{"level"},
        "one stored horizontal relation removes one actual endpoint freedom");
    ConstraintAuthoringIntent intent;
    intent.relation_mutations = {ConstraintRelationMutation::upsert(*decode_constraint_entity(level).constraint)};
    intent.relation_anchor = bindings.front();
    const auto edit_preview = preview_constraint_authoring(before, intent);
    require_accepted(edit_preview, "persistent rank comparison fixture must create a valid anchored edit preview");
    require(edit_preview.degrees_of_freedom() == 5 &&
        analyze_persistent_constraint_component(edit_preview.candidate_entities(), {"measure"}).degrees_of_freedom == 7,
        "temporary edit anchor freedom must remain separate from stored component freedom");
    auto fixed = relation("fixed-a", ConstraintRelationKind::fixed_anchor, {bindings.front()}); fixed.anchor = Vec2{0,0};
    candidate.emplace(fixed.id, encode_constraint_entity(fixed));
    require(analyze_persistent_constraint_component(candidate, {"measure"}).degrees_of_freedom == 5,
        "a persistent fixed anchor must remove two freedoms in addition to the horizontal relation");
    auto fully_locked = before.entities();
    for (const auto& edge : boundary.segments) {
        auto pin = relation("fixed-"+edge.start_vertex_id, ConstraintRelationKind::fixed_anchor,
            {{boundary.id,WallEndpointRole::start,edge.segment_id,edge.start_vertex_id}});
        pin.anchor = edge.segment.start; fully_locked.emplace(pin.id, encode_constraint_entity(pin));
    }
    const auto locked = analyze_persistent_constraint_component(fully_locked, {"measure"});
    require(locked.supported && locked.degrees_of_freedom == 0, "four real persisted anchors must report a fully constrained rectangle");
    auto conflict_entities = fully_locked;
    auto contradictory_pin = fixed; contradictory_pin.id = "conflicting-a"; contradictory_pin.anchor = Vec2{1,0};
    conflict_entities.emplace(contradictory_pin.id,encode_constraint_entity(contradictory_pin));
    const auto conflict = analyze_persistent_constraint_component(conflict_entities,{"measure"});
    require(!conflict.supported && conflict.degrees_of_freedom == -1 && !conflict.conflicting_constraint_ids.empty(),
        "conflicting persisted locks must expose their IDs without presenting a valid freedom number");
    auto duplicate = *decode_constraint_entity(level).constraint; duplicate.id = "level-copy";
    candidate = before.entities(); candidate.emplace(level.id,level); candidate.emplace(duplicate.id,encode_constraint_entity(duplicate));
    const auto redundant = analyze_persistent_constraint_component(candidate, {"measure"});
    require(redundant.supported && redundant.degrees_of_freedom == 7 && !redundant.redundant_constraint_ids.empty() &&
        std::all_of(redundant.redundant_constraint_ids.begin(), redundant.redundant_constraint_ids.end(),
            [&](const auto& id) { return id == level.id || id == duplicate.id; }), "redundancy must preserve rank and expose persisted relation IDs");

    candidate = before.entities();
    candidate.emplace("other",encode_identified_boundary_entity(rectangle("other")));
    candidate.emplace("isolated",encode_identified_boundary_entity(rectangle("isolated")));
    candidate.emplace("wall",wall("wall",{0,0},{1,0}));
    require(analyze_persistent_constraint_component(candidate,{"measure"}).degrees_of_freedom == 8,
        "coordinate-coincident owners and duplicate vertex names must not imply persistent relationships");
    auto join = relation("join",ConstraintRelationKind::coincident,
        {{"measure",WallEndpointRole::start,"ab","a"},{"other",WallEndpointRole::start,"ab","a"}});
    candidate.emplace(join.id,encode_constraint_entity(join));
    const auto joined = analyze_persistent_constraint_component(candidate,{"measure"});
    require(joined.supported && joined.degrees_of_freedom == 14 && joined.point_count == 8 && joined.owner_ids.size() == 2,
        "explicit shared-point relation must expand only its two owner components");
    auto wall_join = relation("wall-join",ConstraintRelationKind::coincident,
        {{"measure",WallEndpointRole::start,"ab","a"},endpoint("wall",WallEndpointRole::start)});
    candidate.emplace(wall_join.id,encode_constraint_entity(wall_join));
    const auto mixed = analyze_persistent_constraint_component(candidate,{"measure"});
    require(mixed.supported && mixed.degrees_of_freedom == 16 && mixed.point_count == 10 && mixed.owner_ids.size() == 3,
        "mixed wall and boundary component must include all independently scoped endpoints and only explicit locks");
    const auto universe = mixed.owner_ids;
    candidate.erase(join.id); candidate.erase(wall_join.id);
    require(analyze_persistent_constraint_component(candidate,universe).degrees_of_freedom == 20,
        "before/after lock removal must retain the same owner universe rather than hide disconnected freedom");
    candidate = before.entities(); candidate.emplace(level.id,level);
    candidate.emplace("copy",encode_identified_boundary_entity(rectangle("copy")));
    require(analyze_persistent_constraint_component(candidate,{"measure"}).degrees_of_freedom == 7 &&
        analyze_persistent_constraint_component(candidate,{"copy"}).degrees_of_freedom == 8,
        "a free copy must not inherit its source component's locks or point identities");
    auto unknown = level; unknown.id = "unknown"; unknown.properties["version"] = 99;
    candidate.emplace(unknown.id,unknown);
    const auto unavailable = analyze_persistent_constraint_component(candidate,{"measure"});
    require(!unavailable.supported && unavailable.degrees_of_freedom == -1 && !unavailable.diagnostics.empty(),
        "connected unknown relations must refuse a partial freedom number");
    candidate.erase(unknown.id);
    unknown.properties["entity_ids"] = {"copy"};
    for (auto& binding : unknown.properties["bindings"]) binding["owner_id"] = "copy";
    candidate.emplace(unknown.id,unknown);
    require(analyze_persistent_constraint_component(candidate,{"measure"}).degrees_of_freedom == 7,
        "known-scope disconnected unsupported relations must not suppress a supported component");
    unknown.properties["bindings"][0].erase("owner_id"); candidate[unknown.id] = unknown;
    require(!analyze_persistent_constraint_component(candidate,{"measure"}).supported,
        "indeterminate constraint owner scope must fail closed");
    candidate = before.entities(); auto malformed = level;
    malformed.properties["bindings"][1]["vertex_id"] = "a"; candidate.emplace(malformed.id,malformed);
    require(!analyze_persistent_constraint_component(candidate,{"measure"}).supported,
        "ambiguous endpoint roles must not produce a partial number");
    candidate = before.entities(); candidate.at("measure").properties["segments"][0]["sweep_radians"] = 0.2;
    require(analyze_persistent_constraint_component(candidate,{"measure"}).supported &&
        !analyze_persistent_constraint_component(before,{"missing"}).supported &&
        !analyze_persistent_constraint_component(before,{}).supported,
        "missing seeds must be explicitly unavailable");
    candidate = before.entities(); candidate.emplace("curve",wall("curve",{0,0},{1,0},0.2));
    require(analyze_persistent_constraint_component(candidate,{"curve"}).supported,
        "curved wall endpoint rank must be available at fixed sweep");
    require(document.snapshot().entities() == before.entities() && document.revision() == before.revision() &&
        document.snapshot().history().size() == before.history().size() && document.snapshot().saved_revision_optional() == before.saved_revision_optional(),
        "all persistent analysis paths must leave revision, history, entities and saved state unchanged");
}

void test_mixed_boundary_wall_authoring_and_resize() {
    const auto owner = encode_identified_boundary_entity(IdentifiedBoundary{"area", "measurement_boundary", {
        {"ab","a","b",{{0,0},{3,0},0}}, {"bc","b","c",{{3,0},{3,3},0}},
        {"cd","c","d",{{3,3},{0,3},0}}, {"da","d","a",{{0,3},{0,0},0}}}});
    const WallEndpointBinding corner{"area",WallEndpointRole::end,"ab","b"};
    auto architectural_wall = wall("wall",{4,0},{6,0});
    architectural_wall.properties["baseline"]["future_geometry_metadata"] = {{"retain",17}};
    auto document = Document::create({owner, architectural_wall});
    const auto initial = document.snapshot();
    ConstraintAuthoringIntent join;
    join.relation_anchor = corner;
    join.relation_mutations.push_back(ConstraintRelationMutation::upsert(relation(
        "join",ConstraintRelationKind::coincident,{corner,endpoint("wall",WallEndpointRole::start)})));
    const auto joined = preview_constraint_authoring(initial,join);
    require_accepted(joined,"mixed boundary-anchored relation must move a connected wall");
    require_near(baseline(joined.candidate_entities().at("wall")).start.x,3,1e-7,
        "mixed relation did not move wall to the anchored boundary vertex");
    require(joined.candidate_entities().at("area") == owner,"mixed anchor moved its boundary");
    (void)apply_constraint_authoring(document,joined);
    const auto connected = document.snapshot();
    ConstraintAuthoringIntent resize;
    resize.wall_resize = WallResizeIntent{"wall",parse_quantity("4 m"),WallResizeAnchor::end,true};
    const auto preview = preview_constraint_authoring(connected,resize);
    require_accepted(preview,"wall-only resize seed must discover its boundary relation component");
    require(preview.changed_walls().size() == 1 && preview.changed_boundaries().size() == 1,
        "mixed resize preview must expose both owner types");
    require_near(decode_identified_boundary_entity(preview.candidate_entities().at("area")).segments[0].segment.end.x,
        2,1e-7,"mixed resize did not move the joined boundary vertex");
    require(document.snapshot().entities() == connected.entities(),"mixed preview mutated source");
    resize.wall_resize->move_connected_walls = false;
    const auto frozen = preview_constraint_authoring(connected,resize);
    require(!frozen.accepted(),"frozen mixed neighbor must reject a conflicting resize");
    require_rejected_unchanged(document,frozen,"rejected mixed resize applied");
    (void)apply_constraint_authoring(document,preview);
    const auto committed = document.snapshot();
    require(committed.entities() == preview.candidate_entities() && committed.revision() == connected.revision()+1,
        "mixed resize must commit exactly the shown state in one revision");
    require(committed.history().back().boundary_constraint_changes.has_value(),
        "mixed boundary and wall movement must carry typed replay evidence");
    const auto encoded = command_to_json(*committed.history().back().boundary_constraint_changes);
    require(encoded.at("version") == 2 && encoded.contains("wall_edits"),
        "mixed typed command requires versioned wall geometry replay");
    require(command_to_json(command_from_json(encoded)) == encoded,"mixed command codec lost evidence");
    const auto rejects_command = [&](json malformed) {
        bool rejected = false;
        try { auto fork = Document::fork(connected); (void)fork.apply(command_from_json(malformed)); }
        catch (const std::exception&) { rejected = true; }
        require(rejected,"tampered mixed command must reject atomically");
        require(document.snapshot().entities() == committed.entities(),"tampered replay changed live source");
    };
    auto malformed = encoded;
    malformed["wall_edits"][0]["extra"] = true;
    rejects_command(malformed);
    malformed = encoded;
    malformed["wall_edits"][0]["wall_id"] = "area";
    rejects_command(malformed);
    malformed = encoded;
    malformed["wall_edits"].push_back(malformed["wall_edits"][0]);
    rejects_command(malformed);
    malformed = encoded;
    malformed["wall_edits"][0]["length_entry"]["exact_metres"]["numerator"] = 5;
    rejects_command(malformed);
    malformed = encoded;
    malformed["wall_edits"][0]["baseline"]["start"] = {1,0};
    malformed["wall_edits"][0]["length_entry"] = nullptr;
    rejects_command(malformed); // final coincidence must check both moved owners
    auto tampered = preview;
    auto& entities = const_cast<std::map<std::string,Entity,std::less<>>&>(tampered.candidate_entities());
    entities.at("wall").properties["classification"] = "tampered";
    auto untampered_source = Document::fork(connected);
    require_rejected_unchanged(untampered_source,tampered,"tampered mixed preview applied");
    const auto path = std::filesystem::temp_directory_path() / ("constraint-mixed-"+make_stable_id()+".bldproj");
    (void)ProjectStore::save(path,committed);
    auto reopened = ProjectStore::load(path);
    std::filesystem::remove(path);
    require(reopened.document.snapshot().entities() == committed.entities(),"mixed reopen differs");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == connected.entities(),"mixed undo split the transaction");
    reopened.document.redo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == committed.entities(),"mixed redo differs");
    require_rejected_unchanged(document,preview,"stale mixed candidate applied");
    ConstraintAuthoringIntent removal;
    removal.relation_mutations = {ConstraintRelationMutation::remove("join")};
    const auto removed = preview_constraint_authoring(document.snapshot(),removal);
    require_accepted(removed,"mixed relation removal must preserve all geometry");
    require(removed.changed_walls().empty() && removed.changed_boundaries().empty(),
        "mixed relation removal moved endpoint geometry");
    (void)apply_constraint_authoring(document,removed);
    require(document.snapshot().entities().at("area") == committed.entities().at("area") &&
        document.snapshot().entities().at("wall") == committed.entities().at("wall"),
        "mixed relation removal lost geometry or exact receipts");
    require(committed.entities().at("wall").properties.at("baseline").at("future_geometry_metadata") ==
        architectural_wall.properties.at("baseline").at("future_geometry_metadata"),
        "mixed surgical wall edit lost opaque baseline metadata");

    const auto coincident = encode_constraint_entity(relation("join",ConstraintRelationKind::coincident,
        {corner,endpoint("wall",WallEndpointRole::start)}));
    auto pin = relation("corner-pin",ConstraintRelationKind::fixed_anchor,{corner});
    pin.anchor = Vec2{3,0};
    auto locked = Document::create({owner,wall("wall",{3,0},{6,0}),coincident,encode_constraint_entity(pin)});
    resize.wall_resize->move_connected_walls = true;
    require(!preview_constraint_authoring(locked.snapshot(),resize).accepted(),
        "mixed resize must respect a persistent boundary anchor");
    auto hosted = Document::create({owner,wall("wall",{3,0},{6,0}),coincident,
        opening("door","wall",2.6,0.3)});
    auto shrink = resize;
    shrink.wall_resize->exact_length = parse_quantity("2 m");
    require(!preview_constraint_authoring(hosted.snapshot(),shrink).accepted(),
        "mixed solve must reject stranding a hosted opening");
    auto arc_owner = owner;
    auto arc = decode_identified_boundary_entity(arc_owner);
    arc.segments.front().segment.sweep_radians = 0.2;
    arc_owner = encode_identified_boundary_entity(arc,&arc_owner);
    const auto require_curved_relation_supported = [&](const Entity& boundary_owner, const Entity& wall_owner) {
        auto curved_document=Document::create({boundary_owner,wall_owner,coincident});
        require(curved_document.snapshot().entities().at("join")==coincident,
            "persisted curved endpoint relationship changed meaning");
    };
    require_curved_relation_supported(arc_owner,wall("wall",{3,0},{6,0}));
    require_curved_relation_supported(owner,wall("wall",{3,0},{6,0},0.2));
    // Curved owners can exist without endpoint constraints. Attempt the mixed
    // relation through authoring so its refusal is exercised before persistence.
    auto curved_boundary = Document::create({arc_owner,wall("wall",{3,0},{6,0})});
    const auto curved_boundary_before = curved_boundary.snapshot();
    const auto curved_boundary_preview = preview_constraint_authoring(curved_boundary_before,join);
    require_accepted(curved_boundary_preview,"mixed curved boundary endpoint relation rejected");
    require(curved_boundary_preview.candidate_entities().at("area")==arc_owner,"mixed component flattened curved boundary");
    auto curved_wall = Document::create({owner,wall("wall",{3,0},{6,0},0.2)});
    const auto curved_wall_preview = preview_constraint_authoring(curved_wall.snapshot(),join);
    require_accepted(curved_wall_preview,"mixed curved wall endpoint relation rejected");
    require(baseline(curved_wall_preview.candidate_entities().at("wall")).sweep_radians==0.2,"mixed component flattened curved wall");
    auto misplaced = owner;
    misplaced.properties["floor_id"] = "floor";
    auto unresolved = Document::create({misplaced,wall("wall",{3,0},{6,0}),coincident,
        {"property","property",{{"name","Property"}}},
        {"building","building",{{"name","Building"},{"property_id","property"}}},
        {"floor","floor",{{"name","Floor"},{"building_id","building"}}}});
    require(!preview_constraint_authoring(unresolved.snapshot(),resize).accepted(),
        "mixed boundary neighbor must have a resolved explicit drawing context");
    auto unrelated = Document::create({owner,wall("wall",{3,0},{6,0}),wall("curve",{9,9},{11,9},0.2)});
    const auto detached = preview_constraint_authoring(unrelated.snapshot(),resize);
    require_accepted(detached,"unrelated curved owner must not block a known straight component");
    require(detached.changed_boundaries().empty() && detached.candidate_entities().at("area") == owner,
        "coordinate coincidence must not create an implicit mixed relation");
}

void test_boundary_resize_canonical_shape_and_related_owners() {
    const IdentifiedBoundary shape{"area", "measurement_boundary", {
        {"ab","a","b",{{0,0},{3,0},0}}, {"bc","b","c",{{3,0},{3,3},0}},
        {"cd","c","d",{{3,3},{0,3},0}}, {"da","d","a",{{0,3},{0,0},0}}}};
    const auto owner = encode_identified_boundary_entity(shape);
    for (const auto anchor : {BoundaryFixedEndpoint::start, BoundaryFixedEndpoint::end}) {
        for (const auto local_chain : {false, true}) {
            const BoundaryGeometryEdit edit{"area", BoundaryGeometryEditKind::resize_segment,
                "ab", {}, 4, anchor, local_chain};
            const WallEndpointBinding moving{"area", anchor == BoundaryFixedEndpoint::start
                ? WallEndpointRole::end : WallEndpointRole::start, "ab",
                anchor == BoundaryFixedEndpoint::start ? "b" : "a"};
            const auto start = anchor == BoundaryFixedEndpoint::start ? Vec2{3,0} : Vec2{0,0};
            auto neighbor = shape;
            neighbor.id = "neighbor";
            for (auto& edge : neighbor.segments) {
                edge.segment.start.x += start.x;
                edge.segment.start.y -= 3;
                edge.segment.end.x += start.x;
                edge.segment.end.y -= 3;
            }
            const WallEndpointBinding neighbor_corner{"neighbor",WallEndpointRole::start,"da","d"};
            const auto join = encode_constraint_entity(relation("join",ConstraintRelationKind::coincident,
                {moving,endpoint("wall",WallEndpointRole::start)}));
            const auto neighbor_join = encode_constraint_entity(relation("neighbor-join",ConstraintRelationKind::coincident,
                {moving,neighbor_corner}));
            auto document = Document::create({owner,wall("wall",start,{start.x+2,start.y}),
                encode_identified_boundary_entity(neighbor),join,neighbor_join});
            const auto before = document.snapshot();
            ConstraintAuthoringIntent intent;
            intent.boundary_resize = BoundaryResizeIntent{edit,true};
            const auto preview = preview_constraint_authoring(before,intent);
            require_accepted(preview,"boundary resize must propagate to explicitly related owners");
            const auto canonical = edited_boundary_entities(before.entities(),edit);
            require(preview.candidate_entities().at("area") == canonical.at("area"),
                "solver changed the canonical selected boundary resize shape");
            const auto after = decode_identified_boundary_entity(canonical.at("area"));
            const auto destination = anchor == BoundaryFixedEndpoint::start
                ? after.segments[0].segment.end : after.segments[0].segment.start;
            require_near(baseline(preview.candidate_entities().at("wall")).start.x,destination.x,1e-7,
                "boundary resize did not propagate to the wall endpoint");
            require_near(decode_identified_boundary_entity(preview.candidate_entities().at("neighbor"))
                .segments[3].segment.start.x,destination.x,1e-7,
                "boundary resize did not propagate to another boundary");
            require(!preview.boundary_edits().empty() && preview.boundary_edits().front() == edit,
                "selected resize must lead the typed geometry proofs");
            require(std::count_if(preview.boundary_edits().begin(),preview.boundary_edits().end(),
                [](const auto& proof) { return proof.boundary_id == "area"; }) == 1,
                "selected canonical resize must not also emit vertex edits");
            intent.boundary_resize->move_related_objects = false;
            const auto frozen = preview_constraint_authoring(before,intent);
            require(!frozen.accepted(),"frozen related owners must reject conflicting boundary resize");
            require_rejected_unchanged(document,frozen,"frozen boundary resize applied");
            (void)apply_constraint_authoring(document,preview);
            const auto committed = document.snapshot();
            require(committed.entities() == preview.candidate_entities() && committed.revision() == before.revision()+1,
                "boundary resize did not commit the shown result atomically");
            require(committed.history().back().boundary_constraint_changes.has_value(),
                "boundary resize omitted typed transaction proof");
            const auto proof = command_to_json(*committed.history().back().boundary_constraint_changes);
            require(command_to_json(command_from_json(proof)) == proof,"boundary resize proof codec changed evidence");
            require_rejected_unchanged(document,preview,"stale boundary resize preview applied");
            const auto path = std::filesystem::temp_directory_path() / ("boundary-resize-"+make_stable_id()+".bldproj");
            (void)ProjectStore::save(path,committed);
            auto reopened = ProjectStore::load(path);
            std::filesystem::remove(path);
            require(reopened.document.snapshot().entities() == committed.entities(),"boundary resize reopen differs");
            reopened.document.undo(reopened.document.revision());
            require(reopened.document.snapshot().entities() == before.entities(),"boundary resize undo split transaction");
            reopened.document.redo(reopened.document.revision());
            require(reopened.document.snapshot().entities() == committed.entities(),"boundary resize redo differs");
        }
    }

    BoundaryGeometryEdit edit{"area",BoundaryGeometryEditKind::resize_segment,"ab",{},4};
    ConstraintAuthoringIntent intent;
    intent.boundary_resize = BoundaryResizeIntent{edit,true};
    auto detached = Document::create({owner,wall("wall",{3,0},{5,0}),wall("curve",{8,8},{9,8},0.2)});
    const auto detached_before = detached.snapshot();
    const auto preview = preview_constraint_authoring(detached_before,intent);
    require_accepted(preview,"unrelated curved owner must not block boundary resize");
    require(preview.changed_walls().empty() && preview.candidate_entities().at("wall") == detached_before.entities().at("wall"),
        "coordinate coincidence must not imply boundary resize propagation");
    intent.boundary_resize->edit.target_length_metres = 3;
    const auto noop = preview_constraint_authoring(detached_before,intent);
    require(!noop.accepted() && has_diagnostic(noop,"no document change"),"unchanged boundary length must reject as no-op");
    require_rejected_unchanged(detached,noop,"no-op boundary resize applied");
    intent.boundary_resize->edit.target_length_metres = 4;
    auto locked_length = relation("length",ConstraintRelationKind::fixed_length,
        {{"area",WallEndpointRole::start,"ab","a"},{"area",WallEndpointRole::end,"ab","b"}});
    locked_length.length = parse_quantity("3 m");
    auto locked = Document::create({owner,encode_constraint_entity(locked_length)});
    const auto conflict = preview_constraint_authoring(locked.snapshot(),intent);
    require(!conflict.accepted(),"canonical resize must reject a conflicting selected internal relation");
    require_rejected_unchanged(locked,conflict,"locked boundary resize applied");
    auto curved_shape = shape;
    curved_shape.segments[1].segment.sweep_radians = 0.1;
    auto curved = Document::create({encode_identified_boundary_entity(curved_shape)});
    require_accepted(preview_constraint_authoring(curved.snapshot(),intent),"boundary resize must retain unrelated owner arc");
    intent.wall_resize = WallResizeIntent{"wall",parse_quantity("3 m")};
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"concurrent wall and boundary resize must reject");
    intent.wall_resize.reset();
    intent.boundary_resize->edit.kind = BoundaryGeometryEditKind::move_vertex;
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"boundary resize must reject nonresize edit kinds");
    intent.boundary_resize->edit = edit;
    intent.boundary_resize->edit.fixed_endpoint = static_cast<BoundaryFixedEndpoint>(99);
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"boundary resize must reject invalid anchors");
    intent.boundary_resize->edit = edit;
    intent.boundary_resize->edit.target_length_metres = std::numeric_limits<double>::infinity();
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"boundary resize must reject nonfinite lengths");
    intent.boundary_resize->edit = edit;
    intent.boundary_resize->edit.target_id = "absent";
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"boundary resize must reject unresolved edge IDs");
    intent.boundary_resize->edit = edit;
    const auto join = encode_constraint_entity(relation("join",ConstraintRelationKind::coincident,
        {{"area",WallEndpointRole::end,"ab","b"},endpoint("wall",WallEndpointRole::start)}));
    auto hosted = Document::create({owner,wall("wall",{3,0},{5,0}),join,opening("door","wall",1.6,0.3)});
    const auto stranded = preview_constraint_authoring(hosted.snapshot(),intent);
    require(!stranded.accepted(),"boundary propagation must not strand a hosted wall opening");
    require_rejected_unchanged(hosted,stranded,"boundary resize with stranded opening applied");
    auto misplaced = owner;
    misplaced.properties["floor_id"] = "floor";
    auto unresolved = Document::create({misplaced,
        {"property","property",{{"name","Property"}}},
        {"building","building",{{"name","Building"},{"property_id","property"}}},
        {"floor","floor",{{"name","Floor"},{"building_id","building"}}}});
    require(!preview_constraint_authoring(unresolved.snapshot(),intent).accepted(),
        "boundary resize must refuse unresolved explicit drawing context");
}

void test_physical_arc_length_authoring_and_connected_editing() {
    for (const auto sweep : {0.6,-0.6,4.0,-4.0}) {
        const auto measured=parse_quantity("7 m");
        const auto chord=measured.metres*2*std::sin(std::abs(sweep)/2)/std::abs(sweep);
        auto owner=wall("arc",{0,0},{chord,0},sweep);
        const auto angle=angle_from_radians(sweep);
        owner.extensions["curve_input"]={{"version",2},{"construction","arc_length"},
            {"measure",measured.original_expression},{"normalized_measure",format_quantity(measured,Unit::metre)},
            {"measure_value",measured.metres},{"clockwise",sweep<0},{"start",{0,0}},{"end",{chord,0}},
            {"radians",sweep},{"sweep",angle.original_expression},{"normalized_sweep",angle.normalized_expression},
            {"vendor","original measured curve"}};
        const auto source_input=owner.extensions.at("curve_input");
        require_near(segment_length(baseline(owner)),measured.metres,1e-10,
            "physical fixture source does not reproduce its measured arc receipt");
        auto level=relation("level",ConstraintRelationKind::horizontal,
            {endpoint("arc",WallEndpointRole::start),endpoint("arc",WallEndpointRole::end)});
        auto join=relation("join",ConstraintRelationKind::coincident,
            {endpoint("arc",WallEndpointRole::end),endpoint("neighbor",WallEndpointRole::start)});
        auto document=Document::create({owner,wall("neighbor",{chord,0},{chord+3,0}),opening("door","arc",0.3,0.5),
            encode_constraint_entity(level),encode_constraint_entity(join)});
        const auto before=document.snapshot();
        auto physical=relation("physical",ConstraintRelationKind::fixed_arc_length,
            {endpoint("arc",WallEndpointRole::start),endpoint("arc",WallEndpointRole::end)});
        physical.length=parse_quantity("5 m");
        ConstraintAuthoringIntent intent;
        intent.relation_anchor=endpoint("arc",WallEndpointRole::start);
        intent.relation_mutations={ConstraintRelationMutation::upsert(physical)};
        const auto preview=preview_constraint_authoring(before,intent);
        require_accepted(preview,"anchored physical minor/major arc length authoring rejected");
        const auto changed=baseline(preview.candidate_entities().at("arc"));
        require(changed.sweep_radians==sweep && changed.start.x==0 && changed.start.y==0,
            "physical arc solve changed signed sweep or anchor");
        require_near(segment_length(changed),5,1e-6,"physical target was solved as endpoint distance");
        require(std::abs(length(changed)-5)>0.01,"physical length was confused with chord length");
        require_near(baseline(preview.candidate_entities().at("neighbor")).start.x,changed.end.x,1e-6,
            "connected neighbor did not follow physical arc edit");
        require(preview.candidate_entities().at("arc").extensions.at("curve_input_derivation").at("source_input")==source_input,
            "physical arc edit lost original curve measurement/metadata");
        require(preview.candidate_entities().at("door")==before.entities().at("door"),"physical arc edit rewrote hosted opening");
        (void)apply_constraint_authoring(document,preview);
        const auto after=document.snapshot();
        require(after.history().size()==before.history().size()+1,"physical arc solve must be one command");
        const auto analysis=analyze_persistent_constraint_component(after,{"arc"});
        require(analysis.supported && analysis.conflicting_constraint_ids.empty(),"persisted physical arc DOF analysis rejected");
        require(Document::fork(after).snapshot().entities()==after.entities(),"physical arc history proof cannot replay");
        document.undo(document.revision()); require(document.snapshot().entities()==before.entities(),"physical arc undo lost atomicity");
        document.redo(document.revision()); require(document.snapshot().entities()==after.entities(),"physical arc redo differs");
        physical.length=parse_quantity("6 m");
        intent.relation_mutations={ConstraintRelationMutation::upsert(physical)};
        const auto edited=preview_constraint_authoring(document.snapshot(),intent);
        require_accepted(edited,"editing physical arc target with anchor rejected");
        require_near(segment_length(baseline(edited.candidate_entities().at("arc"))),6,1e-6,"edited physical target not applied");
        (void)apply_constraint_authoring(document,edited);
        auto incompatible=relation("chord-lock",ConstraintRelationKind::fixed_length,physical.bindings);
        incompatible.length=parse_quantity("6 m");
        intent.relation_mutations={ConstraintRelationMutation::upsert(incompatible)};
        const auto conflict=preview_constraint_authoring(document.snapshot(),intent);
        require(!conflict.accepted(),"contradictory physical arc/chord lengths accepted");
        require_rejected_unchanged(document,conflict,"contradictory arc apply must reject atomically");
    }
    for (const auto sweep : {0.6,-0.6}) {
        auto owner=encode_identified_boundary_entity(IdentifiedBoundary{"outline","measurement_boundary",{
            {"ab","a","b",{{0,0},{4,0},sweep}}, {"bc","b","c",{{4,0},{4,3},0}},
            {"cd","c","d",{{4,3},{0,3},0}}, {"da","d","a",{{0,3},{0,0},0}}}});
        owner.extensions["vendor"]="retained";
        auto document=Document::create({owner});
        const auto before=document.snapshot();
        auto physical=relation("physical",ConstraintRelationKind::fixed_arc_length,
            {{"outline",WallEndpointRole::start,"ab","a"},{"outline",WallEndpointRole::end,"ab","b"}});
        physical.length=parse_quantity("5 m");
        ConstraintAuthoringIntent intent;
        intent.relation_anchor=physical.bindings.front();
        intent.relation_mutations={ConstraintRelationMutation::upsert(physical)};
        const auto preview=preview_constraint_authoring(before,intent);
        require_accepted(preview,"stable identified boundary physical arc edit rejected");
        const auto boundary=decode_identified_boundary_entity(preview.candidate_entities().at("outline"));
        require(boundary.segments[0].segment_id=="ab" && boundary.segments[0].start_vertex_id=="a" &&
            boundary.segments[0].end_vertex_id=="b" && boundary.segments[0].segment.sweep_radians==sweep,
            "physical boundary edit changed stable IDs or signed sweep");
        require_near(segment_length(boundary.segments[0].segment),5,1e-6,"boundary physical length differs");
        (void)apply_constraint_authoring(document,preview);
        require(document.snapshot().entities().at("outline").extensions==owner.extensions,"boundary arc metadata lost");
        require(Document::fork(document.snapshot()).snapshot().entities()==document.snapshot().entities(),"boundary arc proof cannot replay");
        physical.bindings[1].vertex_id="c";
        intent.relation_mutations={ConstraintRelationMutation::upsert(physical)};
        require(!preview_constraint_authoring(document.snapshot(),intent).accepted(),"physical arc accepted wrong stable endpoint");
    }
    auto straight=Document::create({wall("straight",{0,0},{4,0})});
    auto invalid_arc=relation("physical",ConstraintRelationKind::fixed_arc_length,
        {endpoint("straight",WallEndpointRole::start),endpoint("straight",WallEndpointRole::end)});
    invalid_arc.length=parse_quantity("5 m");
    ConstraintAuthoringIntent invalid_intent;
    invalid_intent.relation_mutations={ConstraintRelationMutation::upsert(invalid_arc)};
    const auto rejected=preview_constraint_authoring(straight.snapshot(),invalid_intent);
    require(!rejected.accepted() && has_diagnostic(rejected,"genuinely curved"),"straight owner lacks clear physical arc refusal");
    require_rejected_unchanged(straight,rejected,"straight physical arc apply accepted");
}

void test_physical_arc_length_conditioning_at_near_full_turn_and_translated_origin() {
    const auto pi=std::acos(-1.0);
    for (const bool near_full : {true,false}) for (const double sign : {-1.0,1.0}) {
        const auto sweep=sign*(near_full ? 2*pi-1e-5 : 0.6);
        const auto chord=7*2*std::sin(std::abs(sweep)/2)/std::abs(sweep);
        const Vec2 start=near_full ? Vec2{0,0} : Vec2{1e6,-1e6};
        auto owner=wall("arc",start,{start.x+chord,start.y},sweep);
        require_near(segment_length(baseline(owner)),7,1e-6,
            "conditioning fixture source does not independently measure seven metres");
        auto level=relation("level",ConstraintRelationKind::horizontal,
            {endpoint("arc",WallEndpointRole::start),endpoint("arc",WallEndpointRole::end)});
        auto document=Document::create({owner,encode_constraint_entity(level)});
        document.mark_saved(document.revision());
        const auto before=document.snapshot();
        auto physical=relation("physical",ConstraintRelationKind::fixed_arc_length,level.bindings);
        physical.length=parse_quantity("5 m");
        ConstraintAuthoringIntent intent;
        intent.relation_anchor=physical.bindings.front();
        intent.relation_mutations={ConstraintRelationMutation::upsert(physical)};
        const auto preview=preview_constraint_authoring(before,intent);
        require_accepted(preview,near_full ? "near-full physical arc solve rejected" : "translated physical arc solve rejected");
        require(document.snapshot().entities()==before.entities() && document.revision()==before.revision() &&
            document.snapshot().saved_revision_optional()==before.saved_revision_optional() &&
            document.snapshot().history().size()==before.history().size(),"conditioning preview mutated source");
        const auto proposed=baseline(preview.candidate_entities().at("arc"));
        require(proposed.start.x==start.x && proposed.start.y==start.y && proposed.sweep_radians==sweep,
            "conditioned physical arc solve changed exact anchor or sweep");
        require_near(segment_length(proposed),5,1e-6,"conditioned proposed physical length exceeds admission tolerance");
        (void)apply_constraint_authoring(document,preview);
        require(Document::fork(document.snapshot()).snapshot().entities()==document.snapshot().entities(),
            "conditioned physical arc proof cannot replay");
    }
    // A well-formed source can have a target chord below the geometric range;
    // that refusal must be explicit and cannot be disguised as solver success.
    auto document=Document::create({wall("arc",{0,0},{4,0},2*pi-1e-12)});
    auto physical=relation("physical",ConstraintRelationKind::fixed_arc_length,
        {endpoint("arc",WallEndpointRole::start),endpoint("arc",WallEndpointRole::end)});
    physical.length=parse_quantity("5 m");
    ConstraintAuthoringIntent intent;
    intent.relation_anchor=physical.bindings.front();
    intent.relation_mutations={ConstraintRelationMutation::upsert(physical)};
    const auto before=document.snapshot();
    const auto rejected=preview_constraint_authoring(before,intent);
    require(!rejected.accepted() && has_diagnostic(rejected,"chord target is outside the supported range"),
        "unrepresentable physical target lacks explicit range diagnosis");
    require(document.snapshot().entities()==before.entities() && document.revision()==before.revision(),
        "unrepresentable physical target preview mutated source");
    require_rejected_unchanged(document,rejected,"unrepresentable physical target Apply accepted");
}

void test_measured_curve_rigid_transform_provenance() {
    std::vector<Entity> sources;
    for (const auto sweep : {0.6,-0.6,4.0,-4.0}) {
        for (const auto version : {1,2}) {
            auto owner=wall("arc",{2,3},{6,3},sweep);
            const auto angle=angle_from_radians(sweep);
            owner.extensions["curve_input"]={{"version",version},{"construction","angle"},
                {"measure",angle.original_expression},{"normalized_measure",angle.normalized_expression},
                {"measure_value",sweep},{"clockwise",sweep<0},{"radians",sweep},
                {"start",{2,3}},{"end",{6,3}},{"vendor",{{"exact",17}}}};
            sources.push_back(owner);
        }
        for (const auto expression : {"7", "16 3/8 ft"}) {
            const auto quantity=parse_quantity(expression,Unit::metre);
            const auto chord=quantity.metres*2*std::sin(std::abs(sweep)/2)/std::abs(sweep);
            auto owner=wall("arc",{2,3},{2+chord,3},sweep);
            owner.extensions["curve_input"]={{"version",2},{"construction","arc_length"},
                {"measure",expression},{"normalized_measure",format_quantity(quantity,Unit::metre)},
                {"measure_value",quantity.metres},{"clockwise",sweep<0},{"radians",sweep},
                {"start",{2,3}},{"end",{2+chord,3}},{"vendor",{{"exact",17}}}};
            sources.push_back(owner);
        }
    }
    for (const auto expression : {"1/2 m","-1/2 m","3 m","-3 m"}) {
        const auto quantity=parse_quantity(expression);
        const auto geometry=arc_from_chord_height({2,3},{6,3},quantity.metres);
        auto owner=wall("arc",geometry.start,geometry.end,geometry.sweep_radians);
        owner.extensions["curve_input"]={{"version",2},{"construction","arc_height"},
            {"measure",expression},{"normalized_measure",format_quantity(quantity,Unit::metre)},
            {"measure_value",quantity.metres},{"clockwise",geometry.sweep_radians<0},
            {"radians",geometry.sweep_radians},{"start",{2,3}},{"end",{6,3}},{"vendor",{{"exact",17}}}};
        sources.push_back(owner);
    }
    const std::vector<PlanarTransform> transforms={
        {{2,3},0.3,false,false,{1,-2}},{{1,2},0,true,false,{3,4}},
        {{0,0},-0.2,false,true,{-1,2}},{{2,3},0.1,true,true,{0,0}},
        {{0,0},0,true,false,{0,0}},{{0,0},0,true,false,{0,0}}};
    for (auto source : sources) {
        source.properties["baseline"]["vendor_baseline"]={{"retain",true}};
        const auto original_input=source.extensions.at("curve_input");
        auto document=Document::create({source});
        auto noop=source; transform_wall_curve_input(noop,{});
        require(noop==source,"no-op rigid transform created a provenance-only archive");
        for (const auto& transform : transforms) {
            const auto before=document.snapshot();
            const auto& original=before.entities().at("arc");
            auto candidate=original;
            const auto geometry=transform_segment(baseline(original),transform);
            transform_wall_curve_input(candidate,transform);
            require(candidate.properties==original.properties,"rigid provenance helper changed geometry prematurely");
            candidate.properties["baseline"]["start"]={geometry.start.x,geometry.start.y};
            candidate.properties["baseline"]["end"]={geometry.end.x,geometry.end.y};
            candidate.properties["baseline"]["sweep_radians"]=geometry.sweep_radians;
            validate_wall_curve_input(candidate);
            const auto& proof=candidate.extensions.at("curve_input_derivation");
            require(proof.at("version")==2 && proof.at("source_input")==original_input &&
                proof.at("source_baseline")==source.properties.at("baseline"),
                "rigid transform lost exact original input or baseline metadata");
            require(candidate.properties.at("baseline").at("vendor_baseline")==source.properties.at("baseline").at("vendor_baseline") &&
                candidate.extensions.at("curve_input").at("vendor")==original_input.at("vendor") &&
                candidate.extensions.at("future_extension")==source.extensions.at("future_extension"),
                "rigid transform lost opaque metadata");
            require_near(segment_length(geometry),segment_length(baseline(original)),1e-6,"rigid transform changed physical length");
            if (transform.flip_horizontal==transform.flip_vertical)
                require(candidate.extensions.at("curve_input").at("measure")==original.extensions.at("curve_input").at("measure"),
                    "orientation-preserving transform rewrote active measurement");
            const Command command=ApplyEntityChanges{document.revision(),{EntityChange::upsert(candidate)}, {},"Rigid measured curve transform"};
            const auto preview=Document::preview_command(before,command);
            require(document.snapshot().entities()==before.entities() && document.revision()==before.revision(),
                "rigid transform preview mutated source");
            document.apply(command);
            const auto after=document.snapshot();
            require(after.entities()==preview.entities() && Document::fork(after).snapshot().entities()==after.entities(),
                "rigid curve command did not independently replay");
            document.undo(document.revision()); require(document.snapshot().entities()==before.entities(),"rigid curve undo lost input");
            document.redo(document.revision()); require(document.snapshot().entities()==after.entities(),"rigid curve redo differs");
        }
        auto invalid=source; const auto unchanged=invalid;
        bool rejected=false;
        try { transform_wall_curve_input(invalid,{{0,0},std::numeric_limits<double>::infinity(),false,false,{0,0}}); }
        catch (const std::exception&) { rejected=true; }
        require(rejected && invalid==unchanged,"failed rigid helper partially mutated source");
    }
    auto source=sources.at(2);
    auto document=Document::create({source});
    const PlanarTransform reflection{{0,0},0.2,true,false,{3,1}};
    auto good=source; transform_wall_curve_input(good,reflection);
    const auto geometry=transform_segment(baseline(source),reflection);
    good.properties["baseline"]=segment_json(geometry.start,geometry.end,geometry.sweep_radians);
    std::vector<Entity> corruptions;
    auto bad=good; bad.extensions["curve_input_derivation"]["operations"][0]["transform"]["flip_horizontal"]=false; corruptions.push_back(bad);
    bad=good; bad.extensions["curve_input_derivation"]["operations"][0]["transform"]["vendor"]=true; corruptions.push_back(bad);
    bad=good; bad.extensions["curve_input_derivation"]["version"]=1; corruptions.push_back(bad);
    bad=good; bad.extensions["curve_input_derivation"]["version"]=2.0; corruptions.push_back(bad);
    bad=good; bad.extensions["curve_input_derivation"]["version"]=2.5; corruptions.push_back(bad);
    bad=good; bad.extensions["curve_input_derivation"]["operations"][0]["transform"]["version"]=1.0; corruptions.push_back(bad);
    bad=good; bad.extensions["curve_input_derivation"]["source_input"]["vendor"]["exact"]=18;
    bad.extensions["curve_input"]["vendor"]["exact"]=18; corruptions.push_back(bad);
    bad=good; bad.extensions["curve_input_derivation"]["operations"].push_back(bad.extensions.at("curve_input_derivation").at("operations").front()); corruptions.push_back(bad);
    for (const auto& corrupted : corruptions) {
        const auto before=document.snapshot(); bool rejected=false;
        const Command command=ApplyEntityChanges{document.revision(),{EntityChange::upsert(corrupted)}, {},"Forged rigid provenance"};
        try { (void)Document::preview_command(before,command); } catch (const DocumentError&) { rejected=true; }
        require(rejected,"forged rigid curve preview accepted");
        rejected=false; try { document.apply(command); } catch (const DocumentError&) { rejected=true; }
        require(rejected && document.revision()==before.revision() && document.snapshot().entities()==before.entities(),
            "forged rigid provenance changed document atomically");
    }
    // Upgrade an existing endpoint archive without rewriting its prefix, and
    // retain an exact physical-length receipt through reflected rigid motion.
    source=sources.at(2); auto resized=Document::create({source});
    ConstraintAuthoringIntent resize;
    resize.wall_resize=WallResizeIntent{"arc",parse_quantity("5 m"),WallResizeAnchor::start,true};
    const auto first=preview_constraint_authoring(resized.snapshot(),resize);
    require_accepted(first,"measured archive fixture resize rejected"); (void)apply_constraint_authoring(resized,first);
    const auto archive=resized.snapshot().entities().at("arc").extensions.at("curve_input_derivation");
    auto legacy=resized.snapshot().entities().at("arc");
    legacy.extensions["curve_input_derivation"]["operations"][0]["kind"]="vendor-baseline-kind";
    auto legacy_document=Document::create({legacy});
    const auto legacy_prefix=legacy.extensions.at("curve_input_derivation").at("operations");
    auto upgraded=legacy;
    const auto upgraded_geometry=transform_segment(baseline(legacy),reflection);
    transform_wall_curve_input(upgraded,reflection);
    rebase_wall_length_receipt(upgraded,upgraded_geometry);
    upgraded.properties["baseline"]=segment_json(upgraded_geometry.start,upgraded_geometry.end,upgraded_geometry.sweep_radians);
    legacy_document.apply(ApplyEntityChanges{legacy_document.revision(),{EntityChange::upsert(upgraded)}, {},"Upgrade vendor baseline archive"});
    require(upgraded.extensions.at("curve_input_derivation").at("version")==2 &&
        upgraded.extensions.at("curve_input_derivation").at("operations").front()==legacy_prefix.front() &&
        Document::fork(legacy_document.snapshot()).snapshot().entities()==legacy_document.snapshot().entities(),
        "legacy baseline vendor kind must remain opaque through archive upgrade and replay");
    auto moved=resized.snapshot().entities().at("arc");
    const auto mirrored=transform_segment(baseline(moved),reflection);
    transform_wall_curve_input(moved,reflection); rebase_wall_length_receipt(moved,mirrored);
    moved.properties["baseline"]=segment_json(mirrored.start,mirrored.end,mirrored.sweep_radians);
    require(moved.extensions.at("curve_input_derivation").at("operations").front()==archive.at("operations").front(),
        "archive upgrade rewrote endpoint derivation prefix");
    validate_wall_length_input(moved);
    resized.apply(ApplyEntityChanges{resized.revision(),{EntityChange::upsert(moved)}, {},"Reflect physical receipt curve"});
    resize.wall_resize->exact_length=parse_quantity("600 cm");
    const auto second=preview_constraint_authoring(resized.snapshot(),resize);
    require_accepted(second,"physical resize after rigid provenance rejected"); (void)apply_constraint_authoring(resized,second);
    require(resized.snapshot().entities().at("arc").extensions.at("curve_input_derivation").at("version")==2 &&
        resized.snapshot().entities().at("arc").extensions.at("curve_input_derivation").at("source_input")==source.extensions.at("curve_input"),
        "subsequent endpoint proof lost transformed source archive");
    auto construction=resized.snapshot().entities().at("arc"); const auto previous=construction;
    auto opaque=construction;
    opaque.extensions["constraint_authoring"]["last_length_entry"]["vendor"]=true;
    const auto opaque_before=opaque;
    bool clear_rejected=false;
    try { clear_wall_length_input(opaque); } catch (const std::exception&) { clear_rejected=true; }
    require(clear_rejected && opaque==opaque_before,"receipt invalidation lost opaque input or mutated on refusal");
    clear_wall_length_input(construction);
    const auto new_geometry=arc_from_chord_height(baseline(construction).start,baseline(construction).end,0.5);
    construction.properties["baseline"]=segment_json(new_geometry.start,new_geometry.end,new_geometry.sweep_radians);
    auto& input=construction.extensions["curve_input"];
    input["version"]=2; input["construction"]="arc_height"; input["measure"]="1/2 m";
    input["normalized_measure"]="0.5 m"; input["measure_value"]=0.5; input["clockwise"]=false;
    input["radians"]=new_geometry.sweep_radians;
    preserve_wall_curve_construction(construction,previous);
    resized.apply(ApplyEntityChanges{resized.revision(),{EntityChange::upsert(construction)}, {},"Explicit construction after rigid provenance"});
    require(Document::fork(resized.snapshot()).snapshot().entities()==resized.snapshot().entities(),
        "construction after reflected physical receipt cannot replay");
}

void test_direct_curved_wall_physical_resize() {
    for (const auto sweep : {0.6,-0.6,4.0,-4.0}) for (const auto anchor : {WallResizeAnchor::start,WallResizeAnchor::end}) {
        const auto chord=7*2*std::sin(std::abs(sweep)/2)/std::abs(sweep);
        auto owner=wall("arc",{2,3},{2+chord,3},sweep);
        const auto angle=angle_from_radians(sweep);
        owner.extensions["curve_input"]={{"version",2},{"construction","arc_length"},{"measure","7 m"},
            {"normalized_measure","7 m"},{"measure_value",7},{"clockwise",sweep<0},
            {"start",{2,3}},{"end",{2+chord,3}},{"radians",sweep},{"sweep",angle.original_expression},
            {"normalized_sweep",angle.normalized_expression},{"vendor","source retained"}};
        const auto original_input=owner.extensions.at("curve_input");
        const auto moved_role=anchor==WallResizeAnchor::start ? WallEndpointRole::end : WallEndpointRole::start;
        const auto neighbor_role=anchor==WallResizeAnchor::start ? WallEndpointRole::start : WallEndpointRole::end;
        auto neighbor=anchor==WallResizeAnchor::start ? wall("neighbor",{2+chord,3},{5+chord,3},0.2) :
            wall("neighbor",{-1,3},{2,3},0.2);
        const auto join=encode_constraint_entity(relation("join",ConstraintRelationKind::coincident,
            {endpoint("arc",moved_role),endpoint("neighbor",neighbor_role)}));
        auto document=Document::create({owner,neighbor,join,opening("door","arc",0.3,0.5)});
        const auto before=document.snapshot();
        ConstraintAuthoringIntent intent;
        intent.wall_resize=WallResizeIntent{"arc",parse_quantity("5 m"),anchor,true};
        const auto preview=preview_constraint_authoring(before,intent);
        require_accepted(preview,"direct curved-wall physical resize must accept representable target");
        require(document.snapshot().entities()==before.entities() && document.revision()==before.revision(),
            "direct curved resize preview mutated source");
        const auto proposed=baseline(preview.candidate_entities().at("arc"));
        require_near(segment_length(proposed),5,1e-6,"direct curved-wall resize measured chord rather than physical length");
        require(proposed.sweep_radians==sweep && proposed.start.y==3 && proposed.end.y==3,
            "direct curved resize changed signed sweep or chord direction");
        require(anchor==WallResizeAnchor::start ? proposed.start.x==2 : proposed.end.x==2+chord,
            "direct curved resize changed selected exact anchor");
        require(preview.candidate_entities().at("arc").extensions.at("curve_input_derivation").at("source_input")==original_input,
            "direct resize lost original measured curve receipt");
        require(preview.candidate_entities().at("door")==before.entities().at("door"),"resize rewrote hosted opening metadata");
        const auto& receipt=preview.candidate_entities().at("arc").extensions.at("constraint_authoring").at("last_length_entry");
        require(receipt.at("version")==2 && receipt.at("original_expression")=="5 m" &&
            receipt.at("baseline")==preview.candidate_entities().at("arc").properties.at("baseline"),
            "curved physical receipt does not record exact target and signed baseline");
        validate_wall_length_input(preview.candidate_entities().at("arc"));
        auto freeze=intent; freeze.wall_resize->move_connected_walls=false;
        const auto conflict=preview_constraint_authoring(before,freeze);
        require(!conflict.accepted(),"disabled connected movement silently detached resized curve");
        require_rejected_unchanged(document,conflict,"frozen curved component Apply accepted");
        auto arc_lock=relation("physical-lock",ConstraintRelationKind::fixed_arc_length,
            {endpoint("arc",WallEndpointRole::start),endpoint("arc",WallEndpointRole::end)});
        arc_lock.length=parse_quantity("7 m");
        auto locked=Document::create({owner,encode_constraint_entity(arc_lock)});
        const auto locked_preview=preview_constraint_authoring(locked.snapshot(),intent);
        require(!locked_preview.accepted(),"direct curved resize bypassed fixed physical arc lock");
        require_rejected_unchanged(locked,locked_preview,"locked physical curve Apply accepted");
        (void)apply_constraint_authoring(document,preview);
        const auto after=document.snapshot();
        require(after.history().size()==before.history().size()+1,"curved resize split atomic command");
        const auto& proof=*after.history().back().boundary_constraint_changes;
        bool selected=false,related=false;
        for (const auto& edit : proof.wall_edits) {
            if (edit.wall_id=="arc") {
                selected=edit.version==3 && edit.length_entry && edit.length_entry->original_expression=="5 m";
                const auto encoded=encode_constraint_wall_edit(edit);
                require(encoded.at("version")==3 && encode_constraint_wall_edit(decode_constraint_wall_edit(encoded))==encoded,
                    "curved physical proof codec changed exact entry");
                auto bad=encoded; bad["length_entry"]=nullptr;
                bool rejected=false; try { (void)decode_constraint_wall_edit(bad); } catch (const std::exception&) { rejected=true; }
                require(rejected,"physical curved proof accepted missing exact entry");
                bad=encoded; bad["version"]=2; rejected=false;
                try { (void)decode_constraint_wall_edit(bad); } catch (const std::exception&) { rejected=true; }
                require(rejected,"physical curve proof downgraded to endpoint-only version");
            } else if (edit.wall_id=="neighbor") related=edit.version==2 && !edit.length_entry;
        }
        require(selected && related,"selected/related curved walls used wrong proof semantics");
        require(Document::fork(after).snapshot().entities()==after.entities(),"curved resize proof cannot replay");
        document.undo(document.revision()); require(document.snapshot().entities()==before.entities(),"curved resize undo lost atomicity");
        document.redo(document.revision()); require(document.snapshot().entities()==after.entities(),"curved resize redo differs");
        auto annotated=document.snapshot().entities().at("arc");
        auto& retained=annotated.extensions["constraint_authoring"]["last_length_entry"];
        retained["vendor"]="retain"; retained["exact_metres"]["vendor"]=17; retained["baseline"]["vendor"]=true;
        document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(annotated)},{},"Annotate exact receipt"});
        intent.wall_resize->exact_length=parse_quantity("600 cm");
        const auto second=preview_constraint_authoring(document.snapshot(),intent);
        require_accepted(second,"second measured curved resize rejected");
        (void)apply_constraint_authoring(document,second);
        const auto resized=document.snapshot();
        const auto& latest=resized.entities().at("arc").extensions.at("constraint_authoring").at("last_length_entry");
        require(latest.at("version")==2 && latest.at("original_expression")=="600 cm" && latest.at("entered_unit")=="cm" &&
            latest.at("vendor")=="retain" && latest.at("exact_metres").at("vendor")==17 && latest.at("baseline").at("vendor")==true,
            "second curved resize lost exact entry or opaque receipt fields");
        require(resized.entities().at("arc").extensions.at("curve_input_derivation").at("source_input")==original_input,
            "second resize rewrote source construction archive");
        auto forged=resized.entities().at("arc");
        forged.extensions["constraint_authoring"]["last_length_entry"]["baseline"]["end"][0]=0;
        bool receipt_rejected=false; try { validate_wall_length_input(forged); }
        catch (const std::exception&) { receipt_rejected=true; }
        require(receipt_rejected,"standalone physical receipt validation accepted stale baseline");
        auto reflected=resized.entities().at("arc");
        auto reflection=baseline(reflected); reflection.sweep_radians=-reflection.sweep_radians;
        rebase_wall_length_receipt(reflected,reflection);
        bool provenance_rejected=false; try { rebase_wall_curve_input(reflected,reflection); }
        catch (const std::exception&) { provenance_rejected=true; }
        require(provenance_rejected,"measured curve mirror bypassed original construction provenance");
        reflected.properties["baseline"]=segment_json(reflection.start,reflection.end,reflection.sweep_radians);
        bool mirror_rejected=false;
        try { document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(reflected)},{},"Invalid measured mirror"}); }
        catch (const DocumentError&) { mirror_rejected=true; }
        require(mirror_rejected && document.snapshot().entities()==resized.entities() && document.revision()==resized.revision(),
            "measured curve mirror changed document despite invalid provenance");
        const PlanarTransform transform{{0,0},0.5,false,false,{10,-2}};
        std::vector<EntityChange> transforms;
        for (const auto* id : {"arc","neighbor"}) {
            auto moved=resized.entities().at(id);
            const auto transformed=transform_segment(baseline(moved),transform);
            rebase_wall_length_receipt(moved,transformed);
            rebase_wall_curve_input(moved,transformed);
            moved.properties["baseline"]=segment_json(transformed.start,transformed.end,transformed.sweep_radians);
            validate_wall_length_input(moved);
            transforms.push_back(EntityChange::upsert(moved));
        }
        document.apply(ApplyEntityChanges{document.revision(),std::move(transforms),{},"Rigid curved receipt transform"});
        require(Document::fork(document.snapshot()).snapshot().entities()==document.snapshot().entities(),
            "rigid curved receipt transform cannot replay");
        const auto moved_snapshot=document.snapshot();
        const auto& moved_receipt=moved_snapshot.entities().at("arc").extensions.at("constraint_authoring").at("last_length_entry");
        require(moved_receipt.at("original_expression")=="600 cm" && moved_receipt.at("vendor")=="retain",
            "rigid transform lost physical receipt metadata");
        auto invalidated=resized.entities().at("arc");
        auto deformed=baseline(invalidated); deformed.end.x+=0.5;
        bool rejected=false;
        try { (void)replay_constraint_wall_edit(invalidated,{"arc",deformed,std::nullopt,2}); }
        catch (const std::exception&) { rejected=true; }
        require(rejected,"geometry-only edit erased opaque physical receipt data");
        auto known=invalidated;
        auto& known_receipt=known.extensions["constraint_authoring"]["last_length_entry"];
        known_receipt.erase("vendor"); known_receipt["exact_metres"].erase("vendor"); known_receipt["baseline"].erase("vendor");
        const auto cleared=replay_constraint_wall_edit(known,{"arc",deformed,std::nullopt,2});
        require(!cleared.extensions.at("constraint_authoring").contains("last_length_entry"),
            "geometry-only length change retained stale physical receipt");
        auto future=resized.entities().at("arc");
        future.extensions["constraint_authoring"]["last_length_entry"]["version"]=99;
        const auto future_before=future;
        validate_wall_length_input(future);
        require(future==future_before,"restoration validator rewrote optional future receipt metadata");
        bool future_rejected=false;
        try { (void)replay_constraint_wall_edit(future,{"arc",baseline(future),parse_quantity("6 m"),3}); }
        catch (const std::exception&) { future_rejected=true; }
        require(future_rejected,"resize overwrote unsupported physical receipt metadata");
    }
    // A direct receipt does not introduce a new reflection restriction for a
    // numeric curve that has no separate measured-construction provenance.
    auto numeric=Document::create({wall("arc",{0,0},{4,0},0.6)});
    ConstraintAuthoringIntent resize;
    resize.wall_resize=WallResizeIntent{"arc",parse_quantity("5 m"),WallResizeAnchor::start,true};
    const auto preview=preview_constraint_authoring(numeric.snapshot(),resize);
    require_accepted(preview,"numeric curve direct resize rejected");
    (void)apply_constraint_authoring(numeric,preview);
    const auto before_mirror=numeric.snapshot();
    auto reflected=before_mirror.entities().at("arc");
    const auto mirrored=transform_segment(baseline(reflected),{{0,0},0,false,true,{3,2}});
    rebase_wall_length_receipt(reflected,mirrored);
    rebase_wall_curve_input(reflected,mirrored);
    reflected.properties["baseline"]=segment_json(mirrored.start,mirrored.end,mirrored.sweep_radians);
    validate_wall_length_input(reflected);
    numeric.apply(ApplyEntityChanges{numeric.revision(),{EntityChange::upsert(reflected)},{},"Mirror numeric measured-length curve"});
    const auto mirrored_snapshot=numeric.snapshot();
    const auto& receipt=mirrored_snapshot.entities().at("arc").extensions.at("constraint_authoring").at("last_length_entry");
    require(receipt.at("version")==2 && receipt.at("original_expression")=="5 m" &&
        receipt.at("baseline").at("sweep_radians")==-0.6 &&
        receipt.at("baseline")==reflected.properties.at("baseline"),"mirror lost exact physical receipt or reflected signed baseline");
    require_near(segment_length(baseline(reflected)),5,1e-6,"reflection changed physical arc length");
    require(Document::fork(mirrored_snapshot).snapshot().entities()==mirrored_snapshot.entities(),"reflected receipt cannot replay");
    numeric.undo(numeric.revision()); require(numeric.snapshot().entities()==before_mirror.entities(),"mirror undo lost receipt");
    numeric.redo(numeric.revision()); require(numeric.snapshot().entities()==mirrored_snapshot.entities(),"mirror redo differs");
}

void test_curved_endpoint_relations_and_typed_propagation() {
    for (const auto kind : {ConstraintRelationKind::horizontal, ConstraintRelationKind::vertical,
            ConstraintRelationKind::coincident, ConstraintRelationKind::fixed_length,
            ConstraintRelationKind::parallel, ConstraintRelationKind::perpendicular,
            ConstraintRelationKind::fixed_anchor}) {
        auto document=Document::create({wall("arc",{0,0},{4,0},0.6),
            wall("vertical",{4,0},{4,3},-0.4),wall("parallel",{0,6},{4,6},0.3)});
        auto value=relation("curve-lock",kind,{endpoint("arc",WallEndpointRole::start),endpoint("arc",WallEndpointRole::end)});
        if (kind==ConstraintRelationKind::vertical)
            value.bindings={endpoint("vertical",WallEndpointRole::start),endpoint("vertical",WallEndpointRole::end)};
        else if (kind==ConstraintRelationKind::coincident)
            value.bindings={endpoint("arc",WallEndpointRole::end),endpoint("vertical",WallEndpointRole::start)};
        else if (kind==ConstraintRelationKind::fixed_length) value.length=parse_quantity("4 m");
        else if (kind==ConstraintRelationKind::fixed_anchor) { value.bindings.resize(1); value.anchor=Vec2{0,0}; }
        else if (kind==ConstraintRelationKind::parallel || kind==ConstraintRelationKind::perpendicular) {
            const auto other=kind==ConstraintRelationKind::parallel ? "parallel" : "vertical";
            value.bindings.push_back(endpoint(other,WallEndpointRole::start));
            value.bindings.push_back(endpoint(other,WallEndpointRole::end));
        }
        const auto before=document.snapshot();
        ConstraintAuthoringIntent intent;
        intent.relation_mutations={ConstraintRelationMutation::upsert(value)};
        const auto preview=preview_constraint_authoring(before,intent);
        require_accepted(preview,"already-satisfied chord relation on curved endpoints rejected");
        (void)apply_constraint_authoring(document,preview);
        require(document.snapshot().entities().at("arc")==before.entities().at("arc"),"curved relation flattened or moved owner");
        require(segment_length(baseline(document.snapshot().entities().at("arc")))>4,
            "fixed endpoint distance was reinterpreted as arc length");
        require(analyze_persistent_constraint_component(document.snapshot(),{"arc"}).supported,
            "curved endpoint component rank must be available");
    }
    auto owner=encode_identified_boundary_entity(IdentifiedBoundary{"area","measurement_boundary",{
        {"ab","a","b",{{0,0},{3,0},0.2}}, {"bc","b","c",{{3,0},{3,2},0}},
        {"cd","c","d",{{3,2},{0,2},0}}, {"da","d","a",{{0,2},{0,0},0}}}});
    const auto curved_baseline=arc_from_chord_height({3,0},{6,0},0.25);
    auto arc=wall("arc",{3,0},{6,0},curved_baseline.sweep_radians);
    const auto sweep=angle_from_radians(curved_baseline.sweep_radians);
    arc.extensions["curve_input"]={{"version",2},{"construction","arc_height"},{"measure","0.25 m"},
        {"normalized_measure","0.25 m"},{"measure_value",0.25},{"clockwise",false},
        {"start",{3,0}},{"end",{6,0}},{"sweep",sweep.original_expression},{"normalized_sweep",sweep.normalized_expression},
        {"radians",curved_baseline.sweep_radians},{"vendor",{{"retain",17}}}};
    const auto original_input=arc.extensions.at("curve_input");
    const auto join=encode_constraint_entity(relation("join",ConstraintRelationKind::coincident,
        {{"area",WallEndpointRole::end,"ab","b"},endpoint("arc",WallEndpointRole::start)}));
    auto document=Document::create({owner,arc,join,opening("door","arc",0.3,0.5)});
    const auto before=document.snapshot();
    ConstraintAuthoringIntent intent;
    intent.boundary_vertex_move=BoundaryVertexMoveIntent{{"area",BoundaryGeometryEditKind::move_vertex,"b",{4,0.5}},true};
    const auto preview=preview_constraint_authoring(before,intent);
    require_accepted(preview,"curved mixed endpoint propagation rejected");
    require_near(baseline(preview.candidate_entities().at("arc")).start.x,4,1e-7,"curved wall endpoint did not follow");
    require(baseline(preview.candidate_entities().at("arc")).sweep_radians==curved_baseline.sweep_radians,
        "connected endpoint solve changed signed sweep");
    require(preview.candidate_entities().at("arc").extensions.at("curve_input_derivation").at("source_input")==original_input,
        "nonrigid curved edit lost original construction receipt or opaque metadata");
    (void)apply_constraint_authoring(document,preview);
    require(command_to_json(Command{*document.snapshot().history().back().boundary_constraint_changes}).at("version")==3,
        "curved typed replay requires command version three");
    const auto after=document.snapshot();
    document.undo(document.revision()); require(document.snapshot().entities()==before.entities(),"curved transaction undo split state");
    document.redo(document.revision()); require(document.snapshot().entities()==after.entities(),"curved transaction redo differs");
    auto restored=Document::fork(after); require(restored.snapshot().entities()==after.entities(),"curved proof replay differs");
    // A subsequent explicit construction is a complete independently checked
    // input, not an endpoint-only payload. It may follow a derived curve, but
    // it must retain the source archive and every prior operation.
    auto next_baseline=baseline(after.entities().at("arc"));
    next_baseline.end.x+=0.5;
    const auto second=replay_constraint_wall_edit(after.entities().at("arc"),
        {"arc",next_baseline,std::nullopt,2});
    bool endpoint_payload_rejected=false;
    try { auto copy=Document::fork(after); copy.apply(ApplyEntityChanges{copy.revision(),
        {EntityChange::upsert(second)}, {},"unqualified derived endpoint payload"}); }
    catch (const DocumentError&) { endpoint_payload_rejected=true; }
    require(endpoint_payload_rejected,"derived endpoint-only payload must require its typed proof");
    auto construction=second;
    auto& operation=construction.extensions["curve_input_derivation"]["operations"].back();
    operation={{"baseline",construction.properties.at("baseline")},
        {"input",construction.extensions.at("curve_input")}};
    auto explicit_document=Document::fork(after);
    explicit_document.apply(ApplyEntityChanges{explicit_document.revision(),
        {EntityChange::upsert(construction)}, {},"explicit full curve construction"});
    require(explicit_document.snapshot().entities().at("arc").extensions.at("curve_input_derivation").at("source_input")==original_input,
        "explicit reconstruction must retain the original measured construction");
    require(Document::fork(explicit_document.snapshot()).snapshot().entities()==explicit_document.snapshot().entities(),
        "explicit reconstruction history must independently replay");

    for (const auto unit : {Unit::metre,Unit::foot}) {
        const auto quantity=parse_quantity("5",unit);
        const auto source_baseline=arc_from_chord_arc_length({0,0},{1,0},quantity.metres,false);
        auto measured=wall("measured",{0,0},{1,0},source_baseline.sweep_radians);
        const auto angle=angle_from_radians(source_baseline.sweep_radians);
        measured.extensions["curve_input"]={{"version",2},{"construction","arc_length"},{"measure","5"},
            {"normalized_measure",format_quantity(quantity,Unit::metre)},{"measure_value",quantity.metres},
            {"clockwise",false},{"start",{0,0}},{"end",{1,0}},{"radians",source_baseline.sweep_radians},
            {"sweep",angle.original_expression},{"normalized_sweep",angle.normalized_expression}};
        auto measured_document=Document::create({measured});
        auto horizontal=relation("level",ConstraintRelationKind::horizontal,
            {endpoint("measured",WallEndpointRole::start),endpoint("measured",WallEndpointRole::end)});
        ConstraintAuthoringIntent add;
        add.relation_mutations={ConstraintRelationMutation::upsert(horizontal)};
        require_accepted(preview_constraint_authoring(measured_document.snapshot(),add),
            "implicit metric/foot curve receipt must validate against stored SI value");
    }
    auto wall_document=Document::create({wall("curve",{0,0},{4,0},0.6),
        encode_constraint_entity(relation("level",ConstraintRelationKind::horizontal,
            {endpoint("curve",WallEndpointRole::start),endpoint("curve",WallEndpointRole::end)}))});
    const auto wall_before=wall_document.snapshot();
    auto distance=relation("distance",ConstraintRelationKind::fixed_length,
        {endpoint("curve",WallEndpointRole::start),endpoint("curve",WallEndpointRole::end)});
    distance.length=parse_quantity("5 m");
    ConstraintAuthoringIntent resize_chord;
    resize_chord.relation_anchor=endpoint("curve",WallEndpointRole::start);
    resize_chord.relation_mutations={ConstraintRelationMutation::upsert(distance)};
    const auto wall_preview=preview_constraint_authoring(wall_before,resize_chord);
    require_accepted(wall_preview,"wall-only fixed chord solve rejected");
    (void)apply_constraint_authoring(wall_document,wall_preview);
    const auto wall_after=wall_document.snapshot();
    require(wall_after.history().back().boundary_constraint_changes.has_value() &&
        wall_after.history().back().boundary_constraint_changes->boundary_edits.empty(),
        "wall-only curved solve must retain its typed proof without invented boundary edits");
    auto raw_wall=wall_after.entities().at("curve");
    bool rejected=false;
    try { auto raw=Document::fork(wall_before); raw.apply(ApplyEntityChanges{raw.revision(),{EntityChange::upsert(raw_wall)}, {},"raw endpoint"}); }
    catch (const DocumentError&) { rejected=true; }
    require(rejected,"raw wall payload laundered a constrained curved endpoint solve");
    auto missing=wall_after;
    const_cast<std::vector<RevisionRecord>&>(missing.history()).back().boundary_constraint_changes.reset();
    rejected=false;
    try { (void)Document::fork(missing); } catch (const DocumentError&) { rejected=true; }
    require(rejected,"history accepted curved endpoint deformation with missing proof");
    const auto crossing=segment_intersection({{0,0},{4,0},1.0},{{2,-2},{2,1},0});
    require(crossing.kind==SegmentIntersectionKind::proper,"analytical arc/line crossing was treated as its chord");
    const auto overlap=segment_intersection({{0,0},{4,0},1.0},{{0,0},{4,0},1.0});
    require(overlap.kind==SegmentIntersectionKind::overlap,"coincident analytical arcs were not recognized as overlap");
}

void test_boundary_vertex_move_propagates_explicit_relations() {
    const IdentifiedBoundary shape{"area","measurement_boundary",{
        {"ab","a","b",{{0,0},{3,0},0}}, {"bc","b","c",{{3,0},{3,3},0}},
        {"cd","c","d",{{3,3},{0,3},0}}, {"da","d","a",{{0,3},{0,0},0}}}};
    const auto owner = encode_identified_boundary_entity(shape);
    auto neighbor = shape;
    neighbor.id = "neighbor";
    for (auto& edge : neighbor.segments) {
        edge.segment.start.x += 3; edge.segment.end.x += 3;
        edge.segment.start.y -= 3; edge.segment.end.y -= 3;
    }
    const WallEndpointBinding corner{"area",WallEndpointRole::end,"ab","b"};
    const auto join = encode_constraint_entity(relation("join",ConstraintRelationKind::coincident,
        {corner,endpoint("wall",WallEndpointRole::start)}));
    const auto neighbor_join = encode_constraint_entity(relation("neighbor-join",ConstraintRelationKind::coincident,
        {corner,{"neighbor",WallEndpointRole::start,"da","d"}}));
    auto document = Document::create({owner,wall("wall",{3,0},{6,0}),
        encode_identified_boundary_entity(neighbor),join,neighbor_join});
    const auto before = document.snapshot();
    const BoundaryGeometryEdit edit{"area",BoundaryGeometryEditKind::move_vertex,"b",{4,-0.5}};
    ConstraintAuthoringIntent intent;
    intent.boundary_vertex_move = BoundaryVertexMoveIntent{edit,true};
    const auto preview = preview_constraint_authoring(before,intent);
    require_accepted(preview,"boundary vertex move must propagate saved mixed relations");
    require(preview.candidate_entities().at("area") == edited_boundary_entities(before.entities(),edit).at("area"),
        "vertex move solver changed canonical selected geometry");
    const auto moved_wall = baseline(preview.candidate_entities().at("wall"));
    require_near(moved_wall.start.x,4,1e-7,"vertex move did not propagate to wall x");
    require_near(moved_wall.start.y,-0.5,1e-7,"vertex move did not propagate to wall y");
    const auto moved_neighbor = decode_identified_boundary_entity(preview.candidate_entities().at("neighbor"));
    require_near(moved_neighbor.segments[3].segment.start.x,4,1e-7,"vertex move did not propagate to neighbor x");
    require_near(moved_neighbor.segments[3].segment.start.y,-0.5,1e-7,"vertex move did not propagate to neighbor y");
    require(preview.boundary_edits().front() == edit && std::count_if(
        preview.boundary_edits().begin(),preview.boundary_edits().end(),
        [](const auto& proof) { return proof.boundary_id == "area"; }) == 1,
        "canonical vertex move must appear exactly once at the start of typed proof");
    intent.boundary_vertex_move->move_related_objects = false;
    const auto frozen = preview_constraint_authoring(before,intent);
    require(!frozen.accepted(),"frozen mixed neighbors must reject vertex move conflict");
    require_rejected_unchanged(document,frozen,"frozen vertex move applied");
    (void)apply_constraint_authoring(document,preview);
    const auto committed = document.snapshot();
    require(committed.entities() == preview.candidate_entities() && committed.revision() == before.revision()+1,
        "vertex move did not atomically commit the shown result");
    require(committed.history().back().boundary_constraint_changes.has_value(),"vertex move omitted typed transaction");
    const auto command = command_to_json(*committed.history().back().boundary_constraint_changes);
    require(command_to_json(command_from_json(command)) == command,"vertex move proof codec changed evidence");
    auto tampered = preview;
    auto& candidate = const_cast<std::map<std::string,Entity,std::less<>>&>(tampered.candidate_entities());
    candidate.at("area").properties["name"] = "forged";
    auto untouched = Document::fork(before);
    require_rejected_unchanged(untouched,tampered,"tampered vertex move preview applied");
    require_rejected_unchanged(document,preview,"stale vertex move preview applied");
    const auto path = std::filesystem::temp_directory_path() / ("boundary-vertex-move-"+make_stable_id()+".bldproj");
    (void)ProjectStore::save(path,committed);
    auto reopened = ProjectStore::load(path);
    std::filesystem::remove(path);
    require(reopened.document.snapshot().entities() == committed.entities(),"vertex move reopen differs");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == before.entities(),"vertex move undo split transaction");
    reopened.document.redo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == committed.entities(),"vertex move redo differs");

    intent.boundary_vertex_move->move_related_objects = true;
    auto lock = relation("pin",ConstraintRelationKind::fixed_anchor,{corner});
    lock.anchor = Vec2{3,0};
    auto locked = Document::create({owner,encode_constraint_entity(lock)});
    const auto conflict = preview_constraint_authoring(locked.snapshot(),intent);
    require(!conflict.accepted(),"vertex move must refuse persisted endpoint lock");
    require_rejected_unchanged(locked,conflict,"locked vertex move applied");
    auto detached = Document::create({owner,wall("wall",{3,0},{6,0})});
    const auto detached_before = detached.snapshot();
    const auto independent = preview_constraint_authoring(detached_before,intent);
    require_accepted(independent,"unrelated wall must not block vertex move");
    require(independent.changed_walls().empty() && independent.candidate_entities().at("wall") == detached_before.entities().at("wall"),
        "coordinate coincidence must not propagate vertex move");
    intent.boundary_vertex_move->edit.target_position = {3,0};
    const auto noop = preview_constraint_authoring(detached_before,intent);
    require(!noop.accepted() && has_diagnostic(noop,"no document change"),"same vertex position must reject as no-op");
    require_rejected_unchanged(detached,noop,"no-op vertex move applied");
    intent.boundary_vertex_move->edit = edit;
    auto curved_shape = shape;
    curved_shape.segments[2].segment.sweep_radians = 0.1;
    auto curved = Document::create({encode_identified_boundary_entity(curved_shape)});
    require_accepted(preview_constraint_authoring(curved.snapshot(),intent),"vertex solve must support fixed-sweep curved owner");
    intent.boundary_vertex_move->edit.target_id = "missing";
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"vertex move must reject missing stable target");
    intent.boundary_vertex_move->edit = edit;
    intent.boundary_vertex_move->edit.target_position.x = std::numeric_limits<double>::infinity();
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"vertex move must reject nonfinite position");
    intent.boundary_vertex_move->edit = {"area",BoundaryGeometryEditKind::resize_segment,"ab",{},4};
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"vertex move intent must reject other edit kinds");
    intent.boundary_vertex_move->edit = edit;
    intent.boundary_resize = BoundaryResizeIntent{{"area",BoundaryGeometryEditKind::resize_segment,"ab",{},4},true};
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"two boundary coordinate intents must reject");
    intent.boundary_resize.reset();
    intent.wall_resize = WallResizeIntent{"wall",parse_quantity("4 m")};
    require(!preview_constraint_authoring(detached_before,intent).accepted(),"wall resize plus boundary vertex move must reject");
}

void test_wall_geometry_move_propagates_explicit_connections_only() {
    auto document = Document::create({wall("wall-a", {0,0}, {2,0}),
        wall("wall-b", {2,0}, {2,3}), wall("wall-c", {2,0}, {3,0})});
    ConstraintAuthoringIntent establish_receipt;
    establish_receipt.wall_resize = WallResizeIntent{"wall-b", parse_quantity("2 m"),
        WallResizeAnchor::start, false};
    const auto receipt_preview = preview_constraint_authoring(document.snapshot(), establish_receipt);
    require_accepted(receipt_preview, "connected neighbor exact-length fixture resize rejected");
    (void)apply_constraint_authoring(document, receipt_preview);

    auto joined = relation("join", ConstraintRelationKind::coincident,
        {endpoint("wall-a", WallEndpointRole::end), endpoint("wall-b", WallEndpointRole::start)});
    auto pin = relation("wall-b-end", ConstraintRelationKind::fixed_anchor,
        {endpoint("wall-b", WallEndpointRole::end)});
    pin.anchor = Vec2{2,2};
    ConstraintAuthoringIntent connect;
    connect.relation_mutations = {ConstraintRelationMutation::upsert(joined),
        ConstraintRelationMutation::upsert(pin)};
    const auto connected = preview_constraint_authoring(document.snapshot(), connect);
    require_accepted(connected, "coincident relation and neighbor endpoint lock fixture rejected");
    (void)apply_constraint_authoring(document, connected);
    const auto before = document.snapshot();
    ConstraintAuthoringIntent intent;
    intent.wall_geometry_move = WallGeometryMoveIntent{{
        {"wall-a", {10,5}, {12,5}}}, true};

    const auto preview = preview_constraint_authoring(before, intent);
    require_accepted(preview, "explicit wall move must solve its persisted connected component");
    const auto moved_a = baseline(preview.candidate_entities().at("wall-a"));
    const auto moved_b = baseline(preview.candidate_entities().at("wall-b"));
    require_near(moved_a.start.x, 10, 1e-9, "selected wall start x");
    require_near(moved_a.start.y, 5, 1e-9, "selected wall start y");
    require_near(moved_a.end.x, 12, 1e-9, "selected wall end x");
    require_near(moved_a.end.y, 5, 1e-9, "selected wall end y");
    require_near(moved_b.start.x, moved_a.end.x, 1e-7,
        "persisted coincident neighbor did not follow the moved endpoint");
    require_near(moved_b.start.y, moved_a.end.y, 1e-7,
        "persisted coincident neighbor did not follow the moved endpoint");
    require_near(moved_b.end.x, 2, 1e-9, "persisted neighbor endpoint lock moved");
    require_near(moved_b.end.y, 2, 1e-9, "persisted neighbor endpoint lock moved");
    require(std::abs(length(moved_b) - length(baseline(before.entities().at("wall-b")))) > 1,
        "connected neighbor fixture did not exercise a length-changing replay");
    require(!preview.candidate_entities().at("wall-b").extensions.at("constraint_authoring")
        .contains("last_length_entry"),
        "length-changing connected neighbor edit retained a stale exact-length receipt");
    require(preview.candidate_entities().at("wall-c") == before.entities().at("wall-c"),
        "coordinate-only coincidence moved an unrelated wall");
    require(document.snapshot().entities() == before.entities(),
        "wall move preview mutated the source document");

    (void)apply_constraint_authoring(document, preview);
    const auto committed = document.snapshot();
    require(committed.entities() == preview.candidate_entities() &&
        committed.revision() == before.revision() + 1,
        "wall move did not atomically commit the shown candidate");
    require(committed.history().back().boundary_constraint_changes.has_value(),
        "wall move did not use the typed constraint replay command");
    const auto encoded = command_to_json(Command{
        *committed.history().back().boundary_constraint_changes});
    require(encoded.contains("wall_edits") && encoded.at("wall_edits").size() == 2,
        "wall move proof omitted selected and connected geometry edits");
    const auto neighbor_proof = std::find_if(encoded.at("wall_edits").begin(),
        encoded.at("wall_edits").end(), [](const json& edit) { return edit.at("wall_id") == "wall-b"; });
    require(neighbor_proof != encoded.at("wall_edits").end() &&
        neighbor_proof->at("length_entry").is_null(),
        "length-changing connected neighbor proof must use typed receipt invalidation");
    for (const auto& [id, entity] : committed.entities()) {
        (void)entity;
        require(id.find("__constraint_authoring_") == std::string::npos,
            "temporary solve anchors were persisted as entities");
    }
    require(Document::fork(committed).snapshot().entities() == committed.entities(),
        "wall move typed proof did not independently replay");

    const auto path = std::filesystem::temp_directory_path() /
        ("wall-geometry-move-" + make_stable_id() + ".bldproj");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{path};
    (void)ProjectStore::save(path, committed);
    auto reopened = ProjectStore::load(path);
    require(reopened.document.snapshot().entities() == committed.entities(),
        "saved wall move proof reopened with different geometry");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == before.entities(),
        "wall move undo did not restore the complete original state");
    reopened.document.redo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == committed.entities(),
        "wall move redo did not restore the typed candidate");
    require_rejected_unchanged(document, preview, "stale wall move preview applied twice");
}

void test_wall_geometry_move_multiselection_validation_and_hosting() {
    auto document = Document::create({wall("alpha", {0,0}, {2,0}),
        wall("beta", {10,0}, {10,3})});
    ConstraintAuthoringIntent intent;
    // Both targets describe the same quarter-turn and translation. Reversing
    // the array order must not change which geometry belongs to either ID.
    intent.wall_geometry_move = WallGeometryMoveIntent{{
        {"beta", {4,15}, {1,15}}, {"alpha", {4,5}, {4,7}}}, false};
    const auto before = document.snapshot();
    const auto preview = preview_constraint_authoring(before, intent);
    require_accepted(preview, "explicit multi-wall move rejected valid per-wall targets");
    const auto moved_alpha = baseline(preview.candidate_entities().at("alpha"));
    const auto moved_beta = baseline(preview.candidate_entities().at("beta"));
    require_near(moved_alpha.start.x, 4, 1e-9, "multi-selection alpha start x");
    require_near(moved_alpha.start.y, 5, 1e-9, "multi-selection alpha start y");
    require_near(moved_alpha.end.x, 4, 1e-9, "multi-selection alpha end x");
    require_near(moved_alpha.end.y, 7, 1e-9, "multi-selection alpha end y");
    require_near(moved_beta.start.x, 4, 1e-9, "multi-selection beta start x");
    require_near(moved_beta.start.y, 15, 1e-9, "multi-selection beta start y");
    require_near(moved_beta.end.x, 1, 1e-9, "multi-selection beta end x");
    require_near(moved_beta.end.y, 15, 1e-9, "multi-selection beta end y");

    auto reject = [&](ConstraintAuthoringIntent invalid_intent, std::string_view message) {
        require(!preview_constraint_authoring(before, invalid_intent).accepted(), message);
    };
    auto invalid = intent;
    invalid.wall_geometry_move->targets.push_back(invalid.wall_geometry_move->targets.front());
    reject(invalid, "duplicate selected wall IDs must be rejected");
    invalid = intent;
    invalid.wall_geometry_move->targets = {{"missing", {0,0}, {2,0}}};
    reject(invalid, "unknown selected wall ID must be rejected");
    invalid = intent;
    invalid.wall_geometry_move->targets = {{"alpha", {0,0}, {0,0}}};
    reject(invalid, "degenerate target wall must be rejected");
    invalid = intent;
    invalid.wall_geometry_move->targets = {{"alpha", {0,0}, {3,0}}};
    reject(invalid, "length-changing selected wall target must be rejected");
    invalid = intent;
    invalid.wall_geometry_move->targets = {{"alpha",
        {std::numeric_limits<double>::infinity(), 0}, {2,0}}};
    reject(invalid, "non-finite wall target must be rejected");
    invalid = intent;
    invalid.wall_geometry_move->targets.clear();
    reject(invalid, "empty selected wall set must be rejected");
    invalid = intent;
    invalid.wall_geometry_move->targets = {{"alpha", {0,0}, {2,0}}};
    reject(invalid, "wall move with no document change must be rejected");

    const auto lock = encode_constraint_entity([&] {
        auto value = relation("pin", ConstraintRelationKind::fixed_anchor,
            {endpoint("alpha", WallEndpointRole::start)});
        value.anchor = Vec2{0,0};
        return value;
    }());
    auto locked = Document::create({wall("alpha", {0,0}, {2,0}),
        wall("beta", {10,0}, {10,3}), lock});
    const auto conflict = preview_constraint_authoring(locked.snapshot(), intent);
    require(!conflict.accepted(), "wall move must reject a conflicting persisted endpoint lock");
    require_rejected_unchanged(locked, conflict, "locked wall move applied");

    auto frozen = intent;
    frozen.wall_geometry_move->move_connected_walls = false;
    const auto join = encode_constraint_entity(relation("join", ConstraintRelationKind::coincident,
        {endpoint("alpha", WallEndpointRole::end), endpoint("neighbor", WallEndpointRole::start)}));
    auto connected = Document::create({wall("alpha", {0,0}, {2,0}),
        wall("beta", {10,0}, {10,3}), wall("neighbor", {2,0}, {2,2}), join});
    require(!preview_constraint_authoring(connected.snapshot(), frozen).accepted(),
        "disabled connected movement must freeze the related wall and reject conflict");

    auto read_only = Document::create({wall("alpha", {0,0}, {2,0}), wall("beta", {10,0}, {10,3})});
    read_only.mark_read_only("wall move read-only fixture");
    require(!preview_constraint_authoring(read_only.snapshot(), intent).accepted(),
        "wall move must reject read-only sources");

    auto foreign = Document::create({wall("alpha", {0,0}, {2,0}), wall("beta", {10,0}, {10,3})});
    require_rejected_unchanged(foreign, preview, "foreign document accepted a wall move preview");
    auto stale = Document::create({wall("alpha", {0,0}, {2,0}), wall("beta", {10,0}, {10,3})});
    const auto stale_preview = preview_constraint_authoring(stale.snapshot(), intent);
    auto renamed = stale.snapshot().entities().at("alpha");
    renamed.properties["classification"] = "new head";
    stale.apply(ApplyEntityChanges{stale.revision(), {EntityChange::upsert(renamed)}, {}, "advance"});
    require_rejected_unchanged(stale, stale_preview, "stale wall move preview applied");

    const auto host_join = encode_constraint_entity(relation("host-join",
        ConstraintRelationKind::coincident,
        {endpoint("host-selected", WallEndpointRole::end), endpoint("host-wall", WallEndpointRole::start)}));
    auto hosted = Document::create({wall("host-selected", {0,0}, {2,0}),
        wall("host-wall", {2,0}, {2,2}), host_join, opening("door", "host-wall", 1.7, 0.25)});
    ConstraintAuthoringIntent host_move;
    host_move.wall_geometry_move = WallGeometryMoveIntent{{
        {"host-selected", {0,1.95}, {2,1.95}}}, true};
    const auto stranded = preview_constraint_authoring(hosted.snapshot(), host_move);
    require(!stranded.accepted(), "wall move must reject a solve that strands a hosted opening");
    require_rejected_unchanged(hosted, stranded, "stranded hosted opening wall move applied");
}

void test_wall_geometry_move_preserves_arc_provenance_and_exact_length_receipt() {
    const auto original_length = parse_quantity("7 m");
    const auto source_baseline = arc_from_chord_arc_length({0,0}, {4,0},
        original_length.metres, true);
    const auto angle = angle_from_radians(source_baseline.sweep_radians);
    auto source = wall("measured-arc", source_baseline.start, source_baseline.end,
        source_baseline.sweep_radians);
    source.extensions["curve_input"] = {{"version",2}, {"construction","arc_length"},
        {"measure",original_length.original_expression},
        {"normalized_measure",format_quantity(original_length, Unit::metre)},
        {"measure_value",original_length.metres}, {"clockwise",true},
        {"start",{source_baseline.start.x,source_baseline.start.y}},
        {"end",{source_baseline.end.x,source_baseline.end.y}},
        {"radians",source_baseline.sweep_radians}, {"sweep",angle.original_expression},
        {"normalized_sweep",angle.normalized_expression}, {"vendor",{{"retain",17}}}};
    const auto original_input = source.extensions.at("curve_input");
    auto document = Document::create({source});

    ConstraintAuthoringIntent resize;
    resize.wall_resize = WallResizeIntent{"measured-arc", parse_quantity("5 m"),
        WallResizeAnchor::start, false};
    const auto resized = preview_constraint_authoring(document.snapshot(), resize);
    require_accepted(resized, "measured arc receipt fixture could not establish exact length entry");
    (void)apply_constraint_authoring(document, resized);
    auto annotated = document.snapshot().entities().at("measured-arc");
    auto& receipt = annotated.extensions["constraint_authoring"]["last_length_entry"];
    receipt["vendor"] = "retain";
    receipt["exact_metres"]["vendor"] = 23;
    receipt["baseline"]["vendor"] = true;
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(annotated)}, {}, "Annotate exact length receipt"});

    const auto before = document.snapshot();
    const auto old = baseline(before.entities().at("measured-arc"));
    ConstraintAuthoringIntent move;
    move.wall_geometry_move = WallGeometryMoveIntent{{
        {"measured-arc", {10,-3}, {10,-3 + old.end.x - old.start.x}}}, false};
    const auto preview = preview_constraint_authoring(before, move);
    require_accepted(preview, "rigid arc endpoint movement rejected");
    const auto moved = baseline(preview.candidate_entities().at("measured-arc"));
    require(moved.start.x == 10 && moved.start.y == -3 && moved.end.x == 10 &&
        moved.sweep_radians == old.sweep_radians,
        "arc movement changed its explicit endpoints or signed sweep");
    require_near(segment_length(moved), segment_length(old), 1e-7,
        "rigid arc movement changed physical arc length");
    const auto& candidate = preview.candidate_entities().at("measured-arc");
    require(candidate.extensions.at("curve_input_derivation").at("source_input") == original_input,
        "rigid arc movement rewrote the original curve construction provenance");
    const auto& moved_receipt = candidate.extensions.at("constraint_authoring").at("last_length_entry");
    require(moved_receipt.at("original_expression") == "5 m" &&
        moved_receipt.at("entered_unit") == "m" &&
        moved_receipt.at("exact_metres").at("numerator") == 5 &&
        moved_receipt.at("exact_metres").at("denominator") == 1 &&
        moved_receipt.at("vendor") == "retain" &&
        moved_receipt.at("exact_metres").at("vendor") == 23 &&
        moved_receipt.at("baseline").at("vendor") == true &&
        moved_receipt.at("baseline").at("start") == candidate.properties.at("baseline").at("start") &&
        moved_receipt.at("baseline").at("end") == candidate.properties.at("baseline").at("end") &&
        moved_receipt.at("baseline").at("sweep_radians") == candidate.properties.at("baseline").at("sweep_radians"),
        "rigid arc movement lost or failed to rebase the original exact length receipt");

    (void)apply_constraint_authoring(document, preview);
    const auto committed = document.snapshot();
    const auto& proof = *committed.history().back().boundary_constraint_changes;
    const auto edit = std::find_if(proof.wall_edits.begin(), proof.wall_edits.end(),
        [](const ConstraintWallGeometryEdit& value) { return value.wall_id == "measured-arc"; });
    require(edit != proof.wall_edits.end() && edit->version == 3 && edit->length_entry.has_value() &&
        edit->length_entry->original_expression == "5 m" &&
        edit->length_entry->exact_metres.numerator == 5 &&
        edit->length_entry->exact_metres.denominator == 1,
        "typed wall proof did not retain the existing exact length quantity");
    require(Document::fork(committed).snapshot().entities() == committed.entities(),
        "arc movement proof did not independently reconstruct its candidate");
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(),
        "arc movement undo did not restore its source curve and receipt");
    document.redo(document.revision());
    require(document.snapshot().entities() == committed.entities(),
        "arc movement redo did not restore its exact receipt and provenance");
}

void test_calculation_candidate_snapshot_replays_without_mutation() {
    auto document = Document::create({wall("candidate-wall", {0,0}, {3,0})},
        {Asset::create("candidate-asset", "application/octet-stream", {std::byte{42}})});
    document.mark_saved(document.revision());
    const auto before = document.snapshot();
    ConstraintAuthoringIntent intent;
    intent.wall_resize = WallResizeIntent{"candidate-wall", parse_quantity("14 ft"), WallResizeAnchor::start, false};
    const auto preview = preview_constraint_authoring(before, intent);
    require_accepted(preview, "calculation candidate needs a valid exact wall edit");
    const auto candidate = preview_constraint_authoring_snapshot(before, preview);
    require(candidate.entities() == preview.candidate_entities() && candidate.assets() == before.assets() &&
                candidate.revision() == before.revision()+1 &&
                candidate.history().back().boundary_constraint_changes.has_value(),
            "calculation candidate must contain the actual typed replay and unchanged assets");
    require(document.revision() == before.revision() && document.snapshot().entities() == before.entities() &&
                document.snapshot().history().size() == before.history().size() && !document.snapshot().dirty(),
            "deriving a calculation candidate must preserve live geometry, history and saved state");
    const auto refuses = [&](const DocumentSnapshot& source, const ConstraintAuthoringPreview& proposal) {
        bool rejected = false;
        try { (void)preview_constraint_authoring_snapshot(source, proposal); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "calculation candidate must reject stale, foreign or modified authority");
    };
    refuses(Document::create({wall("candidate-wall", {0,0}, {3,0})}).snapshot(), preview);
    auto modified = preview;
    auto& entities = const_cast<std::map<std::string,Entity,std::less<>>&>(modified.candidate_entities());
    entities.at("candidate-wall").properties["name"] = "tampered";
    refuses(before, modified);
    refuses(before, preview_constraint_authoring(before, ConstraintAuthoringIntent{}));
    (void)apply_constraint_authoring(document, preview);
    require(document.snapshot().entities() == candidate.entities() && document.snapshot().assets() == candidate.assets(),
            "committing an unchanged source must reproduce the calculation candidate exactly");
    refuses(document.snapshot(), preview);
}

Document source_measured_constraint_fixture() {
    std::vector<Entity> entities{
        {"property", "property", {{"calculation_workflow", "appraisal"},
            {"appraisal_policy", {{"policy_kind", "residential_declared"}, {"version", 1},
                {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"}}}}, false, json::object()},
        {"building", "building", {{"property_id", "property"}}, false, json::object()},
        {"floor", "floor", {{"building_id", "building"}, {"appraisal_facts", {{"grade", "above"}}}}, false, json::object()},
        {"layer", "layer", {{"floor_id", "floor"}}, false, json::object()},
        wall("bottom", {0,0}, {4,0}), wall("right", {4,0}, {4,3}),
        wall("top", {4,3}, {0,3}), wall("left", {0,3}, {0,0})};
    const std::vector<std::string> ids{"bottom", "right", "top", "left"};
    for (auto& entity : entities) {
        if (entity.type != "wall") continue;
        entity.properties["property_id"] = "property";
        entity.properties["building_id"] = "building";
        entity.properties["floor_id"] = "floor";
        entity.properties["layer_id"] = "layer";
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        entities.push_back(encode_constraint_entity(relation("join-" + std::to_string(i),
            ConstraintRelationKind::coincident,
            {endpoint(ids[i], WallEndpointRole::end), endpoint(ids[(i+1)%ids.size()], WallEndpointRole::start)})));
        entities.push_back(encode_constraint_entity(relation("direction-" + std::to_string(i),
            i%2 == 0 ? ConstraintRelationKind::horizontal : ConstraintRelationKind::vertical,
            {endpoint(ids[i], WallEndpointRole::start), endpoint(ids[i], WallEndpointRole::end)})));
    }
    auto anchor = relation("top-left-anchor", ConstraintRelationKind::fixed_anchor,
        {endpoint("left", WallEndpointRole::start)});
    anchor.anchor = Vec2{0,3};
    entities.push_back(encode_constraint_entity(anchor));
    auto walls = Document::create(entities);
    const auto measured = derive_exterior_wall_measurement(walls.snapshot(), ids);
    IdentifiedBoundary outline{"area", "measurement_boundary", {}};
    for (std::size_t i = 0; i < measured.boundary.size(); ++i)
        outline.segments.push_back({"edge-" + std::to_string(i), "corner-" + std::to_string(i),
            "corner-" + std::to_string((i+1)%measured.boundary.size()), measured.boundary[i]});
    auto owner = encode_identified_boundary_entity(outline);
    owner.properties["property_id"] = "property";
    owner.properties["building_id"] = "building";
    owner.properties["floor_id"] = "floor";
    owner.properties["layer_id"] = "layer";
    owner.properties["calculation_scope"] = "building";
    owner.properties["wall_measurement_source"] = measured.source;
    owner.properties["appraisal_facts"] = {{"finish", "finished"}, {"access", "direct_interior"},
        {"ceiling_eligibility", "standard"}, {"area_use", "dwelling"}, {"boundary_role", "measured_area"}};
    entities.push_back(owner);
    auto consumer = owner;
    consumer.id = "consumer";
    consumer.properties["calculation_scope"] = "site";
    entities.push_back(consumer);
    BoundaryDimension area_dimension;
    area_dimension.id = "area-dimension";
    area_dimension.boundary_id = owner.id;
    area_dimension.kind = BoundaryDimensionKind::area;
    area_dimension.text_position = {2,1};
    entities.push_back(encode_boundary_dimension_entity(area_dimension));
    BoundaryDimension length_dimension;
    length_dimension.id = "length-dimension";
    length_dimension.boundary_id = owner.id;
    const auto bottom_edge = std::find_if(outline.segments.begin(), outline.segments.end(),
        [](const auto& edge) { return edge.segment.start.y < 0 && edge.segment.end.y < 0; });
    require(bottom_edge != outline.segments.end(), "source fixture omitted the bottom exterior edge");
    length_dimension.segment_id = bottom_edge->segment_id;
    length_dimension.text_position = {2,-0.3};
    length_dimension.placement = BoundaryDimensionPlacement::automatic;
    length_dimension.automatic_placement_version = 2;
    entities.push_back(encode_boundary_dimension_entity(length_dimension));
    return Document::create(std::move(entities));
}

void test_source_measured_resize_seals_all_consumers_and_replays_one_event() {
    auto document = source_measured_constraint_fixture();
    document.mark_saved(document.revision());
    const auto before = document.snapshot();
    ConstraintAuthoringIntent resize;
    resize.wall_resize = WallResizeIntent{"bottom", parse_quantity("6 m"), WallResizeAnchor::start, true};
    const auto preview = preview_constraint_authoring(before, resize);
    require_accepted(preview, "numeric connected shell resize must complete its source-measured owners");
    const auto candidate = preview_constraint_authoring_snapshot(before, preview);
    require(candidate.entities() == preview.candidate_entities(), "sealed preview and typed candidate differ");
    require(document.snapshot().entities() == before.entities() && !document.snapshot().dirty() &&
        document.snapshot().history().size() == before.history().size(), "cancelling a sourced preview changed live state");
    require(preview.changed_boundaries().size() == 2,
        "source completion must expose every affected measured consumer");
    const auto& proof = *candidate.history().back().boundary_constraint_changes;
    require(proof.exterior_source_edits.size() == 2 && proof.boundary_edits.empty() &&
        proof.physical_entity_changes.empty() && proof.wall_edits.size() == 3,
        "source completion must retain typed wall edits and explicit separate source proofs");
    for (const auto* id : {"area", "consumer"}) {
        const auto& owner = candidate.entities().at(id);
        require(wall_measurement_source_current(candidate, owner), "resized shell consumer remains stale");
        const auto original = decode_identified_boundary_entity(before.entities().at(id));
        const auto changed = decode_identified_boundary_entity(owner);
        require_near(std::abs(signed_area(boundary_geometry(changed))), 6.14*3.14, 1e-7,
            "completed exterior area did not follow connected shell resize");
        for (std::size_t i = 0; i < original.segments.size(); ++i)
            require(original.segments[i].segment_id == changed.segments[i].segment_id &&
                original.segments[i].start_vertex_id == changed.segments[i].start_vertex_id &&
                original.segments[i].end_vertex_id == changed.segments[i].end_vertex_id,
                "automatic completion replaced stable measured edge identities");
    }
    const auto dimension = *decode_boundary_dimension_entity(candidate.entities().at("area-dimension")).dimension;
    require_near(dimension.resolve(candidate.entities().at("area")).area_square_metres, 6.14*3.14, 1e-7,
        "stable area dimension did not resolve the completed measured geometry");
    const auto length_dimension = *decode_boundary_dimension_entity(candidate.entities().at("length-dimension")).dimension;
    require_near(length_dimension.resolve(candidate.entities().at("area")).segment_length_metres, 6.14, 1e-7,
        "stable exterior dimension did not resolve the completed measured length");
    require_near(length_dimension.text_position.x, 3.0, 1e-7,
        "automatic exterior dimension was not repositioned before sealing");
    const auto report = build_appraisal_document_report(candidate, "property");
    require(report.qualified && report.calculation.has_value(), "completed candidate withheld appraisal totals");
    const auto status = std::find_if(report.boundaries.begin(), report.boundaries.end(),
        [](const auto& value) { return value.boundary_id == "area"; });
    require(status != report.boundaries.end() && status->measurement.has_value(), "completed candidate omitted area trace");
    require_near(status->measurement->display.unrounded, (6.14*3.14)/0.09290304, 1e-5,
        "numeric wall resize did not update appraisal square feet");
    (void)apply_constraint_authoring(document, preview);
    const auto committed = document.snapshot();
    require(committed.entities() == candidate.entities() && committed.history().size() == before.history().size()+1,
        "sourced shell Apply did not reproduce sealed candidate in one history event");
    require(Document::fork(committed).snapshot().entities() == committed.entities(), "sourced shell proof did not reconstruct");
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(), "one Undo did not restore physical and measured geometry");
    document.redo(document.revision());
    require(document.snapshot().entities() == committed.entities(), "one Redo did not restore completed geometry");
    require_rejected_unchanged(document, preview, "stale sourced preview applied again");
}

void test_source_measured_connected_corner_move_and_proof_tampering() {
    auto document = source_measured_constraint_fixture();
    const auto before = document.snapshot();
    ConstraintAuthoringIntent move;
    move.wall_geometry_move = WallGeometryMoveIntent{{{"right", {6,0}, {6,3}}}, true};
    const auto preview = preview_constraint_authoring(before, move);
    require_accepted(preview, "connected source wall endpoint move must complete measured corners");
    const auto candidate = preview_constraint_authoring_snapshot(before, preview);
    require_near(baseline(candidate.entities().at("bottom")).end.x, 6, 1e-7,
        "connected corner did not follow selected source wall");
    require_near(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(candidate.entities().at("area"))))),
        6.14*3.14, 1e-7, "connected corner move left the exterior consumer unchanged");
    require(preview.exterior_source_edits().size() == 2, "preview omitted explicit source completion proof");
    auto tampered = preview;
    auto& source_edits = const_cast<std::vector<BoundaryGeometryEdit>&>(tampered.exterior_source_edits());
    source_edits.clear();
    require_rejected_unchanged(document, tampered, "altered source completion display was applied");
    auto stripped = *candidate.history().back().boundary_constraint_changes;
    require(stripped.exterior_source_completion, "source completion lacks its retained replay discriminator");
    stripped.exterior_source_edits.clear();
    bool rejected = false;
    try { (void)Document::preview_command(before, Command{stripped}); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "stripping all source proofs downgraded a completed edit to legacy replay");
    (void)apply_constraint_authoring(document, preview);
    require(document.snapshot().entities() == candidate.entities(), "connected corner commit differs from sealed source preview");
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(), "connected corner Undo failed to restore measured consumers");
}

void test_source_measured_invalid_dependencies_reject_before_sealing() {
    const auto source = source_measured_constraint_fixture().snapshot();
    ConstraintAuthoringIntent resize;
    resize.wall_resize = WallResizeIntent{"bottom", parse_quantity("6 m"), WallResizeAnchor::start, true};
    const auto rejects = [&](std::map<std::string, Entity, std::less<>> entities, std::string_view message,
                             const ConstraintAuthoringIntent& intent) {
        std::vector<Entity> values;
        for (auto& [id, entity] : entities) { (void)id; values.push_back(std::move(entity)); }
        auto document = Document::create(std::move(values));
        const auto preview = preview_constraint_authoring(document.snapshot(), intent);
        require(!preview.accepted() && preview.exterior_source_edits().empty() &&
            preview.candidate_entities() == document.snapshot().entities(), message);
        require_rejected_unchanged(document, preview, message);
    };
    auto invalid = source.entities();
    const auto outline = decode_identified_boundary_entity(invalid.at("area"));
    const auto edge = std::find_if(outline.segments.begin(), outline.segments.end(),
        [](const auto& value) { return value.segment.start.y < 0 && value.segment.end.y < 0; });
    auto lock = relation("measured-edge-lock", ConstraintRelationKind::fixed_length,
        {{"area", WallEndpointRole::start, edge->segment_id, edge->start_vertex_id},
         {"area", WallEndpointRole::end, edge->segment_id, edge->end_vertex_id}});
    lock.length = parse_quantity("4.14 m");
    invalid.emplace(lock.id, encode_constraint_entity(lock));
    rejects(invalid, "source completion bypassed a measured edge lock", resize);
    invalid = source.entities();
    auto child = encode_identified_boundary_entity(IdentifiedBoundary{"deduction", "measurement_boundary", {
        {"ab","a","b",{{3,1},{3.5,1},0}}, {"bc","b","c",{{3.5,1},{3.5,2},0}},
        {"cd","c","d",{{3.5,2},{3,2},0}}, {"da","d","a",{{3,2},{3,1},0}}}});
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
        child.properties[key] = invalid.at("area").properties.at(key);
    child.properties["calculation_scope"] = "site";
    invalid.emplace(child.id, child);
    invalid.at("area").properties["deduction_ids"] = {child.id};
    auto shrink = resize;
    shrink.wall_resize->exact_length = parse_quantity("2 m");
    rejects(invalid, "source completion stranded a retained deduction", shrink);
    auto frozen = resize;
    frozen.wall_resize->move_connected_walls = false;
    rejects(source.entities(), "disabled connected movement detached the measured source cycle", frozen);
}

void test_source_measured_stale_owner_keeps_explicit_repair_workflow() {
    auto entities = source_measured_constraint_fixture().snapshot().entities();
    auto& stale = entities.at("area");
    for (auto& edge : stale.properties.at("segments"))
        for (const auto* endpoint : {"start", "end"})
            if (edge.at(endpoint).at(0).get<double>() > 4.0)
                edge.at(endpoint).at(0) = edge.at(endpoint).at(0).get<double>() + 0.2;
    std::vector<Entity> values;
    for (auto& [id, entity] : entities) { (void)id; values.push_back(std::move(entity)); }
    auto document = Document::create(std::move(values));
    const auto before = document.snapshot();
    require(!wall_measurement_source_current(before, before.entities().at("area")), "stale fixture must require explicit repair");
    ConstraintAuthoringIntent resize;
    resize.wall_resize = WallResizeIntent{"bottom", parse_quantity("6 m"), WallResizeAnchor::start, true};
    const auto preview = preview_constraint_authoring(before, resize);
    require_accepted(preview, "an already stale consumer must preserve the physical repair workflow");
    const auto candidate = preview_constraint_authoring_snapshot(before, preview);
    require(preview.exterior_source_edits().size() == 1 &&
        candidate.entities().at("area") == before.entities().at("area") &&
        !wall_measurement_source_current(candidate, candidate.entities().at("area")) &&
        wall_measurement_source_current(candidate, candidate.entities().at("consumer")),
        "physical edit must update current consumers while preserving already stale owners for explicit repair");
    const auto report = build_appraisal_document_report(candidate, "property");
    require(!report.qualified && !report.calculation.has_value(), "already stale appraisal owner exposed aggregate totals");
    (void)apply_constraint_authoring(document, preview);
    require(document.snapshot().entities() == candidate.entities(), "stale repair workflow commit differs from its sealed candidate");
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(), "stale repair workflow Undo did not restore the prior state");
}

Document exterior_corner_fixture(double bottom_sweep = 0, double right_sweep = 0,
    bool reverse_sources = false, bool partitions = false) {
    auto entities = source_measured_constraint_fixture().snapshot().entities();
    for (auto it = entities.begin(); it != entities.end();)
        if (it->second.type == "constraint") it = entities.erase(it); else ++it;
    entities.at("bottom").properties["baseline"]["sweep_radians"] = bottom_sweep;
    entities.at("right").properties["baseline"]["sweep_radians"] = right_sweep;
    entities.at("bottom").properties["thickness_m"] = 0.2;
    entities.at("right").properties["thickness_m"] = 0.3;
    entities.at("left").properties["thickness_m"] = 0.24;
    if (reverse_sources) for (const auto* id : {"bottom", "right", "top", "left"}) {
        auto old = baseline(entities.at(id));
        entities.at(id).properties["baseline"] = segment_json(old.end, old.start, -old.sweep_radians);
    }
    if (bottom_sweep != 0) {
        auto& source = entities.at("bottom");
        const auto b = baseline(source);
        const auto angle = angle_from_radians(b.sweep_radians);
        source.extensions["curve_input"] = {{"version", 2}, {"construction", "angle"},
            {"measure", angle.original_expression}, {"measure_value", b.sweep_radians},
            {"radians", b.sweep_radians}, {"clockwise", b.sweep_radians < 0},
            {"start", {b.start.x,b.start.y}}, {"end", {b.end.x,b.end.y}}, {"vendor", "exact-source"}};
    }
    const auto measured = derive_exterior_wall_measurement(entities, {"bottom","right","top","left"});
    for (const auto* id : {"area", "consumer"}) {
        auto owner = decode_identified_boundary_entity(entities.at(id));
        for (std::size_t i = 0; i < measured.boundary.size(); ++i) owner.segments[i].segment = measured.boundary[i];
        if (reverse_sources) {
            std::reverse(owner.segments.begin(), owner.segments.end());
            for (auto& edge : owner.segments) {
                std::swap(edge.start_vertex_id, edge.end_vertex_id);
                std::swap(edge.segment.start, edge.segment.end);
                edge.segment.sweep_radians = -edge.segment.sweep_radians;
            }
        }
        auto changed = encode_identified_boundary_entity(owner);
        entities.at(id).properties["segments"] = changed.properties.at("segments");
        entities.at(id).properties["wall_measurement_source"] = measured.source;
    }
    // Fixtures retain measured dimensions, while recomputing their initial
    // automatic placements is irrelevant to the corner transaction contract.
    entities.erase("length-dimension");
    if (partitions) {
        auto first = wall("partition-a", {2,0}, {2,1});
        auto second = wall("partition-b", {2,0.5}, {3,0.5});
        for (auto* entity : {&first, &second})
            for (const auto* key : {"property_id","building_id","floor_id","layer_id"})
                entity->properties[key] = entities.at("bottom").properties.at(key);
        // Non-identical base elevations still share a physical vertical span.
        first.properties["elevation_m"] = 0.2;
        second.properties["elevation_m"] = 0.4;
        entities.emplace(first.id, first); entities.emplace(second.id, second);
    }
    entities.emplace("hosted", opening("hosted", "bottom", 0.4, 0.5));
    std::vector<Entity> values;
    for (auto& [id, entity] : entities) { (void)id; values.push_back(std::move(entity)); }
    return Document::create(std::move(values));
}

ConstraintAuthoringIntent exterior_corner_intent(const DocumentSnapshot& source) {
    const auto owner = decode_identified_boundary_entity(source.entities().at("area"));
    const auto edge = std::max_element(owner.segments.begin(), owner.segments.end(), [](const auto& a, const auto& b) {
        return a.segment.start.x - a.segment.start.y < b.segment.start.x - b.segment.start.y;
    });
    BoundaryGeometryEdit edit;
    edit.boundary_id = "area"; edit.target_id = edge->start_vertex_id;
    edit.target_position = {edge->segment.start.x + 0.35, edge->segment.start.y - 0.12};
    ConstraintAuthoringIntent intent;
    intent.boundary_vertex_move = BoundaryVertexMoveIntent{edit, true};
    return intent;
}

void test_exterior_corner_inverse_curve_lineage_and_replay() {
    for (const auto sweeps : {std::pair{0.0,0.0}, std::pair{0.6,0.0}, std::pair{-0.6,0.0}, std::pair{0.6,0.4}, std::pair{4.0,0.0}, std::pair{0.001,0.0}})
        for (const bool reversed : {false,true}) {
        auto document = exterior_corner_fixture(sweeps.first, sweeps.second, reversed);
        const auto before = document.snapshot();
        const auto intent = exterior_corner_intent(before);
        const auto preview = preview_constraint_authoring(before, intent);
        require_accepted(preview, "exterior analytical corner inverse rejected valid line/arc shell");
        const auto candidate = preview_constraint_authoring_snapshot(before, preview);
        const auto original = decode_identified_boundary_entity(before.entities().at("area"));
        const auto changed = decode_identified_boundary_entity(candidate.entities().at("area"));
        for (std::size_t i = 0; i < original.segments.size(); ++i) {
            const auto expected = original.segments[i].start_vertex_id == intent.boundary_vertex_move->edit.target_id
                ? intent.boundary_vertex_move->edit.target_position : original.segments[i].segment.start;
            require_near(changed.segments[i].segment.start.x, expected.x, 1e-7, "inverse changed a requested exterior X coordinate");
            require_near(changed.segments[i].segment.start.y, expected.y, 1e-7, "inverse changed a requested exterior Y coordinate");
            require(changed.segments[i].segment_id == original.segments[i].segment_id, "inverse lost stable exterior edge identity");
        }
        for (const auto* id : {"area","consumer"})
            require(wall_measurement_source_current(candidate, candidate.entities().at(id)), "inverse left another source consumer stale");
        require(candidate.entities().at("hosted") == before.entities().at("hosted"), "corner edit changed physical opening station metadata");
        for (const auto* id : {"bottom","right","top","left"})
            require(candidate.entities().at(id).properties.at("thickness_m") == before.entities().at(id).properties.at("thickness_m"),
                "inverse lost unequal physical wall thickness");
        const auto proof = *candidate.history().back().boundary_constraint_changes;
        auto encoded = command_to_json(Command{proof});
        require(encoded.at("version") == 8 && proof.exterior_corner_move.has_value(), "corner edit lacks its dedicated persisted intent");
        require(command_to_json(command_from_json(encoded)) == encoded, "corner proof codec does not preserve exact bytes");
        require(ProjectStore::required_format_version(candidate) == 22, "corner proof did not raise native format floor");
        auto tampered = encoded;
        tampered["exterior_corner_move"]["position"][0] = intent.boundary_vertex_move->edit.target_position.x + 0.01;
        bool refused = false;
        try { (void)Document::preview_command(before, command_from_json(tampered)); } catch (const std::exception&) { refused = true; }
        require(refused, "corner proof accepted a target that differs from its retained redraws");
        (void)apply_constraint_authoring(document, preview);
        require(Document::fork(document.snapshot()).snapshot().entities() == candidate.entities(), "corner history does not exactly replay");
        document.undo(document.revision());
        require(document.snapshot().entities() == before.entities(), "corner Undo did not restore source geometry and provenance");
        document.redo(document.revision());
        require(document.snapshot().entities() == candidate.entities(), "corner Redo did not restore complete derived state");
        if (sweeps.first == 0.6 && sweeps.second == 0.4 && reversed) {
            const auto path = std::filesystem::temp_directory_path() / ("exterior-corner-" + make_stable_id() + ".sketchproj");
            (void)ProjectStore::save(path, document.snapshot());
            auto reopened = ProjectStore::load(path);
            require(reopened.document.snapshot().entities() == candidate.entities(), "native corner reopen differs from the sealed preview");
            reopened.document.undo(reopened.document.revision());
            require(reopened.document.snapshot().entities() == before.entities(), "native corner reopen lost exact Undo state");
            reopened.document.redo(reopened.document.revision());
            require(reopened.document.snapshot().entities() == candidate.entities(), "native corner reopen lost exact Redo state");
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        if (sweeps.first != 0) {
            const auto& archive = candidate.entities().at("bottom").extensions.at("curve_input_derivation");
            require(archive.at("source_input") == before.entities().at("bottom").extensions.at("curve_input"), "corner curve reconstruction lost exact original input");
            validate_wall_curve_input(candidate.entities().at("bottom"));
        }
    }
}

void test_exterior_corner_partitions_constraints_and_refusals() {
    auto document = exterior_corner_fixture(0,0,false,true);
    const auto before = document.snapshot();
    bool branch_source_refused = false;
    try { (void)derive_exterior_wall_measurement(before,{"bottom","right","top","left","partition-a"}); }
    catch (const std::invalid_argument&) { branch_source_refused = true; }
    require(branch_source_refused,"source derivation unexpectedly admitted an interior branch as perimeter lineage");
    auto intent = exterior_corner_intent(before);
    const auto preview = preview_constraint_authoring(before, intent);
    require_accepted(preview, "exterior corner with partition T chain rejected");
    const auto candidate = preview_constraint_authoring_snapshot(before, preview);
    const auto first = baseline(candidate.entities().at("partition-a"));
    const auto host = baseline(candidate.entities().at("bottom"));
    require_near(first.start.x, (host.start.x + host.end.x)/2, 1e-7, "partition T station X did not follow source host");
    require_near(first.start.y, (host.start.y + host.end.y)/2, 1e-7, "partition T station Y did not follow source host");
    require(first.end.x == 2 && first.end.y == 1, "partition unattached endpoint moved");
    const auto second = baseline(candidate.entities().at("partition-b"));
    require_near(second.start.x, (first.start.x + first.end.x)/2, 1e-7, "partition chain lost its original T station");
    require_near(second.start.y, (first.start.y + first.end.y)/2, 1e-7, "partition chain lost its original T station Y");
    auto frozen = intent;
    frozen.boundary_vertex_move->move_related_objects = false;
    require(!preview_constraint_authoring(before, frozen).accepted(), "frozen partitions silently detached");
    auto stale = before.entities();
    for (auto& edge : stale.at("area").properties.at("segments"))
        for (const auto* endpoint : {"start","end"}) edge[endpoint][0] = edge.at(endpoint).at(0).get<double>() + 0.2;
    std::vector<Entity> values;
    for (auto& [id, entity] : stale) { (void)id; values.push_back(std::move(entity)); }
    auto stale_document = Document::create(std::move(values));
    require(!preview_constraint_authoring(stale_document.snapshot(), intent).accepted(), "stale source accepted an exterior inverse");
    // A relation from the derived corner to an independent wall must solve
    // against the final exterior coordinates rather than an intermediate shell.
    auto related = exterior_corner_fixture().snapshot().entities();
    const auto relation_intent = exterior_corner_intent(exterior_corner_fixture().snapshot());
    const auto outline = decode_identified_boundary_entity(related.at("area"));
    const auto edge = std::find_if(outline.segments.begin(),outline.segments.end(), [&](const auto& value) {
        return value.start_vertex_id == relation_intent.boundary_vertex_move->edit.target_id;
    });
    auto remote = wall("related", edge->segment.start, {edge->segment.start.x + 1, edge->segment.start.y - 1});
    related.emplace(remote.id, remote);
    auto join = relation("measured-join", ConstraintRelationKind::coincident,
        {{"area", WallEndpointRole::start,edge->segment_id,edge->start_vertex_id}, endpoint("related",WallEndpointRole::start)});
    related.emplace(join.id, encode_constraint_entity(join));
    values.clear();
    for (auto& [id, entity] : related) { (void)id; values.push_back(std::move(entity)); }
    auto related_document = Document::create(std::move(values));
    const auto related_preview = preview_constraint_authoring(related_document.snapshot(), relation_intent);
    require_accepted(related_preview, "measured corner relation rejected before complete derived solve");
    const auto related_candidate = preview_constraint_authoring_snapshot(related_document.snapshot(), related_preview);
    require_near(baseline(related_candidate.entities().at("related")).start.x, relation_intent.boundary_vertex_move->edit.target_position.x,
        1e-7, "related wall did not follow final derived measured corner");
    auto short_host = exterior_corner_fixture().snapshot().entities();
    short_host.at("hosted").properties["offset_m"] = 3.7;
    short_host.at("hosted").properties["width_m"] = 0.25;
    values.clear();
    for (auto& [id, entity] : short_host) { (void)id; values.push_back(std::move(entity)); }
    auto host_document = Document::create(std::move(values));
    auto shrink = exterior_corner_intent(host_document.snapshot());
    shrink.boundary_vertex_move->edit.target_position.x -= 1.35;
    const auto no_fit = preview_constraint_authoring(host_document.snapshot(), shrink);
    require(!no_fit.accepted() && no_fit.candidate_entities() == host_document.snapshot().entities(),
        "exterior corner shortened a source past its hosted opening fit");
    auto curved = exterior_corner_fixture(0.6).snapshot().entities();
    auto lock = relation("physical-curve-lock",ConstraintRelationKind::fixed_arc_length,
        {endpoint("bottom",WallEndpointRole::start), endpoint("bottom",WallEndpointRole::end)});
    lock.length = parse_quantity(std::to_string(segment_length(baseline(curved.at("bottom")))) + " m");
    curved.emplace(lock.id, encode_constraint_entity(lock));
    values.clear();
    for (auto& [id, entity] : curved) { (void)id; values.push_back(std::move(entity)); }
    auto curve_document = Document::create(std::move(values));
    const auto locked = preview_constraint_authoring(curve_document.snapshot(), exterior_corner_intent(curve_document.snapshot()));
    require(!locked.accepted() && locked.candidate_entities() == curve_document.snapshot().entities(),
        "exterior corner bypassed a persisted physical curve-length lock");
}

void test_exterior_corner_cross_layer_locked_partition_chain() {
    auto entities = exterior_corner_fixture(0,0,false,true).snapshot().entities();
    entities.emplace("partition-layer", Entity{"partition-layer","layer",{{"floor_id","floor"}}});
    entities.at("partition-a").properties["layer_id"] = "partition-layer";
    entities.at("partition-b").properties["layer_id"] = "partition-layer";
    auto joint_branch = wall("partition-joint",{2,1},{3,1});
    for (const auto* key : {"property_id","building_id","floor_id","layer_id"})
        joint_branch.properties[key] = entities.at("partition-a").properties.at(key);
    joint_branch.properties["elevation_m"] = 0.3;
    entities.emplace(joint_branch.id,joint_branch);
    auto lock = relation("partition-length",ConstraintRelationKind::fixed_length,
        {endpoint("partition-a",WallEndpointRole::start), endpoint("partition-a",WallEndpointRole::end)});
    lock.length = parse_quantity("1 m");
    entities.emplace(lock.id,encode_constraint_entity(lock));
    entities.emplace("partition-opening",opening("partition-opening","partition-a",0.7,0.25));
    entities.emplace("other-floor",Entity{"other-floor","floor",{{"building_id","building"}}});
    entities.emplace("other-layer",Entity{"other-layer","layer",{{"floor_id","other-floor"}}});
    auto unrelated = entities.at("partition-a");
    unrelated.id = "unrelated-floor-wall";
    unrelated.properties["floor_id"] = "other-floor";
    unrelated.properties["layer_id"] = "other-layer";
    entities.emplace(unrelated.id,unrelated);
    auto inactive = entities.at("partition-a");
    inactive.id = "inactive-alternative-partition";
    auto demolished = inactive; demolished.id = "demolished-partition";
    entities.emplace(inactive.id,inactive); entities.emplace(demolished.id,demolished);
    const std::vector<std::string> baseline_ids{"bottom","right","top","left","partition-a","partition-b","partition-joint",demolished.id};
    auto registry_ids = baseline_ids; registry_ids.push_back(inactive.id);
    const auto phases = ModelPhases::create(registry_ids,baseline_ids,
        {{"active","Active",{demolished.id},{}},{"inactive","Inactive",{}, {inactive.id}}},"active");
    entities.emplace("phase-model",Entity{"phase-model","model_phases",{{"model",phases.to_json()}}});
    std::vector<Entity> values;
    for (const auto& [id,entity] : entities) { (void)id; values.push_back(entity); }
    auto document = Document::create(values);
    const auto before = document.snapshot();
    auto intent = exterior_corner_intent(before);
    intent.boundary_vertex_move->edit.target_position.y += 0.52; // Provisional host is too short for its opening.
    const auto preview = preview_constraint_authoring(before,intent);
    require_accepted(preview,"cross-layer attached partition with solvable length lock rejected");
    const auto candidate = preview_constraint_authoring_snapshot(before,preview);
    const auto a = baseline(candidate.entities().at("partition-a"));
    const auto b = baseline(candidate.entities().at("partition-b"));
    const auto joint = baseline(candidate.entities().at("partition-joint"));
    const auto host = baseline(candidate.entities().at("bottom"));
    require_near(a.start.x,(host.start.x+host.end.x)/2,1e-7,"cross-layer source T station detached");
    require_near(a.start.y,(host.start.y+host.end.y)/2,1e-7,"cross-layer source T station Y detached");
    require_near(segment_length(a),1,1e-6,"partition free endpoint did not solve its retained length lock");
    require_near(b.start.x,(a.start.x+a.end.x)/2,1e-7,"partition chain did not follow final solved host");
    require_near(b.start.y,(a.start.y+a.end.y)/2,1e-7,"partition chain did not follow final solved host Y");
    require_near(joint.start.x,a.end.x,1e-7,"reciprocal partition joint did not follow the length-locked free endpoint");
    require_near(joint.start.y,a.end.y,1e-7,"reciprocal partition joint did not follow the length-locked free endpoint Y");
    require(candidate.entities().at(unrelated.id) == before.entities().at(unrelated.id),"another-floor coincidence moved with source shell");
    require(candidate.entities().at(inactive.id) == before.entities().at(inactive.id) &&
        candidate.entities().at(demolished.id) == before.entities().at(demolished.id),
        "inactive alternative or demolished coincident wall moved with current physical contacts");
    require(candidate.entities().at("partition-opening") == before.entities().at("partition-opening"),"partition opening offset metadata changed");
    validate_constraint_wall_host("partition-a",candidate.entities());
    auto smuggled = *candidate.history().back().boundary_constraint_changes;
    auto hidden_geometry = baseline(before.entities().at(inactive.id));
    hidden_geometry.start.x += 0.1;
    smuggled.wall_edits.push_back({inactive.id,hidden_geometry,std::nullopt,1});
    bool refused_hidden_edit = false;
    try { (void)Document::preview_command(before,Command{smuggled}); }
    catch (const std::exception&) { refused_hidden_edit = true; }
    require(refused_hidden_edit,"qualified exterior topology authority admitted an inactive alternative wall edit");
    (void)apply_constraint_authoring(document,preview);
    require(Document::fork(document.snapshot()).snapshot().entities() == candidate.entities(),"locked attachment typed proof differs on history replay");
    const auto path = std::filesystem::temp_directory_path() / ("locked-partition-joint-" + make_stable_id() + ".bldproj");
    (void)ProjectStore::save(path,document.snapshot());
    auto reopened = ProjectStore::load(path);
    require(reopened.document.snapshot().entities() == candidate.entities(),"locked reciprocal partition joint did not reopen exactly");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == before.entities(),"locked reciprocal partition joint reopen lost Undo");
    std::filesystem::remove(path);
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(),"locked attachment Undo lost original geometry");
    auto anchor = relation("partition-free-end-lock",ConstraintRelationKind::fixed_anchor,
        {endpoint("partition-a",WallEndpointRole::end)});
    anchor.anchor = Vec2{2,1};
    entities.emplace(anchor.id,encode_constraint_entity(anchor));
    values.clear();
    for (const auto& [id,entity] : entities) { (void)id; values.push_back(entity); }
    auto conflict = Document::create(values);
    const auto rejected = preview_constraint_authoring(conflict.snapshot(),intent);
    require(!rejected.accepted() && rejected.candidate_entities() == conflict.snapshot().entities(),"conflicting attachment locks did not refuse atomically");
}

void test_exterior_corner_t_station_persistent_controls() {
    for (const int control : {0,1,2,3,4,5,6}) {
        auto entities = exterior_corner_fixture(0,0,false,true).snapshot().entities();
        const double sweep = control == 3 ? 0.6 : control == 4 ? -0.6 : control == 5 ? -0.5 : control == 6 ? -2.0 : 0;
        const Vec2 station{2+0.5*std::tan(sweep/4),0.5};
        entities.at("partition-a").properties["baseline"]["sweep_radians"] = sweep;
        entities.at("partition-b").properties["baseline"] = segment_json(station,{station.x+1,station.y},0);
        if (control == 0 || control >= 3) {
            auto anchor = relation("T-anchor",ConstraintRelationKind::fixed_anchor,
                {endpoint("partition-b",WallEndpointRole::start)});
            anchor.anchor = station;
            entities.emplace(anchor.id,encode_constraint_entity(anchor));
        } else {
            auto anchor = relation("branch-far-anchor",ConstraintRelationKind::fixed_anchor,
                {endpoint("partition-b",WallEndpointRole::end)});
            anchor.anchor = Vec2{station.x+1,station.y};
            entities.emplace(anchor.id,encode_constraint_entity(anchor));
            auto control_relation = relation("branch-control",control == 1 ? ConstraintRelationKind::fixed_length : ConstraintRelationKind::horizontal,
                {endpoint("partition-b",WallEndpointRole::start),endpoint("partition-b",WallEndpointRole::end)});
            if (control == 1) control_relation.length = parse_quantity("1 m");
            entities.emplace(control_relation.id,encode_constraint_entity(control_relation));
        }
        std::vector<Entity> values;
        for (const auto& [id,entity] : entities) { (void)id; values.push_back(entity); }
        auto document = Document::create(values);
        const auto before = document.snapshot();
        const auto preview = preview_constraint_authoring(before,exterior_corner_intent(before));
        if (!preview.accepted()) std::cerr << "T station fixture control=" << control << " sweep=" << sweep << '\n';
        require_accepted(preview,"persistent T control overpinned a solvable physical host");
        const auto candidate = preview_constraint_authoring_snapshot(before,preview);
        const auto host = baseline(candidate.entities().at("partition-a"));
        const auto branch = baseline(candidate.entities().at("partition-b"));
        const double b = -0.5*std::tan(sweep/4);
        require_near(branch.start.x,(host.start.x+host.end.x)/2-b*(host.end.y-host.start.y),1e-7,"saved T fraction X did not follow final solved host");
        require_near(branch.start.y,(host.start.y+host.end.y)/2+b*(host.end.x-host.start.x),1e-7,"saved T fraction Y did not follow final solved host");
        if (control == 0 || control >= 3) {
            require_near(branch.start.x,station.x,1e-7,"saved T point anchor X moved");
            require_near(branch.start.y,station.y,1e-7,"saved T point anchor Y moved");
            require(candidate.entities().at("partition-b") == before.entities().at("partition-b"),
                "unchanged solved attachment retained a provisional contact redraw");
        } else if (control == 1) require_near(segment_length(branch),1,1e-6,"saved branch length did not control T point");
        else require_near(branch.start.y,branch.end.y,1e-6,"saved branch direction did not control T point");
        validate_exterior_corner_physical_contacts(before.entities(),candidate.entities());
        (void)apply_constraint_authoring(document,preview);
        require(document.snapshot().entities() == candidate.entities(),"T control Apply differs from preview");
        require(Document::fork(document.snapshot()).snapshot().entities() == candidate.entities(),"T control typed proof differs on replay");
        const auto path = std::filesystem::temp_directory_path() / ("T-control-" + make_stable_id() + ".bldproj");
        (void)ProjectStore::save(path,document.snapshot());
        auto reopened = ProjectStore::load(path);
        require(reopened.document.snapshot().entities() == candidate.entities(),"T control native reopen differs from preview");
        reopened.document.undo(reopened.document.revision());
        require(reopened.document.snapshot().entities() == before.entities(),"T control native Undo lost original geometry");
        std::filesystem::remove(path);
    }
}

void test_exterior_corner_preserves_future_receipt_metadata() {
    for (const bool future_section : {true,false}) {
        auto source = wall("opaque-length",{0,0},{4,0});
        source.extensions["constraint_authoring"] = future_section
            ? json{{"version",2},{"last_length_entry",{{"vendor","opaque"}}}}
            : json{{"version",1},{"last_length_entry",{{"version",99},{"vendor","opaque"}}}};
        const auto before = source;
        bool refused = false;
        try { (void)reconstruct_exterior_corner_wall(source,{{0,1},{4,1},0}); }
        catch (const std::exception&) { refused = true; }
        require(refused && source == before,"length-preserving exterior deformation rewrote future receipt metadata");
    }
}

void test_exterior_corner_retained_legacy_tangent_source() {
    auto entities = exterior_corner_fixture().snapshot().entities();
    const auto pi = std::acos(-1.0);
    entities.at("bottom").properties["baseline"] = segment_json({0,0},{5,0});
    entities.at("right").properties["baseline"] = segment_json({5,0},{5,3},pi);
    entities.at("top").properties["baseline"] = segment_json({5,3},{0,3});
    entities.at("left").properties["baseline"] = segment_json({0,3},{0,0},pi);
    for (const auto* id : {"bottom","right","top","left"}) entities.at(id).properties["thickness_m"] = 0.4;
    entities.at("top").properties["thickness_m"] = 0.400000002;
    bool stable_refused = false;
    try { (void)derive_exterior_wall_measurement(entities,{"bottom","right","top","left"}); }
    catch (const std::invalid_argument&) { stable_refused = true; }
    require(stable_refused,"legacy tangent fixture must exercise stable derivation refusal");
    const auto retained = derive_legacy_exterior_wall_measurement(entities,{"bottom","right","top","left"});
    for (const auto* id : {"area","consumer"}) {
        auto owner = decode_identified_boundary_entity(entities.at(id));
        for (std::size_t i = 0; i < owner.segments.size(); ++i) owner.segments[i].segment = retained.boundary[i];
        entities.at(id).properties["segments"] = encode_identified_boundary_entity(owner).properties.at("segments");
        entities.at(id).properties["wall_measurement_source"] = retained.source;
    }
    std::vector<Entity> values;
    for (const auto& [id,entity] : entities) { (void)id; values.push_back(entity); }
    auto document = Document::create(values);
    const auto before = document.snapshot();
    require(wall_measurement_source_current(before,before.entities().at("area")),"retained legacy tangent owner lost current-source compatibility");
    const auto boundary = decode_identified_boundary_entity(before.entities().at("area"));
    const auto edge = std::max_element(boundary.segments.begin(),boundary.segments.end(),[](const auto& a,const auto& b) {
        return a.segment.start.x + a.segment.start.y < b.segment.start.x + b.segment.start.y;
    });
    ConstraintAuthoringIntent intent;
    intent.exterior_corner_move = ExteriorCornerMoveIntent{"area",edge->start_vertex_id,
        {edge->segment.start.x+0.3,edge->segment.start.y+0.1},true};
    const auto preview = preview_constraint_authoring(before,intent);
    require_accepted(preview,"current retained legacy tangent outline did not enter supported exterior inverse lane");
    const auto candidate = preview_constraint_authoring_snapshot(before,preview);
    require(wall_measurement_source_current(candidate,candidate.entities().at("area")),"legacy tangent inverse retained a stale analytical outline");
}

void test_exterior_corner_scaled_level_bound_contact_graph() {
    auto entities = exterior_corner_fixture(0,0,false,true).snapshot().entities();
    const VerticalLevelGraph levels({{"ground",0},{"upper",3}},{{"storey","ground","upper"}});
    entities.emplace("levels",Entity{"levels","vertical_levels",{{"model",json::parse(levels.serialize())}}});
    entities.emplace("upper-floor",Entity{"upper-floor","floor",{{"building_id","building"},
        {"vertical_level_binding",{{"version",1},{"graph_id","levels"},{"level_id","upper"}}}}});
    entities.emplace("upper-layer",Entity{"upper-layer","layer",{{"floor_id","upper-floor"}}});
    for (unsigned i = 0; i < 360; ++i) {
        auto entity = wall("unrelated-"+std::to_string(i),{100+2.0*i,10},{101+2.0*i,10});
        entity.properties["property_id"] = "property"; entity.properties["building_id"] = "building";
        entity.properties["floor_id"] = "upper-floor"; entity.properties["layer_id"] = "upper-layer";
        entity.properties["vertical_placement"] = {{"version",1},{"mode","level"},{"offset_m",0.0}};
        const auto id = entity.id;
        entities.emplace(id,std::move(entity));
    }
    std::vector<Entity> values;
    for (const auto& [id,entity] : entities) { (void)id; values.push_back(entity); }
    auto document = Document::create(values);
    const auto before = document.snapshot();
    const auto started = std::chrono::steady_clock::now();
    const auto preview = preview_constraint_authoring(before,exterior_corner_intent(before));
    const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    std::cout << "Exterior corner preview with 366 walls (360 level-bound unrelated): " << elapsed << " ms\n";
    require_accepted(preview,"scaled physical contact graph rejected a supported partition chain");
    for (unsigned i = 0; i < 360; ++i) {
        const auto id = "unrelated-"+std::to_string(i);
        require(preview.candidate_entities().at(id) == before.entities().at(id),"scaled contact discovery altered an unrelated level-bound wall");
    }
    require(wall_measurement_source_current(preview.candidate_entities(),preview.candidate_entities().at("area")),
        "scaled contact candidate left its source owner stale");
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_exterior_corner_inverse_curve_lineage_and_replay();
        test_exterior_corner_partitions_constraints_and_refusals();
        test_exterior_corner_cross_layer_locked_partition_chain();
        test_exterior_corner_t_station_persistent_controls();
        test_exterior_corner_preserves_future_receipt_metadata();
        test_exterior_corner_retained_legacy_tangent_source();
        test_exterior_corner_scaled_level_bound_contact_graph();
        test_source_measured_resize_seals_all_consumers_and_replays_one_event();
        test_source_measured_connected_corner_move_and_proof_tampering();
        test_source_measured_invalid_dependencies_reject_before_sealing();
        test_source_measured_stale_owner_keeps_explicit_repair_workflow();
        test_calculation_candidate_snapshot_replays_without_mutation();
        test_wall_geometry_move_propagates_explicit_connections_only();
        test_wall_geometry_move_multiselection_validation_and_hosting();
        test_wall_geometry_move_preserves_arc_provenance_and_exact_length_receipt();
        test_straight_wall_only_authoring_retains_guarded_typed_intent();
        test_physical_arc_length_authoring_and_connected_editing();
        test_physical_arc_length_conditioning_at_near_full_turn_and_translated_origin();
        test_direct_curved_wall_physical_resize();
        test_measured_curve_rigid_transform_provenance();
        test_curved_endpoint_relations_and_typed_propagation();
        test_boundary_vertex_move_propagates_explicit_relations();
        test_boundary_resize_canonical_shape_and_related_owners();
        test_mixed_boundary_wall_authoring_and_resize();
        test_persisted_component_analysis_excludes_edit_pins();
        test_boundary_horizontal_authoring();
        test_boundary_cross_relations_and_fixed_length();
        test_boundary_receipt_and_dimension_preview();
        test_resize_twelve_to_fourteen_feet_with_either_anchor_and_exact_receipt();
        test_nested_metadata_and_receipt_validation();
        test_rigid_transform_rebases_length_receipt_without_losing_metadata();
        test_connected_resize_moves_only_an_explicit_component();
        test_disabled_connected_movement_freezes_other_walls_and_rejects_conflict();
        test_solver_conflicts_use_semantic_wall_diagnostics();
        test_overlap_and_endpoint_reversal_are_rejected_after_solve();
        test_topology_is_scale_translation_and_drawing_plane_aware();
        test_explicit_wall_cycle_preserves_winding_branch();
        test_all_seven_relations_add_edit_remove_and_relation_solves_move_geometry();
        test_stale_foreign_same_revision_head_and_mutated_preview_are_rejected();
        test_history_reopen_and_host_failures();
        test_noop_and_cancel_leave_revision_saved_state_and_history_unchanged();
        std::cout << "Constraint authoring tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint_authoring_tests: unexpected exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "constraint_authoring_tests: unexpected non-standard exception\n";
        return 1;
    }
}
