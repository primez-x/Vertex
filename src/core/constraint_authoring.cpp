#include "sketch/constraint_authoring.hpp"

#include "sketch/constraint_integrity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sketch {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
using Entities = std::map<std::string, Entity, std::less<>>;

constexpr double kPointComparisonTolerance = 1e-10;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

double finite_number(const json& value, std::string_view description) {
    if (!value.is_number()) {
        invalid(std::string(description) + " must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        invalid(std::string(description) + " must be finite");
    }
    return result;
}

Vec2 point(const json& value, std::string_view description) {
    if (!value.is_array() || value.size() != 2) {
        invalid(std::string(description) + " must contain exactly two coordinates");
    }
    return {finite_number(value.at(0), description), finite_number(value.at(1), description)};
}

Segment read_baseline(const Entity& entity) {
    if (entity.type != "wall") {
        invalid("Constraint owner is not a wall: " + entity.id);
    }
    if (!entity.properties.is_object()) {
        invalid("Wall properties must be an object: " + entity.id);
    }
    try {
        const auto& baseline = entity.properties.at("baseline");
        if (!baseline.is_object()) {
            invalid("Wall baseline must be an object: " + entity.id);
        }
        return {point(baseline.at("start"), "Wall start"),
                point(baseline.at("end"), "Wall end"),
                finite_number(baseline.at("sweep_radians"), "Wall sweep")};
    } catch (const std::out_of_range&) {
        invalid("Wall baseline is missing required geometry: " + entity.id);
    }
}

const Entity& require_wall(const Entities& entities, const std::string& id) {
    const auto found = entities.find(id);
    if (found == entities.end()) {
        invalid("Constraint owner wall does not exist: " + id);
    }
    if (found->second.type != "wall") {
        invalid("Constraint owner is not a wall: " + id);
    }
    const auto baseline = read_baseline(found->second);
    if (baseline.sweep_radians != 0.0) {
        invalid("Straight-wall constraint authoring does not support curved wall: " + id);
    }
    return found->second;
}

void validate_wall_host(const std::string& wall_id, const Entities& entities) {
    const auto& entity = require_wall(entities, wall_id);
    const auto baseline = read_baseline(entity);
    try {
        Wall wall{
            wall_id,
            baseline,
            finite_number(entity.properties.at("thickness_m"), "Wall thickness"),
            finite_number(entity.properties.at("height_m"), "Wall height"),
            finite_number(entity.properties.at("elevation_m"), "Wall elevation"),
            {},
        };
        for (const auto& [id, candidate] : entities) {
            if (candidate.type != "opening" || !candidate.properties.is_object()) {
                continue;
            }
            const auto host = candidate.properties.find("wall_id");
            if (host == candidate.properties.end() || !host->is_string() ||
                host->get_ref<const std::string&>() != wall_id) {
                continue;
            }
            wall.openings.push_back(
                {id, finite_number(candidate.properties.at("offset_m"), "Opening offset"),
                 finite_number(candidate.properties.at("width_m"), "Opening width"),
                 finite_number(candidate.properties.at("sill_m"), "Opening sill"),
                 finite_number(candidate.properties.at("height_m"), "Opening height")});
        }
        validate_wall_semantics(wall);
    } catch (const std::out_of_range&) {
        invalid("Wall or hosted opening is missing required geometry: " + wall_id);
    }
}

std::string unit_name(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return "m";
        case Unit::millimetre:
            return "mm";
        case Unit::centimetre:
            return "cm";
        case Unit::foot:
            return "ft";
        case Unit::inch:
            return "in";
    }
    invalid("Unknown quantity unit");
}

Quantity normalize_positive_quantity(const Quantity& value) {
    Quantity parsed;
    try {
        parsed = parse_quantity(value.original_expression, value.entered_unit);
    } catch (const std::exception&) {
        invalid("Wall length quantity is not exactly parseable");
    }
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit ||
        parsed.original_expression != value.original_expression || !(parsed.metres > 0.0) ||
        !std::isfinite(parsed.metres)) {
        invalid("Wall length quantity is internally inconsistent or non-positive");
    }
    return parsed;
}

std::string digest_json(const ordered_json& value) {
    const auto encoded = value.dump();
    return sha256_hex(std::as_bytes(std::span(encoded.data(), encoded.size())));
}

ordered_json segment_json(const Segment& segment) {
    return {{"start", {segment.start.x, segment.start.y}},
            {"end", {segment.end.x, segment.end.y}},
            {"sweep_radians", segment.sweep_radians}};
}

std::string digest_shown_result(const ConstraintAuthoringPreview& preview) {
    auto changes = ordered_json::array();
    for (const auto& change : preview.changed_walls()) {
        changes.push_back({{"wall_id", change.wall_id},
                           {"old", segment_json(change.old_baseline)},
                           {"proposed", segment_json(change.proposed_baseline)}});
    }
    return digest_json({{"accepted", preview.accepted()},
                        {"document_id", preview.document_id()},
                        {"revision", preview.expected_revision()},
                        {"source_digest", preview.source_snapshot_digest()},
                        {"candidate_digest", preview.candidate_digest()},
                        {"changes", std::move(changes)},
                        {"diagnostics", preview.diagnostics()}});
}

std::string point_id(const WallEndpointBinding& binding) {
    return std::to_string(binding.owner_id.size()) + ":" + binding.owner_id +
        (binding.role == WallEndpointRole::start ? ":start" : ":end");
}

Vec2 endpoint_position(const Segment& baseline, WallEndpointRole role) {
    return role == WallEndpointRole::start ? baseline.start : baseline.end;
}

void validate_binding(const WallEndpointBinding& binding) {
    if (binding.owner_id.empty()) {
        invalid("Constraint endpoint owner id cannot be empty");
    }
    if (binding.role != WallEndpointRole::start && binding.role != WallEndpointRole::end) {
        invalid("Constraint endpoint role is invalid");
    }
}

