#include "sketch/terrain_surface.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using sketch::TerrainPoint;
using sketch::TerrainSurface;
using sketch::TerrainTriangle;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function>
void rejects(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid terrain surface accepted");
}

TerrainSurface fixture() {
    return TerrainSurface(
        "field survey",
        {TerrainPoint{"p0", 0.0, 0.0, 100.0},
         TerrainPoint{"p1", 10.0, 0.0, 101.0},
         TerrainPoint{"p2", 10.0, 10.0, 103.0},
         TerrainPoint{"p3", 0.0, 10.0, 102.0}},
        {TerrainTriangle{{0, 1, 2}}, TerrainTriangle{{0, 2, 3}}}, 1.0, true);
}

void round_trip_and_derived_geometry() {
    const auto surface = fixture();
    const auto encoded = surface.to_json();
    require(TerrainSurface::from_json(encoded).to_json() == encoded,
            "terrain surface did not round-trip through canonical JSON");
    const auto bounds = surface.elevation_bounds();
    require(bounds.first == 100.0 && bounds.second == 103.0,
            "terrain elevation bounds are not deterministic");
    require(surface.plan_edges().size() == 5,
            "terrain plan edges must deduplicate the shared diagonal");
    const auto contours = surface.contours();
    require(!contours.empty(), "terrain contour derivation produced no lines");
    for (const auto& contour : contours) {
        require(std::isfinite(contour.start.x) && std::isfinite(contour.start.y) &&
                    std::isfinite(contour.end.x) && std::isfinite(contour.end.y) &&
                    std::isfinite(contour.elevation_m),
                "terrain contour contains a non-finite value");
        require(contour.elevation_m >= 100.0 && contour.elevation_m <= 103.0,
                "terrain contour level is outside the surface range");
    }
}

void validation() {
    const auto encoded = fixture().to_json();
    auto malformed = encoded;
    malformed["version"] = 2;
    rejects([&] { (void)TerrainSurface::from_json(malformed); });

    malformed = encoded;
    malformed["points"][1]["elevation_m"] = std::numeric_limits<double>::infinity();
    rejects([&] { (void)TerrainSurface::from_json(malformed); });

    malformed = encoded;
    malformed["triangles"][0][2] = 99;
    rejects([&] { (void)TerrainSurface::from_json(malformed); });

    rejects([&] {
        (void)TerrainSurface("duplicate", {TerrainPoint{"p", 0, 0, 0},
                                             TerrainPoint{"p", 1, 0, 0},
                                             TerrainPoint{"q", 0, 1, 0}},
                             {TerrainTriangle{{0, 1, 2}}});
    });
    rejects([&] {
        (void)TerrainSurface("degenerate", {TerrainPoint{"a", 0, 0, 0},
                                              TerrainPoint{"b", 1, 0, 0},
                                              TerrainPoint{"c", 2, 0, 0}},
                             {TerrainTriangle{{0, 1, 2}}});
    });
    rejects([&] {
        (void)TerrainSurface("interval", {TerrainPoint{"a", 0, 0, 0},
                                            TerrainPoint{"b", 1, 0, 0},
                                            TerrainPoint{"c", 0, 1, 0}},
                             {TerrainTriangle{{0, 1, 2}}}, 0.0);
    });
}

}  // namespace

int main() {
    try {
        round_trip_and_derived_geometry();
        validation();
    } catch (const std::exception& error) {
        std::cerr << "terrain_surface_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "terrain surface tests passed\n";
}
