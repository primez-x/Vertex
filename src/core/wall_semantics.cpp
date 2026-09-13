#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <nlohmann/json.hpp>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

constexpr double tolerance = default_geometry_tolerance_metres;
constexpr double full_turn = 2.0 * std::numbers::pi;

struct ArcData {
    Vec2 center;
    double radius = 0.0;
};

struct OpeningBounds {
    double offset = 0.0;
    double end = 0.0;
    double sill = 0.0;
    double top = 0.0;
};

[[noreturn]] void reject(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

bool finite(Vec2 point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

void positive(double value, std::string_view message) {
    if (!std::isfinite(value) || value <= tolerance) {
        reject(message);
    }
}

void require_finite_sum(double left, double right, std::string_view message) {
    if (!std::isfinite(left + right)) {
        reject(message);
    }
}

ArcData arc_data(const Segment& segment) {
    const double dx = segment.end.x - segment.start.x;
    const double dy = segment.end.y - segment.start.y;
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        reject("Wall arc chord exceeds the supported numeric range");
    }
    const double chord_length = std::hypot(dx, dy);
    if (!std::isfinite(chord_length) || !(chord_length > 0.0)) {
        reject("Wall arc endpoints must be distinct");
    }

    const double half_sweep = segment.sweep_radians * 0.5;
    const double sine = std::sin(std::abs(half_sweep));
    const double tangent = std::tan(half_sweep);
    if (!(sine > 0.0) || tangent == 0.0 || !std::isfinite(tangent)) {
        reject("Wall arc geometry cannot be represented");
    }

    const Vec2 midpoint{(segment.start.x + segment.end.x) * 0.5,
                        (segment.start.y + segment.end.y) * 0.5};
    if (!finite(midpoint)) {
        reject("Wall arc centre exceeds the supported numeric range");
    }
    const Vec2 left_normal{-dy / chord_length, dx / chord_length};
    const double center_offset = chord_length / (2.0 * tangent);
    if (!std::isfinite(center_offset)) {
        reject("Wall arc centre exceeds the supported numeric range");
    }
    const Vec2 center{midpoint.x + left_normal.x * center_offset,
                      midpoint.y + left_normal.y * center_offset};
    const double radius = chord_length / (2.0 * sine);
    if (!finite(center) || !std::isfinite(radius) || !(radius > 0.0)) {
        reject("Wall arc geometry exceeds the supported numeric range");
    }
    return {center, radius};
}

void require_finite_line_strip(const Segment& baseline, double length, double thickness) {
    const double dx = baseline.end.x - baseline.start.x;
    const double dy = baseline.end.y - baseline.start.y;
    const double denominator = 2.0 * length;
    const double normal_x = -dy * thickness / denominator;
    const double normal_y = dx * thickness / denominator;
    if (!std::isfinite(denominator)) {
        reject("Wall baseline offset exceeds the supported numeric range");
    }
    if (!std::isfinite(normal_x) || !std::isfinite(normal_y)) {
        reject("Wall baseline offset exceeds the supported numeric range");
    }
    for (const auto point : {baseline.start, baseline.end}) {
        for (const double sign : {-1.0, 1.0}) {
            if (!std::isfinite(point.x + sign * normal_x) ||
                !std::isfinite(point.y + sign * normal_y)) {
                reject("Wall baseline offset exceeds the supported numeric range");
            }
        }
    }
}

void require_finite_arc_strip(const Segment& baseline, const ArcData& arc, double thickness) {
    const double half = thickness * 0.5;
    for (const double offset : {-half, half}) {
        const double radius = arc.radius + offset;
        if (!std::isfinite(radius) || !(radius > 0.0)) {
            reject("Wall thickness crosses its arc centre");
        }
        const double scale = radius / arc.radius;
        if (!std::isfinite(scale)) {
            reject("Wall arc offset exceeds the supported numeric range");
        }
        for (const auto point : {baseline.start, baseline.end}) {
            const double radial_x = point.x - arc.center.x;
            const double radial_y = point.y - arc.center.y;
            const double offset_x = radial_x * scale;
            const double offset_y = radial_y * scale;
            const double result_x = arc.center.x + offset_x;
            const double result_y = arc.center.y + offset_y;
            if (!std::isfinite(radial_x) || !std::isfinite(radial_y) ||
                !std::isfinite(offset_x) || !std::isfinite(offset_y) ||
                !std::isfinite(result_x) || !std::isfinite(result_y)) {
                reject("Wall arc offset exceeds the supported numeric range");
            }
        }
    }
}

bool valid_reference_id(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character != 0 &&
               ((character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' ||
                character == '_' || character == '.' || character == ':');
    });
}

