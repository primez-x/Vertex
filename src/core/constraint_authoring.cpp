#include "sketch/constraint_authoring.hpp"

#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/boundary_integrity.hpp"
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
    return found->second;
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
    auto boundary_changes = ordered_json::array();
    for (const auto& change : preview.changed_boundaries()) {
        boundary_changes.push_back({{"before", encode_identified_boundary_entity(change.before).properties},
                                    {"after", encode_identified_boundary_entity(change.after).properties}});
    }
    auto boundary_edits = ordered_json::array();
    for (const auto& edit : preview.boundary_edits())
        boundary_edits.push_back(encode_boundary_geometry_edit(edit));
    return digest_json({{"accepted", preview.accepted()},
                        {"document_id", preview.document_id()},
                        {"revision", preview.expected_revision()},
                        {"source_digest", preview.source_snapshot_digest()},
                        {"candidate_digest", preview.candidate_digest()},
                        {"changes", std::move(changes)},
                        {"boundary_changes", std::move(boundary_changes)},
                        {"degrees_of_freedom", preview.degrees_of_freedom()},
                        {"boundary_edits", std::move(boundary_edits)},
                        {"diagnostics", preview.diagnostics()}});
}

std::string point_id(const WallEndpointBinding& binding) {
    if (!binding.vertex_id.empty())
        return std::to_string(binding.owner_id.size()) + ":" + binding.owner_id +
            ":vertex:" + binding.vertex_id;
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
    const auto coordinate_intents = static_cast<unsigned>(result.wall_resize.has_value()) +
        static_cast<unsigned>(result.boundary_resize.has_value()) +
        static_cast<unsigned>(result.boundary_vertex_move.has_value());
    if (coordinate_intents > 1)
        invalid("Only one wall or boundary coordinate intent may be authored at a time");
    if (result.boundary_resize.has_value()) {
        const auto& edit = result.boundary_resize->edit;
        if (edit.kind != BoundaryGeometryEditKind::resize_segment)
            invalid("Boundary resize intent requires a segment resize edit");
        validate_boundary_geometry_edit(edit);
        if (edit.fixed_endpoint != BoundaryFixedEndpoint::start &&
            edit.fixed_endpoint != BoundaryFixedEndpoint::end)
            invalid("Boundary resize anchor is invalid");
    }
    if (result.boundary_vertex_move.has_value()) {
        const auto& edit = result.boundary_vertex_move->edit;
        if (edit.kind != BoundaryGeometryEditKind::move_vertex)
            invalid("Boundary vertex move intent requires a vertex move edit");
        validate_boundary_geometry_edit(edit);
    }
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
    if (!result.wall_resize.has_value() && !result.boundary_resize.has_value() &&
        !result.boundary_vertex_move.has_value() &&
        result.relation_mutations.empty()) {
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

void append_relation(ConstraintSolveRequest& request, const PersistentConstraint& value,const Entities& entities) {
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
        case ConstraintRelationKind::fixed_arc_length:
            request.constraints.push_back(FixedLengthConstraint{id,point_at(0),point_at(1),
                constraint_arc_chord_target(value,entities)});
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

bool has_organization_reference(const Entity& entity) {
    return entity.properties.contains("layer_id") || entity.properties.contains("floor_id") ||
        entity.properties.contains("building_id") || entity.properties.contains("property_id");
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
    const auto& wall = entities.at(wall_id);
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
        case ConstraintRelationKind::fixed_arc_length:
            return "fixed physical arc length ("+constraint.length->original_expression+") on "+
                wall_display_name(entities,constraint.bindings.at(0).owner_id);
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
        const BoundaryGeometryEdit* boundary_edit = intent.boundary_resize
            ? &intent.boundary_resize->edit
            : intent.boundary_vertex_move ? &intent.boundary_vertex_move->edit : nullptr;
        const bool move_related_objects = intent.boundary_resize
            ? intent.boundary_resize->move_related_objects
            : intent.boundary_vertex_move && intent.boundary_vertex_move->move_related_objects;
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
        if (boundary_edit) {
            seeds.insert(boundary_edit->boundary_id);
        }
        const auto constraints = decode_supported_constraints(candidate);
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> adjacency;
        for (const auto& [id, value] : constraints) {
            (void)id;
            std::set<std::string, std::less<>> owners;
            for (const auto& binding : value.bindings) {
                validate_binding(binding);
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
            invalid("Constraint authoring intent has no affected owners");
        }
        if (has_upsert && intent.relation_anchor.has_value() &&
            !affected.contains(intent.relation_anchor->owner_id)) {
            invalid("Relation anchor is outside the changed relation component");
        }
        for (const auto& wall_id : affected) {
            const auto& wall_entity = candidate.at(wall_id);
            if (has_organization_reference(wall_entity) &&
                !organization.drawing_context(wall_id).has_value()) {
                invalid("Affected owner has an unresolved explicit drawing context: " + wall_id);
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
        std::map<std::string, IdentifiedBoundary, std::less<>> boundaries;
        std::map<std::string, Vec2, std::less<>> positions;
        std::set<std::string, std::less<>> affected_walls;
        std::map<std::string, WallEndpointBinding, std::less<>> point_bindings;
        SolverConstraintDescriptions constraint_descriptions;
        ConstraintSolveRequest request;
        request.expected_revision = snapshot.revision();
        for (const auto& wall_id : affected) {
            const auto owner = candidate.find(wall_id);
            if (owner == candidate.end()) invalid("Constraint owner does not exist: " + wall_id);
            if (can_recognize_boundary_entity_type(owner->second.type)) {
                auto boundary = decode_identified_boundary_entity(owner->second);
                WindingInvariant winding;
                winding.orientation = signed_area(boundary_geometry(boundary)) > 0
                    ? WindingOrientation::counter_clockwise : WindingOrientation::clockwise;
                for (const auto& edge : boundary.segments) {
                    WallEndpointBinding binding{wall_id, WallEndpointRole::start,
                        edge.segment_id, edge.start_vertex_id};
                    const auto id = point_id(binding);
                    point_bindings.emplace(id, binding);
                    positions.emplace(id, edge.segment.start);
                    request.points.push_back({id, edge.segment.start.x, edge.segment.start.y});
                    winding.loop.push_back(id);
                }
                if (std::all_of(boundary.segments.begin(),boundary.segments.end(),
                    [](const auto& edge) { return edge.segment.sweep_radians==0; }))
                    request.winding_invariants.push_back(std::move(winding));
                boundaries.emplace(wall_id, std::move(boundary));
                continue;
            }
            const auto baseline = read_baseline(require_wall(candidate, wall_id));
            old_baselines.emplace(wall_id, baseline);
            affected_walls.insert(wall_id);
            for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                WallEndpointBinding binding{wall_id, role};
                const auto id = point_id(binding);
                const auto position = endpoint_position(baseline, role);
                point_bindings.emplace(id, binding);
                positions.emplace(id, position);
                request.points.push_back({id, position.x, position.y});
            }
        }
        const auto resolve = [&](const WallEndpointBinding& binding) {
            validate_binding(binding);
            const auto boundary = boundaries.find(binding.owner_id);
            if (boundary != boundaries.end()) {
                const auto& edges = boundary->second.segments;
                const auto edge = std::find_if(edges.begin(), edges.end(),
                    [&](const auto& e) { return e.segment_id == binding.segment_id; });
                if (edge == edges.end() ||
                    (binding.role == WallEndpointRole::start ? edge->start_vertex_id : edge->end_vertex_id)
                        != binding.vertex_id)
                    invalid("Boundary binding does not resolve its stable segment endpoint");
            } else if (!old_baselines.contains(binding.owner_id) ||
                       !binding.segment_id.empty() || !binding.vertex_id.empty()) {
                invalid("Wall binding does not resolve its stable endpoint role");
            }
            const auto id = point_id(binding);
            if (!positions.contains(id)) invalid("Binding is outside the affected component");
            return id;
        };
        std::set<std::string, std::less<>> persistent_ids;
        for (const auto& [id, value] : constraints) {
            if (!value.bindings.empty() && affected.contains(value.bindings.front().owner_id)) {
                for (const auto& binding : value.bindings) (void)resolve(binding);
                append_relation(request, value,candidate);
                persistent_ids.insert(id);
                constraint_descriptions.emplace(id, relation_description(candidate, value));
            }
        }

        std::map<std::string, Vec2, std::less<>> fixed_points;
        const auto add_fixed = [&](const WallEndpointBinding& binding, Vec2 position) {
            validate_binding(binding);
            if (!affected.contains(binding.owner_id)) {
                invalid("Authoring anchor is outside the explicit affected component");
            }
            const auto id = resolve(binding);
            const auto found = fixed_points.find(id);
            if (found != fixed_points.end() && !points_near(found->second, position)) {
                invalid("Authoring intent contains contradictory endpoint anchors");
            }
            fixed_points[id] = position;
        };

        if (intent.wall_resize.has_value()) {
            const auto& resize = *intent.wall_resize;
            const auto old = old_baselines.at(resize.wall_id);
            if (old.sweep_radians!=0) invalid("Wall length resize requires a straight wall; endpoint/chord relations support curves");
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
            for (const auto& [id, position] : positions) {
                const auto& binding = point_bindings.at(id);
                if (binding.owner_id == resize.wall_id) {
                    continue;
                }
                if (!resize.move_connected_walls || !resize_component.contains(binding.owner_id)) {
                    add_fixed(binding, position);
                }
            }
        } else if (boundary_edit) {
            const auto& owner_id = boundary_edit->boundary_id;
            if (!boundaries.contains(owner_id))
                invalid("Boundary coordinate edit owner must be an identified boundary");
            // This geometry-only replay preserves receipts and deliberately
            // precedes final constraint validation: neighbors have not moved yet.
            const auto edited_entities = edited_boundary_entities(candidate, *boundary_edit);
            const auto target = decode_identified_boundary_entity(edited_entities.at(owner_id));
            for (const auto& edge : target.segments) {
                add_fixed({owner_id, WallEndpointRole::start,
                    edge.segment_id, edge.start_vertex_id}, edge.segment.start);
            }
            const auto coordinate_component = connected_from(owner_id);
            if (has_upsert && intent.relation_anchor.has_value() &&
                !coordinate_component.contains(intent.relation_anchor->owner_id))
                invalid("Relation anchor is outside the edited boundary component");
            for (const auto& [id, position] : positions) {
                const auto& binding = point_bindings.at(id);
                if (binding.owner_id == owner_id) continue;
                if (!move_related_objects || !coordinate_component.contains(binding.owner_id))
                    add_fixed(binding, position);
            }
        } else if (!has_upsert || !intent.relation_anchor.has_value()) {
            for (const auto& [id, position] : positions) add_fixed(point_bindings.at(id), position);
        } else {
            const auto& anchor = *intent.relation_anchor;
            add_fixed(anchor, positions.at(resolve(anchor)));
            const auto anchor_component = connected_from(anchor.owner_id);
            for (const auto& [id, position] : positions) {
                const auto& binding = point_bindings.at(id);
                if (binding.owner_id == anchor.owner_id) {
                    continue;
                }
                if (!intent.relation_move_connected_walls ||
                    !anchor_component.contains(binding.owner_id)) {
                    add_fixed(binding, position);
                }
            }
        }
        if (has_upsert && intent.relation_anchor.has_value() &&
            (intent.wall_resize.has_value() || boundary_edit)) {
            const auto& anchor = *intent.relation_anchor;
            add_fixed(anchor, positions.at(resolve(anchor)));
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
        result.degrees_of_freedom_ = solver_preview.degrees_of_freedom;
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

        for (const auto& wall_id : affected_walls) {
            const auto old = old_baselines.at(wall_id);
            Segment proposed{
                solved_points.at(point_id({wall_id, WallEndpointRole::start})),
                solved_points.at(point_id({wall_id, WallEndpointRole::end})), old.sweep_radians};
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
            wall_entity = replay_constraint_wall_edit(wall_entity, {wall_id, proposed,
                resized ? std::optional<Quantity>{intent.wall_resize->exact_length} : std::nullopt,
                old.sweep_radians==0 ? 1ULL : 2ULL});
            validate_constraint_wall_host(wall_id, candidate);
            result.changed_walls_.push_back({wall_id, old, proposed});
        }

        if (boundary_edit) result.boundary_edits_.push_back(*boundary_edit);
        for (const auto& [id, binding] : point_bindings) {
            if (!boundaries.contains(binding.owner_id) || points_near(solved_points.at(id), positions.at(id)))
                continue;
            if (boundary_edit && binding.owner_id == boundary_edit->boundary_id)
                continue;
            BoundaryGeometryEdit edit;
            edit.boundary_id = binding.owner_id;
            edit.target_id = binding.vertex_id;
            edit.target_position = solved_points.at(id);
            result.boundary_edits_.push_back(std::move(edit));
        }
        candidate = edited_boundary_entities_batch(candidate, result.boundary_edits_);
        for (const auto& [id, before] : boundaries) {
            const auto after = decode_identified_boundary_entity(candidate.at(id));
            if (after != before) result.changed_boundaries_.push_back({before, after});
        }
        validate_constraint_edit_topology(snapshot.entities(),candidate);
        (void)validate_boundary_integrity(candidate);
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
            result.changed_boundaries_.clear();
            result.boundary_edits_.clear();
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
        result.changed_boundaries_.clear();
        result.boundary_edits_.clear();
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
const std::vector<ConstraintBoundaryChange>& ConstraintAuthoringPreview::changed_boundaries() const noexcept {
    return changed_boundaries_;
}
int ConstraintAuthoringPreview::degrees_of_freedom() const noexcept { return degrees_of_freedom_; }
const std::vector<BoundaryGeometryEdit>& ConstraintAuthoringPreview::boundary_edits() const noexcept {
    return boundary_edits_;
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

PersistentConstraintComponentAnalysis analyze_persistent_constraint_component(
    const Entities& entities, const std::vector<std::string>& seed_owner_ids,
    std::optional<Revision> revision) {
    PersistentConstraintComponentAnalysis result;
    try {
        if (seed_owner_ids.empty()) invalid("Select at least one endpoint owner for persistent analysis");
        std::set<std::string, std::less<>> affected;
        for (const auto& id : seed_owner_ids) {
            if (id.empty() || !entities.contains(id)) invalid("Persistent analysis seed owner does not exist: " + id);
            affected.insert(id);
        }
        struct ScopedRelation {
            std::string id;
            std::set<std::string, std::less<>> owners;
            std::optional<PersistentConstraint> relation;
            std::string error;
        };
        std::vector<ScopedRelation> relations;
        for (const auto& [id, entity] : entities) {
            if (entity.type != "constraint") continue;
            ScopedRelation item{id, {}, {}, {}};
            // A malformed relation can be excluded only when every possible
            // owner is known. Do not infer scope from partial bindings.
            try {
                if (!entity.properties.is_object()) invalid("constraint properties are not an object");
                const auto& bindings = entity.properties.at("bindings");
                if (!bindings.is_array() || bindings.empty()) invalid("constraint bindings have no owner scope");
                const auto add_owner = [&](const json& value) {
                    if (!value.is_string() || value.get_ref<const std::string&>().empty())
                        invalid("constraint owner scope is ambiguous");
                    item.owners.insert(value.get<std::string>());
                };
                for (const auto& binding : bindings) {
                    if (!binding.is_object() || !binding.contains("owner_id"))
                        invalid("constraint binding has no owner scope");
                    add_owner(binding.at("owner_id"));
                }
                for (const auto* key : {"entity_ids", "wall_ids"}) {
                    const auto declared = entity.properties.find(key);
                    if (declared == entity.properties.end()) continue;
                    if (!declared->is_array() || declared->empty()) invalid("declared constraint owner scope is ambiguous");
                    for (const auto& owner : *declared) add_owner(owner);
                }
            } catch (const std::exception& error) {
                invalid("Constraint " + id + " has indeterminate owner scope: " + error.what());
            }
            try {
                if (entity.id != id) invalid("constraint map identity differs from its entity identity");
                const auto decoded = decode_constraint_entity(entity);
                item.relation = decoded.constraint;
                if (!decoded.supported()) item.error = decoded.unsupported_reason;
            } catch (const std::exception& error) {
                item.error = error.what();
            }
            relations.push_back(std::move(item));
        }
        bool expanded = true;
        while (expanded) {
            expanded = false;
            for (const auto& item : relations) {
                if (!item.relation || !std::any_of(item.owners.begin(), item.owners.end(),
                        [&](const auto& id) { return affected.contains(id); })) continue;
                for (const auto& binding : item.relation->bindings)
                    expanded = affected.insert(binding.owner_id).second || expanded;
            }
        }
        result.owner_ids.assign(affected.begin(), affected.end());
        for (const auto& item : relations) {
            if (!std::any_of(item.owners.begin(), item.owners.end(),
                    [&](const auto& id) { return affected.contains(id); })) continue;
            result.constraint_ids.push_back(item.id);
            if (!item.relation) invalid("Constraint " + item.id + " cannot be analyzed: " + item.error);
        }
        std::map<std::string, IdentifiedBoundary, std::less<>> boundaries;
        std::map<std::string, Segment, std::less<>> walls;
        std::map<std::string, Vec2, std::less<>> positions;
        const auto add_point = [&](const WallEndpointBinding& binding, Vec2 coordinate) {
            const auto [found, inserted] = positions.emplace(point_id(binding), coordinate);
            if (!inserted && (found->second.x != coordinate.x || found->second.y != coordinate.y))
                invalid("Shared endpoint identity has ambiguous coordinates");
        };
        for (const auto& id : affected) {
            const auto found = entities.find(id);
            if (found == entities.end()) invalid("Persistent component owner does not exist: " + id);
            const auto& entity = found->second;
            if (entity.id != id) invalid("Persistent owner map identity differs from its entity identity: " + id);
            if (entity.type == "wall") {
                validate_constraint_wall_host(id, entities);
                const auto baseline = read_baseline(entity);
                walls.emplace(id, baseline);
                for (const auto role : {WallEndpointRole::start, WallEndpointRole::end})
                    add_point({id,role}, endpoint_position(baseline,role));
            } else if (can_recognize_boundary_entity_type(entity.type)) {
                auto boundary = decode_identified_boundary_entity(entity);
                for (const auto& edge : boundary.segments) {
                    add_point({id,WallEndpointRole::start,edge.segment_id,edge.start_vertex_id}, edge.segment.start);
                }
                boundaries.emplace(id, std::move(boundary));
            } else {
                invalid("Persistent endpoint analysis requires a wall or identified closed boundary: " + id);
            }
        }
        const auto resolve = [&](const WallEndpointBinding& binding) {
            validate_binding(binding);
            if (walls.contains(binding.owner_id)) {
                if (!binding.segment_id.empty() || !binding.vertex_id.empty())
                    invalid("Wall endpoint binding contains boundary child identities");
            } else {
                const auto owner = boundaries.find(binding.owner_id);
                if (owner == boundaries.end()) invalid("Constraint endpoint owner is outside its component");
                const auto edge = std::find_if(owner->second.segments.begin(), owner->second.segments.end(),
                    [&](const auto& value) { return value.segment_id == binding.segment_id; });
                if (edge == owner->second.segments.end() ||
                    (binding.role == WallEndpointRole::start ? edge->start_vertex_id : edge->end_vertex_id) != binding.vertex_id)
                    invalid("Constraint binding does not resolve its stable boundary endpoint");
            }
            if (!positions.contains(point_id(binding))) invalid("Constraint binding has no unique endpoint variable");
        };
        ConstraintSolveRequest request;
        request.expected_revision = revision.value_or(0);
        for (const auto& [id, position] : positions) request.points.push_back({id,position.x,position.y});
        result.point_count = request.points.size();
        SolverConstraintDescriptions descriptions;
        for (const auto& item : relations) {
            if (!item.relation || !affected.contains(item.relation->bindings.front().owner_id)) continue;
            for (const auto& binding : item.relation->bindings) resolve(binding);
            append_relation(request, *item.relation,entities);
            descriptions.emplace(item.id, std::string(constraint_relation_name(item.relation->relation)) +
                " persisted relation " + item.id);
        }
        const auto diagnosed = diagnose_planar_constraints(request);
        result.redundant_constraint_ids = diagnosed.redundant_constraints;
        result.conflicting_constraint_ids = diagnosed.conflicting_constraints;
        result.diagnostics = solver_diagnostics(diagnosed, descriptions);
        if (diagnosed.accepted() && diagnosed.degrees_of_freedom >= 0 &&
            static_cast<std::size_t>(diagnosed.degrees_of_freedom) <= request.points.size()*2) {
            result.supported = true;
            result.degrees_of_freedom = diagnosed.degrees_of_freedom;
        } else if (result.diagnostics.empty()) {
            result.diagnostics.push_back("Persistent endpoint rank diagnosis is unavailable");
        }
    } catch (const std::exception& error) {
        result.supported = false;
        result.degrees_of_freedom = -1;
        result.diagnostics.push_back(error.what());
    }
    return result;
}

PersistentConstraintComponentAnalysis analyze_persistent_constraint_component(
    const DocumentSnapshot& snapshot, const std::vector<std::string>& seed_owner_ids) {
    return analyze_persistent_constraint_component(snapshot.entities(), seed_owner_ids, snapshot.revision());
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
    if (!recomputed.boundary_edits_.empty() || !recomputed.changed_walls_.empty()) {
        std::vector<EntityChange> constraint_changes;
        for (const auto& change : changes) {
            const auto id = change.kind == EntityChangeKind::upsert
                ? change.entity.id : change.entity_id;
            const auto before = current.entities().find(id);
            const bool was_constraint = before != current.entities().end() &&
                before->second.type == "constraint";
            const bool is_constraint = change.kind == EntityChangeKind::upsert &&
                change.entity.type == "constraint";
            if (was_constraint || is_constraint) {
                constraint_changes.push_back(change);
            }
        }
        ApplyBoundaryConstraintChanges command{
            current.revision(), recomputed.boundary_edits_,
            std::move(constraint_changes), recomputed.normalized_intent_.message};
        for (const auto& wall : recomputed.changed_walls_) {
            const auto& resize = recomputed.normalized_intent_.wall_resize;
            command.wall_edits.push_back({wall.wall_id, wall.proposed_baseline,
                resize && resize->wall_id == wall.wall_id
                    ? std::optional<Quantity>{resize->exact_length} : std::nullopt,
                wall.old_baseline.sweep_radians==0 ? 1ULL : 2ULL});
        }
        const auto verified = Document::preview_command(current, Command{command});
        if (verified.entities() != recomputed.candidate_entities_ ||
            verified.assets() != current.assets()) {
            throw DocumentError(DocumentErrorCode::invalid_entity,
                                "Typed boundary constraint replay does not reproduce the shown result");
        }
        return document.apply(Command{std::move(command)});
    }
    return document.apply(ApplyEntityChanges{
        .expected_revision = current.revision(),
        .entity_changes = std::move(changes),
        .message = recomputed.normalized_intent_.message,
    });
}

}  // namespace sketch
