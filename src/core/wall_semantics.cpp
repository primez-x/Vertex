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

struct WallTopData {
    double length;
    double chord;
    double rise;
    Vec2 gradient;
    double along_gradient;
    double across_gradient;
    bool explicit_plane;
    std::optional<ArcData> arc;
};

WallTopData wall_top_data(const Wall& wall) {
    const double length = wall_baseline_length(wall.baseline);
    if (!std::isfinite(wall.height) ||
        (wall.slope_rise && !std::isfinite(*wall.slope_rise))) {
        reject("Wall top height and rise must be finite");
    }
    const double dx = wall.baseline.end.x - wall.baseline.start.x;
    const double dy = wall.baseline.end.y - wall.baseline.start.y;
    const double chord = std::hypot(dx, dy);
    if (!std::isfinite(chord) || !(chord > 0.0)) {
        reject("Wall top chord must have a representable positive length");
    }
    const double supplied_rise = wall.slope_rise.value_or(0.0);
    const double rise = std::abs(supplied_rise) <= tolerance ? 0.0 : supplied_rise;
    const Vec2 direction{dx / chord, dy / chord};
    const double along = rise / chord;
    Vec2 gradient{direction.x * along, direction.y * along};
    if (wall.top_gradient_m_per_m) {
        gradient = *wall.top_gradient_m_per_m;
        if (!finite(gradient)) reject("Wall top gradient must be finite");
        const double end_rise = dx * gradient.x + dy * gradient.y;
        if (!std::isfinite(end_rise)) {
            reject("Wall top end rise exceeds the supported numeric range");
        }
        if (wall.slope_rise && std::abs(end_rise - supplied_rise) >
            std::max(tolerance, std::max(std::abs(end_rise), std::abs(supplied_rise)) * 1e-9)) {
            reject("Wall slope rise disagrees with its retained top plane");
        }
    }
    const double along_gradient = wall.top_gradient_m_per_m
        ? direction.x * gradient.x + direction.y * gradient.y : along;
    const double across_gradient = wall.top_gradient_m_per_m
        ? -direction.y * gradient.x + direction.x * gradient.y : 0.0;
    if (!finite(gradient) || !std::isfinite(std::hypot(gradient.x, gradient.y)) ||
        !std::isfinite(along_gradient) || !std::isfinite(across_gradient) || !finite(direction)) {
        reject("Wall top gradient exceeds the supported numeric range");
    }
    return {length, chord, rise, gradient, along_gradient, across_gradient,
            wall.top_gradient_m_per_m.has_value(),
            wall.baseline.sweep_radians == 0.0
                ? std::nullopt : std::optional<ArcData>(arc_data(wall.baseline))};
}

double top_station(double station, double length) {
    if (!std::isfinite(station) || station < -tolerance ||
        station > length + tolerance) {
        reject("Wall top station lies outside its baseline");
    }
    return std::clamp(station, 0.0, length);
}

double top_radius(const Wall& wall, const WallTopData& data, double offset) {
    if (!std::isfinite(offset)) reject("Wall top normal offset must be finite");
    const double direction = wall.baseline.sweep_radians > 0.0 ? 1.0 : -1.0;
    const double radius = data.arc->radius - direction * offset;
    positive(radius, "Wall top normal offset crosses its arc centre");
    return radius;
}

double top_height_at_position(const Wall& wall, const WallTopData& data,
                              double along, double across) {
    const double height = wall.height + data.along_gradient * along +
                          data.across_gradient * across;
    if (!std::isfinite(along) || !std::isfinite(across) || !std::isfinite(height)) {
        reject("Wall top height exceeds the supported numeric range");
    }
    return height;
}

// Unwrapped start radial angle relative to the chord direction. Express it
// using the sweep to avoid subtracting large absolute world coordinates.
double top_start_angle(const Wall& wall) {
    return (wall.baseline.sweep_radians > 0.0 ? -std::numbers::pi * 0.5
                                            : std::numbers::pi * 0.5) -
           wall.baseline.sweep_radians * 0.5;
}