ConstraintAuthoringIntent normalize_intent(const ConstraintAuthoringIntent& input) {
    ConstraintAuthoringIntent result = input;
    if (result.wall_resize.has_value()) {
        if (result.wall_resize->wall_id.empty()) {
            invalid("Wall resize id cannot be empty");
        }
        result.wall_resize->exact_length =
            normalize_positive_quantity(result.wall_resize->exact_length);
        if (result.wall_resize->anchored_endpoint != WallResizeAnchor::start &&
            result.wall_resize->anchored_endpoint != WallResizeAnchor::end) {
            invalid("Wall resize anchor is invalid");
        }
    }
    if (result.relation_anchor.has_value()) {
        validate_binding(*result.relation_anchor);
    }
    std::set<std::string, std::less<>> mutation_ids;
    for (auto& mutation : result.relation_mutations) {
        if (mutation.kind == ConstraintRelationMutationKind::upsert) {
            if (mutation.constraint.id.empty()) {
                invalid("Constraint upsert id cannot be empty");
            }
            mutation.constraint_id = mutation.constraint.id;
            (void)encode_constraint_entity(mutation.constraint);
        } else if (mutation.kind == ConstraintRelationMutationKind::remove) {
            if (mutation.constraint_id.empty()) {
                invalid("Constraint removal id cannot be empty");
            }
        } else {
            invalid("Constraint relation mutation kind is invalid");
        }
        if (!mutation_ids.insert(mutation.constraint_id).second) {
            invalid("Constraint relation mutation ids must be unique");
        }
    }
    if (!result.wall_resize.has_value() && result.relation_mutations.empty()) {
        invalid("Constraint authoring intent has no changes");
    }
    if (result.message.empty()) {
        result.message = "author persistent constraints";
    }
    return result;
}

std::map<std::string, PersistentConstraint, std::less<>>
decode_supported_constraints(const Entities& entities) {
    std::map<std::string, PersistentConstraint, std::less<>> result;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "constraint") {
            continue;
        }
        const auto decoded = decode_constraint_entity(entity);
        if (!decoded.constraint.has_value()) {
            invalid("Unsupported persistent constraint cannot be authored: " + id + ": " +
                    decoded.unsupported_reason);
        }
        result.emplace(id, *decoded.constraint);
    }
    return result;
}

void append_relation(ConstraintSolveRequest& request, const PersistentConstraint& value) {
    const auto id = value.id;
    const auto point_at = [&](std::size_t index) { return point_id(value.bindings.at(index)); };
    switch (value.relation) {
        case ConstraintRelationKind::horizontal:
            request.constraints.push_back(HorizontalConstraint{id, point_at(0), point_at(1)});
            break;
        case ConstraintRelationKind::vertical:
            request.constraints.push_back(VerticalConstraint{id, point_at(0), point_at(1)});
            break;
        case ConstraintRelationKind::coincident:
            request.constraints.push_back(CoincidentConstraint{id, point_at(0), point_at(1)});
            break;
        case ConstraintRelationKind::fixed_length:
            if (!value.length.has_value()) {
                invalid("Fixed length constraint is missing its exact quantity");
            }
            request.constraints.push_back(
                FixedLengthConstraint{id, point_at(0), point_at(1), value.length->metres});
            break;
        case ConstraintRelationKind::parallel:
            request.constraints.push_back(
                ParallelConstraint{id, point_at(0), point_at(1), point_at(2), point_at(3)});
            break;
        case ConstraintRelationKind::perpendicular:
            request.constraints.push_back(
                PerpendicularConstraint{id, point_at(0), point_at(1), point_at(2), point_at(3)});
            break;
        case ConstraintRelationKind::fixed_anchor:
            if (!value.anchor.has_value()) {
                invalid("Fixed anchor constraint is missing its position");
            }
            request.constraints.push_back(
                FixedAnchorConstraint{id, point_at(0), value.anchor->x, value.anchor->y});
            break;
    }
}

struct DisjointPoints {
    std::map<std::string, std::string, std::less<>> parent;

    void add(const std::string& value) { parent.try_emplace(value, value); }

    std::string root(const std::string& value) {
        auto found = parent.find(value);
        if (found == parent.end()) {
            add(value);
            return value;
        }
        if (found->second == value) {
            return value;
        }
        found->second = root(found->second);
        return found->second;
    }

    void unite(const std::string& first, const std::string& second) {
        auto first_root = root(first);
        auto second_root = root(second);
        if (first_root == second_root) {
            return;
        }
        if (second_root < first_root) {
            std::swap(first_root, second_root);
        }
        parent[second_root] = first_root;
    }
};

void append_winding_invariants(
    ConstraintSolveRequest& request,
    const std::set<std::string, std::less<>>& affected_walls,
    const std::map<std::string, Segment, std::less<>>& old_baselines,
    const std::map<std::string, PersistentConstraint, std::less<>>& constraints) {
    DisjointPoints points;
    for (const auto& wall_id : affected_walls) {
        points.add(point_id({wall_id, WallEndpointRole::start}));
        points.add(point_id({wall_id, WallEndpointRole::end}));
    }
    for (const auto& [id, constraint] : constraints) {
        (void)id;
        if (constraint.relation == ConstraintRelationKind::coincident &&
            affected_walls.contains(constraint.bindings.at(0).owner_id)) {
            points.unite(point_id(constraint.bindings.at(0)), point_id(constraint.bindings.at(1)));
        }
    }

    struct Edge {
        std::string wall_id;
        std::string first;
        std::string second;
    };
    std::vector<Edge> edges;
    std::map<std::string, std::vector<std::size_t>, std::less<>> incident;
    for (const auto& wall_id : affected_walls) {
        auto first = points.root(point_id({wall_id, WallEndpointRole::start}));
        auto second = points.root(point_id({wall_id, WallEndpointRole::end}));
        if (first == second) {
            continue;
        }
        const auto index = edges.size();
        edges.push_back({wall_id, first, second});
        incident[first].push_back(index);
        incident[second].push_back(index);
    }

    std::set<std::string, std::less<>> visited_vertices;
    for (const auto& [start, start_edges] : incident) {
        if (visited_vertices.contains(start)) {
            continue;
        }
        std::set<std::string, std::less<>> component_vertices;
        std::set<std::size_t> component_edges;
        std::vector<std::string> queue{start};
        while (!queue.empty()) {
            auto vertex = std::move(queue.back());
            queue.pop_back();
            if (!component_vertices.insert(vertex).second) {
                continue;
            }
            visited_vertices.insert(vertex);
            for (const auto edge_index : incident.at(vertex)) {
                component_edges.insert(edge_index);
                const auto& edge = edges[edge_index];
                queue.push_back(edge.first == vertex ? edge.second : edge.first);
            }
        }
        if (component_vertices.size() < 3 || component_edges.size() != component_vertices.size() ||
            std::any_of(component_vertices.begin(), component_vertices.end(),
                        [&](const std::string& vertex) { return incident.at(vertex).size() != 2; })) {
            continue;
        }

        std::vector<std::string> loop_vertices;
        std::set<std::size_t> used_edges;
        std::string current = *component_vertices.begin();
        std::string previous;
        while (loop_vertices.size() < component_vertices.size()) {
            loop_vertices.push_back(current);
            std::optional<std::size_t> chosen;
            for (const auto edge_index : incident.at(current)) {
                if (!used_edges.contains(edge_index)) {
                    chosen = edge_index;
                    break;
                }
            }
            if (!chosen.has_value()) {
                loop_vertices.clear();
                break;
            }
            used_edges.insert(*chosen);
            const auto& edge = edges[*chosen];
            previous = current;
            current = edge.first == current ? edge.second : edge.first;
            (void)previous;
        }
        if (loop_vertices.size() != component_vertices.size() ||
            current != loop_vertices.front()) {
            continue;
        }

        std::map<std::string, std::string, std::less<>> representative;
        std::map<std::string, Vec2, std::less<>> coordinates;
        for (const auto& wall_id : affected_walls) {
            for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                const auto semantic_id = point_id({wall_id, role});
                const auto vertex = points.root(semantic_id);
                const auto coordinate = endpoint_position(old_baselines.at(wall_id), role);
                if (!representative.contains(vertex) || semantic_id < representative.at(vertex)) {
                    representative[vertex] = semantic_id;
                    coordinates[vertex] = coordinate;
                }
            }
        }
        long double twice_area = 0.0L;
        for (std::size_t index = 0; index < loop_vertices.size(); ++index) {
            const auto first = coordinates.at(loop_vertices[index]);
            const auto second = coordinates.at(loop_vertices[(index + 1) % loop_vertices.size()]);
            twice_area += static_cast<long double>(first.x) * static_cast<long double>(second.y) -
                          static_cast<long double>(second.x) * static_cast<long double>(first.y);
        }
        if (!std::isfinite(twice_area) || std::abs(twice_area) <=
                static_cast<long double>(constraint_linear_tolerance_metres) *
                    constraint_linear_tolerance_metres) {
            continue;
        }
        WindingInvariant invariant;
        invariant.orientation = twice_area > 0.0L ? WindingOrientation::counter_clockwise
                                                   : WindingOrientation::clockwise;
        for (const auto& vertex : loop_vertices) {
            invariant.loop.push_back(representative.at(vertex));
        }
        request.winding_invariants.push_back(std::move(invariant));
    }
}

