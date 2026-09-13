#include "sketch/architecture.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
void near(double actual, double expected, double tolerance, const char* description) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(description);
    }
}
template <typename Function> void rejected(Function&& function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid architectural object was accepted");
}
}

int main() {
    try {
        using namespace sketch;
        Wall wall{"wall-1", {{0, 0}, {4, 0}, 0}, 0.2, 3.0, 0.0, {}};
        near(solid_volume(make_wall(wall)), 2.4, 1e-8, "Straight wall volume");
        auto sloped = wall;
        sloped.id = "sloped-wall";
        sloped.slope_rise = 1.0;
        near(solid_volume(make_wall(sloped)), 2.8, 1e-8,
             "Positive sloped wall volume uses the average top height");
        sloped.slope_rise = -1.0;
        near(solid_volume(make_wall(sloped)), 2.0, 1e-8,
             "Negative sloped wall volume uses the average top height");
        sloped.openings.push_back({"too-tall", 3.0, 0.5, 0.0, 2.6});
        rejected([&] { (void)make_wall(sloped); });
        sloped.openings.clear();
        wall.openings.push_back({"door-1", 0.5, 1.0, 0.0, 2.1});
        wall.openings.push_back({"window-1", 2.5, 1.0, 1.0, 1.0});
        near(solid_volume(make_wall(wall)), 2.4 - 0.42 - 0.2, 1e-8, "Hosted openings cut wall volume");
        wall.elevation = 4.5;
        near(solid_volume(make_wall(wall)), 1.78, 1e-8, "Elevation preserves volume");
        wall.openings.push_back({"overlap", 0.6, 1.0, 1.0, 1.0});
        rejected([&] { (void)make_wall(wall); });
        wall.openings.pop_back();
        wall.openings.front().offset = -0.2;
        rejected([&] { (void)make_wall(wall); });

        Wall curved{"curved-1", {{2, 0}, {0, 2}, std::numbers::pi / 2}, 0.2, 3, 0, {}};
        near(solid_volume(make_wall(curved)), std::numbers::pi * 0.2 * 3, 1e-7, "Curved wall is an exact annular sector");
        curved.openings.push_back({"opening", 0.3, 1.0, 0.8, 1.2});
        near(solid_volume(make_wall(curved)), std::numbers::pi * 0.2 * 3 - 0.24, 1e-7,
             "Curved opening width follows the baseline arc");
        Wall reverse_curve{"reverse", {{0, 2}, {2, 0}, -std::numbers::pi / 2}, 0.2, 3, 0, {}};
        near(solid_volume(make_wall(reverse_curve)), std::numbers::pi * 0.2 * 3, 1e-7,
             "Reversing a curved wall preserves its physical volume");
        auto curved_composite = curved;
        curved_composite.id = "curved-composite";
        curved_composite.layers = {
            {"outer", 0.03, std::nullopt},
            {"core", 0.14, WallLayerMaterial{"catalog", "block"}},
            {"inner", 0.03, std::nullopt},
        };
        curved_composite.openings.clear();
        near(solid_volume(make_wall(curved_composite)), std::numbers::pi * 0.2 * 3, 1e-7,
             "Curved composite wall layers retain exact annular volume");
        reverse_curve.thickness = 4;
        rejected([&] { (void)make_wall(reverse_curve); });

        Wall composite{"composite-1", {{0, 0}, {4, 0}, 0}, 0.2, 3.0, 0.0, {}};
        composite.layers = {
            {"outer", 0.02, std::nullopt},
            {"core", 0.16, WallLayerMaterial{"catalog", "brick"}},
            {"inner", 0.02, WallLayerMaterial{"catalog", "plaster"}},
        };
        near(solid_volume(make_wall(composite)), 4.0 * 0.2 * 3.0, 1e-8,
             "Composite wall layers retain total solid volume");
        composite.openings.push_back({"door", 1.0, 1.0, 0.0, 2.0});
        near(solid_volume(make_wall(composite)), 4.0 * 0.2 * 3.0 - 1.0 * 0.2 * 2.0, 1e-8,
             "Composite wall openings cut every layer exactly once");

        Slab slab{"floor-1", {{{0, 0}, {4, 0}, 0}, {{4, 0}, {4, 3}, 0},
                             {{4, 3}, {0, 3}, 0}, {{0, 3}, {0, 0}, 0}}, {}, 0.25, -0.25};
        if (slab_element_kind_name(SlabElementKind::floor) != "floor" ||
            slab_element_kind_name(SlabElementKind::ceiling) != "ceiling" ||
            slab_element_kind_name(SlabElementKind::foundation) != "foundation" ||
            !parse_slab_element_kind("floor").has_value() ||
            parse_slab_element_kind("unsupported").has_value()) {
            throw std::runtime_error("Slab element kind codec is inconsistent");
        }
        near(solid_volume(make_slab(slab)), 3.0, 1e-8, "Slab volume");
        slab.element_kind = SlabElementKind::floor;
        near(solid_volume(make_slab(slab)), 3.0, 1e-8, "Floor element preserves slab geometry");
        auto invalid_kind_slab = slab;
        invalid_kind_slab.element_kind = static_cast<SlabElementKind>(99);
        rejected([&] { (void)make_slab(invalid_kind_slab); });
        slab.holes.push_back({{{1, 1}, {2, 1}, 0}, {{2, 1}, {2, 2}, 0},
                              {{2, 2}, {1, 2}, 0}, {{1, 2}, {1, 1}, 0}});
        near(solid_volume(make_slab(slab)), 2.75, 1e-8, "Slab opening subtracts exact area");
        auto invalid_slab = slab;
        invalid_slab.holes.push_back(slab.holes.front());
        rejected([&] { (void)make_slab(invalid_slab); });
        invalid_slab = slab;
        for (auto& edge : invalid_slab.holes.front()) {
            edge.start.x += 10;
            edge.end.x += 10;
        }
        rejected([&] { (void)make_slab(invalid_slab); });
        invalid_slab = slab;
        for (auto& edge : invalid_slab.holes.front()) {
            edge.start.x -= 1;
            edge.end.x -= 1;
        }
        rejected([&] { (void)make_slab(invalid_slab); });
        slab.thickness = 0;
        rejected([&] { (void)make_slab(slab); });
        const TerrainSurface terrain(
            "native fixture",
            {TerrainPoint{"p0", 0.0, 0.0, 0.0},
             TerrainPoint{"p1", 4.0, 0.0, 1.0},
             TerrainPoint{"p2", 4.0, 3.0, 2.0},
             TerrainPoint{"p3", 0.0, 3.0, 0.5}},
            {TerrainTriangle{{0, 1, 2}}, TerrainTriangle{{0, 2, 3}}});
        if (make_terrain_surface(terrain).IsNull()) {
            throw std::runtime_error("Terrain surface produced a null OCCT shape");
        }
        std::cout << "Architectural solid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