double top_station_height(const Wall& wall, const WallTopData& data,
                           double station, double offset) {
    if (!std::isfinite(offset)) reject("Wall top normal offset must be finite");
    if (!data.arc) {
        if (!data.explicit_plane) {
            const double height = wall.height + data.rise * (station / data.length);
            if (!std::isfinite(height)) reject("Wall top height exceeds the supported numeric range");
            return height;
        }
        return top_height_at_position(wall, data, station, offset);
    }
    const double radius = top_radius(wall, data, offset);
    if (data.along_gradient == 0.0 && data.across_gradient == 0.0) return wall.height;
    const double centre_along = data.chord * 0.5;
    const double centre_across = data.chord / (2.0 * std::tan(wall.baseline.sweep_radians * 0.5));
    // Local chord coordinates avoid subtracting large world coordinates.
    // Across the thickness the plane is affine in radius; along the arc it
    // is sinusoidal. Endpoint radial coordinates avoid trig cancellation.
    double along, across;
    if (station == 0.0) {
        const double scale = 1.0 - radius / data.arc->radius;
        along = centre_along * scale;
        across = centre_across * scale;
    } else if (station == data.length) {
        const double scale = radius / data.arc->radius;
        along = centre_along * (1.0 + scale);
        across = centre_across * (1.0 - scale);
    } else {
        const double angle = wall.baseline.sweep_radians * (station / data.length);
        const double scale = radius / data.arc->radius;
        const double half_sine = std::sin(angle * 0.5);
        // 1 - scale*cos(angle), written without subtracting nearly equal
        // numbers on shallow arcs. Rotate the start radial vector relative
        // to the start point rather than adding huge circle coordinates.
        const double radial_change = (1.0 - scale) + 2.0 * scale * half_sine * half_sine;
        along = centre_along * radial_change + scale * centre_across * std::sin(angle);
        across = centre_across * radial_change - scale * centre_along * std::sin(angle);
    }
    return top_height_at_position(wall, data, along, across);
}

}  // namespace

Vec2 wall_top_gradient(const Wall& wall) {
    const auto data = wall_top_data(wall);
    return data.gradient;
}

double wall_top_height(const Wall& wall, double station_metres,
                       double normal_offset_metres) {
    const auto data = wall_top_data(wall);
    return top_station_height(wall, data, top_station(station_metres, data.length),
                              normal_offset_metres);
}

WallTopHeightRange wall_top_height_range(const Wall& wall, double from_metres,
                                        double to_metres, double inner_offset_metres,
                                        double outer_offset_metres) {
    const auto data = wall_top_data(wall);
    if (!std::isfinite(inner_offset_metres) || !std::isfinite(outer_offset_metres) ||
        inner_offset_metres > outer_offset_metres || from_metres > to_metres) {
        reject("Wall top interval and offsets must form ordered finite ranges");
    }
    const double from = top_station(from_metres, data.length);
    const double to = top_station(to_metres, data.length);
    WallTopHeightRange range{std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()};
    const auto retain = [&](double height) {
        range.minimum = std::min(range.minimum, height);
        range.maximum = std::max(range.maximum, height);
    };
    for (double offset : {inner_offset_metres, outer_offset_metres}) {
        retain(top_station_height(wall, data, from, offset));
        retain(top_station_height(wall, data, to, offset));
        if (!data.arc || (data.along_gradient == 0.0 && data.across_gradient == 0.0)) continue;
        const double first_angle = top_start_angle(wall) +
            wall.baseline.sweep_radians * (from / data.length);
        const double sweep = wall.baseline.sweep_radians * ((to - from) / data.length);
        // A directed interval below one full turn contains at most one of
        // each plane-height extremum. Include both for major/negative arcs.
        const double maximum_angle = std::atan2(data.across_gradient, data.along_gradient);
        for (double extremum : {maximum_angle, maximum_angle + std::numbers::pi}) {
            double travel = std::fmod(sweep >= 0.0 ? extremum - first_angle
                                                  : first_angle - extremum, full_turn);
            if (travel < 0.0) travel += full_turn;
            if (travel <= std::abs(sweep)) {
                const double station = std::clamp(from + data.length *
                    (travel / std::abs(wall.baseline.sweep_radians)), from, to);
                retain(top_station_height(wall, data, station, offset));
            }
        }
    }
    return range;
}

