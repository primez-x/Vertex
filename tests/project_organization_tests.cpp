#include "sketch/project_organization.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/terrain_surface.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using sketch::Entity;
using nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Entity make_entity(std::string id, std::string type, json properties = json::object()) {
    return {std::move(id), std::move(type), std::move(properties), false, json::object()};
}

void test_real_hierarchy_and_host_membership() {
    const auto document = sketch::Document::create({
        make_entity("site", "property", {{"name", "Mixed use property"}}),
        make_entity("house", "building", {{"property_id", "site"}, {"name", "House"}}),
        make_entity("shop", "building", {{"property_id", "site"}, {"name", "Shop"}}),
        make_entity("upper", "floor", {{"building_id", "house"}, {"elevation_m", 3.2}}),
        make_entity("ground", "floor", {{"building_id", "shop"}}),
        make_entity("walls", "layer", {{"floor_id", "upper"}}),
        make_entity("fixtures", "layer", {{"floor_id", "ground"}}),
        make_entity("wall", "wall", {{"layer_id", "walls"}, {"floor_id", "upper"}, {"elevation_m", 3.5}}),
        make_entity("door", "opening", {{"wall_id", "wall"}}),
        make_entity("column", "column", {{"layer_id", "fixtures"}, {"base_center_m", {1.0, 2.0, 0.3}}}),
        make_entity("railing", "railing", {{"layer_id", "fixtures"}}),
        make_entity("loose", "beam"),
        make_entity("future", "future_optional_object", {{"layer_id", "walls"}, {"payload", 42}}),
    });
    const auto snapshot = document.snapshot();
    const auto organization = sketch::organize_project(snapshot);
    require(organization.nodes.size() == snapshot.entities().size(), "every entity must remain visible");
    require(organization.nodes.at("upper").parent_id == "house", "floor belongs to actual building");
    require(organization.nodes.at("column").parent_id == "fixtures", "column belongs to actual layer");
    require(organization.nodes.at("railing").parent_id == "fixtures", "railing belongs to actual layer");
    require(organization.nodes.at("door").parent_id == "wall", "opening appears under its host");
    require(organization.drawing_context("door") == organization.drawing_context("wall"),
            "hosted opening inherits the host context");
    const auto door_context = organization.drawing_context("door");
    require(door_context && *door_context == sketch::DrawingContext{"site", "house", "upper", "walls"},
            "host context resolves through every hierarchy level");
    require(organization.nodes.at("loose").parent_id.empty() && !organization.drawing_context("loose"),
            "unassigned objects do not acquire a fabricated floor");
    require(organization.nodes.at("future").parent_id == "walls", "optional unknown objects remain navigable");
    require(document.snapshot().entities() == snapshot.entities(), "organization does not mutate geometry or metadata");
    require(snapshot.entities().at("wall").properties.at("elevation_m") == 3.5,
            "floor metadata must not offset absolute wall coordinates");
}

void test_terrain_surface_placement_is_explicit() {
    const auto model = sketch::TerrainSurface(
        "organization fixture",
        {sketch::TerrainPoint{"p0", 0.0, 0.0, 0.0},
         sketch::TerrainPoint{"p1", 2.0, 0.0, 1.0},
         sketch::TerrainPoint{"p2", 0.0, 2.0, 2.0}},
        {sketch::TerrainTriangle{{0, 1, 2}}})
                             .to_json();
    const auto document = sketch::Document::create({
        make_entity("site", "property"),
        make_entity("building", "building", {{"property_id", "site"}}),
        make_entity("floor", "floor", {{"building_id", "building"}}),
        make_entity("layer", "layer", {{"floor_id", "floor"}}),
        make_entity("terrain", "terrain_surface",
                    {{"property_id", "site"}, {"building_id", "building"},
                     {"floor_id", "floor"}, {"layer_id", "layer"}, {"model", model}}),
    });
    const auto organization = sketch::organize_project(document.snapshot());
    require(organization.nodes.at("terrain").parent_id == "layer",
            "terrain surfaces should appear under their drawing layer");
    require(organization.drawing_context("terrain") ==
                sketch::DrawingContext{"site", "building", "floor", "layer"},
            "terrain surfaces should resolve the ordinary drawing context");

    const auto property_only = sketch::Document::create({
        make_entity("site", "property"),
        make_entity("terrain", "terrain_surface",
                    {{"property_id", "site"}, {"model", model}}),
    });
    const auto property_organization = sketch::organize_project(property_only.snapshot());
    require(property_organization.nodes.at("terrain").parent_id == "site" &&
                property_organization.nodes.at("terrain").issues.empty(),
            "property-level terrain surfaces should remain navigable without a fabricated layer");
}

