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
        reverse_curve.thickness = 4;
        rejected([&] { (void)make_wall(reverse_curve); });

        Slab slab{"floor-1", {{{0, 0}, {4, 0}, 0}, {{4, 0}, {4, 3}, 0},
                             {{4, 3}, {0, 3}, 0}, {{0, 3}, {0, 0}, 0}}, {}, 0.25, -0.25};
        near(solid_volume(make_slab(slab)), 3.0, 1e-8, "Slab volume");
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
        std::cout << "Architectural solid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