std::vector<double> wall_top_height_crossings(const Wall& wall, double relative_height_metres) {
    if (!std::isfinite(relative_height_metres)) reject("Wall top crossing height must be finite");
    const auto data = wall_top_data(wall);
    if (data.along_gradient == 0.0 && (!data.arc || data.across_gradient == 0.0)) return {};
    std::vector<double> intervals{0.0, data.length};
    if (data.arc) {
        const auto first_angle = top_start_angle(wall);
        const auto sweep = wall.baseline.sweep_radians;
        const auto maximum_angle = std::atan2(data.across_gradient, data.along_gradient);
        for (const auto extremum : {maximum_angle, maximum_angle + std::numbers::pi}) {
            auto travel = std::fmod(sweep > 0.0 ? extremum - first_angle : first_angle - extremum, full_turn);
            if (travel < 0.0) travel += full_turn;
            if (travel > 0.0 && travel < std::abs(sweep))
                intervals.push_back(data.length * (travel / std::abs(sweep)));
        }
    }
    std::sort(intervals.begin(), intervals.end());
    intervals.erase(std::unique(intervals.begin(), intervals.end()), intervals.end());
    std::vector<double> roots;
    for (std::size_t index = 1; index < intervals.size(); ++index) {
        auto low = intervals[index - 1], high = intervals[index];
        const auto low_height = top_station_height(wall, data, low, 0.0);
        const auto high_height = top_station_height(wall, data, high, 0.0);
        if (low_height == relative_height_metres) roots.push_back(low);
        if (high_height == relative_height_metres) roots.push_back(high);
        if (low_height == high_height || relative_height_metres <= std::min(low_height, high_height) ||
            relative_height_metres >= std::max(low_height, high_height)) continue;
        const bool increasing = high_height > low_height;
        // Finite station endpoints bracket one root on each monotone arc.
        // Midpoint stagnation is the representable-precision stopping point.
        for (int step = 0; step < 96; ++step) {
            const auto middle = std::midpoint(low, high);
            if (middle == low || middle == high) break;
            const auto height = top_station_height(wall, data, middle, 0.0);
            if (height == relative_height_metres) { low = high = middle; break; }
            if ((height < relative_height_metres) == increasing) low = middle;
            else high = middle;
        }
        roots.push_back(std::midpoint(low, high));
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end(), [](double first, double second) {
        return std::abs(first - second) <= tolerance;
    }), roots.end());
    return roots;
}

Vec2 parse_wall_top_plane(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 2 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.contains("gradient_m_per_m")) {
        reject("Wall top plane requires version 1 and gradient_m_per_m");
    }
    const auto& components = value.at("gradient_m_per_m");
    if (!components.is_array() || components.size() != 2 ||
        !components.at(0).is_number() || !components.at(1).is_number()) {
        reject("Wall top plane gradient must be a finite two-vector");
    }
    const Vec2 gradient{components.at(0).get<double>(), components.at(1).get<double>()};
    if (!finite(gradient)) reject("Wall top plane gradient must be finite");
    return gradient;
}

