#include "sketch/slab_plan_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sketch {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

struct Sum {
    double value{}, compensation{};
    void add(double term) {
        require(std::isfinite(term), "Slab plan quantity must be finite");
        const auto adjusted = term - compensation;
        const auto next = value + adjusted;
        require(std::isfinite(next), "Slab plan quantity exceeds the supported numeric range");
        compensation = (next - value) - adjusted;
        value = next;
    }
};

double mapped_source_tolerance(const AssemblyProfilePlanarAffine& affine) {
    // A conservative inverse-norm bound makes source topology admission cover
    // actual physical clearance too. Use the shared exact inverse arithmetic,
    // rather than a separately rounded determinant or origin subtraction.
    auto linear = affine;
    linear.source_origin = {};
    linear.target_origin = {};
    const auto x = assembly_profile_affine_inverse_point({1, 0}, linear);
    const auto y = assembly_profile_affine_inverse_point({0, 1}, linear);
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto largest = std::max({std::abs(x.x), std::abs(x.y), std::abs(y.x), std::abs(y.y)});
    require(std::isfinite(largest) && largest > 0, "Slab plan inverse tolerance is unrepresentable");
    double norm = 0;
    for (const auto coefficient : {x.x, x.y, y.x, y.y}) {
        if (coefficient != 0)
            norm = std::nextafter(norm + std::nextafter(std::abs(coefficient) / largest, infinity), infinity);
    }
    // Cover inverse rounding, normalized-product roundings and the final grid.
    norm = std::nextafter(norm * (1 + 8 * std::numeric_limits<double>::epsilon()), infinity);
    const auto tolerance = std::nextafter(
        (default_geometry_tolerance_metres * norm) * largest, infinity);
    require(std::isfinite(tolerance), "Slab plan source tolerance is unrepresentable");
    return std::max(default_geometry_tolerance_metres, tolerance);
}

struct AdmittedPlan {
    std::size_t segments{};
    double source_area{};
    Bounds2 bounds;
};

AdmittedPlan admit(const SlabPlanSource& slab) {
    AdmittedPlan result;
    const auto count = [&](const Boundary& ring) {
        require(!ring.empty() && ring.size() <= assembly_profile_geometry_max_segments - result.segments,
            "Slab plan exceeds its aggregate source-segment budget or has an empty ring");
        result.segments += ring.size();
    };
    count(slab.boundary);
    require(slab.holes.size() <= assembly_profile_geometry_max_segments,
        "Slab plan exceeds its hole budget");
    for (const auto& hole : slab.holes) count(hole);

    const auto tolerance = slab.planar_affine ? mapped_source_tolerance(*slab.planar_affine)
                                            : default_geometry_tolerance_metres;
    if (const auto error = validate_boundary_holes(slab.boundary, slab.holes, tolerance))
        throw std::invalid_argument("Invalid slab plan topology: " + *error);

    const auto ring_bounds = [&](const Boundary& ring) {
        if (!slab.planar_affine) return boundary_bounds(ring);
        const auto& affine = *slab.planar_affine;
        // Exact closure and representable analytic area/range are mandatory;
        // endpoint-only checks cannot bound an affine circular arc.
        (void)assembly_profile_closed_boundary_signed_area(ring, affine, tolerance);
        const auto bounds = assembly_profile_boundary_bounds(ring, affine);
        const auto width = bounds.maximum.x - bounds.minimum.x;
        const auto height = bounds.maximum.y - bounds.minimum.y;
        require(std::isfinite(width) && std::isfinite(height) &&
                width > default_geometry_tolerance_metres && height > default_geometry_tolerance_metres,
            "Mapped slab plan bounds collapse below physical tolerance");
        for (const auto& segment : ring) {
            const auto start = assembly_profile_affine_point(segment.start, affine);
            const auto end = assembly_profile_affine_point(segment.end, affine);
            const auto chord = std::hypot(end.x - start.x, end.y - start.y);
            require(std::isfinite(chord) && chord > default_geometry_tolerance_metres,
                "Mapped slab plan endpoint chord is unrepresentable or below physical tolerance");
        }
        return bounds;
    };
    result.bounds = ring_bounds(slab.boundary);
    Sum source_area;
    source_area.add(std::abs(signed_area(slab.boundary)));
    for (const auto& hole : slab.holes) {
        (void)ring_bounds(hole);
        source_area.add(-std::abs(signed_area(hole)));
    }
    require(source_area.value >= 0, "Slab plan net area must be nonnegative");
    result.source_area = source_area.value;
    if (slab.planar_affine) {
        require(result.source_area > 0 &&
                assembly_profile_affine_scaled_area(result.source_area, *slab.planar_affine) >
                    default_geometry_tolerance_metres * default_geometry_tolerance_metres,
            "Mapped slab plan net area collapses below physical tolerance");
    }
    return result;
}

} // namespace

