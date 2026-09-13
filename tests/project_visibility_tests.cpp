#include "sketch/project_visibility.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sketch::Entity;
using sketch::ProjectViewFilter;
using VisibleIds = std::set<std::string, std::less<>>;
using nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Entity entity(std::string id, std::string type, json properties = json::object()) {
    return Entity{std::move(id), std::move(type), std::move(properties), false,
                  json::object()};
}

void require_ids(const VisibleIds& actual,
                 std::initializer_list<std::string_view> expected,
                 std::string_view message) {
    VisibleIds expected_ids;
    for (const auto id : expected) {
        expected_ids.emplace(id);
    }
    require(actual == expected_ids, message);
}

void test_default_visibility_and_hierarchy_masks() {
    auto document = sketch::Document::create({
        entity("site-a", "property"),
        entity("site-b", "property"),
        entity("building-a", "building", {{"property_id", "site-a"}}),
        entity("building-b", "building", {{"property_id", "site-b"}}),
        entity("floor-a", "floor", {{"building_id", "building-a"}}),
        entity("floor-b", "floor", {{"building_id", "building-b"}}),
        entity("layer-a", "layer", {{"floor_id", "floor-a"}}),
        entity("layer-b", "layer", {{"floor_id", "floor-b"}}),
        entity("layer-b-sibling", "layer", {{"floor_id", "floor-b"}}),
        entity("wall-a", "wall", {{"layer_id", "layer-a"}}),
        entity("wall-b", "wall", {{"layer_id", "layer-b"}}),
        entity("door-a", "opening", {{"wall_id", "wall-a"}}),
        entity("column-b", "column", {{"layer_id", "layer-b"}}),
        entity("future-b-sibling", "future_optional_object",
               {{"layer_id", "layer-b-sibling"}}),
        entity("future-a", "future_optional_object", {{"layer_id", "layer-a"}}),
        entity("loose", "beam"),
    });
    const auto snapshot = document.snapshot();

    const auto all = sketch::visible_project_entities(snapshot, {});
    require(all.size() == snapshot.entities().size(),
            "an empty view filter must retain every entity ID");
    for (const auto& [id, unused] : snapshot.entities()) {
        (void)unused;
        require(all.contains(id), "every snapshot entity must be visible by default");
    }

    ProjectViewFilter floor_filter;
    floor_filter.hidden_floor_ids.emplace("floor-a");
    require_ids(sketch::visible_project_entities(snapshot, floor_filter),
                {"site-a", "site-b", "building-a", "building-b", "floor-b", "layer-b",
                 "layer-b-sibling", "wall-b", "column-b", "future-b-sibling", "loose"},
                "a hidden floor must mask the floor, its layers, and all contents");

    ProjectViewFilter layer_filter;
    layer_filter.hidden_layer_ids.emplace("layer-a");
    require_ids(sketch::visible_project_entities(snapshot, layer_filter),
                {"site-a", "site-b", "building-a", "building-b", "floor-a", "floor-b",
                 "layer-b", "layer-b-sibling", "wall-b", "column-b", "future-b-sibling",
                 "loose"},
                "a hidden layer must mask its contents while retaining its floor");

    ProjectViewFilter combined_filter;
    combined_filter.hidden_floor_ids.emplace("floor-a");
    combined_filter.hidden_layer_ids.emplace("layer-b");
    require_ids(sketch::visible_project_entities(snapshot, combined_filter),
                {"site-a", "site-b", "building-a", "building-b", "floor-b", "layer-b-sibling",
                 "future-b-sibling", "loose"},
                "floor and layer masks must combine independently across containers");
}

void test_host_and_unresolved_contexts_remain_visible() {
    auto document = sketch::Document::create({
        entity("site", "property"),
        entity("building", "building", {{"property_id", "site"}}),
        entity("floor", "floor", {{"building_id", "building"}}),
        entity("alternate-floor", "floor", {{"building_id", "building"}}),
        entity("layer", "layer", {{"floor_id", "floor"}}),
        entity("wall", "wall", {{"layer_id", "layer"}}),
        entity("hosted-door", "opening", {{"wall_id", "wall"}}),
        entity("future-good", "future_optional_object", {{"layer_id", "layer"}}),
        entity("missing-layer", "future_optional_object", {{"layer_id", "does-not-exist"}}),
        entity("malformed-layer", "future_optional_object", {{"layer_id", 42}}),
        entity("contradictory", "column",
               {{"layer_id", "layer"}, {"floor_id", "alternate-floor"}}),
        entity("unassigned", "beam"),
    });
    const auto snapshot = document.snapshot();

    ProjectViewFilter filter;
    filter.hidden_floor_ids.emplace("floor");
    filter.hidden_layer_ids.emplace("layer");
    const auto visible = sketch::visible_project_entities(snapshot, filter);
    require(!visible.contains("wall") && !visible.contains("hosted-door") &&
                !visible.contains("future-good"),
            "resolved host and unknown entities must inherit a hidden wall context");
    for (const auto* id : {"site", "building", "alternate-floor", "missing-layer",
                           "malformed-layer", "contradictory", "unassigned"}) {
        require(visible.contains(id),
                "unresolved, contradictory, or unassigned entities must remain visible");
    }
}