nlohmann::json wall_top_plane_json(Vec2 gradient) {
    if (!finite(gradient)) reject("Wall top plane gradient must be finite");
    return {{"version", 1}, {"gradient_m_per_m", {gradient.x, gradient.y}}};
}

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

    const auto top_range = wall_top_height_range(wall, 0.0, length,
                                                 -wall.thickness * 0.5,
                                                 wall.thickness * 0.5);
    positive(top_range.minimum, "Wall slope leaves a non-positive top height");
    require_finite_sum(wall.elevation, top_range.minimum,
                       "Wall minimum top elevation exceeds the supported numeric range");
    require_finite_sum(wall.elevation, top_range.maximum,
                       "Wall maximum top elevation exceeds the supported numeric range");
    const double length_limit = length + tolerance;
    if (!std::isfinite(length_limit) || !std::isfinite(top_range.maximum + tolerance)) {
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
        if (end > length_limit) {
            reject("Opening extends beyond its wall");
        }
        const auto local_top = wall_top_height_range(wall, opening.offset, end,
                                                      -wall.thickness * 0.5,
                                                      wall.thickness * 0.5);
        if (top > local_top.minimum + tolerance) {
            reject("Opening extends beyond its wall top");
        }
        bounds.push_back({opening.offset, end, opening.sill, top});
    }

    reject_overlapping_openings(bounds);

    // This is an exact rectangle-union proof over the unwrapped baseline and
    // wall height. It intentionally does not treat a positive gap smaller
    // than tolerance as covered; the OCCT volume check remains authoritative
    // for such numerically ambiguous near-boundary states.
    const auto gradient = wall_top_gradient(wall);
    if (gradient.x == 0.0 && gradient.y == 0.0 &&
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

void validate_wall_join_semantics(const WallJoin& join) {
    if (!valid_reference_id(join.id)) {
        reject("Wall join ID must be a non-empty ASCII identifier");
    }
    if (join.style != WallJoinStyle::fused) {
        reject("Wall join style is unsupported");
    }
    if (join.wall_ids.size() < 2 || join.wall_ids.size() > 32) {
        reject("Wall joins require between two and thirty-two walls");
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& wall_id : join.wall_ids) {
        if (!valid_reference_id(wall_id) || !ids.insert(wall_id).second) {
            reject("Wall join wall IDs must be unique ASCII identifiers");
        }
    }
}

std::string_view wall_join_style_name(WallJoinStyle style) noexcept {
    switch (style) {
    case WallJoinStyle::fused:
        return "fused";
    }
    return "invalid";
}

std::optional<WallJoinStyle> parse_wall_join_style(std::string_view value) noexcept {
    if (value == "fused") return WallJoinStyle::fused;
    return std::nullopt;
}

WallJoin parse_wall_join(const nlohmann::json& value, std::string_view id) {
    WallJoin result;
    result.id = std::string(id);
    if (!value.is_object() || value.size() != 3 || !value.contains("version") ||
        !value.contains("style") || !value.contains("wall_ids")) {
        throw std::invalid_argument(
            "Wall join properties must contain exactly version, style, and wall_ids");
    }
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || version != 1) {
        throw std::invalid_argument("Wall join version must be 1");
    }
    const auto& style = value.at("style");
    if (!style.is_string()) {
        throw std::invalid_argument("Wall join style must be a string");
    }
    const auto parsed_style = parse_wall_join_style(style.get<std::string>());
    if (!parsed_style.has_value()) {
        throw std::invalid_argument("Wall join style is unsupported");
    }
    result.style = *parsed_style;
    const auto& wall_ids = value.at("wall_ids");
    if (!wall_ids.is_array()) {
        throw std::invalid_argument("Wall join wall_ids must be an array");
    }
    result.wall_ids.reserve(wall_ids.size());
    for (const auto& wall_id : wall_ids) {
        if (!wall_id.is_string()) {
            throw std::invalid_argument("Wall join wall_ids must contain strings");
        }
        result.wall_ids.push_back(wall_id.get<std::string>());
    }
    validate_wall_join_semantics(result);
    return result;
}

nlohmann::json wall_join_json(const WallJoin& join) {
    validate_wall_join_semantics(join);
    return { {"version", 1}, {"style", wall_join_style_name(join.style)},
             {"wall_ids", join.wall_ids} };
}

}  // namespace sketch