nlohmann::json encode_slab_planar_affine(const AssemblyProfilePlanarAffine& affine) {
    validate_assembly_profile_affine(affine);
    return {{"version", 1}, {"source_origin_m", {affine.source_origin.x, affine.source_origin.y}},
        {"target_origin_m", {affine.target_origin.x, affine.target_origin.y}},
        {"matrix", {affine.xx, affine.xy, affine.yx, affine.yy}}};
}

AssemblyProfilePlanarAffine decode_slab_planar_affine(const nlohmann::json& value) {
    require(value.is_object() && value.size() == 4 && value.contains("version") &&
            value.contains("source_origin_m") && value.contains("target_origin_m") && value.contains("matrix"),
        "Invalid slab planar affine fields");
    require(value.at("version").is_number_integer() && value.at("version") == 1,
        "Unsupported slab planar affine version");
    for (const auto* key : {"source_origin_m", "target_origin_m", "matrix"}) {
        const auto& numbers = value.at(key);
        require(numbers.is_array() && numbers.size() == (std::string_view(key) == "matrix" ? 4u : 2u),
            "Invalid slab planar affine coefficient count");
        for (const auto& number : numbers)
            require(number.is_number() && std::isfinite(number.get<double>()),
                "Slab planar affine coefficients must be finite numbers");
    }
    const auto& source = value.at("source_origin_m");
    const auto& target = value.at("target_origin_m");
    const auto& matrix = value.at("matrix");
    AssemblyProfilePlanarAffine result{{source[0].get<double>(), source[1].get<double>()},
        {target[0].get<double>(), target[1].get<double>()}, matrix[0].get<double>(), matrix[1].get<double>(),
        matrix[2].get<double>(), matrix[3].get<double>()};
    validate_assembly_profile_affine(result);
    return result;
}

double slab_plan_signed_area(const SlabPlanSource& slab) {
    const auto admitted = admit(slab);
    return slab.planar_affine ? assembly_profile_affine_scaled_area(admitted.source_area, *slab.planar_affine)
                             : admitted.source_area;
}

double slab_plan_perimeter(const SlabPlanSource& slab) {
    const auto admitted = admit(slab);
    Sum length, error;
    const AssemblyProfileLengthOptions total_options;
    std::size_t evaluations = 0;
    const auto measure = [&](const Boundary& ring) {
        if (!slab.planar_affine) {
            length.add(perimeter(ring));
            return;
        }
        auto options = total_options;
        options.absolute_tolerance_metres *= static_cast<double>(ring.size()) / admitted.segments;
        // A ring of exact lines/similarities can consume zero evaluations even
        // after the prior ring used the whole allowance. Refuse any added work.
        options.maximum_evaluations = std::max<std::size_t>(1, total_options.maximum_evaluations - evaluations);
        const auto measured = assembly_profile_boundary_perimeter(ring, *slab.planar_affine, options);
        require(measured.evaluations <= total_options.maximum_evaluations - evaluations,
            "Slab plan perimeter exceeds its aggregate evaluation budget");
        evaluations += measured.evaluations;
        length.add(measured.length_metres);
        error.add(measured.estimated_error_metres);
    };
    measure(slab.boundary);
    for (const auto& hole : slab.holes) measure(hole);
    require(length.value >= 0 && error.value <=
            total_options.absolute_tolerance_metres + total_options.relative_tolerance * length.value,
        "Slab plan perimeter aggregate tolerance was not met");
    return length.value;
}

Bounds2 slab_plan_bounds(const SlabPlanSource& slab) {
    return admit(slab).bounds;
}

std::vector<Boundary> slab_plan_display_boundaries(
    const SlabPlanSource& slab, double maximum_deviation_metres, std::size_t maximum_segments) {
    const auto admitted = admit(slab);
    require(std::isfinite(maximum_deviation_metres) && maximum_deviation_metres > 0 &&
            maximum_segments > 0 && maximum_segments <= 65536 && admitted.segments <= maximum_segments,
        "Invalid slab plan display deviation or aggregate segment budget");
    std::vector<Boundary> result;
    result.reserve(1 + slab.holes.size());
    std::size_t used = 0;
    const auto display = [&](const Boundary& ring) {
        require(ring.size() <= maximum_segments - used, "Slab plan display exceeds its aggregate segment budget");
        auto path = slab.planar_affine ? assembly_profile_boundary_display_path(
            ring, *slab.planar_affine, maximum_deviation_metres, maximum_segments - used) : ring;
        used += path.size();
        result.push_back(std::move(path));
    };
    display(slab.boundary);
    for (const auto& hole : slab.holes) display(hole);
    return result;
}

} // namespace sketch