void validate_layer_stack(const std::vector<WallLayer>& layers,
                          std::optional<double> wall_thickness) {
    if (layers.empty()) return;
    if (layers.size() > 1024) reject("Wall layer count exceeds the supported limit");
    std::set<std::string, std::less<>> layer_ids;
    double total = 0.0;
    for (const auto& layer : layers) {
        if (!valid_reference_id(layer.id) || !layer_ids.insert(layer.id).second) {
            reject("Wall layer IDs must be unique ASCII identifiers");
        }
        positive(layer.thickness, "Wall layer thickness must be positive");
        if (!std::isfinite(total + layer.thickness)) {
            reject("Wall layer thickness total exceeds the supported numeric range");
        }
        total += layer.thickness;
        if (layer.material.has_value()) {
            if (!valid_reference_id(layer.material->catalog_id) ||
                !valid_reference_id(layer.material->material_id)) {
                reject("Wall layer material references must be paired identifiers");
            }
        }
    }
    if (wall_thickness.has_value()) {
        const auto tolerance_limit = std::max(tolerance, std::abs(*wall_thickness) * 1e-9);
        if (!std::isfinite(*wall_thickness) ||
            std::abs(total - *wall_thickness) > tolerance_limit) {
            reject("Wall layer thicknesses must sum to the wall thickness");
        }
    }
}

std::string required_layer_string(const nlohmann::json& value,
                                  const char* field) {
    if (!value.is_object() || !value.contains(field) || !value.at(field).is_string()) {
        throw std::invalid_argument(std::string("Wall layer ") + field +
                                    " must be a non-empty string");
    }
    const auto result = value.at(field).get<std::string>();
    if (result.empty()) {
        throw std::invalid_argument(std::string("Wall layer ") + field +
                                    " must be a non-empty string");
    }
    return result;
}

double required_layer_number(const nlohmann::json& value, const char* field) {
    if (!value.is_object() || !value.contains(field) || !value.at(field).is_number()) {
        throw std::invalid_argument(std::string("Wall layer ") + field +
                                    " must be a finite number");
    }
    const auto result = value.at(field).get<double>();
    if (!std::isfinite(result)) {
        throw std::invalid_argument(std::string("Wall layer ") + field +
                                    " must be a finite number");
    }
    return result;
}

class MaxSegmentTree {
public:
    explicit MaxSegmentTree(std::size_t value_count)
        : leaf_count_(1), values_(2, -std::numeric_limits<double>::infinity()) {
        while (leaf_count_ < value_count) leaf_count_ *= 2;
        values_.assign(leaf_count_ * 2, -std::numeric_limits<double>::infinity());
    }

    void set(std::size_t index, double value) {
        auto cursor = leaf_count_ + index;
        values_[cursor] = value;
        while (cursor > 1) {
            cursor /= 2;
            values_[cursor] = std::max(values_[cursor * 2], values_[cursor * 2 + 1]);
        }
    }

    [[nodiscard]] double prefix_max(std::size_t end) const {
        double result = -std::numeric_limits<double>::infinity();
        auto left = leaf_count_;
        auto right = leaf_count_ + end;
        while (left < right) {
            if (left % 2 == 1) result = std::max(result, values_[left++]);
            if (right % 2 == 1) result = std::max(result, values_[--right]);
            left /= 2;
            right /= 2;
        }
        return result;
    }

private:
    std::size_t leaf_count_;
    std::vector<double> values_;
};

