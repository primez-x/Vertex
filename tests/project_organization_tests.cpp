#include "sketch/project_organization.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
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
        make_entity("loose", "beam"),
        make_entity("future", "future_optional_object", {{"layer_id", "walls"}, {"payload", 42}}),
    });
    const auto snapshot = document.snapshot();
    const auto organization = sketch::organize_project(snapshot);
    require(organization.nodes.size() == snapshot.entities().size(), "every entity must remain visible");
    require(organization.nodes.at("upper").parent_id == "house", "floor belongs to actual building");
    require(organization.nodes.at("column").parent_id == "fixtures", "column belongs to actual layer");
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
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_real_hierarchy_and_host_membership();
        test_inconsistent_membership_stays_visible();
        test_unknown_optional_references_are_safe_and_diagnostic();
        test_redundant_property_and_building_links_must_agree();
        test_malformed_unknown_and_opening_links_stay_diagnostic();
        test_unresolved_container_descendants_are_not_promoted();
        test_roots_and_children_have_stable_id_order();
        std::cout << "Project organization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