void test_inconsistent_membership_stays_visible() {
    const auto document = sketch::Document::create({
        make_entity("site", "property"),
        make_entity("building", "building", {{"property_id", "site"}}),
        make_entity("floor-a", "floor", {{"building_id", "building"}}),
        make_entity("floor-b", "floor", {{"building_id", "building"}}),
        make_entity("layer-a", "layer", {{"floor_id", "floor-a"}}),
        make_entity("bad", "column", {{"floor_id", "floor-b"}, {"layer_id", "layer-a"}}),
        make_entity("wall", "wall", {{"layer_id", "layer-a"}}),
        make_entity("bad-door", "opening", {{"wall_id", "wall"}, {"floor_id", "floor-b"}}),
    });
    const auto organization = sketch::organize_project(document.snapshot());
    for (const auto* id : {"bad", "bad-door"}) {
        const auto& node = organization.nodes.at(id);
        require(!node.issues.empty() && node.parent_id.empty() && !organization.drawing_context(id),
                "contradictory membership must be explicit and unavailable as a drawing context");
    }
    require(!organization.drawing_context("missing"), "missing context must not resolve");
}

void test_unknown_optional_references_are_safe_and_diagnostic() {
    const auto document = sketch::Document::create({
        make_entity("site", "property"),
        make_entity("building", "building", {{"property_id", "site"}}),
        make_entity("floor", "floor", {{"building_id", "building"}}),
        make_entity("layer", "layer", {{"floor_id", "floor"}}),
        make_entity("future-good", "future_optional_object", {{"layer_id", "layer"}}),
        make_entity("future-missing", "future_optional_object",
                    {{"layer_id", "missing-layer"}}),
        make_entity("future-malformed", "future_optional_object", {{"layer_id", 42}}),
    });
    const auto organization = sketch::organize_project(document.snapshot());
    require(organization.nodes.at("future-good").parent_id == "layer" &&
                organization.drawing_context("future-good").has_value(),
            "valid organization links on unknown optional entities should resolve");
    for (const auto* id : {"future-missing", "future-malformed"}) {
        const auto& node = organization.nodes.at(id);
        require(node.parent_id.empty() && !node.issues.empty() &&
                    !organization.drawing_context(id),
                "malformed unknown organization links must stay at the diagnostic root");
    }
}

void test_redundant_property_and_building_links_must_agree() {
    const auto document = sketch::Document::create({
        make_entity("site-a", "property"),
        make_entity("site-b", "property"),
        make_entity("building-a", "building", {{"property_id", "site-a"}}),
        make_entity("building-b", "building", {{"property_id", "site-b"}}),
        make_entity("floor-a", "floor", {{"building_id", "building-a"},
                                             {"property_id", "site-b"}}),
        make_entity("floor-b", "floor", {{"building_id", "building-a"}}),
        make_entity("layer-a", "layer", {{"floor_id", "floor-b"},
                                           {"building_id", "building-b"},
                                           {"property_id", "site-b"}}),
        make_entity("object", "column", {{"layer_id", "layer-a"},
                                           {"building_id", "building-a"},
                                           {"property_id", "site-a"}}),
    });
    const auto organization = sketch::organize_project(document.snapshot());
    for (const auto* id : {"floor-a", "layer-a", "object"}) {
        const auto& node = organization.nodes.at(id);
        require(node.parent_id.empty() && !node.issues.empty() &&
                    !organization.drawing_context(id),
                "redundant hierarchy links must not silently select one parent");
    }
}