// The old implementation compared every pair of openings. Sweep in baseline
// order and retain the largest active vertical end for each compressed sill
// coordinate. A prefix-max query finds an overlapping active y interval in
// logarithmic time while the expiry heap removes intervals whose x overlap is
// no longer greater than the geometric tolerance.
void reject_overlapping_openings(const std::vector<OpeningBounds>& bounds) {
    if (bounds.size() < 2) return;

    std::vector<double> sill_coordinates;
    sill_coordinates.reserve(bounds.size());
    for (const auto& opening : bounds) sill_coordinates.push_back(opening.sill);
    std::sort(sill_coordinates.begin(), sill_coordinates.end());
    sill_coordinates.erase(
        std::unique(sill_coordinates.begin(), sill_coordinates.end()), sill_coordinates.end());

    std::vector<std::multiset<double>> active_tops(sill_coordinates.size());
    MaxSegmentTree active_max(sill_coordinates.size());
    std::vector<std::size_t> order(bounds.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (bounds[left].offset != bounds[right].offset)
            return bounds[left].offset < bounds[right].offset;
        return left < right;
    });
    using Expiry = std::pair<double, std::size_t>;
    std::priority_queue<Expiry, std::vector<Expiry>, std::greater<Expiry>> expiries;

    for (const auto index : order) {
        const auto& opening = bounds[index];
        while (!expiries.empty() && expiries.top().first <= opening.offset + tolerance) {
            const auto expired = expiries.top();
            expiries.pop();
            const auto sill = static_cast<std::size_t>(std::lower_bound(
                sill_coordinates.begin(), sill_coordinates.end(), bounds[expired.second].sill) -
                sill_coordinates.begin());
            auto& tops = active_tops[sill];
            const auto found = tops.find(bounds[expired.second].top);
            if (found != tops.end()) tops.erase(found);
            active_max.set(sill, tops.empty() ? -std::numeric_limits<double>::infinity()
                                              : *tops.rbegin());
        }

        const auto y_prefix_end = static_cast<std::size_t>(std::lower_bound(
            sill_coordinates.begin(), sill_coordinates.end(), opening.top - tolerance) -
            sill_coordinates.begin());
        if (y_prefix_end != 0 &&
            active_max.prefix_max(y_prefix_end) > opening.sill + tolerance) {
            reject("Hosted openings overlap");
        }

        const auto sill = static_cast<std::size_t>(std::lower_bound(
            sill_coordinates.begin(), sill_coordinates.end(), opening.sill) -
            sill_coordinates.begin());
        active_tops[sill].insert(opening.top);
        active_max.set(sill, *active_tops[sill].rbegin());
        expiries.emplace(opening.end, index);
    }
}

class CoverageSegmentTree {
public:
    explicit CoverageSegmentTree(std::size_t segment_count)
        : segment_count_(segment_count), cover_(segment_count * 4 + 4, 0),
          covered_(segment_count * 4 + 4, 0) {}

    void add(std::size_t first, std::size_t last, int delta) {
        if (first >= last) return;
        add(first, last, delta, 1, 0, segment_count_);
    }

    [[nodiscard]] bool fully_covered() const noexcept {
        return covered_[1] == segment_count_;
    }

private:
    void pull(std::size_t node, std::size_t first, std::size_t last) {
        if (cover_[node] > 0) {
            covered_[node] = last - first;
        } else if (last - first == 1) {
            covered_[node] = 0;
        } else {
            covered_[node] = covered_[node * 2] + covered_[node * 2 + 1];
        }
    }

    void add(std::size_t update_first, std::size_t update_last, int delta,
             std::size_t node, std::size_t first, std::size_t last) {
        if (update_first >= last || update_last <= first) return;
        if (update_first <= first && last <= update_last) {
            cover_[node] += delta;
            pull(node, first, last);
            return;
        }
        const auto middle = first + (last - first) / 2;
        add(update_first, update_last, delta, node * 2, first, middle);
        add(update_first, update_last, delta, node * 2 + 1, middle, last);
        pull(node, first, last);
    }

    std::size_t segment_count_;
    std::vector<int> cover_;
    std::vector<std::size_t> covered_;
};

