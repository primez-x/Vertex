#include "sketch/architecture.hpp"
#include "sketch/document.hpp"
#include "sketch/wall_semantics.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using sketch::Entity;
using sketch::Wall;
using sketch::WallJoin;
using sketch::WallJoinStyle;
using Json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

Wall wall(std::string id, double x0, double y0, double x1, double y1) {
    return {std::move(id), {{x0, y0}, {x1, y1}, 0.0}, 0.2, 3.0, 0.0, {}, {}, std::nullopt};
}

Entity wall_entity(const Wall& value) {
    return {value.id, "wall",
            {{"baseline", {{"start", {value.baseline.start.x, value.baseline.start.y}},
                            {"end", {value.baseline.end.x, value.baseline.end.y}},
                            {"sweep_radians", value.baseline.sweep_radians}}},
             {"thickness_m", value.thickness},
             {"height_m", value.height},
             {"elevation_m", value.elevation}},
            false, Json::object()};
}

Json join_properties(std::vector<std::string> ids) {
    return {{"version", 1}, {"style", "fused"}, {"wall_ids", std::move(ids)}};
}

void test_join_codec_is_versioned_and_lossless() {
    const auto encoded = join_properties({"wall-a", "wall-b"});
    const auto decoded = sketch::parse_wall_join(encoded, "join-1");
    require(decoded.id == "join-1" && decoded.style == WallJoinStyle::fused &&
                decoded.wall_ids == std::vector<std::string>{"wall-a", "wall-b"},
            "wall join codec changed the canonical identity or ordering");
    require(sketch::wall_join_json(decoded) == encoded,
            "wall join JSON did not round-trip canonically");

    auto malformed = encoded;
    malformed["wall_ids"] = Json::array({"wall-a", "wall-a"});
    rejected([&] { (void)sketch::parse_wall_join(malformed, "join-1"); },
             "duplicate wall IDs must be rejected");
    malformed = encoded;
    malformed["version"] = 2;
    rejected([&] { (void)sketch::parse_wall_join(malformed, "join-1"); },
             "unsupported wall join versions must be rejected");
    malformed = encoded;
    malformed["style"] = "miter";
    rejected([&] { (void)sketch::parse_wall_join(malformed, "join-1"); },
             "unsupported wall join styles must be rejected");
}

void test_fused_join_requires_connected_walls_and_returns_real_solid() {
    const auto first = wall("wall-a", 0.0, 0.0, 4.0, 0.0);
    const auto second = wall("wall-b", 4.0, 0.0, 4.0, 3.0);
    const WallJoin join{"join-1", {"wall-a", "wall-b"}, WallJoinStyle::fused};
    const auto shape = sketch::make_wall_join(join, std::vector<Wall>{first, second});
    const auto volume = sketch::solid_volume(shape);
    const auto source_volume = sketch::solid_volume(sketch::make_wall(first)) +
                               sketch::solid_volume(sketch::make_wall(second));
    require(!shape.IsNull() && std::isfinite(volume) && volume > 0.0,
            "fused wall join did not produce a valid solid");
    require(volume <= source_volume + 1e-8,
            "fused wall join volume exceeded its source wall volumes");

    auto disconnected = second;
    disconnected.id = "wall-c";
    disconnected.baseline.start = {20.0, 0.0};
    disconnected.baseline.end = {20.0, 3.0};
    const WallJoin invalid{"join-2", {"wall-a", "wall-c"}, WallJoinStyle::fused};
    rejected([&] { (void)sketch::make_wall_join(invalid, std::vector<Wall>{first, disconnected}); },
             "disconnected walls must not be accepted as a join");
}