void test_malformed_unknown_and_opening_links_stay_diagnostic() {
    const auto document = sketch::Document::create({
        make_entity("site", "property"),
        make_entity("building", "building", {{"property_id", "site"}}),
        make_entity("floor", "floor", {{"building_id", "building"}}),
        make_entity("alternate-floor", "floor", {{"building_id", "building"}}),
        make_entity("layer", "layer", {{"floor_id", "floor"}}),
        make_entity("unplaced-wall", "wall"),
        make_entity("future-wrong-target", "future_optional_object", {{"layer_id", "floor"}}),
        make_entity("future-contradictory", "future_optional_object",
                    {{"layer_id", "layer"}, {"floor_id", "alternate-floor"}}),
        make_entity("unhosted-opening", "opening"),
        make_entity("unresolved-opening", "opening", {{"wall_id", "unplaced-wall"}}),
    });
    const auto snapshot = document.snapshot();
    const auto organization = sketch::organize_project(snapshot);
    require(organization.nodes.size() == snapshot.entities().size(),
            "malformed relationships must not hide any entity");
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)entity;
        require(organization.nodes.contains(id), "every malformed fixture must remain indexed");
    }
    for (const auto* id : {"future-wrong-target", "future-contradictory", "unhosted-opening",
                           "unresolved-opening"}) {
        const auto& node = organization.nodes.at(id);
        require(node.parent_id.empty() && !node.issues.empty() &&
                    !organization.drawing_context(id),
                "malformed optional or host links must remain diagnostic roots");
    }
    require(!organization.nodes.at("unplaced-wall").issues.empty() &&
                !organization.drawing_context("unplaced-wall"),
            "an opening host without organization must not provide a fabricated context");
}

void test_unresolved_container_descendants_are_not_promoted() {
    const auto document = sketch::Document::create({
        make_entity("orphan-building", "building"),
        make_entity("orphan-floor", "floor", {{"building_id", "orphan-building"}}),
        make_entity("orphan-layer", "layer", {{"floor_id", "orphan-floor"}}),
        make_entity("deep-column", "column", {{"layer_id", "orphan-layer"}}),
    });
    const auto organization = sketch::organize_project(document.snapshot());
    for (const auto* id : {"orphan-building", "orphan-floor", "orphan-layer", "deep-column"}) {
        const auto& node = organization.nodes.at(id);
        require(node.parent_id.empty() && !node.issues.empty() &&
                    !organization.drawing_context(id),
                "descendants of an unresolved container must remain unresolved at the root");
    }
}

void test_roots_and_children_have_stable_id_order() {
    const auto document = sketch::Document::create({
        make_entity("building-z", "building", {{"property_id", "property-z"}}),
        make_entity("layer-z", "layer", {{"floor_id", "floor-z"}}),
        make_entity("property-z", "property"),
        make_entity("object-z", "future_object", {{"layer_id", "layer-z"}}),
        make_entity("floor-z", "floor", {{"building_id", "building-z"}}),
        make_entity("building-a", "building", {{"property_id", "property-a"}}),
        make_entity("layer-a", "layer", {{"floor_id", "floor-a"}}),
        make_entity("property-a", "property"),
        make_entity("object-a", "future_object", {{"layer_id", "layer-a"}}),
        make_entity("floor-a", "floor", { {"building_id", "building-a"} }),
    });
    const auto first = sketch::organize_project(document.snapshot());
    const auto second = sketch::organize_project(document.snapshot());
    require(first.roots == second.roots, "organization roots must be deterministic");
    require(std::is_sorted(first.roots.begin(), first.roots.end()),
            "organization roots must use stable ID order");
    for (const auto& [id, node] : first.nodes) {
        (void)id;
        require(std::is_sorted(node.children.begin(), node.children.end()),
                "organization children must use stable ID order");
    }
    require(first.nodes.at("property-a").children ==
                std::vector<std::string>{"building-a"},
            "stable hierarchy should retain the first building under its property");
    require(first.nodes.at("layer-z").children == std::vector<std::string>{"object-z"},
            "stable hierarchy should retain the object under its layer");
}

void test_floor_level_binding_is_exposed_in_context() {
    const auto graph = sketch::VerticalLevelGraph({{"ground", 0}, {"upper", 3}},
                                                  {{"storey", "ground", "upper"}});
    const auto document = sketch::Document::create({
        make_entity("site", "property"),
        make_entity("building", "building", {{"property_id", "site"}}),
        make_entity("floor", "floor", {{"building_id", "building"},
                                         {"vertical_level_binding", {
                                             {"version", 1}, {"graph_id", "levels"},
                                             {"level_id", "upper"}}}}),
        make_entity("layer", "layer", {{"floor_id", "floor"}}),
        make_entity("levels", "vertical_levels",
                    {{"model", nlohmann::json::parse(graph.serialize())}}),
        make_entity("wall", "wall", {{"layer_id", "layer"}}),
    });
    const auto organization = sketch::organize_project(document.snapshot());
    const auto context = organization.drawing_context("wall");
    require(context && context->level_id == "upper",
            "resolved drawing context should expose the floor's bound level");
    require(*context == sketch::DrawingContext{"site", "building", "floor", "layer", "upper"},
            "level binding should not change the existing hierarchy context");
}