std::string unique_temporary_id(const std::set<std::string, std::less<>>& persistent_ids,
                                std::size_t index) {
    auto id = std::string("__constraint_authoring_anchor_") + std::to_string(index);
    while (persistent_ids.contains(id)) {
        id.push_back('_');
    }
    return id;
}

bool points_near(Vec2 first, Vec2 second, double tolerance = kPointComparisonTolerance) {
    return std::isfinite(first.x) && std::isfinite(first.y) && std::isfinite(second.x) &&
        std::isfinite(second.y) &&
        std::hypot(first.x - second.x, first.y - second.y) <= tolerance;
}

void update_baseline_json(json& target, const Segment& baseline) {
    target["start"] = {baseline.start.x, baseline.start.y};
    target["end"] = {baseline.end.x, baseline.end.y};
    target["sweep_radians"] = baseline.sweep_radians;
}

void set_baseline(Entity& wall, const Segment& baseline) {
    update_baseline_json(wall.properties.at("baseline"), baseline);
}

// Unknown members are opaque metadata. Updating recognized fields preserves
// them; invalidating a receipt containing them must fail closed rather than
// destroy data whose meaning this version cannot establish.
bool validate_length_receipt(const json& receipt, const Entity& wall) {
    if (!receipt.is_object() || !receipt.contains("version") ||
        !receipt.at("version").is_number_integer() || receipt.at("version") != 1) {
        invalid("Wall has unsupported last_length_entry extension metadata: " + wall.id);
    }
    try {
        const auto& expression = receipt.at("original_expression");
        const auto& entered_unit = receipt.at("entered_unit");
        if (!expression.is_string() || !entered_unit.is_string()) {
            invalid("Wall length receipt expression and unit must be strings");
        }
        std::optional<Unit> unit;
        for (const auto candidate : {Unit::metre, Unit::millimetre, Unit::centimetre,
                                     Unit::foot, Unit::inch}) {
            if (entered_unit == unit_name(candidate)) {
                unit = candidate;
                break;
            }
        }
        if (!unit) {
            invalid("Wall length receipt has an unsupported entered unit");
        }
        const auto quantity = parse_quantity(expression.get_ref<const std::string&>(), *unit);
        const auto& exact = receipt.at("exact_metres");
        if (quantity.entered_unit != *unit || !std::isfinite(quantity.metres) ||
            quantity.metres <= 0.0 || !exact.is_object() ||
            !exact.at("numerator").is_number_integer() ||
            !exact.at("denominator").is_number_integer() ||
            exact.at("numerator") != quantity.exact_metres.numerator ||
            exact.at("denominator") != quantity.exact_metres.denominator) {
            invalid("Wall length receipt has inconsistent exact quantity metadata");
        }
        auto receipt_wall = wall;
        receipt_wall.properties["baseline"] = receipt.at("baseline");
        const auto recorded = read_baseline(receipt_wall);
        const auto current = read_baseline(wall);
        // Receipts store the coordinates produced by the command, not a
        // measurement approximation; any subsequent coordinate change stales it.
        if (recorded.sweep_radians != 0.0 || current.sweep_radians != 0.0 ||
            recorded.start.x != current.start.x || recorded.start.y != current.start.y ||
            recorded.end.x != current.end.x || recorded.end.y != current.end.y ||
            std::abs(std::hypot(recorded.end.x - recorded.start.x,
                                recorded.end.y - recorded.start.y) - quantity.metres) >
                constraint_linear_tolerance_metres) {
            invalid("Wall length receipt does not match its stored baseline");
        }
        return receipt.size() != 5 || exact.size() != 2 || receipt.at("baseline").size() != 3;
    } catch (const json::exception&) {
        invalid("Wall length receipt is missing or has malformed required metadata: " + wall.id);
    }
}

void validate_or_clear_length_receipt(Entity& wall, bool write_receipt,
                                      const Quantity* quantity, const Segment& baseline) {
    auto section = wall.extensions.find("constraint_authoring");
    if (section != wall.extensions.end()) {
        if (!section->is_object() || !section->contains("version") ||
            !section->at("version").is_number_integer() || section->at("version") != 1) {
            invalid("Wall has unsupported constraint_authoring extension metadata: " + wall.id);
        }
        const auto receipt = section->find("last_length_entry");
        if (receipt != section->end()) {
            const bool opaque_metadata = validate_length_receipt(*receipt, wall);
            if (!write_receipt && opaque_metadata) {
                invalid("Wall edit would discard unsupported last_length_entry metadata: " + wall.id);
            }
        }
    }
    if (write_receipt) {
        if (quantity == nullptr) {
            invalid("Wall length receipt is missing its exact quantity");
        }
        if (section == wall.extensions.end()) {
            wall.extensions["constraint_authoring"] = ordered_json{{"version", 1}};
            section = wall.extensions.find("constraint_authoring");
        }
        auto& receipt = (*section)["last_length_entry"];
        receipt["version"] = 1;
        receipt["original_expression"] = quantity->original_expression;
        receipt["entered_unit"] = unit_name(quantity->entered_unit);
        receipt["exact_metres"]["numerator"] = quantity->exact_metres.numerator;
        receipt["exact_metres"]["denominator"] = quantity->exact_metres.denominator;
        update_baseline_json(receipt["baseline"], baseline);
    } else if (section != wall.extensions.end()) {
        section->erase("last_length_entry");
    }
}

