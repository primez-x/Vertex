#include "sketch/constraint_authoring.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
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

int main() {
    sketch::testing::noninteractive_errors();
    try {
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
