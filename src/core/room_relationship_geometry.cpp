#include "sketch/room_relationship_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace sketch {
namespace {

constexpr double full_turn = 2.0 * std::numbers::pi;

const char* kind_name(RoomReferenceKind kind) {
    switch (kind) {
    case RoomReferenceKind::room_boundary: return "room_boundary";
    case RoomReferenceKind::appraisal_measurement_boundary:
        return "appraisal_measurement_boundary";
    case RoomReferenceKind::architectural_wall: return "architectural_wall";
    }
    throw std::invalid_argument("Unknown room reference kind");
}

bool finite(Vec2 point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

double distance(Vec2 left, Vec2 right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

bool same_geometry(const Boundary& left, const Boundary& right, double tolerance) {
    if (left.size() != right.size()) return false;
    const double sweep_tolerance = std::max(1e-12, tolerance);
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (distance(left[index].start, right[index].start) > tolerance ||
            distance(left[index].end, right[index].end) > tolerance ||
            std::abs(left[index].sweep_radians - right[index].sweep_radians) > sweep_tolerance) {
            return false;
        }
    }
    return true;
}

bool same_transform(const PlanarTransform& left, const PlanarTransform& right,
                    double tolerance) {
    const auto angle = std::remainder(left.rotation_radians - right.rotation_radians, full_turn);
    return std::abs(angle) <= 1e-9 && left.flip_horizontal == right.flip_horizontal &&
        left.flip_vertical == right.flip_vertical && distance(left.pivot, right.pivot) <= tolerance &&
        distance(left.offset, right.offset) <= tolerance;
}

PlanarTransform identity_transform() {
    return {{0.0, 0.0}, 0.0, false, false, {0.0, 0.0}};
}

bool valid_segment(const Segment& segment, double tolerance) {
    if (!finite(segment.start) || !finite(segment.end) || !std::isfinite(segment.sweep_radians)) {
        return false;
    }
    try {
        // A finite arc length alone does not prove that radius, centre and
        // extrema are representable (notably for a near-zero signed sweep).
        (void)segment_bounds(segment);
        const auto length = segment_length(segment);
        return std::isfinite(length) && length > tolerance;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

template <typename AddDiagnostic>
bool validate_record(const RelationshipGeometry& record, RoomReferenceKind expected_kind,
                     double tolerance, const char* side, AddDiagnostic&& add_diagnostic,
                     std::size_t expected_wall_segments=1) {
    if (record.kind != expected_kind) {
        add_diagnostic("geometry role mismatch for " + record.id + " in " + side +
                       " snapshot: expected " + kind_name(expected_kind));
        return false;
    }
    if (expected_kind == RoomReferenceKind::architectural_wall) {
        if (record.geometry.size() != expected_wall_segments) {
            add_diagnostic("wall " + record.id + " in " + side +
                           " snapshot does not match declared physical membership");
            return false;
        }
        try { validate_room_relationship_wall_path(record.geometry,tolerance); }
        catch (const std::invalid_argument&) {
            add_diagnostic("invalid wall geometry for " + record.id + " in " + side +
                           " snapshot");
            return false;
        }
        return true;
    }

    const auto issues = validate_boundary(record.geometry, tolerance);
    if (!issues.empty()) {
        add_diagnostic("invalid boundary geometry for " + record.id + " in " + side +
                       " snapshot: " + issues.front().message);
        return false;
    }
    return true;
}

struct DerivedTransform {
    PlanarTransform transform;
    std::string diagnostic;
};

DerivedTransform derive_rigid_transform(const Boundary& before, const Boundary& after,
                                        double tolerance) {
    if (before.size() != after.size()) {
        return {{}, "non-rigid target geometry change: segment topology changed"};
    }
    if (before.empty()) {
        return {{}, "non-rigid target geometry change: no segments are available"};
    }

    const auto first_before = before.front();
    const auto first_after = after.front();
    const Vec2 before_vector{first_before.end.x - first_before.start.x,
                             first_before.end.y - first_before.start.y};
    const Vec2 after_vector{first_after.end.x - first_after.start.x,
                            first_after.end.y - first_after.start.y};
    const auto before_length = std::hypot(before_vector.x, before_vector.y);
    const auto after_length = std::hypot(after_vector.x, after_vector.y);
    const auto length_tolerance = std::max(tolerance,
                                           std::max(before_length, after_length) * 1e-10);
    if (!std::isfinite(before_length) || !std::isfinite(after_length) ||
        std::abs(before_length - after_length) > length_tolerance) {
        return {{}, "non-rigid target geometry change: segment length changed"};
    }

    const auto rotation = std::atan2(before_vector.x * after_vector.y -
                                         before_vector.y * after_vector.x,
                                     before_vector.x * after_vector.x +
                                         before_vector.y * after_vector.y);
    if (!std::isfinite(rotation)) {
        return {{}, "non-rigid target geometry change: segment direction is not representable"};
    }
    const auto cosine = std::cos(rotation);
    const auto sine = std::sin(rotation);
    const Vec2 rotated_start{cosine * first_before.start.x - sine * first_before.start.y,
                             sine * first_before.start.x + cosine * first_before.start.y};
    PlanarTransform transform{{0.0, 0.0}, rotation, false, false,
                              {first_after.start.x - rotated_start.x,
                               first_after.start.y - rotated_start.y}};
    try {
        for (std::size_t index = 0; index < before.size(); ++index) {
            const auto transformed = transform_segment(before[index], transform);
            if (distance(transformed.start, after[index].start) > tolerance ||
                distance(transformed.end, after[index].end) > tolerance ||
                std::abs(transformed.sweep_radians - after[index].sweep_radians) >
                    std::max(1e-12, tolerance)) {
                return {{}, "non-rigid target geometry change: coordinates, ordering, or arc sweep changed"};
            }
        }
    } catch (const std::invalid_argument&) {
        return {{}, "non-rigid target geometry change: transform is not representable"};
    }
    return {transform, {}};
}

} // namespace

void validate_room_relationship_wall_path(const Boundary& geometry,double tolerance) {
    if (!std::isfinite(tolerance) || !(tolerance>0) || geometry.empty() || geometry.size()>256)
        throw std::invalid_argument("Room wall path requires bounded nonempty geometry and a positive tolerance");
    for (std::size_t i=0;i<geometry.size();++i) {
        if (!valid_segment(geometry[i],tolerance)) throw std::invalid_argument("Room wall path contains invalid geometry");
        if (i) {
            const auto gap=distance(geometry[i-1].end,geometry[i].start);
            if (!std::isfinite(gap) || gap>tolerance)
                throw std::invalid_argument("Room wall path is disconnected or reverses native direction");
        }
        for (std::size_t j=0;j<i;++j) {
            const auto hit=segment_intersection(geometry[j],geometry[i],tolerance);
            if (hit.kind==SegmentIntersectionKind::none) continue;
            if (j+1!=i || hit.kind==SegmentIntersectionKind::overlap ||
                hit.kind==SegmentIntersectionKind::indeterminate || hit.points.empty() ||
                std::any_of(hit.points.begin(),hit.points.end(),[&](Vec2 point){
                    return distance(point,geometry[j].end)>tolerance || distance(point,geometry[i].start)>tolerance;
                })) throw std::invalid_argument("Room wall path has overlap or unexpected analytical intersections");
        }
    }
    const auto outer_distance=distance(geometry.front().start,geometry.back().end);
    if (!std::isfinite(outer_distance) || outer_distance<=tolerance)
        throw std::invalid_argument("Room wall path requires distinct outer endpoints");
}

RoomRelationshipGeometryResult propose_room_relationship_geometry(
    const RoomRelationshipSnapshot& relationships,
    const std::vector<RelationshipGeometry>& before,
    const std::vector<RelationshipGeometry>& after,
    double tolerance_metres) {
    if (!std::isfinite(tolerance_metres) || !(tolerance_metres > 0.0)) {
        throw std::invalid_argument("geometry propagation tolerance must be finite and positive");
    }

    std::map<std::string, RoomReferenceKind, std::less<>> roles;
    std::map<std::string,std::size_t,std::less<>> wall_sizes;
    for (const auto& reference : relationships.references()) {
        (void)kind_name(reference.kind);
        roles.emplace(reference.id, reference.kind);
        wall_sizes.emplace(reference.id,room_reference_wall_ids(reference).size());
    }

    const auto index_records = [](const std::vector<RelationshipGeometry>& records,
                                  const char* side) {
        std::map<std::string, RelationshipGeometry, std::less<>> indexed;
        for (const auto& record : records) {
            (void)kind_name(record.kind);
            if (record.id.empty()) {
                throw std::invalid_argument(std::string("empty geometry identity in ") + side +
                                            " snapshot");
            }
            if (!indexed.emplace(record.id, record).second) {
                throw std::invalid_argument(std::string("duplicate geometry identity in ") + side +
                                            " snapshot");
            }
        }
        return indexed;
    };

    const auto before_records = index_records(before, "before");
    auto working_records = index_records(after, "after");
    RoomRelationshipGeometryResult result;
    std::set<std::string, std::less<>> diagnostic_keys;
    const auto add_diagnostic = [&](std::string diagnostic) {
        if (diagnostic_keys.insert(diagnostic).second) result.diagnostics.push_back(std::move(diagnostic));
    };
    const auto valid_record=[&](const RelationshipGeometry& record,RoomReferenceKind kind,const char* side) {
        return validate_record(record,kind,tolerance_metres,side,add_diagnostic,wall_sizes.at(record.id));
    };

    std::map<std::string, std::vector<std::string>, std::less<>> drivers;
    for (const auto& relation : relationships.relations()) {
        if (relation.kind != RoomRelationKind::independent) {
            drivers[relation.source_id].push_back(relation.target_id);
        }
    }
    for (auto& [source, targets] : drivers) {
        (void)source;
        std::sort(targets.begin(), targets.end());
    }

    std::map<std::string, RelationshipGeometryChange, std::less<>> changes;
    std::set<std::string, std::less<>> visiting;
    std::set<std::string, std::less<>> visited;
    std::set<std::string, std::less<>> blocked;

    std::function<void(const std::string&)> visit = [&](const std::string& source_id) {
        if (visited.contains(source_id)) return;
        if (!visiting.insert(source_id).second) {
            // RoomRelationshipSnapshot rejects cycles. Keep this defensive
            // branch so a future snapshot implementation cannot recurse.
            blocked.insert(source_id);
            add_diagnostic("relationship cycle prevents geometry propagation at " + source_id);
            return;
        }
        const auto driver_list = drivers.find(source_id);
        if (driver_list != drivers.end()) {
            for (const auto& target_id : driver_list->second) visit(target_id);

            bool usable = true;
            bool any_changed = false;
            std::vector<PlanarTransform> transforms;
            transforms.reserve(driver_list->second.size());
            for (const auto& target_id : driver_list->second) {
                const auto role = roles.find(target_id);
                if (role == roles.end()) {
                    add_diagnostic("relationship target " + target_id + " has no declared role");
                    usable = false;
                    continue;
                }
                if (blocked.contains(target_id)) {
                    add_diagnostic("cannot propagate through blocked relationship target " + target_id);
                    usable = false;
                    continue;
                }
                const auto before_it = before_records.find(target_id);
                const auto working_it = working_records.find(target_id);
                if (before_it == before_records.end()) {
                    add_diagnostic("missing before geometry for target " + target_id);
                    usable = false;
                    continue;
                }
                if (working_it == working_records.end()) {
                    add_diagnostic("missing after geometry for target " + target_id);
                    usable = false;
                    continue;
                }
                if (!valid_record(before_it->second,role->second,"before") ||
                    !valid_record(working_it->second,role->second,"after")) {
                    usable = false;
                    continue;
                }
                if (same_geometry(before_it->second.geometry, working_it->second.geometry,
                                  tolerance_metres)) {
                    transforms.push_back(identity_transform());
                    continue;
                }
                const auto derived = derive_rigid_transform(before_it->second.geometry,
                                                             working_it->second.geometry,
                                                             tolerance_metres);
                if (!derived.diagnostic.empty()) {
                    add_diagnostic("target " + target_id + ": " + derived.diagnostic);
                    usable = false;
                    continue;
                }
                any_changed = true;
                transforms.push_back(derived.transform);
            }

            if (!usable) blocked.insert(source_id);

            if (usable && any_changed) {
                bool compatible = true;
                for (std::size_t index = 1; index < transforms.size(); ++index) {
                    if (!same_transform(transforms.front(), transforms[index], tolerance_metres)) {
                        compatible = false;
                        break;
                    }
                }
                if (!compatible) {
                    add_diagnostic("source " + source_id +
                                   " has conflicting driver transforms");
                    blocked.insert(source_id);
                } else {
                    const auto role = roles.find(source_id);
                    const auto before_it = before_records.find(source_id);
                    const auto working_it = working_records.find(source_id);
                    if (role == roles.end()) {
                        add_diagnostic("relationship source " + source_id + " has no declared role");
                        blocked.insert(source_id);
                    } else if (before_it == before_records.end()) {
                        add_diagnostic("missing before geometry for source " + source_id);
                        blocked.insert(source_id);
                    } else if (working_it == working_records.end()) {
                        add_diagnostic("missing after geometry for source " + source_id);
                        blocked.insert(source_id);
                    } else if (!valid_record(before_it->second,role->second,"before") ||
                               !valid_record(working_it->second,role->second,"after")) {
                        blocked.insert(source_id);
                    } else {
                        Boundary candidate;
                        try {
                            candidate.reserve(before_it->second.geometry.size());
                            for (const auto& segment : before_it->second.geometry) {
                                candidate.push_back(transform_segment(segment, transforms.front()));
                            }
                        } catch (const std::invalid_argument&) {
                            add_diagnostic("source " + source_id +
                                           " cannot receive the rigid relationship transform");
                            blocked.insert(source_id);
                            candidate.clear();
                        }
                        const bool candidate_valid = !candidate.empty() &&
                            valid_record({source_id,role->second,candidate},role->second,"proposed");
                        if (candidate_valid) {
                            if (!same_geometry(working_it->second.geometry, candidate,
                                               tolerance_metres)) {
                                if (!same_geometry(before_it->second.geometry,
                                                   working_it->second.geometry,
                                                   tolerance_metres)) {
                                    add_diagnostic("source " + source_id +
                                                   " has an uncommitted edit that conflicts with relationship propagation");
                                    blocked.insert(source_id);
                                } else {
                                    working_records.at(source_id).geometry = candidate;
                                    RelationshipGeometryChange change{source_id, role->second,
                                                                      transforms.front(),
                                                                      std::move(candidate),
                                                                      driver_list->second};
                                    changes[source_id] = std::move(change);
                                }
                            }
                        } else {
                            blocked.insert(source_id);
                        }
                    }
                }
            }
        }
        visiting.erase(source_id);
        visited.insert(source_id);
    };

    for (const auto& reference : relationships.references()) visit(reference.id);

    for (auto& [source_id, change] : changes) {
        (void)source_id;
        std::sort(change.driver_ids.begin(), change.driver_ids.end());
    }
    for (const auto& [source_id, change] : changes) {
        (void)source_id;
        result.changes.push_back(change);
    }
    std::sort(result.diagnostics.begin(), result.diagnostics.end());
    return result;
}

} // namespace sketch