void test_level_placement_resolves_without_mutating_source() {
    const auto graph = sketch::VerticalLevelGraph({{"ground", 1.25}, {"upper", 4.75}},
                                                  {{"storey", "ground", "upper"}});
    const auto column = make_entity("column", "column", {
        {"layer_id", "layer"},
        {"base_center_m", {2.0, 3.0, 0.15}},
        {"vertical_placement", {{"version", 1}, {"mode", "level"}, {"offset_m", 0.4}}},
    });
    const auto document = sketch::Document::create({
        make_entity("site", "property"),
        make_entity("building", "building", {{"property_id", "site"}}),
        make_entity("floor", "floor", {{"building_id", "building"},
            {"vertical_level_binding", {{"version", 1}, {"graph_id", "levels"},
                                           {"level_id", "upper"}}}}),
        make_entity("layer", "layer", {{"floor_id", "floor"}}),
        make_entity("levels", "vertical_levels",
                    {{"model", nlohmann::json::parse(graph.serialize())}}),
        column,
    });
    const auto snapshot = document.snapshot();
    const auto& source = snapshot.entities().at("column");
    const auto resolved = sketch::resolve_vertical_placement(snapshot, source);
    require(std::abs(resolved.properties.at("base_center_m").at(2).get<double>() - 5.3) < 1e-9,
            "level placement must add the bound level elevation and local offset to Z");
    require(source.properties.at("base_center_m").at(2) == 0.15,
            "derived level placement must not mutate source geometry");

    auto moved_graph = graph.with_elevation("upper", 6.0);
    auto moved = snapshot.entities().at("levels");
    moved.properties["model"] = nlohmann::json::parse(moved_graph.serialize());
    std::vector<Entity> copy_entities;
    copy_entities.reserve(snapshot.entities().size());
    for (const auto& [id, value] : snapshot.entities()) {
        (void)id;
        copy_entities.push_back(value);
    }
    sketch::Document copy = sketch::Document::create(std::move(copy_entities));
    copy.apply(sketch::ApplyEntityChanges{copy.revision(),
        {sketch::EntityChange::upsert(std::move(moved))}, {}, "move upper level"});
    const auto moved_snapshot = copy.snapshot();
    const auto moved_column = sketch::resolve_vertical_placement(
        moved_snapshot, moved_snapshot.entities().at("column"));
    require(std::abs(moved_column.properties.at("base_center_m").at(2).get<double>() - 6.55) < 1e-9,
            "level edits must change the derived placement on the next projection");
}

void test_level_placement_rejects_invalid_or_unbound_requests() {
    const auto unbound = make_entity("column", "column", {
        {"base_center_m", {0.0, 0.0, 0.0}},
        {"vertical_placement", {{"version", 1}, {"mode", "level"}, {"offset_m", 0.0}}},
    });
    const auto document = sketch::Document::create({unbound});
    const auto unbound_snapshot = document.snapshot();
    bool rejected = false;
    try {
        (void)sketch::resolve_vertical_placement(
            unbound_snapshot, unbound_snapshot.entities().at("column"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "level placement without a bound floor must fail closed");

    auto malformed = make_entity("column", "column", {
        {"base_center_m", {0.0, 0.0, 0.0}},
        {"vertical_placement", {{"version", 1}, {"mode", "unknown"}, {"offset_m", 0.0}}},
    });
    const auto malformed_document = sketch::Document::create({malformed});
    const auto malformed_snapshot = malformed_document.snapshot();
    rejected = false;
    try {
        (void)sketch::resolve_vertical_placement(
            malformed_snapshot, malformed_snapshot.entities().at("column"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "unknown vertical placement modes must fail closed");
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_real_hierarchy_and_host_membership();
        test_terrain_surface_placement_is_explicit();
        test_inconsistent_membership_stays_visible();
        test_unknown_optional_references_are_safe_and_diagnostic();
        test_redundant_property_and_building_links_must_agree();
        test_malformed_unknown_and_opening_links_stay_diagnostic();
        test_unresolved_container_descendants_are_not_promoted();
        test_roots_and_children_have_stable_id_order();
        test_floor_level_binding_is_exposed_in_context();
        test_level_placement_resolves_without_mutating_source();
        test_level_placement_rejects_invalid_or_unbound_requests();
        std::cout << "Project organization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
