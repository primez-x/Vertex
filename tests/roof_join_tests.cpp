#include "sketch/architecture.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/roof_join_semantics.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sketch::Entity;
using sketch::RoofJoin;
using sketch::RoofJoinStyle;
using sketch::SlopedRoofPanel;
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

SlopedRoofPanel panel(std::string id, double x) {
    return {std::move(id), {x, 0.0, 0.0}, 0.0, 2.0, 4.0, 0.0, 0.0,
            0.0, 0.2, {}};
}

Json join_properties(std::vector<std::string> ids) {
    return {{"version", 1}, {"style", "fused"}, {"roof_ids", std::move(ids)}};
}

void test_join_codec_is_versioned_and_lossless() {
    const auto encoded = join_properties({"roof-a", "roof-b"});
    const auto decoded = sketch::parse_roof_join(encoded, "join-1");
    require(decoded.id == "join-1" && decoded.style == RoofJoinStyle::fused &&
                decoded.roof_ids == std::vector<std::string>{"roof-a", "roof-b"},
            "roof join codec changed the canonical identity or ordering");
    require(sketch::roof_join_json(decoded) == encoded,
            "roof join JSON did not round-trip canonically");

    auto malformed = encoded;
    malformed["roof_ids"] = Json::array({"roof-a", "roof-a"});
    rejected([&] { (void)sketch::parse_roof_join(malformed, "join-1"); },
             "duplicate roof IDs must be rejected");
    malformed = encoded;
    malformed["version"] = 2;
    rejected([&] { (void)sketch::parse_roof_join(malformed, "join-1"); },
             "unsupported roof join versions must be rejected");
    malformed = encoded;
    malformed["style"] = "miter";
    rejected([&] { (void)sketch::parse_roof_join(malformed, "join-1"); },
             "unsupported roof join styles must be rejected");
}

void test_fused_join_requires_touching_roofs_and_returns_real_solid() {
    const auto first = panel("roof-a", 0.0);
    const auto second = panel("roof-b", 1.9);
    const auto first_shape = sketch::make_building_shape(first);
    const auto second_shape = sketch::make_building_shape(second);
    const RoofJoin join{"join-1", {"roof-a", "roof-b"}, RoofJoinStyle::fused};
    const auto shape = sketch::make_roof_join(join,
                                               std::vector<TopoDS_Shape>{first_shape, second_shape});
    const auto volume = sketch::solid_volume(shape);
    const auto source_volume = sketch::solid_volume(first_shape) +
                               sketch::solid_volume(second_shape);
    require(!shape.IsNull() && std::isfinite(volume) && volume > 0.0,
            "fused roof join did not produce a valid solid");
    require(volume <= source_volume + 1e-8,
            "fused roof join volume exceeded its source roof volumes");

    const auto disconnected = panel("roof-c", 20.0);
    const RoofJoin invalid{"join-2", {"roof-a", "roof-c"}, RoofJoinStyle::fused};
    rejected([&] {
        (void)sketch::make_roof_join(
            invalid, std::vector<TopoDS_Shape>{first_shape, sketch::make_building_shape(disconnected)});
    }, "disconnected roofs must not be accepted as a join");
}

void test_join_rejects_two_disconnected_pairs() {
    const std::vector<TopoDS_Shape> roofs{
        sketch::make_building_shape(panel("roof-a", 0.0)),
        sketch::make_building_shape(panel("roof-b", 1.9)),
        sketch::make_building_shape(panel("roof-c", 20.0)),
        sketch::make_building_shape(panel("roof-d", 21.9))};
    const RoofJoin join{"join-pairs", {"roof-a", "roof-b", "roof-c", "roof-d"},
                        RoofJoinStyle::fused};
    rejected([&] { (void)sketch::make_roof_join(join, roofs); },
             "two disconnected roof pairs must not be accepted as one join");
}

void test_join_accepts_chain_in_nonadjacent_order() {
    const std::vector<TopoDS_Shape> roofs{
        sketch::make_building_shape(panel("roof-a", 0.0)),
        sketch::make_building_shape(panel("roof-c", 3.8)),
        sketch::make_building_shape(panel("roof-b", 1.9))};
    const RoofJoin join{"join-chain", {"roof-a", "roof-c", "roof-b"}, RoofJoinStyle::fused};
    const auto shape = sketch::make_roof_join(join, roofs);
    const auto volume = sketch::solid_volume(shape);
    require(!shape.IsNull() && std::isfinite(volume) && volume > 0.0,
            "transitively connected roof chain must produce a solid regardless of member order");
}

void test_document_validates_join_references_and_persists_record() {
    const auto first = sketch::encode_building_entity(panel("roof-a", 0.0));
    const auto second = sketch::encode_building_entity(panel("roof-b", 1.9));
    const Entity join{"join-1", "roof_join", join_properties({"roof-a", "roof-b"}), false,
                      Json::object()};
    const auto document = sketch::Document::create({first, second, join});
    require(document.snapshot().entities().at("join-1") == join,
            "document changed the roof join record during admission");

    auto missing = join;
    missing.properties["roof_ids"] = Json::array({"roof-a", "missing-roof"});
    rejected([&] { (void)sketch::Document::create({first, second, missing}); },
             "document accepted a roof join with a dangling roof reference");

    auto wrong_type = join;
    wrong_type.properties["roof_ids"] = Json::array({"roof-a", "property-1"});
    Entity property{"property-1", "property", Json::object(), false, Json::object()};
    rejected([&] { (void)sketch::Document::create({first, property, wrong_type}); },
             "document accepted a roof join whose target is not a roof");
}

}  // namespace

int main() {
    try {
        test_join_codec_is_versioned_and_lossless();
        test_fused_join_requires_touching_roofs_and_returns_real_solid();
        test_join_accepts_chain_in_nonadjacent_order();
        test_join_rejects_two_disconnected_pairs();
        test_document_validates_join_references_and_persists_record();
        std::cout << "Roof join tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