enum class IntersectionKind { none, touch, proper, overlap };

long double cross(Vec2 a, Vec2 b, Vec2 c) {
    return (static_cast<long double>(b.x) - a.x) *
               (static_cast<long double>(c.y) - a.y) -
           (static_cast<long double>(b.y) - a.y) *
               (static_cast<long double>(c.x) - a.x);
}

int side_sign(Vec2 first, Vec2 second, Vec2 value) {
    const long double dx = static_cast<long double>(second.x) - first.x;
    const long double dy = static_cast<long double>(second.y) - first.y;
    const long double line_length = std::hypot(dx, dy);
    if (!std::isfinite(line_length) || line_length <= default_geometry_tolerance_metres) {
        invalid("Topology classification requires a finite non-degenerate wall");
    }
    const long double signed_distance = cross(first, second, value) / line_length;
    constexpr long double tolerance = constraint_linear_tolerance_metres;
    return signed_distance > tolerance ? 1 : (signed_distance < -tolerance ? -1 : 0);
}

bool within(double value, double first, double second) {
    const auto low = std::min(first, second) - constraint_linear_tolerance_metres;
    const auto high = std::max(first, second) + constraint_linear_tolerance_metres;
    return value >= low && value <= high;
}

bool on_segment(Vec2 point_value, Vec2 first, Vec2 second) {
    return side_sign(first, second, point_value) == 0 &&
        within(point_value.x, first.x, second.x) && within(point_value.y, first.y, second.y);
}

IntersectionKind intersection_kind(const Segment& first, const Segment& second) {
    const auto o1 = side_sign(first.start, first.end, second.start);
    const auto o2 = side_sign(first.start, first.end, second.end);
    const auto o3 = side_sign(second.start, second.end, first.start);
    const auto o4 = side_sign(second.start, second.end, first.end);
    if (o1 * o2 < 0 && o3 * o4 < 0) {
        return IntersectionKind::proper;
    }
    const bool touches = (o1 == 0 && on_segment(second.start, first.start, first.end)) ||
        (o2 == 0 && on_segment(second.end, first.start, first.end)) ||
        (o3 == 0 && on_segment(first.start, second.start, second.end)) ||
        (o4 == 0 && on_segment(first.end, second.start, second.end));
    if (!touches) {
        return IntersectionKind::none;
    }
    if (o1 == 0 && o2 == 0 && o3 == 0 && o4 == 0) {
        const auto use_x = std::abs(first.end.x - first.start.x) >=
            std::abs(first.end.y - first.start.y);
        const auto a0 = use_x ? first.start.x : first.start.y;
        const auto a1 = use_x ? first.end.x : first.end.y;
        const auto b0 = use_x ? second.start.x : second.start.y;
        const auto b1 = use_x ? second.end.x : second.end.y;
        const auto overlap = std::min(std::max(a0, a1), std::max(b0, b1)) -
            std::max(std::min(a0, a1), std::min(b0, b1));
        if (overlap > constraint_linear_tolerance_metres) {
            return IntersectionKind::overlap;
        }
    }
    return IntersectionKind::touch;
}

bool explicitly_coincident_endpoints(
    const std::string& first_wall, const Segment& first,
    const std::string& second_wall, const Segment& second,
    const std::map<std::string, PersistentConstraint, std::less<>>& constraints) {
    for (const auto& [id, value] : constraints) {
        (void)id;
        if (value.relation != ConstraintRelationKind::coincident) {
            continue;
        }
        const auto& a = value.bindings.at(0);
        const auto& b = value.bindings.at(1);
        const bool owners_match =
            (a.owner_id == first_wall && b.owner_id == second_wall) ||
            (a.owner_id == second_wall && b.owner_id == first_wall);
        if (!owners_match) {
            continue;
        }
        const auto& a_segment = a.owner_id == first_wall ? first : second;
        const auto& b_segment = b.owner_id == first_wall ? first : second;
        if (points_near(endpoint_position(a_segment, a.role),
                        endpoint_position(b_segment, b.role),
                        constraint_linear_tolerance_metres)) {
            return true;
        }
    }
    return false;
}

bool has_organization_reference(const Entity& entity) {
    return entity.properties.contains("layer_id") || entity.properties.contains("floor_id") ||
        entity.properties.contains("building_id") || entity.properties.contains("property_id");
}

std::optional<DrawingContext> resolved_context(const Entity& entity,
                                               const ProjectOrganization& organization) {
    const auto context = organization.drawing_context(entity.id);
    if (has_organization_reference(entity) && !context.has_value()) {
        invalid("Wall has an unresolved explicit drawing context: " + entity.id);
    }
    return context;
}

bool same_drawing_plane(const Entity& first, const Entity& second,
                        const ProjectOrganization& organization) {
    const auto first_context = resolved_context(first, organization);
    const auto second_context = resolved_context(second, organization);
    if (first_context.has_value() && second_context.has_value() &&
        first_context->floor_id != second_context->floor_id) {
        return false;
    }
    const auto first_elevation =
        finite_number(first.properties.at("elevation_m"), "Wall elevation");
    const auto second_elevation =
        finite_number(second.properties.at("elevation_m"), "Wall elevation");
    const auto first_height = finite_number(first.properties.at("height_m"), "Wall height");
    const auto second_height = finite_number(second.properties.at("height_m"), "Wall height");
    const long double first_top = static_cast<long double>(first_elevation) + first_height;
    const long double second_top = static_cast<long double>(second_elevation) + second_height;
    if (!std::isfinite(first_top) || !std::isfinite(second_top)) {
        invalid("Wall vertical extent exceeds the supported numeric range");
    }
    return std::max(static_cast<long double>(first_elevation),
                    static_cast<long double>(second_elevation)) <
        std::min(first_top, second_top) + constraint_linear_tolerance_metres;
}