bool openings_cover_wall(const std::vector<OpeningBounds>& bounds,
                         double length,
                         double height) {
    std::vector<double> baseline_breaks{0.0, length};
    std::vector<double> height_breaks{0.0, height};
    struct ClippedOpening {
        double x_start = 0.0;
        double x_end = 0.0;
        double y_start = 0.0;
        double y_end = 0.0;
    };
    std::vector<ClippedOpening> clipped;
    clipped.reserve(bounds.size());

    for (const auto& opening : bounds) {
        const double x_start = std::clamp(opening.offset, 0.0, length);
        const double x_end = std::clamp(opening.end, 0.0, length);
        if (!(x_start < x_end)) continue;
        baseline_breaks.push_back(x_start);
        baseline_breaks.push_back(x_end);
        const double y_start = std::max(0.0, opening.sill);
        const double y_end = std::min(height, opening.top);
        if (y_end > y_start) {
            height_breaks.push_back(y_start);
            height_breaks.push_back(y_end);
            clipped.push_back({x_start, x_end, y_start, y_end});
        }
    }

    std::sort(baseline_breaks.begin(), baseline_breaks.end());
    baseline_breaks.erase(
        std::unique(baseline_breaks.begin(), baseline_breaks.end()), baseline_breaks.end());
    std::sort(height_breaks.begin(), height_breaks.end());
    height_breaks.erase(
        std::unique(height_breaks.begin(), height_breaks.end()), height_breaks.end());

    CoverageSegmentTree coverage(height_breaks.size() - 1);
    std::map<double, std::vector<std::size_t>, std::less<>> starts;
    std::map<double, std::vector<std::size_t>, std::less<>> ends;
    for (std::size_t index = 0; index < clipped.size(); ++index) {
        starts[clipped[index].x_start].push_back(index);
        ends[clipped[index].x_end].push_back(index);
    }

    const auto height_index = [&](double value) {
        return static_cast<std::size_t>(std::lower_bound(
            height_breaks.begin(), height_breaks.end(), value) - height_breaks.begin());
    };
    for (std::size_t index = 0; index + 1 < baseline_breaks.size(); ++index) {
        const double left = baseline_breaks[index];
        const double right = baseline_breaks[index + 1];
        if (const auto found = ends.find(left); found != ends.end()) {
            for (const auto opening : found->second) {
                coverage.add(height_index(clipped[opening].y_start),
                             height_index(clipped[opening].y_end), -1);
            }
        }
        if (const auto found = starts.find(left); found != starts.end()) {
            for (const auto opening : found->second) {
                coverage.add(height_index(clipped[opening].y_start),
                             height_index(clipped[opening].y_end), 1);
            }
        }
        if (right > left && !coverage.fully_covered()) return false;
    }
    return true;
}

double wall_baseline_length(const Segment& baseline) {
    if (!finite(baseline.start) || !finite(baseline.end) ||
        !std::isfinite(baseline.sweep_radians)) {
        reject("Wall baseline contains a non-finite value");
    }
    const double dx = baseline.end.x - baseline.start.x;
    const double dy = baseline.end.y - baseline.start.y;
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        reject("Wall baseline chord exceeds the supported numeric range");
    }
    if (baseline.sweep_radians != 0.0 &&
        !(std::abs(baseline.sweep_radians) < full_turn)) {
        reject("Wall arc sweep magnitude must be less than two pi");
    }
    double length = 0.0;
    try {
        length = segment_length(baseline);
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument(std::string("Wall baseline is invalid: ") + error.what());
    }
    positive(length, "Wall baseline must have a representable positive length");
    return length;
}

}  // namespace

void validate_wall_semantics(const Wall& wall) {
    positive(wall.thickness, "Wall thickness must be positive");
    positive(wall.height, "Wall height must be positive");
    validate_layer_stack(wall.layers, wall.thickness);
    if (!std::isfinite(wall.elevation)) {
        reject("Wall elevation must be finite");
    }
    require_finite_sum(wall.elevation, wall.height,
                       "Wall elevation plus height exceeds the supported numeric range");

    if (wall.slope_rise.has_value()) {
        if (!std::isfinite(*wall.slope_rise)) {
            reject("Wall slope rise must be finite");
        }
        if (std::abs(*wall.slope_rise) > tolerance && wall.baseline.sweep_radians != 0.0) {
            reject("Sloped walls require a straight baseline");
        }
        const auto end_height = wall.height + *wall.slope_rise;
        if (!std::isfinite(end_height) || end_height <= tolerance) {
            reject("Wall slope leaves a non-positive end height");
        }
        require_finite_sum(wall.elevation, *wall.slope_rise,
                           "Wall end elevation exceeds the supported numeric range");
        require_finite_sum(wall.elevation + *wall.slope_rise, wall.height,
                           "Wall end elevation exceeds the supported numeric range");
    }

    const double length = wall_baseline_length(wall.baseline);
    if (wall.baseline.sweep_radians == 0.0) {
        require_finite_line_strip(wall.baseline, length, wall.thickness);
    } else {
        const auto arc = arc_data(wall.baseline);
        const double half_thickness = wall.thickness * 0.5;
        if (!std::isfinite(half_thickness) ||
            half_thickness >= arc.radius - tolerance) {
            reject("Wall thickness crosses its arc centre");
        }
        require_finite_arc_strip(wall.baseline, arc, wall.thickness);
    }

    const double length_limit = length + tolerance;
    const double height_limit = wall.height + tolerance;
    if (!std::isfinite(length_limit) || !std::isfinite(height_limit)) {
        reject("Wall bounds exceed the supported numeric range");
    }

    std::set<std::string, std::less<>> opening_ids;
    std::vector<OpeningBounds> bounds;
    bounds.reserve(wall.openings.size());
    for (const auto& opening : wall.openings) {
        if (opening.id.empty() || !opening_ids.insert(opening.id).second) {
            reject("Opening IDs must be unique");
        }
        positive(opening.width, "Opening width must be positive");
        positive(opening.height, "Opening height must be positive");
        if (!std::isfinite(opening.offset) || !std::isfinite(opening.sill) ||
            opening.offset < 0.0 || opening.sill < 0.0) {
            reject("Opening offset and sill must be finite and nonnegative");
        }

        const double end = opening.offset + opening.width;
        const double top = opening.sill + opening.height;
        if (!std::isfinite(end) || !std::isfinite(top)) {
            reject("Opening dimensions exceed the supported numeric range");
        }
        require_finite_sum(wall.elevation, opening.sill,
                           "Opening sill elevation exceeds the supported numeric range");
        require_finite_sum(wall.elevation + opening.sill, opening.height,
                           "Opening top elevation exceeds the supported numeric range");
        if (end > length_limit || top > height_limit) {
            reject("Opening extends beyond its wall");
        }
        if (wall.slope_rise.has_value() &&
            std::abs(*wall.slope_rise) > tolerance) {
            const auto start_fraction = opening.offset / length;
            const auto end_fraction = end / length;
            const auto start_height = wall.height + *wall.slope_rise * start_fraction;
            const auto end_height = wall.height + *wall.slope_rise * end_fraction;
            const auto local_height = std::min(start_height, end_height);
            if (!std::isfinite(local_height) || top > local_height + tolerance) {
                reject("Opening extends beyond the sloped wall top");
            }
        }
        bounds.push_back({opening.offset, end, opening.sill, top});
    }

    reject_overlapping_openings(bounds);

    // This is an exact rectangle-union proof over the unwrapped baseline and
    // wall height. It intentionally does not treat a positive gap smaller
    // than tolerance as covered; the OCCT volume check remains authoritative
    // for such numerically ambiguous near-boundary states.
    if ((!wall.slope_rise.has_value() || std::abs(*wall.slope_rise) <= tolerance) &&
        openings_cover_wall(bounds, length, wall.height)) {
        reject("Openings remove the entire wall");
    }
}