void test_stale_filters_and_snapshot_determinism() {
    auto document = sketch::Document::create({
        entity("site", "property"),
        entity("building", "building", {{"property_id", "site"}}),
        entity("floor", "floor", {{"building_id", "building"}}),
        entity("layer", "layer", {{"floor_id", "floor"}}),
        entity("wall", "wall", {{"layer_id", "layer"}}),
        entity("loose", "beam"),
    });
    const auto before = document.snapshot();

    ProjectViewFilter stale;
    stale.hidden_floor_ids = {"wall", "missing-floor", "building"};
    stale.hidden_layer_ids = {"floor", "missing-layer", "building"};
    const auto first = sketch::visible_project_entities(before, stale);
    const auto second = sketch::visible_project_entities(before, stale);
    require(first == second, "visibility results must be deterministic for one snapshot and filter");
    require(first.size() == before.entities().size(),
            "stale or type-mismatched filter IDs must not hide any entities");
    require(first.contains("wall") && first.contains("floor") && first.contains("loose"),
            "a floor filter ID naming a wall and a layer filter ID naming a floor are ignored");

    const auto after = document.snapshot();
    require(after.document_id() == before.document_id() && after.revision() == before.revision() &&
                after.saved_revision_optional() == before.saved_revision_optional() &&
                after.entities() == before.entities() && after.assets() == before.assets() &&
                after.history().size() == before.history().size() &&
                after.named_revisions() == before.named_revisions(),
            "visibility derivation must not mutate the original snapshot or document history");
}

void test_join_presentation_visibility_transitions() {
    for (const std::string kind : {"wall", "roof"}) {
        auto document = sketch::Document::create({
            entity("site", "property"),
            entity("building", "building", {{"property_id", "site"}}),
            entity("floor", "floor", {{"building_id", "building"}}),
            entity("layer-a", "layer", {{"floor_id", "floor"}}),
            entity("layer-b", "layer", {{"floor_id", "floor"}}),
            entity("member-a", kind, {{"layer_id", "layer-a"}}),
            entity("member-b", kind, {{"layer_id", "layer-b"}}),
            entity("join", kind + "_join", {{"version", 1}, {"style", "fused"},
                   {kind + "_ids", {"member-a", "member-b"}}}),
        });
        const auto before = document.snapshot();
        const auto all = sketch::visible_project_entities(before, {});
        const auto fused = sketch::derived_join_presentation_entities(before, all);
        require(fused.contains("join") && !fused.contains("member-a") &&
                    !fused.contains("member-b"),
                "all visible join members must be replaced by one fused presentation");

        ProjectViewFilter filter;
        filter.hidden_layer_ids.insert("layer-b");
        const auto one_hidden = sketch::visible_project_entities(before, filter);
        const auto individual = sketch::derived_join_presentation_entities(before, one_hidden);
        require(individual.contains("member-a") && !individual.contains("member-b") &&
                    !individual.contains("join"),
                "a hidden member must disable fusion and preserve the visible member");

        auto join_hidden = all;
        join_hidden.erase("join");
        const auto sources = sketch::derived_join_presentation_entities(before, join_hidden);
        require(sources.contains("member-a") && sources.contains("member-b") &&
                    !sources.contains("join"),
                "a hidden join must not suppress its visible source members");

        filter.hidden_layer_ids.insert("layer-a");
        const auto hidden_members = sketch::derived_join_presentation_entities(
            before, sketch::visible_project_entities(before, filter));
        require(!hidden_members.contains("join") && !hidden_members.contains("member-a") &&
                    !hidden_members.contains("member-b"),
                "all hidden members must not leak through a visible join");
        require(sketch::derived_join_presentation_entities(before, {}).empty(),
                "an empty visibility mask must display no join or sources");
        require(sketch::derived_join_presentation_entities(before, all) == fused &&
                    sketch::derived_join_presentation_entities(before, one_hidden) == individual,
                "mask transitions must deterministically restore fused and individual presentation");

        const auto after = document.snapshot();
        require(after.document_id() == before.document_id() &&
                    after.revision() == before.revision() &&
                    after.saved_revision_optional() == before.saved_revision_optional() &&
                    after.entities() == before.entities() && after.assets() == before.assets() &&
                    after.history().size() == before.history().size() &&
                    after.named_revisions() == before.named_revisions(),
                "join presentation must leave the source snapshot and document history unchanged");
    }
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_default_visibility_and_hierarchy_masks();
        test_host_and_unresolved_contexts_remain_visible();
        test_stale_filters_and_snapshot_determinism();
        test_join_presentation_visibility_transitions();
        std::cout << "Project visibility tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