void validate_topology(
    const Entities& before, const Entities& after,
    const std::set<std::string, std::less<>>& changed_walls,
    const std::map<std::string, PersistentConstraint, std::less<>>& constraints,
    const ProjectOrganization& organization) {
    std::set<std::pair<std::string, std::string>> checked_pairs;
    for (const auto& changed_id : changed_walls) {
        for (const auto& [other_id, other_entity] : after) {
            if (other_id == changed_id || other_entity.type != "wall") {
                continue;
            }
            const auto pair = std::minmax(changed_id, other_id);
            if (!checked_pairs.emplace(pair.first, pair.second).second) {
                continue;
            }
            const auto& first_id = pair.first;
            const auto& second_id = pair.second;
            if (!before.contains(first_id) || !before.contains(second_id) ||
                before.at(first_id).type != "wall" || before.at(second_id).type != "wall") {
                invalid("Constraint solve cannot change wall topology across unknown owners");
            }
            if (!same_drawing_plane(after.at(first_id), after.at(second_id), organization)) {
                continue;
            }
            const auto old_first = read_baseline(before.at(first_id));
            const auto old_second = read_baseline(before.at(second_id));
            if (old_first.sweep_radians != 0.0 || old_second.sweep_radians != 0.0) {
                continue;
            }
            const auto new_first = read_baseline(after.at(first_id));
            const auto new_second = read_baseline(after.at(second_id));
            const auto old_kind = intersection_kind(old_first, old_second);
            const auto new_kind = intersection_kind(new_first, new_second);
            if ((old_kind == IntersectionKind::proper) != (new_kind == IntersectionKind::proper) ||
                (old_kind == IntersectionKind::overlap) != (new_kind == IntersectionKind::overlap)) {
                invalid("Constraint solve would change wall crossing or overlap topology");
            }
            if (old_kind == IntersectionKind::none && new_kind == IntersectionKind::touch &&
                !explicitly_coincident_endpoints(first_id, new_first, second_id, new_second,
                                                 constraints)) {
                invalid("Constraint solve would create an implicit coordinate-only wall connection");
            }
        }
    }
}

bool baseline_same(const Segment& first, const Segment& second) {
    return first.sweep_radians == second.sweep_radians && points_near(first.start, second.start) &&
        points_near(first.end, second.end);
}

void validate_endpoint_identity_not_swapped(const Segment& before, const Segment& after,
                                            const std::string& wall_id) {
    if (points_near(before.start, after.end, constraint_linear_tolerance_metres) &&
        points_near(before.end, after.start, constraint_linear_tolerance_metres)) {
        invalid("Constraint solve would reverse wall endpoint identity: " + wall_id);
    }
}

std::string wall_display_name(const Entities& entities, const std::string& wall_id) {
    const auto& wall = require_wall(entities, wall_id);
    if (wall.properties.is_object()) {
        const auto name = wall.properties.find("name");
        if (name != wall.properties.end() && name->is_string() &&
            !name->get_ref<const std::string&>().empty()) {
            return name->get<std::string>();
        }
    }
    return wall_id;
}

std::string endpoint_description(const Entities& entities,
                                 const WallEndpointBinding& binding) {
    return wall_display_name(entities, binding.owner_id) + " " +
        std::string(wall_endpoint_role_name(binding.role));
}

std::string segment_description(const Entities& entities,
                                const WallEndpointBinding& first,
                                const WallEndpointBinding& second) {
    if (first.owner_id == second.owner_id) {
        return wall_display_name(entities, first.owner_id);
    }
    return endpoint_description(entities, first) + " to " +
        endpoint_description(entities, second);
}

std::string relation_description(const Entities& entities,
                                 const PersistentConstraint& constraint) {
    const auto point_at = [&](std::size_t index) {
        return endpoint_description(entities, constraint.bindings.at(index));
    };
    switch (constraint.relation) {
        case ConstraintRelationKind::horizontal:
            return "horizontal relation: " + point_at(0) + " to " + point_at(1);
        case ConstraintRelationKind::vertical:
            return "vertical relation: " + point_at(0) + " to " + point_at(1);
        case ConstraintRelationKind::coincident:
            return "coincident relation: " + point_at(0) + " and " + point_at(1);
        case ConstraintRelationKind::fixed_length: {
            const auto quantity = constraint.length.has_value()
                ? constraint.length->original_expression
                : std::string{};
            const auto measured = quantity.empty() ? "exact value" : quantity;
            if (constraint.bindings.at(0).owner_id == constraint.bindings.at(1).owner_id) {
                return "fixed length (" + measured + ") on " +
                    wall_display_name(entities, constraint.bindings.at(0).owner_id);
            }
            return "fixed length (" + measured + ") between " + point_at(0) + " and " +
                point_at(1);
        }
        case ConstraintRelationKind::parallel:
            return "parallel relation between " +
                segment_description(entities, constraint.bindings.at(0),
                                    constraint.bindings.at(1)) +
                " and " + segment_description(entities, constraint.bindings.at(2),
                                               constraint.bindings.at(3));
        case ConstraintRelationKind::perpendicular:
            return "perpendicular relation between " +
                segment_description(entities, constraint.bindings.at(0),
                                    constraint.bindings.at(1)) +
                " and " + segment_description(entities, constraint.bindings.at(2),
                                               constraint.bindings.at(3));
        case ConstraintRelationKind::fixed_anchor:
            return "fixed endpoint: " + point_at(0);
    }
    invalid("Unknown persistent relation kind in solver diagnostic mapping");
}

std::string diagnostic_without_constraint(const ConstraintDiagnostic& diagnostic) {
    switch (diagnostic.kind) {
        case ConstraintDiagnosticKind::invalid_input:
            return "Constraint request is invalid";
        case ConstraintDiagnosticKind::underconstrained:
            return "Candidate geometry remains underconstrained";
        case ConstraintDiagnosticKind::solver_failure:
            return "Constraint solver could not produce a valid result";
        case ConstraintDiagnosticKind::conflicting:
            return "Constraints conflict";
        case ConstraintDiagnosticKind::redundant:
            return "Constraint request contains a redundant relation";
        case ConstraintDiagnosticKind::residual_exceeded:
            return "A constraint could not be satisfied within tolerance";
        case ConstraintDiagnosticKind::anchor_moved:
            return "A fixed endpoint could not remain in place";
        case ConstraintDiagnosticKind::orientation_changed:
            return "Candidate geometry would change or collapse protected boundary winding";
    }
    return diagnostic.message.empty() ? "Constraint solver reported an unknown problem"
                                      : diagnostic.message;
}

