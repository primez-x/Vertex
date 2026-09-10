#include "sketch/constraint_authoring.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
        auto rejected_document = Document::create({malformed});
        const auto rejected = preview_constraint_authoring(rejected_document.snapshot(), intent);
        require(!rejected.accepted(), "malformed or inconsistent receipt was silently overwritten");
        require_rejected_unchanged(rejected_document, rejected, "invalid receipt Apply was accepted");
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

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_resize_twelve_to_fourteen_feet_with_either_anchor_and_exact_receipt();
        test_nested_metadata_and_receipt_validation();
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