void test_join_rejects_two_disconnected_pairs() {
    const std::vector<Wall> walls{
        wall("wall-a", 0.0, 0.0, 4.0, 0.0),
        wall("wall-b", 4.0, 0.0, 4.0, 3.0),
        wall("wall-c", 20.0, 0.0, 24.0, 0.0),
        wall("wall-d", 24.0, 0.0, 24.0, 3.0)};
    const WallJoin join{"join-pairs", {"wall-a", "wall-b", "wall-c", "wall-d"},
                        WallJoinStyle::fused};
    rejected([&] { (void)sketch::make_wall_join(join, walls); },
             "two disconnected wall pairs must not be accepted as one join");
}

void test_join_accepts_chain_in_nonadjacent_order() {
    const std::vector<Wall> walls{
        wall("wall-a", 0.0, 0.0, 4.0, 0.0),
        wall("wall-b", 4.0, 0.0, 4.0, 3.0),
        wall("wall-c", 4.0, 3.0, 8.0, 3.0)};
    const WallJoin join{"join-chain", {"wall-a", "wall-c", "wall-b"}, WallJoinStyle::fused};
    const auto shape = sketch::make_wall_join(join, walls);
    const auto volume = sketch::solid_volume(shape);
    require(!shape.IsNull() && std::isfinite(volume) && volume > 0.0,
            "transitively connected wall chain must produce a solid regardless of member order");
}

void test_sloped_join_requires_contact_at_the_shared_endpoint() {
    auto sloped = wall("wall-sloped", 0.0, 0.0, 4.0, 0.0);
    sloped.height = 1.0;
    sloped.slope_rise = 9.0;
    auto separated = wall("wall-separated", 0.0, 0.0, 0.0, 4.0);
    separated.height = 1.0;
    separated.elevation = 5.0;
    rejected([&] {
        (void)sketch::make_wall_join(
            WallJoin{"join-separated-slope", {sloped.id, separated.id}, WallJoinStyle::fused},
            std::vector<Wall>{sloped, separated});
    }, "sloped walls separated at their shared endpoint must not join");

    auto touching = separated;
    touching.id = "wall-touching";
    touching.baseline = {{4.0, 0.0}, {4.0, 4.0}, 0.0};
    const auto result = sketch::make_wall_join(
        WallJoin{"join-touching-slope", {sloped.id, touching.id}, WallJoinStyle::fused},
        std::vector<Wall>{sloped, touching});
    require(!result.IsNull() && sketch::solid_volume(result) > 0.0,
            "sloped walls that physically meet at their shared endpoint must join");
}

void test_document_validates_join_references_and_persists_record() {
    const auto first = wall("wall-a", 0.0, 0.0, 4.0, 0.0);
    const auto second = wall("wall-b", 4.0, 0.0, 4.0, 3.0);
    const Entity join{"join-1", "wall_join", join_properties({"wall-a", "wall-b"}), false,
                      Json::object()};
    auto document = sketch::Document::create({wall_entity(first), wall_entity(second), join});
    require(document.snapshot().entities().at("join-1") == join,
            "document changed the wall join record during admission");

    auto missing = join;
    missing.properties["wall_ids"] = Json::array({"wall-a", "missing-wall"});
    rejected([&] { (void)sketch::Document::create({wall_entity(first), wall_entity(second), missing}); },
             "document accepted a wall join with a dangling wall reference");

    auto wrong_type = join;
    wrong_type.properties["wall_ids"] = Json::array({"wall-a", "property-1"});
    Entity property{"property-1", "property", Json::object(), false, Json::object()};
    rejected([&] { (void)sketch::Document::create({wall_entity(first), property, wrong_type}); },
             "document accepted a wall join whose target is not a wall");
}

}  // namespace

int main() {
    try {
        test_join_codec_is_versioned_and_lossless();
        test_fused_join_requires_connected_walls_and_returns_real_solid();
        test_join_accepts_chain_in_nonadjacent_order();
        test_join_rejects_two_disconnected_pairs();
        test_sloped_join_requires_contact_at_the_shared_endpoint();
        test_document_validates_join_references_and_persists_record();
        std::cout << "Wall join tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