std::string diagnostic_with_constraint(ConstraintDiagnosticKind kind,
                                       const std::string& description) {
    switch (kind) {
        case ConstraintDiagnosticKind::invalid_input:
            return "Invalid " + description;
        case ConstraintDiagnosticKind::underconstrained:
            return "Underconstrained " + description;
        case ConstraintDiagnosticKind::solver_failure:
            return "Could not solve " + description;
        case ConstraintDiagnosticKind::conflicting:
            return "Conflicting " + description;
        case ConstraintDiagnosticKind::redundant:
            return "Redundant " + description;
        case ConstraintDiagnosticKind::residual_exceeded:
            return "Unsatisfied " + description;
        case ConstraintDiagnosticKind::anchor_moved:
            return "Could not preserve " + description;
        case ConstraintDiagnosticKind::orientation_changed:
            return "Protected orientation changed while applying " + description;
    }
    return "Constraint problem: " + description;
}

using SolverConstraintDescriptions =
    std::map<std::string, std::string, std::less<>>;

std::vector<std::string> solver_diagnostics(
    const ConstraintPreview& preview,
    const SolverConstraintDescriptions& descriptions) {
    std::vector<std::string> result;
    result.reserve(preview.diagnostics.size());
    for (const auto& diagnostic : preview.diagnostics) {
        if (diagnostic.constraint_id.has_value()) {
            const auto found = descriptions.find(*diagnostic.constraint_id);
            if (found != descriptions.end()) {
                result.push_back(diagnostic_with_constraint(diagnostic.kind, found->second));
                continue;
            }
        }
        result.push_back(diagnostic_without_constraint(diagnostic));
    }
    return result;
}

}  // namespace

void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline) {
    auto section = wall.extensions.find("constraint_authoring");
    if (section == wall.extensions.end()) {
        return;
    }
    if (!section->is_object() || !section->contains("version") ||
        !section->at("version").is_number_integer() || section->at("version") != 1) {
        invalid("Wall has unsupported constraint_authoring extension metadata: " + wall.id);
    }
    auto receipt = section->find("last_length_entry");
    if (receipt == section->end()) {
        return;
    }
    (void)validate_length_receipt(*receipt, wall);
    const auto original = read_baseline(wall);
    const auto& transformed = transformed_baseline;
    const auto original_length = std::hypot(original.end.x - original.start.x,
                                             original.end.y - original.start.y);
    const auto transformed_length = std::hypot(transformed.end.x - transformed.start.x,
                                                transformed.end.y - transformed.start.y);
    if (transformed.sweep_radians != 0.0 ||
        !std::isfinite(transformed.start.x) || !std::isfinite(transformed.start.y) ||
        !std::isfinite(transformed.end.x) || !std::isfinite(transformed.end.y) ||
        !std::isfinite(original_length) || !std::isfinite(transformed_length) ||
        transformed_length <= 0.0 ||
        std::abs(transformed_length - original_length) > constraint_linear_tolerance_metres) {
        invalid("Wall length receipt requires a finite length-preserving straight transform: " + wall.id);
    }
    auto updated = *receipt;
    update_baseline_json(updated.at("baseline"), transformed);
    auto transformed_wall = wall;
    set_baseline(transformed_wall, transformed);
    (void)validate_length_receipt(updated, transformed_wall);
    receipt->swap(updated);
}

class ConstraintAuthoringBuilder final {
public:
    static ConstraintAuthoringPreview build(const DocumentSnapshot& snapshot,
                                             const ConstraintAuthoringIntent& raw_intent);
};