std::vector<WallLayer> parse_wall_layers(const nlohmann::json& value,
                                         double wall_thickness) {
    if (!std::isfinite(wall_thickness) || wall_thickness <= tolerance) {
        throw std::invalid_argument("Wall layer stack requires a positive wall thickness");
    }
    if (!value.is_array()) {
        throw std::invalid_argument("Wall layers must be an array");
    }
    std::vector<WallLayer> result;
    result.reserve(value.size());
    for (const auto& entry : value) {
        if (!entry.is_object() || (entry.size() != 2 && entry.size() != 3) ||
            !entry.contains("id") || !entry.contains("thickness_m")) {
            throw std::invalid_argument(
                "Wall layer requires id, thickness_m, and optional material_assignment");
        }
        for (const auto& [key, unused] : entry.items()) {
            (void)unused;
            if (key != "id" && key != "thickness_m" && key != "material_assignment") {
                throw std::invalid_argument("Wall layer contains an unknown field");
            }
        }
        WallLayer layer;
        layer.id = required_layer_string(entry, "id");
        layer.thickness = required_layer_number(entry, "thickness_m");
        if (entry.contains("material_assignment")) {
            const auto& assignment = entry.at("material_assignment");
            if (!assignment.is_object() || assignment.size() != 3 ||
                !assignment.contains("version") || !assignment.contains("catalog_id") ||
                !assignment.contains("material_id") ||
                !assignment.at("version").is_number_integer() ||
                assignment.at("version") != 1) {
                throw std::invalid_argument(
                    "Wall layer material_assignment must be version 1");
            }
            layer.material = WallLayerMaterial{
                required_layer_string(assignment, "catalog_id"),
                required_layer_string(assignment, "material_id")};
        }
        result.push_back(std::move(layer));
    }
    validate_layer_stack(result, wall_thickness);
    return result;
}

nlohmann::json wall_layers_json(const std::vector<WallLayer>& layers) {
    validate_layer_stack(layers, std::nullopt);
    auto result = nlohmann::json::array();
    for (const auto& layer : layers) {
        nlohmann::json value{{"id", layer.id}, {"thickness_m", layer.thickness}};
        if (layer.material.has_value()) {
            value["material_assignment"] = {
                {"version", 1},
                {"catalog_id", layer.material->catalog_id},
                {"material_id", layer.material->material_id}};
        }
        result.push_back(std::move(value));
    }
    return result;
}

}  // namespace sketch
