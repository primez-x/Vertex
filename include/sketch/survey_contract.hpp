#pragma once
#include <optional>
#include <string>
#include <vector>

namespace sketch {
enum class BearingQuadrant { north_east, south_east, south_west, north_west };
struct SurveyLeg { std::string id; BearingQuadrant quadrant; double angle_degrees{}; double distance_m{}; };
struct SurveyVertex { double east_m{}, north_m{}; };
struct SurveyClosureDiagnostics {
    double east_error_m{}, north_error_m{}, linear_error_m{}, perimeter_m{}, relative_error{};
    bool closed{};
    std::optional<double> area_m2, acres;
};
// Quadrant angles are measured from north/south toward east/west, in [0,90].
// Ordered legs start at a local origin; no closure adjustment is performed.
class SurveyTraverse {
public:
    static constexpr std::size_t maximum_legs = 4096;
    explicit SurveyTraverse(std::string provenance, std::vector<SurveyLeg> legs,
                            double closure_tolerance_m = 1e-6);
    [[nodiscard]] const std::vector<SurveyVertex>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const SurveyClosureDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] std::string serialize() const;
private:
    std::string provenance_;
    std::vector<SurveyLeg> legs_;
    double tolerance_;
    std::vector<SurveyVertex> vertices_;
    SurveyClosureDiagnostics diagnostics_;
};
}
