#include "sketch/slab_semantics.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using sketch::SlabLayer;
using sketch::WallLayerMaterial;
using Json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function> void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void test_round_trip_and_material_assignments() {
    const std::vector<SlabLayer> layers{
        {"structure", 0.15, WallLayerMaterial{"catalog", "concrete"}},
        {"finish", 0.10, WallLayerMaterial{"catalog", "finish"}},
    };
    sketch::validate_slab_layers(layers, 0.25);
    const auto encoded = sketch::slab_layers_json(layers);
    const auto decoded = sketch::parse_slab_layers(encoded, 0.25);
    require(decoded == layers, "slab layers must round-trip deterministically");
    require(encoded.size() == 2 && encoded[0].at("material_assignment").at("version") == 1,
            "slab layer material assignment must retain its schema version");
}

void test_rejects_invalid_stacks() {
    const Json valid = Json::array({
        {{"id", "structure"}, {"thickness_m", 0.15}},
        {{"id", "finish"}, {"thickness_m", 0.10}},
    });
    rejected([&] { (void)sketch::parse_slab_layers(valid, 0.20); },
             "slab layer thickness must match the host slab");

    auto duplicate = valid;
    duplicate[1]["id"] = "structure";
    rejected([&] { (void)sketch::parse_slab_layers(duplicate, 0.25); },
             "duplicate slab layer IDs must be rejected");

    auto unknown = valid;
    unknown[0]["extra"] = true;
    rejected([&] { (void)sketch::parse_slab_layers(unknown, 0.25); },
             "unknown slab layer fields must be rejected");

    auto malformed_assignment = valid;
    malformed_assignment[0]["material_assignment"] = {
        {"version", 2}, {"catalog_id", "catalog"}, {"material_id", "concrete"}};
    rejected([&] { (void)sketch::parse_slab_layers(malformed_assignment, 0.25); },
             "unsupported slab layer material versions must be rejected");

    auto invalid_thickness = valid;
    invalid_thickness[0]["thickness_m"] = 0.0;
    rejected([&] { (void)sketch::parse_slab_layers(invalid_thickness, 0.25); },
             "non-positive slab layer thickness must be rejected");
}

} // namespace

int main() {
    try {
        test_round_trip_and_material_assignments();
        test_rejects_invalid_stacks();
        std::cout << "Slab semantic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