ConstraintAuthoringPreview ConstraintAuthoringBuilder::build(
    const DocumentSnapshot& snapshot, const ConstraintAuthoringIntent& raw_intent) {
    ConstraintAuthoringPreview result;
    result.document_id_ = snapshot.document_id();
    result.expected_revision_ = snapshot.revision();
    result.source_snapshot_digest_ = document_snapshot_digest(snapshot);
    result.candidate_entities_ = snapshot.entities();

    try {
        if (!snapshot.is_editable()) {
            invalid(snapshot.read_only_reason().empty() ? "Document is read-only"
                                                        : snapshot.read_only_reason());
        }
        result.normalized_intent_ = normalize_intent(raw_intent);
        const auto& intent = result.normalized_intent_;
        const auto organization = organize_project(snapshot);
        const auto before_constraints = decode_supported_constraints(snapshot.entities());
        auto candidate = snapshot.entities();
        std::set<std::string, std::less<>> seeds;
        bool has_upsert = false;

        for (const auto& mutation : intent.relation_mutations) {
            const auto existing = candidate.find(mutation.constraint_id);
            if (mutation.kind == ConstraintRelationMutationKind::remove) {
                if (existing == candidate.end() || existing->second.type != "constraint") {
                    invalid("Constraint removal target does not exist: " + mutation.constraint_id);
                }
                const auto decoded = decode_constraint_entity(existing->second);
                if (!decoded.constraint.has_value()) {
                    invalid("Unsupported constraint cannot be removed through known authoring");
                }
                for (const auto& binding : decoded.constraint->bindings) {
                    seeds.insert(binding.owner_id);
                }
                candidate.erase(existing);
                continue;
            }
            has_upsert = true;
            const Entity* original = nullptr;
            if (existing != candidate.end()) {
                if (existing->second.type != "constraint") {
                    invalid("Constraint upsert id belongs to a different entity type: " +
                            mutation.constraint_id);
                }
                const auto decoded = decode_constraint_entity(existing->second);
                if (!decoded.constraint.has_value()) {
                    invalid("Unsupported constraint cannot be replaced through known authoring");
                }
                for (const auto& binding : decoded.constraint->bindings) {
                    seeds.insert(binding.owner_id);
                }
                original = &existing->second;
            }
            for (const auto& binding : mutation.constraint.bindings) {
                seeds.insert(binding.owner_id);
            }
            candidate[mutation.constraint_id] =
                encode_constraint_entity(mutation.constraint, original);
        }
        if (intent.wall_resize.has_value()) {
            seeds.insert(intent.wall_resize->wall_id);
        }
        const auto constraints = decode_supported_constraints(candidate);
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> adjacency;
        for (const auto& [id, value] : constraints) {
            (void)id;
            std::set<std::string, std::less<>> owners;
            for (const auto& binding : value.bindings) {
                validate_binding(binding);
                (void)require_wall(candidate, binding.owner_id);
                owners.insert(binding.owner_id);
            }
            for (const auto& first : owners) {
                adjacency[first];
                for (const auto& second : owners) {
                    if (first != second) {
                        adjacency[first].insert(second);
                    }
                }
            }
        }
        for (const auto& seed : seeds) {
            (void)require_wall(candidate, seed);
            adjacency[seed];
        }

        std::set<std::string, std::less<>> affected;
        std::vector<std::string> queue(seeds.begin(), seeds.end());
        while (!queue.empty()) {
            auto id = std::move(queue.back());
            queue.pop_back();
            if (!affected.insert(id).second) {
                continue;
            }
            for (const auto& neighbor : adjacency[id]) {
                queue.push_back(neighbor);
            }
        }
        if (affected.empty()) {
            invalid("Constraint authoring intent has no affected walls");
        }
        if (has_upsert && intent.relation_anchor.has_value() &&
            !affected.contains(intent.relation_anchor->owner_id)) {
            invalid("Relation anchor is outside the changed relation component");
        }
        for (const auto& wall_id : affected) {
            const auto& wall_entity = candidate.at(wall_id);
            if (has_organization_reference(wall_entity) &&
                !organization.drawing_context(wall_id).has_value()) {
                invalid("Affected wall has an unresolved explicit drawing context: " + wall_id);
            }
        }
        const auto connected_from = [&](const std::string& origin) {
            std::set<std::string, std::less<>> connected;
            std::vector<std::string> pending{origin};
            while (!pending.empty()) {
                auto id = std::move(pending.back());
                pending.pop_back();
                if (!connected.insert(id).second) {
                    continue;
                }
                for (const auto& neighbor : adjacency[id]) {
                    pending.push_back(neighbor);
                }
            }
            return connected;
        };

        std::map<std::string, Segment, std::less<>> old_baselines;
        std::map<std::string, WallEndpointBinding, std::less<>> point_bindings;
        SolverConstraintDescriptions constraint_descriptions;
        ConstraintSolveRequest request;
        request.expected_revision = snapshot.revision();
        for (const auto& wall_id : affected) {
            const auto baseline = read_baseline(require_wall(candidate, wall_id));
            old_baselines.emplace(wall_id, baseline);
            for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                WallEndpointBinding binding{wall_id, role};
                const auto id = point_id(binding);
                const auto position = endpoint_position(baseline, role);
                point_bindings.emplace(id, binding);
                request.points.push_back({id, position.x, position.y});
            }
        }
        std::set<std::string, std::less<>> persistent_ids;
        for (const auto& [id, value] : constraints) {
            if (!value.bindings.empty() && affected.contains(value.bindings.front().owner_id)) {
                append_relation(request, value);
                persistent_ids.insert(id);
                constraint_descriptions.emplace(id, relation_description(candidate, value));
            }
        }
        append_winding_invariants(request, affected, old_baselines, constraints);

        std::map<std::string, Vec2, std::less<>> fixed_points;
        const auto add_fixed = [&](const WallEndpointBinding& binding, Vec2 position) {
            validate_binding(binding);
            if (!affected.contains(binding.owner_id)) {
                invalid("Authoring anchor is outside the explicit affected component");
            }
            const auto id = point_id(binding);
            const auto found = fixed_points.find(id);
            if (found != fixed_points.end() && !points_near(found->second, position)) {
                invalid("Authoring intent contains contradictory endpoint anchors");
            }
            fixed_points[id] = position;
        };

        if (intent.wall_resize.has_value()) {
            const auto& resize = *intent.wall_resize;
            const auto old = old_baselines.at(resize.wall_id);
            const long double dx = static_cast<long double>(old.end.x) - old.start.x;
            const long double dy = static_cast<long double>(old.end.y) - old.start.y;
            const long double old_length = std::hypot(dx, dy);
            if (!std::isfinite(old_length) || old_length <= default_geometry_tolerance_metres) {
                invalid("Selected wall has a degenerate or unrepresentable baseline");
            }
            const long double ux = dx / old_length;
            const long double uy = dy / old_length;
            Segment target = old;
            const auto length = static_cast<long double>(resize.exact_length.metres);
            if (resize.anchored_endpoint == WallResizeAnchor::start) {
                target.end = {static_cast<double>(static_cast<long double>(old.start.x) + ux * length),
                              static_cast<double>(static_cast<long double>(old.start.y) + uy * length)};
            } else {
                target.start = {static_cast<double>(static_cast<long double>(old.end.x) - ux * length),
                                static_cast<double>(static_cast<long double>(old.end.y) - uy * length)};
            }
            if (!std::isfinite(target.start.x) || !std::isfinite(target.start.y) ||
                !std::isfinite(target.end.x) || !std::isfinite(target.end.y)) {
                invalid("Requested wall length exceeds the supported coordinate range");
            }
            add_fixed({resize.wall_id, WallEndpointRole::start}, target.start);
            add_fixed({resize.wall_id, WallEndpointRole::end}, target.end);
            const auto resize_component = connected_from(resize.wall_id);
            if (has_upsert && intent.relation_anchor.has_value() &&
                !resize_component.contains(intent.relation_anchor->owner_id)) {
                invalid("Relation anchor is outside the resized wall component");
            }
            for (const auto& wall_id : affected) {
                if (wall_id == resize.wall_id) {
                    continue;
                }
                if (!resize.move_connected_walls || !resize_component.contains(wall_id)) {
                    add_fixed({wall_id, WallEndpointRole::start}, old_baselines.at(wall_id).start);
                    add_fixed({wall_id, WallEndpointRole::end}, old_baselines.at(wall_id).end);
                }
            }
        } else if (!has_upsert || !intent.relation_anchor.has_value()) {
            for (const auto& wall_id : affected) {
                add_fixed({wall_id, WallEndpointRole::start}, old_baselines.at(wall_id).start);
                add_fixed({wall_id, WallEndpointRole::end}, old_baselines.at(wall_id).end);
            }
        } else {
            const auto& anchor = *intent.relation_anchor;
            add_fixed(anchor, endpoint_position(old_baselines.at(anchor.owner_id), anchor.role));
            const auto anchor_component = connected_from(anchor.owner_id);
            for (const auto& wall_id : affected) {
                if (wall_id == anchor.owner_id) {
                    continue;
                }
                if (!intent.relation_move_connected_walls ||
                    !anchor_component.contains(wall_id)) {
                    add_fixed({wall_id, WallEndpointRole::start}, old_baselines.at(wall_id).start);
                    add_fixed({wall_id, WallEndpointRole::end}, old_baselines.at(wall_id).end);
                }
            }
        }
        if (has_upsert && intent.relation_anchor.has_value() && intent.wall_resize.has_value()) {
            const auto& anchor = *intent.relation_anchor;
            add_fixed(anchor, endpoint_position(old_baselines.at(anchor.owner_id), anchor.role));
        }

        std::size_t temporary_index = 0;
        for (const auto& [id, position] : fixed_points) {
            const auto temporary_id = unique_temporary_id(persistent_ids, temporary_index++);
            request.constraints.push_back(
                FixedAnchorConstraint{temporary_id, id, position.x, position.y});
            constraint_descriptions.emplace(
                temporary_id,
                "fixed endpoint: " + endpoint_description(candidate, point_bindings.at(id)));
        }
        const auto solver_preview = solve_planar_constraints(request);
        result.diagnostics_ = solver_diagnostics(solver_preview, constraint_descriptions);
        if (!solver_preview.accepted()) {
            if (result.diagnostics_.empty()) {
                result.diagnostics_.push_back("Constraint solver rejected the authoring intent");
            }
            return result;
        }

        std::map<std::string, Vec2, std::less<>> solved_points;
        for (const auto& solved : solver_preview.points) {
            solved_points.emplace(solved.id, Vec2{solved.x, solved.y});
        }
        for (const auto& [id, position] : fixed_points) {
            const auto solved = solved_points.find(id);
            if (solved == solved_points.end() ||
                !points_near(solved->second, position, constraint_linear_tolerance_metres)) {
                invalid("Constraint solver did not preserve an authoring anchor");
            }
            solved->second = position;
        }

        std::set<std::string, std::less<>> changed_ids;
        for (const auto& wall_id : affected) {
            const auto old = old_baselines.at(wall_id);
            Segment proposed{
                solved_points.at(point_id({wall_id, WallEndpointRole::start})),
                solved_points.at(point_id({wall_id, WallEndpointRole::end})), 0.0};
            if (baseline_same(old, proposed)) {
                proposed = old;
            }
            if (proposed.start.x == old.start.x && proposed.start.y == old.start.y &&
                proposed.end.x == old.end.x && proposed.end.y == old.end.y) {
                continue;
            }
            validate_endpoint_identity_not_swapped(old, proposed, wall_id);
            auto& wall_entity = candidate.at(wall_id);
            const bool resized = intent.wall_resize.has_value() &&
                intent.wall_resize->wall_id == wall_id;
            validate_or_clear_length_receipt(
                wall_entity, resized,
                resized ? &intent.wall_resize->exact_length : nullptr, proposed);
            set_baseline(wall_entity, proposed);
            validate_wall_host(wall_id, candidate);
            result.changed_walls_.push_back({wall_id, old, proposed});
            changed_ids.insert(wall_id);
        }

        validate_topology(snapshot.entities(), candidate, changed_ids, constraints, organization);
        (void)validate_constraint_integrity(candidate);

        bool entity_changed = candidate.size() != snapshot.entities().size();
        if (!entity_changed) {
            for (const auto& [id, entity] : candidate) {
                const auto before = snapshot.entities().find(id);
                if (before == snapshot.entities().end() || before->second != entity) {
                    entity_changed = true;
                    break;
                }
            }
        }
        if (!entity_changed) {
            result.changed_walls_.clear();
            result.diagnostics_.push_back("Constraint authoring intent makes no document change");
            return result;
        }

        result.accepted_ = true;
        result.candidate_entities_ = std::move(candidate);
        result.candidate_digest_ = entity_map_digest(result.candidate_entities_);
        result.shown_result_digest_ = digest_shown_result(result);
        return result;
    } catch (const std::exception& error) {
        result.accepted_ = false;
        result.changed_walls_.clear();
        result.candidate_entities_ = snapshot.entities();
        result.candidate_digest_.clear();
        result.shown_result_digest_.clear();
        result.diagnostics_.push_back(error.what());
        return result;
    }
}

