#include "sketch/architecture.hpp"
#include "sketch/document.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

sketch::Wall wall() {
    return {"wall-assembly", {{0.0, 0.0}, {5.0, 0.0}, 0.0}, 0.20, 3.0, 0.0, {}};
}

sketch::HostedOpening opening() {
    return {"opening-assembly", 1.0, 1.2, 0.25, 2.1};
}
}

int main() {
    try {
        using namespace sketch;
        const auto door = default_opening_assembly(OpeningAssemblyKind::door);
        const auto window = default_opening_assembly(OpeningAssemblyKind::window);
        require(parse_opening_assembly_kind("door") == OpeningAssemblyKind::door,
                "door assembly kind codec failed");
        require(parse_opening_assembly_kind("window") == OpeningAssemblyKind::window,
                "window assembly kind codec failed");
        require(!parse_opening_assembly_kind("curtain-wall").has_value(),
                "unknown assembly kind was accepted");
        require(parse_opening_assembly(opening_assembly_json(door)) == door,
                "door assembly JSON round trip failed");
        require(parse_opening_assembly(opening_assembly_json(window)) == window,
                "window assembly JSON round trip failed");

        const auto host = wall();
        const auto hosted = opening();
        const auto door_shape = make_opening_assembly(host, hosted, door,
                                                      DoorOperation{false, true, 90.0});
        const auto window_shape = make_opening_assembly(host, hosted, window);
        require(!door_shape.IsNull() && !window_shape.IsNull(),
                "opening assemblies must produce non-null OCCT compounds");
        require(std::isfinite(solid_volume(door_shape)) && solid_volume(door_shape) > 0.0,
                "door assembly volume must be positive");
        require(std::isfinite(solid_volume(window_shape)) && solid_volume(window_shape) > 0.0,
                "window assembly volume must be positive");

        auto too_deep = door;
        too_deep.frame_depth_m = 0.30;
        rejects([&] { (void)make_opening_assembly(host, hosted, too_deep); },
                "assembly deeper than host wall was accepted");
        auto too_narrow = door;
        too_narrow.frame_width_m = 0.61;
        rejects([&] { (void)make_opening_assembly(host, hosted, too_narrow); },
                "assembly with no clear width was accepted");

        auto wall_entity = Entity::create("wall", {
            {"baseline", {{"start", {0.0, 0.0}}, {"end", {5.0, 0.0}},
                           {"sweep_radians", 0.0}}},
            {"thickness_m", 0.20}, {"height_m", 3.0}, {"elevation_m", 0.0}});
        wall_entity.id = host.id;
        auto opening_entity = Entity::create("opening", {
            {"wall_id", host.id}, {"opening_kind", "door"},
            {"offset_m", hosted.offset}, {"width_m", hosted.width},
            {"sill_m", hosted.sill}, {"height_m", hosted.height},
            {"opening_assembly", opening_assembly_json(door)}});
        opening_entity.id = hosted.id;
        const auto document = Document::create({wall_entity, opening_entity});
        require(document.snapshot().entities().at(hosted.id).properties
                    .at("opening_assembly").at("kind") == "door",
                "document did not retain opening assembly profile");

        auto mismatched = opening_entity;
        mismatched.properties["opening_kind"] = "window";
        rejects([&] { (void)Document::create({wall_entity, mismatched}); },
                "opening kind/profile mismatch was accepted");
        auto malformed = opening_entity;
        malformed.properties["opening_assembly"]["version"] = 2;
        rejects([&] { (void)Document::create({wall_entity, malformed}); },
                "unsupported opening assembly version was accepted");

        std::cout << "Opening assembly tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
