#include "sketch/wall_plan_junctions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace sketch {
namespace {
constexpr double tolerance = default_geometry_tolerance_metres;

struct Polygon {
    std::array<Vec2, 4> vertices{};
    int orientation{};
};

struct SourceStroke {
    Segment segment;
    std::size_t polygon{};
};

struct WallWork {
    const WallPlanGeometry* plan{};
    std::vector<Polygon> polygons;
    std::vector<SourceStroke> strokes;
};

struct Interval {
    double first{};
    double last{};
};

struct PolygonClip {
    bool valid{true};
    bool present{};
    Interval interval;
};

struct SharedSpan {
    bool present{};
    Interval first;
    Interval second;
};

bool finite(Vec2 point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool finite(const Segment& segment) {
    return finite(segment.start) && finite(segment.end) &&
        std::isfinite(segment.sweep_radians);
}

Vec2 subtract(Vec2 first, Vec2 second) {
    return {first.x - second.x, first.y - second.y};
}

Vec2 add(Vec2 first, Vec2 second) {
    return {first.x + second.x, first.y + second.y};
}

Vec2 multiply(Vec2 point, double scalar) {
    return {point.x * scalar, point.y * scalar};
}

double dot(Vec2 first, Vec2 second) {
    return std::fma(first.x, second.x, first.y * second.y);
}

double cross(Vec2 first, Vec2 second) {
    return std::fma(first.x, second.y, -first.y * second.x);
}

double length(Vec2 vector) {
    return std::hypot(vector.x, vector.y);
}

bool same_point(Vec2 first, Vec2 second) {
    const Vec2 delta = subtract(first, second);
    const double distance = length(delta);
    return finite(delta) && std::isfinite(distance) && distance <= tolerance;
}

bool valid_polygon(const Boundary& footprint, std::size_t first,
                   Polygon& polygon) {
    for (std::size_t edge = 0; edge < polygon.vertices.size(); ++edge) {
        const Segment& segment = footprint[first + edge];
        if (!finite(segment) || segment.sweep_radians != 0 ||
            !same_point(segment.end,
                        footprint[first + (edge + 1) % polygon.vertices.size()].start))
            return false;
        polygon.vertices[edge] = segment.start;
        const double edge_length = length(subtract(segment.end, segment.start));
        if (!std::isfinite(edge_length) || edge_length <= tolerance) return false;
    }

    double perimeter = 0;
    double doubled_area = 0;
    int turn_orientation = 0;
    const Vec2 origin = polygon.vertices.front();
    for (std::size_t index = 0; index < polygon.vertices.size(); ++index) {
        const Vec2 current = polygon.vertices[index];
        const Vec2 next = polygon.vertices[(index + 1) % polygon.vertices.size()];
        const Vec2 after = polygon.vertices[(index + 2) % polygon.vertices.size()];
        const Vec2 edge = subtract(next, current);
        const Vec2 following = subtract(after, next);
        const double edge_length = length(edge);
        const double following_length = length(following);
        const double turn = cross(edge, following);
        const Vec2 current_local = subtract(current, origin);
        const Vec2 next_local = subtract(next, origin);
        const double area_term = cross(current_local, next_local);
        if (!finite(edge) || !finite(following) ||
            !finite(current_local) || !finite(next_local) ||
            !std::isfinite(edge_length) || !std::isfinite(following_length) ||
            !std::isfinite(turn) || !std::isfinite(area_term) ||
            edge_length <= tolerance || following_length <= tolerance ||
            std::abs(turn) <= tolerance * edge_length)
            return false;
        const int turn_sign = turn > 0 ? 1 : -1;
        if (turn_orientation != 0 && turn_sign != turn_orientation) return false;
        turn_orientation = turn_sign;
        perimeter += edge_length;
        doubled_area += area_term;
        if (!std::isfinite(perimeter) || !std::isfinite(doubled_area)) return false;
    }
    if (turn_orientation == 0 || !std::isfinite(perimeter) ||
        !std::isfinite(doubled_area) ||
        std::abs(doubled_area) <= tolerance * perimeter)
        return false;
    polygon.orientation = doubled_area > 0 ? 1 : -1;
    return true;
}

bool stroke_is_polygon_edge(const Segment& stroke, const Polygon& polygon) {
    for (std::size_t edge = 0; edge < polygon.vertices.size(); ++edge) {
        const Vec2 start = polygon.vertices[edge];
        const Vec2 end = polygon.vertices[(edge + 1) % polygon.vertices.size()];
        if ((same_point(stroke.start, start) && same_point(stroke.end, end)) ||
            (same_point(stroke.start, end) && same_point(stroke.end, start)))
            return true;
    }
    return false;
}

bool build_work(const std::string& id, const Wall& wall,
                const WallPlanGeometry& plan, WallWork& work) {
    if (wall.id != id || !finite(wall.baseline) || wall.baseline.sweep_radians != 0 ||
        !std::isfinite(wall.thickness) || wall.thickness <= tolerance)
        return false;
    const Vec2 baseline_delta = subtract(wall.baseline.end, wall.baseline.start);
    const double baseline_length = length(baseline_delta);
    if (!finite(baseline_delta) || !std::isfinite(baseline_length) ||
        baseline_length <= tolerance || plan.footprint.size() % 4 != 0)
        return false;

    work.plan = &plan;
    work.polygons.reserve(plan.footprint.size() / 4);
    for (std::size_t first = 0; first < plan.footprint.size(); first += 4) {
        Polygon polygon;
        if (!valid_polygon(plan.footprint, first, polygon)) return false;
        work.polygons.push_back(polygon);
    }
    if (work.polygons.empty() && !plan.strokes.empty()) return false;

    work.strokes.reserve(plan.strokes.size());
    for (const Segment& stroke : plan.strokes) {
        if (!finite(stroke) || stroke.sweep_radians != 0) return false;
        const Vec2 delta = subtract(stroke.end, stroke.start);
        const double stroke_length = length(delta);
        if (!finite(delta) || !std::isfinite(stroke_length) ||
            stroke_length <= tolerance)
            return false;
        std::size_t owner = work.polygons.size();
        for (std::size_t index = 0; index < work.polygons.size(); ++index) {
            if (stroke_is_polygon_edge(stroke, work.polygons[index])) {
                owner = index;
                break;
            }
        }
        if (owner == work.polygons.size()) return false;
        work.strokes.push_back({stroke, owner});
    }
    return true;
}

PolygonClip interior_interval(const Segment& source, const Polygon& polygon) {
    PolygonClip result;
    const Vec2 delta = subtract(source.end, source.start);
    const double source_length = length(delta);
    if (!finite(delta) || !std::isfinite(source_length) ||
        source_length <= tolerance) {
        result.valid = false;
        return result;
    }
    const Vec2 direction = multiply(delta, 1 / source_length);
    if (!finite(direction)) {
        result.valid = false;
        return result;
    }

    double first = 0;
    double last = 1;
    for (std::size_t index = 0; index < polygon.vertices.size(); ++index) {
        const Vec2 edge_start = polygon.vertices[index];
        const Vec2 edge_end = polygon.vertices[(index + 1) % polygon.vertices.size()];
        const Vec2 edge = subtract(edge_end, edge_start);
        const Vec2 relative = subtract(source.start, edge_start);
        const double edge_length = length(edge);
        if (!finite(edge) || !finite(relative) || !std::isfinite(edge_length) ||
            edge_length <= tolerance) {
            result.valid = false;
            return result;
        }
        const double distance_at_start = polygon.orientation *
            (cross(edge, relative) / edge_length);
        const double distance_change = polygon.orientation *
            (cross(edge, direction) / edge_length) * source_length;
        if (!std::isfinite(distance_at_start) || !std::isfinite(distance_change)) {
            result.valid = false;
            return result;
        }

        if (std::abs(distance_change) <= tolerance) {
            // A stroke parallel to, or numerically indistinguishable from, a
            // polygon boundary is not strictly inside that closed polygon.
            if (distance_at_start <= tolerance) return result;
            continue;
        }
        const double boundary_fraction = -distance_at_start / distance_change;
        if (!std::isfinite(boundary_fraction)) {
            result.valid = false;
            return result;
        }
        if (distance_change > 0)
            first = std::max(first, boundary_fraction);
        else
            last = std::min(last, boundary_fraction);
        if (last <= first) return result;
    }

    first = std::max(0.0, first);
    last = std::min(1.0, last);
    if (!std::isfinite(first) || !std::isfinite(last)) {
        result.valid = false;
        return result;
    }
    const double span = (last - first) * source_length;
    if (span <= tolerance) return result;
    result.present = true;
    result.interval = {first * source_length, last * source_length};
    if (!std::isfinite(result.interval.first) ||
        !std::isfinite(result.interval.last))
        result.valid = false;
    return result;
}

SharedSpan collinear_overlap(const Segment& first, const Segment& second) {
    SharedSpan result;
    const Vec2 first_delta = subtract(first.end, first.start);
    const Vec2 second_delta = subtract(second.end, second.start);
    const double first_length = length(first_delta);
    const double second_length = length(second_delta);
    if (!finite(first_delta) || !finite(second_delta) ||
        !std::isfinite(first_length) || !std::isfinite(second_length) ||
        first_length <= tolerance || second_length <= tolerance)
        return result;
    const Vec2 first_direction = multiply(first_delta, 1 / first_length);
    const Vec2 second_direction = multiply(second_delta, 1 / second_length);
    const Vec2 second_start_delta = subtract(second.start, first.start);
    const Vec2 second_end_delta = subtract(second.end, first.start);
    if (!finite(first_direction) || !finite(second_direction) ||
        !finite(second_start_delta) || !finite(second_end_delta))
        return result;
    const double perpendicular_start = cross(first_direction, second_start_delta);
    const double perpendicular_end = cross(first_direction, second_end_delta);
    const double second_start_station = dot(second_start_delta, first_direction);
    const double second_end_station = dot(second_end_delta, first_direction);
    const double alignment = dot(first_direction, second_direction);
    if (!std::isfinite(perpendicular_start) || !std::isfinite(perpendicular_end) ||
        !std::isfinite(second_start_station) || !std::isfinite(second_end_station) ||
        !std::isfinite(alignment) || std::abs(alignment) <= tolerance ||
        std::abs(perpendicular_start) > tolerance ||
        std::abs(perpendicular_end) > tolerance)
        return result;

    const double overlap_first = std::max(0.0,
        std::min(second_start_station, second_end_station));
    const double overlap_last = std::min(first_length,
        std::max(second_start_station, second_end_station));
    if (overlap_last - overlap_first <= tolerance) return result;

    const double second_first = (overlap_first - second_start_station) / alignment;
    const double second_last = (overlap_last - second_start_station) / alignment;
    if (!std::isfinite(second_first) || !std::isfinite(second_last)) return result;
    const double second_overlap_first = std::max(0.0,
        std::min(second_first, second_last));
    const double second_overlap_last = std::min(second_length,
        std::max(second_first, second_last));
    if (second_overlap_last - second_overlap_first <= tolerance) return result;

    result.present = true;
    result.first = {overlap_first, overlap_last};
    result.second = {second_overlap_first, second_overlap_last};
    return result;
}

bool material_side(const Polygon& polygon, Vec2 line_start, Vec2 direction,
                   int& side) {
    Vec2 centroid_delta{};
    for (const Vec2 vertex : polygon.vertices) {
        const Vec2 local = subtract(vertex, line_start);
        if (!finite(local)) return false;
        centroid_delta.x += local.x * .25;
        centroid_delta.y += local.y * .25;
    }
    if (!finite(centroid_delta)) return false;
    const double signed_distance = cross(direction, centroid_delta);
    if (!std::isfinite(signed_distance) || std::abs(signed_distance) <= tolerance)
        return false;
    side = signed_distance > 0 ? 1 : -1;
    return true;
}

bool point_at_station(const Segment& source, Vec2 direction, double source_length,
                      double station, Vec2& point) {
    if (station <= 0) {
        point = source.start;
        return true;
    }
    if (station >= source_length) {
        point = source.end;
        return true;
    }
    const double dx = direction.x * station;
    const double dy = direction.y * station;
    point = {std::fma(direction.x, station, source.start.x),
             std::fma(direction.y, station, source.start.y)};
    const Vec2 represented = subtract(point, source.start);
    if (!finite(point) || !finite(represented) ||
        std::abs(represented.x - dx) > tolerance ||
        std::abs(represented.y - dy) > tolerance)
        return false;
    return true;
}

bool append_interval_fragments(const Segment& source,
                               std::vector<Interval> cuts,
                               Boundary& result) {
    if (cuts.empty()) {
        result.push_back(source);
        return true;
    }
    const Vec2 delta = subtract(source.end, source.start);
    const double source_length = length(delta);
    if (!finite(delta) || !std::isfinite(source_length) ||
        source_length <= tolerance)
        return false;
    const Vec2 direction = multiply(delta, 1 / source_length);
    if (!finite(direction)) return false;

    for (Interval& interval : cuts) {
        if (!std::isfinite(interval.first) || !std::isfinite(interval.last)) return false;
        interval.first = std::clamp(interval.first, 0.0, source_length);
        interval.last = std::clamp(interval.last, 0.0, source_length);
        if (interval.last < interval.first) std::swap(interval.first, interval.last);
    }
    std::sort(cuts.begin(), cuts.end(), [](const Interval& first, const Interval& second) {
        if (first.first != second.first) return first.first < second.first;
        return first.last < second.last;
    });
    std::vector<Interval> merged;
    merged.reserve(cuts.size());
    for (const Interval cut : cuts) {
        if (cut.last - cut.first <= tolerance) continue;
        if (merged.empty() || cut.first > merged.back().last + tolerance)
            merged.push_back(cut);
        else
            merged.back().last = std::max(merged.back().last, cut.last);
    }

    double cursor = 0;
    for (const Interval cut : merged) {
        if (cut.first - cursor > tolerance) {
            Vec2 start, end;
            if (!point_at_station(source, direction, source_length, cursor, start) ||
                !point_at_station(source, direction, source_length, cut.first, end))
                return false;
            const double fragment_length = length(subtract(end, start));
            if (!std::isfinite(fragment_length)) return false;
            if (fragment_length > tolerance) result.push_back({start, end, 0});
        }
        cursor = std::max(cursor, cut.last);
    }
    if (source_length - cursor > tolerance) {
        Vec2 start, end;
        if (!point_at_station(source, direction, source_length, cursor, start) ||
            !point_at_station(source, direction, source_length, source_length, end))
            return false;
        const double fragment_length = length(subtract(end, start));
        if (!std::isfinite(fragment_length)) return false;
        if (fragment_length > tolerance) result.push_back({start, end, 0});
    }
    return true;
}

void add_intersections(const WallWork& source, const WallWork& cutter,
                       std::vector<std::vector<Interval>>& cuts, bool& valid) {
    for (std::size_t stroke = 0; stroke < source.strokes.size() && valid; ++stroke) {
        for (const Polygon& polygon : cutter.polygons) {
            const PolygonClip overlap = interior_interval(source.strokes[stroke].segment,
                                                          polygon);
            if (!overlap.valid) {
                valid = false;
                break;
            }
            if (overlap.present) cuts[stroke].push_back(overlap.interval);
        }
    }
}

std::vector<std::vector<Interval>> empty_cuts(const WallWork& work) {
    return std::vector<std::vector<Interval>>(work.strokes.size());
}

} // namespace

bool wall_baselines_have_interior_contact(const Segment& first,
                                          const Segment& second) noexcept {
    if (!finite(first) || !finite(second) || first.sweep_radians != 0 ||
        second.sweep_radians != 0)
        return false;
    try {
        const SegmentIntersection intersection = segment_intersection(
            first, second, default_geometry_tolerance_metres);
        if (intersection.kind == SegmentIntersectionKind::proper) return true;
        if (intersection.kind != SegmentIntersectionKind::touch) return false;

        const Vec2 first_delta = subtract(first.end, first.start);
        const Vec2 second_delta = subtract(second.end, second.start);
        const double first_length = length(first_delta);
        const double second_length = length(second_delta);
        if (!finite(first_delta) || !finite(second_delta) ||
            !std::isfinite(first_length) || !std::isfinite(second_length) ||
            first_length <= tolerance || second_length <= tolerance)
            return false;
        const Vec2 first_direction = multiply(first_delta, 1 / first_length);
        const Vec2 second_direction = multiply(second_delta, 1 / second_length);
        if (!finite(first_direction) || !finite(second_direction)) return false;
        for (const Vec2 point : intersection.points) {
            if (!finite(point)) return false;
            const Vec2 from_first = subtract(point, first.start);
            const Vec2 from_second = subtract(point, second.start);
            if (!finite(from_first) || !finite(from_second)) return false;
            const double first_station = dot(from_first, first_direction);
            const double second_station = dot(from_second, second_direction);
            if (!std::isfinite(first_station) || !std::isfinite(second_station)) return false;
            const bool first_interior = first_station > tolerance &&
                first_station < first_length - tolerance;
            const bool second_interior = second_station > tolerance &&
                second_station < second_length - tolerance;
            if (first_interior || second_interior) return true;
        }
    } catch (...) {
        // This classifier is a conservative opt-in for the analytic stroke
        // pass; an unresolved intersection must remain on the legacy path.
    }
    return false;
}

void apply_wall_plan_junction_strokes(
    std::map<std::string, WallPlanGeometry, std::less<>>& plans,
    const std::map<std::string, Wall, std::less<>>& walls,
    const std::vector<std::pair<std::string, std::string>>& contacts) {
    std::map<std::string, WallWork, std::less<>> work;
    std::set<std::pair<std::string, std::string>> contact_pairs;
    std::set<std::string, std::less<>> participants;
    for (const auto& [first, second] : contacts) {
        if (first == second) continue;
        contact_pairs.emplace(std::min(first, second), std::max(first, second));
        participants.insert(first);
        participants.insert(second);
    }
    if (contact_pairs.empty()) return;
    for (const auto& id : participants) {
        const auto wall = walls.find(id);
        if (wall == walls.end()) continue;
        const auto plan = plans.find(id);
        if (plan == plans.end()) continue;
        WallWork candidate;
        if (build_work(id, wall->second, plan->second, candidate))
            work.emplace(id, std::move(candidate));
    }

    std::map<std::string, std::vector<std::vector<Interval>>, std::less<>> cuts;
    for (const auto& [id, candidate] : work)
        cuts.emplace(id, empty_cuts(candidate));

    for (const auto& [first_id, second_id] : contact_pairs) {
        const auto first = work.find(first_id);
        const auto second = work.find(second_id);
        if (first == work.end() || second == work.end()) continue;

        auto first_cuts = cuts.at(first_id);
        auto second_cuts = cuts.at(second_id);
        bool valid = true;
        add_intersections(first->second, second->second, first_cuts, valid);
        add_intersections(second->second, first->second, second_cuts, valid);

        for (std::size_t first_stroke = 0;
             first_stroke < first->second.strokes.size() && valid; ++first_stroke) {
            const SourceStroke& a = first->second.strokes[first_stroke];
            for (std::size_t second_stroke = 0;
                 second_stroke < second->second.strokes.size(); ++second_stroke) {
                const SourceStroke& b = second->second.strokes[second_stroke];
                const SharedSpan shared = collinear_overlap(a.segment, b.segment);
                if (!shared.present) continue;
                const Vec2 a_delta = subtract(a.segment.end, a.segment.start);
                const double a_length = length(a_delta);
                if (!finite(a_delta) || !std::isfinite(a_length) || a_length <= tolerance) {
                    valid = false;
                    break;
                }
                const Vec2 a_direction = multiply(a_delta, 1 / a_length);
                int side_a = 0, side_b = 0;
                if (!finite(a_direction) ||
                    !material_side(first->second.polygons[a.polygon], a.segment.start,
                                   a_direction, side_a) ||
                    !material_side(second->second.polygons[b.polygon], a.segment.start,
                                   a_direction, side_b)) {
                    valid = false;
                    break;
                }
                if (side_a != side_b) {
                    first_cuts[first_stroke].push_back(shared.first);
                    second_cuts[second_stroke].push_back(shared.second);
                } else if (first_id < second_id) {
                    // A coincident exterior face is retained by the
                    // lexicographically first wall only.
                    second_cuts[second_stroke].push_back(shared.second);
                } else {
                    first_cuts[first_stroke].push_back(shared.first);
                }
            }
        }
        if (valid) {
            cuts.at(first_id) = std::move(first_cuts);
            cuts.at(second_id) = std::move(second_cuts);
        }
    }

    std::map<std::string, std::vector<std::string>, std::less<>> neighbors;
    for (const auto& [first, second] : contact_pairs) {
        if (!work.contains(first) || !work.contains(second)) continue;
        neighbors[first].push_back(second);
        neighbors[second].push_back(first);
    }
    std::map<std::string, std::string, std::less<>> component;
    for (const auto& [id, candidate] : work) {
        (void)candidate;
        if (component.contains(id)) continue;
        component.emplace(id, id);
        std::vector<std::string> pending{id};
        for (std::size_t index = 0; index < pending.size(); ++index)
            for (const auto& neighbor : neighbors[pending[index]])
                if (component.emplace(neighbor, id).second) pending.push_back(neighbor);
    }
    std::set<std::string, std::less<>> unsupported_components;
    std::map<std::string, Boundary, std::less<>> replacements;
    for (const auto& [id, wall_cuts] : cuts) {
        const WallWork& candidate = work.at(id);
        bool changed = false;
        for (const auto& edge_cuts : wall_cuts)
            changed = changed || !edge_cuts.empty();
        if (!changed) continue;

        Boundary strokes;
        bool representable = true;
        for (std::size_t index = 0; index < candidate.strokes.size(); ++index) {
            if (!append_interval_fragments(candidate.strokes[index].segment,
                                           wall_cuts[index], strokes)) {
                representable = false;
                unsupported_components.insert(component.at(id));
                break;
            }
        }
        if (representable) replacements.emplace(id, std::move(strokes));
    }
    for (auto& [id, strokes] : replacements)
        if (!unsupported_components.contains(component.at(id)))
            plans.at(id).strokes = std::move(strokes);
}

} // namespace sketch