ConstraintRelationMutation ConstraintRelationMutation::upsert(PersistentConstraint constraint) {
    ConstraintRelationMutation mutation;
    mutation.kind = ConstraintRelationMutationKind::upsert;
    mutation.constraint_id = constraint.id;
    mutation.constraint = std::move(constraint);
    return mutation;
}

ConstraintRelationMutation ConstraintRelationMutation::remove(std::string constraint_id) {
    ConstraintRelationMutation mutation;
    mutation.kind = ConstraintRelationMutationKind::remove;
    mutation.constraint_id = std::move(constraint_id);
    return mutation;
}

bool ConstraintAuthoringPreview::accepted() const noexcept { return accepted_; }
const std::string& ConstraintAuthoringPreview::document_id() const noexcept { return document_id_; }
Revision ConstraintAuthoringPreview::expected_revision() const noexcept { return expected_revision_; }
const std::string& ConstraintAuthoringPreview::source_snapshot_digest() const noexcept {
    return source_snapshot_digest_;
}
const std::string& ConstraintAuthoringPreview::candidate_digest() const noexcept {
    return candidate_digest_;
}
const std::vector<ConstraintWallChange>& ConstraintAuthoringPreview::changed_walls() const noexcept {
    return changed_walls_;
}
const Entities& ConstraintAuthoringPreview::candidate_entities() const noexcept {
    return candidate_entities_;
}
const std::vector<std::string>& ConstraintAuthoringPreview::diagnostics() const noexcept {
    return diagnostics_;
}

ConstraintAuthoringPreview preview_constraint_authoring(
    const DocumentSnapshot& snapshot, const ConstraintAuthoringIntent& intent) {
    return ConstraintAuthoringBuilder::build(snapshot, intent);
}

Revision apply_constraint_authoring(Document& document,
                                     const ConstraintAuthoringPreview& preview) {
    if (!preview.accepted_) {
        invalid("A rejected constraint preview cannot be applied");
    }
    const auto current = document.snapshot();
    if (current.document_id() != preview.document_id_) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "Constraint preview belongs to another document");
    }
    if (current.revision() != preview.expected_revision_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "Constraint preview revision is stale");
    }
    if (document_snapshot_digest(current) != preview.source_snapshot_digest_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "Constraint preview source snapshot has changed");
    }
    if (entity_map_digest(preview.candidate_entities_) != preview.candidate_digest_ ||
        digest_shown_result(preview) != preview.shown_result_digest_) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "Constraint preview display data was modified");
    }

    const auto recomputed = ConstraintAuthoringBuilder::build(current, preview.normalized_intent_);
    if (!recomputed.accepted_ || recomputed.candidate_digest_ != preview.candidate_digest_ ||
        recomputed.shown_result_digest_ != preview.shown_result_digest_) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "Constraint preview no longer reproduces the shown result");
    }

    std::vector<EntityChange> changes;
    for (const auto& [id, entity] : current.entities()) {
        const auto found = recomputed.candidate_entities_.find(id);
        if (found == recomputed.candidate_entities_.end()) {
            changes.push_back(EntityChange::erase(id));
        } else if (found->second != entity) {
            changes.push_back(EntityChange::upsert(found->second));
        }
    }
    for (const auto& [id, entity] : recomputed.candidate_entities_) {
        if (!current.entities().contains(id)) {
            changes.push_back(EntityChange::upsert(entity));
        }
    }
    if (changes.empty()) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "Constraint preview does not contain a document change");
    }
    return document.apply(ApplyEntityChanges{
        .expected_revision = current.revision(),
        .entity_changes = std::move(changes),
        .message = recomputed.normalized_intent_.message,
    });
}

}  // namespace sketch
