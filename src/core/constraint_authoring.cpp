#include "sketch/constraint_authoring.hpp"

#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/joint_translation_replay.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/measurement_linework_source.hpp"

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

std::optional<Quantity> known_wall_length_entry(const Entity& wall) {
    validate_wall_length_input(wall);
    const auto section = wall.extensions.find("constraint_authoring");
    if (section == wall.extensions.end() || !section->is_object() ||
        !section->contains("version") || !section->at("version").is_number_integer() ||
        section->at("version") != 1 || !section->contains("last_length_entry")) {
        return std::nullopt;
    }
    const auto& receipt = section->at("last_length_entry");
    if (!receipt.is_object() || !receipt.contains("version") ||
        !receipt.at("version").is_number_integer() ||
        (receipt.at("version") != 1 && receipt.at("version") != 2)) {
        return std::nullopt;
    }
    std::optional<Unit> unit;
    for (const auto candidate : {Unit::metre, Unit::millimetre, Unit::centimetre,
                                 Unit::foot, Unit::inch}) {
        if (receipt.at("entered_unit") == unit_name(candidate)) {
            unit = candidate;
            break;
        }
    }
    if (!unit) {
        return std::nullopt;
    }
    return normalize_positive_quantity(
        parse_quantity(receipt.at("original_expression").get<std::string>(), *unit));
}

std::optional<Quantity> unchanged_wall_length_entry(const Entity& wall,
                                                     const Segment& old_baseline,
                                                     const Segment& proposed_baseline) {
    if (std::abs(segment_length(old_baseline) - segment_length(proposed_baseline)) >
        constraint_linear_tolerance_metres) {
        return std::nullopt;
    }
    return known_wall_length_entry(wall);
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
    auto exterior_source_edits = ordered_json::array();
    for (const auto& edit : preview.exterior_source_edits())
        exterior_source_edits.push_back(encode_boundary_geometry_edit(edit));
    auto measured_changes = ordered_json::array();
    for (const auto& change : preview.changed_measured_strokes()) {
        auto before = ordered_json::array(), after = ordered_json::array();
        for (const auto& edge : change.before.edges) before.push_back(segment_json(edge.segment));
        for (const auto& edge : change.after.edges) after.push_back(segment_json(edge.segment));
        measured_changes.push_back({{"stroke_id",change.stroke_id},{"before",before},{"after",after}});
    }
    return digest_json({{"accepted", preview.accepted()},
                        {"document_id", preview.document_id()},
                        {"revision", preview.expected_revision()},
                        {"source_digest", preview.source_snapshot_digest()},
                        {"candidate_digest", preview.candidate_digest()},
                        {"changes", std::move(changes)},
                        {"boundary_changes", std::move(boundary_changes)},
                        {"measured_changes", std::move(measured_changes)},
                        {"degrees_of_freedom", preview.degrees_of_freedom()},
                        {"boundary_edits", std::move(boundary_edits)},
                        {"exterior_source_edits", std::move(exterior_source_edits)},
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
        static_cast<unsigned>(result.wall_geometry_move.has_value()) +
        static_cast<unsigned>(result.boundary_resize.has_value()) +
        static_cast<unsigned>(result.boundary_vertex_move.has_value()) +
        static_cast<unsigned>(result.exterior_corner_move.has_value()) +
        static_cast<unsigned>(result.exterior_segment_resize.has_value()) +
        static_cast<unsigned>(result.exterior_segment_arc.has_value()) +
        static_cast<unsigned>(result.measured_stroke_resize.has_value()) +
        static_cast<unsigned>(result.measured_stroke_vertex_move.has_value()) +
        static_cast<unsigned>(result.measured_stroke_transform.has_value()) +
        static_cast<unsigned>(result.joint_translation.has_value());
    if (coordinate_intents > 1)
        invalid("Only one wall or boundary coordinate intent may be authored at a time");
    if (result.joint_translation) {
        auto& move=*result.joint_translation;
        if (!std::isfinite(move.offset.x) || !std::isfinite(move.offset.y) || (move.offset.x==0 && move.offset.y==0))
            invalid("Joint translation requires a finite nonzero offset");
        if (move.presentation_offset && (!std::isfinite(move.presentation_offset->x) || !std::isfinite(move.presentation_offset->y)))
            invalid("Joint translation presentation offset must be finite");
        if (move.partial_wall_ids.empty() || (move.rigid_boundary_ids.empty() && move.rigid_stroke_ids.empty()))
            invalid("Joint translation requires rigid owners and selected physical walls");
        std::set<std::string,std::less<>> selected;
        for (auto* ids:{&move.rigid_boundary_ids,&move.rigid_stroke_ids,&move.partial_wall_ids,&move.dimension_ids}) {
            if (ids->size()>4096) invalid("Joint translation target IDs exceed their budget");
            std::sort(ids->begin(),ids->end());
            for (const auto& id:*ids) if (id.empty() || id.size()>128 || !selected.insert(id).second)
                invalid("Joint translation targets must be unique and nonempty");
        }
        if (selected.size()>4096) invalid("Joint translation targets exceed their aggregate budget");
        if (!result.relation_mutations.empty() || result.relation_anchor)
            invalid("Joint translation preserves source relations and cannot author relation changes");
    }
    if (result.measured_stroke_resize) {
        auto& resize = *result.measured_stroke_resize;
        if (resize.edit.kind != BoundaryGeometryEditKind::resize_segment)
            invalid("Measured stroke resize requires a resize intent");
        validate_boundary_geometry_edit(resize.edit);
        resize.exact_length = normalize_positive_quantity(resize.exact_length);
        if (resize.exact_length.metres != resize.edit.target_length_metres)
            invalid("Measured stroke resize differs from its exact entered quantity");
    }
    if (result.measured_stroke_vertex_move) {
        const auto& edit = result.measured_stroke_vertex_move->edit;
        if (edit.kind != BoundaryGeometryEditKind::move_vertex)
            invalid("Measured stroke vertex move requires a move intent");
        validate_boundary_geometry_edit(edit);
    }
    if (result.measured_stroke_transform) {
        const auto& move = *result.measured_stroke_transform;
        if (move.targets.empty()) invalid("Measured stroke transform requires selected owners");
        std::set<std::string,std::less<>> ids;
        for (const auto& target : move.targets)
            if (target.stroke_id.empty() || !ids.insert(target.stroke_id).second)
                invalid("Measured stroke transform owners must be unique and nonempty");
    }
    if (result.boundary_resize.has_value()) {
        const auto& edit = result.boundary_resize->edit;
        if (edit.kind != BoundaryGeometryEditKind::resize_segment)
            invalid("Boundary resize intent requires a segment resize edit");
        validate_boundary_geometry_edit(edit);
        if (edit.fixed_endpoint != BoundaryFixedEndpoint::start &&
            edit.fixed_endpoint != BoundaryFixedEndpoint::end)
            invalid("Boundary resize anchor is invalid");
        if (result.boundary_resize->exact_length) {
            auto& quantity=*result.boundary_resize->exact_length;
            quantity=normalize_positive_quantity(quantity);
            if (quantity.metres!=edit.target_length_metres)
                invalid("Boundary resize differs from its exact entered quantity");
        }
    }
    if (result.boundary_vertex_move.has_value()) {
        const auto& edit = result.boundary_vertex_move->edit;
        if (edit.kind != BoundaryGeometryEditKind::move_vertex)
            invalid("Boundary vertex move intent requires a vertex move edit");
        validate_boundary_geometry_edit(edit);
    }
    if (result.exterior_corner_move) (void)encode_exterior_corner_move(*result.exterior_corner_move);
    if (result.exterior_segment_resize) {
        result.exterior_segment_resize->exact_length=normalize_positive_quantity(result.exterior_segment_resize->exact_length);
        (void)encode_exterior_segment_resize(*result.exterior_segment_resize);
    }
    if (result.exterior_segment_arc) (void)encode_exterior_segment_arc(*result.exterior_segment_arc);
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
        if (const auto& endpoint = result.wall_resize->proposed_endpoint;
            endpoint && (!std::isfinite(endpoint->x) || !std::isfinite(endpoint->y))) {
            invalid("Wall resize endpoint target must be finite");
        }
    }
    if (result.wall_geometry_move.has_value()) {
        auto& move = *result.wall_geometry_move;
        if (move.targets.empty()) {
            invalid("Wall geometry move requires at least one selected wall");
        }
        std::set<std::string, std::less<>> target_ids;
        for (const auto& target : move.targets) {
            if (target.wall_id.empty()) {
                invalid("Wall geometry move id cannot be empty");
            }
            if (!target_ids.insert(target.wall_id).second) {
                invalid("Wall geometry move ids must be unique");
            }
            const auto target_length = std::hypot(target.proposed_end.x - target.proposed_start.x,
                                                  target.proposed_end.y - target.proposed_start.y);
            if (!std::isfinite(target.proposed_start.x) || !std::isfinite(target.proposed_start.y) ||
                !std::isfinite(target.proposed_end.x) || !std::isfinite(target.proposed_end.y) ||
                !std::isfinite(target_length)) {
                invalid("Wall geometry move targets must be finite");
            }
            if (target_length <= default_geometry_tolerance_metres) {
                invalid("Wall geometry move target baseline is degenerate");
            }
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
    if (!result.wall_resize.has_value() && !result.wall_geometry_move.has_value() &&
        !result.boundary_resize.has_value() &&
        !result.boundary_vertex_move.has_value() &&
        !result.exterior_corner_move.has_value() &&
        !result.exterior_segment_resize.has_value() &&
        !result.exterior_segment_arc.has_value() &&
        !result.measured_stroke_resize && !result.measured_stroke_vertex_move && !result.measured_stroke_transform && !result.joint_translation &&
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
        case ConstraintRelationKind::fixed_arc_length: {
            if (value.bindings.size()==2) {
                request.constraints.push_back(FixedLengthConstraint{id,point_at(0),point_at(1),
                    constraint_arc_chord_target(value,entities)});
                break;
            }
            WeightedLengthSumConstraint total{id,{},value.length->metres};
            const auto segments=resolve_constraint_arc_segments(value,entities);
            for (std::size_t i=0;i<segments.size();++i)
                total.terms.push_back({point_at(2*i),point_at(2*i+1),constraint_arc_length_coefficient(segments[i])});
            request.constraints.push_back(std::move(total));
            break;
        }
        case ConstraintRelationKind::parallel:
            request.constraints.push_back(
                ParallelConstraint{id, point_at(0), point_at(1), point_at(2), point_at(3)});
            break;
        case ConstraintRelationKind::perpendicular:
            request.constraints.push_back(
                PerpendicularConstraint{id, point_at(0), point_at(1), point_at(2), point_at(3)});
            break;
        case ConstraintRelationKind::tangent: {
            const auto segments=resolve_constraint_tangent_segments(value,entities);
            const bool first_start=value.bindings.at(0).role==WallEndpointRole::start;
            const bool second_start=value.bindings.at(2).role==WallEndpointRole::start;
            request.constraints.push_back(TangentConstraint{id,point_at(first_start ? 0 : 1),point_at(first_start ? 1 : 0),
                point_at(second_start ? 2 : 3),point_at(second_start ? 3 : 2),segments[0].sweep_radians,segments[1].sweep_radians,
                first_start,second_start});
            break;
        }
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

bool points_exact(Vec2 first, Vec2 second) {
    return first.x == second.x && first.y == second.y;
}

std::map<std::string,Vec2,std::less<>> joint_temporary_pins(
    const ConstraintSolveRequest& request,const std::map<std::string,Vec2,std::less<>>& targets) {
    // Saved coincidence and tangent contacts already identify one physical
    // point. Give that class one temporary coordinate pin, while preserving
    // every saved relation and checking every selected target after solving.
    std::map<std::string,std::string,std::less<>> parents;
    for (const auto& point:request.points) parents.emplace(point.id,point.id);
    const auto root=[&](const std::string& id) {
        auto current=id;
        while (parents.at(current)!=current) current=parents.at(current);
        return current;
    };
    const auto join=[&](const std::string& first,const std::string& second) {
        const auto a=root(first),b=root(second);
        parents.at(std::max(a,b))=std::min(a,b);
    };
    for (const auto& constraint:request.constraints) {
        if (const auto* coincident=std::get_if<CoincidentConstraint>(&constraint)) join(coincident->first,coincident->second);
        else if (const auto* tangent=std::get_if<TangentConstraint>(&constraint))
            join(tangent->first_at_start ? tangent->first_start : tangent->first_end,
                tangent->second_at_start ? tangent->second_start : tangent->second_end);
    }
    std::map<std::string,Vec2,std::less<>> saved;
    for (const auto& constraint:request.constraints) {
        const auto* anchor=std::get_if<FixedAnchorConstraint>(&constraint);
        if (!anchor) continue;
        const Vec2 position{anchor->x,anchor->y};
        const auto [found,inserted]=saved.emplace(root(anchor->point),position);
        if (!inserted && !points_exact(found->second,position))
            invalid("Joint translation has incompatible saved fixed positions at a shared contact");
    }
    std::map<std::string,std::pair<std::string,Vec2>,std::less<>> classes;
    for (const auto& [id,position]:targets) {
        const auto representative=root(id);
        const auto [found,inserted]=classes.emplace(representative,std::pair{id,position});
        if (!inserted && !points_exact(found->second.second,position))
            invalid("Joint translation has incompatible exact selected targets at a shared contact");
        if (const auto fixed=saved.find(representative);fixed!=saved.end() && !points_exact(fixed->second,position))
            invalid("Joint translation conflicts with a saved fixed position");
    }
    std::map<std::string,Vec2,std::less<>> pins;
    for (const auto& [representative,target]:classes)
        if (!saved.contains(representative)) pins.emplace(target.first,target.second);
    return pins;
}

std::set<std::string, std::less<>> canonicalize_coincident_points(
    const ConstraintSolveRequest& request,
    const std::map<std::string, Vec2, std::less<>>& original,
    std::map<std::string, Vec2, std::less<>>& solved) {
    std::map<std::string, std::string, std::less<>> parents;
    const auto root = [&](const std::string& id) {
        parents.try_emplace(id, id);
        auto result = id;
        while (parents.at(result) != result) result = parents.at(result);
        auto current = id;
        while (parents.at(current) != current) {
            const auto next = parents.at(current);
            parents.at(current) = result;
            current = next;
        }
        return result;
    };
    for (const auto& constraint : request.constraints) {
        if (const auto* joint = std::get_if<CoincidentConstraint>(&constraint)) {
            const auto first = root(joint->first), second = root(joint->second);
            parents.at(std::max(first, second)) = std::min(first, second);
        } else if (const auto* tangent=std::get_if<TangentConstraint>(&constraint)) {
            const auto first=root(tangent->first_at_start ? tangent->first_start : tangent->first_end);
            const auto second=root(tangent->second_at_start ? tangent->second_start : tangent->second_end);
            parents.at(std::max(first,second))=std::min(first,second);
        }
    }
    std::map<std::string, std::vector<std::string>, std::less<>> classes;
    std::set<std::string, std::less<>> joined_points;
    for (const auto& [id, parent] : parents) {
        (void)parent;
        classes[root(id)].push_back(id);
        joined_points.insert(id);
    }
    std::map<std::string, Vec2, std::less<>> anchors;
    for (const auto& constraint : request.constraints) {
        const auto* anchor = std::get_if<FixedAnchorConstraint>(&constraint);
        if (!anchor || !joined_points.contains(anchor->point)) continue;
        const Vec2 position{anchor->x, anchor->y};
        const auto [found, inserted] = anchors.emplace(root(anchor->point), position);
        if (!inserted && !points_exact(found->second, position))
            invalid("Coincident endpoints have incompatible exact fixed positions");
    }
    for (const auto& [id, members] : classes) {
        auto position = anchors.contains(id) ? anchors.at(id) : solved.at(id);
        // Restore an unchanged shared vertex once. Restoring whole walls
        // independently can split an exact joint after a tolerance-based solve.
        if (!anchors.contains(id) && std::all_of(members.begin(), members.end(),
            [&](const auto& member) {
                return points_exact(original.at(member), original.at(id)) &&
                    points_near(solved.at(member), original.at(id));
            })) position = original.at(id);
        for (const auto& member : members) {
            if (!points_near(solved.at(member), position, constraint_linear_tolerance_metres))
                invalid("Constraint solver did not preserve a coincident endpoint joint");
        }
        for (const auto& member : members) solved.at(member) = position;
    }
    return joined_points;
}

bool has_organization_reference(const Entity& entity) {
    return entity.properties.contains("layer_id") || entity.properties.contains("floor_id") ||
        entity.properties.contains("building_id") || entity.properties.contains("property_id");
}

// At a fixed signed sweep, an analytical station is affine in both endpoints:
// C = S + (a I + b J)(E-S). It participates in the simultaneous solve.
std::pair<double,double> station_coefficients(double sweep,double fraction) {
    if (sweep == 0) return {fraction,0};
    const auto tangent = std::tan(sweep/2);
    if (!std::isfinite(tangent) || tangent == 0) invalid("Circular T station is numerically indeterminate");
    const auto phi = sweep*fraction;
    const auto half_sine = std::sin(phi/2);
    const auto one_minus_cosine = 2*half_sine*half_sine;
    const auto h = 0.5/tangent;
    return {0.5*one_minus_cosine+h*std::sin(phi), h*one_minus_cosine-0.5*std::sin(phi)};
}

bool baseline_same(const Segment& first, const Segment& second) {
    return first.sweep_radians == second.sweep_radians && points_near(first.start, second.start) &&
        points_near(first.end, second.end);
}

std::optional<PlanarTransform> selected_wall_rigid_transform(
    const ConstraintAuthoringIntent& intent, const std::string& wall_id) {
    if (intent.joint_translation && std::binary_search(intent.joint_translation->partial_wall_ids.begin(),
        intent.joint_translation->partial_wall_ids.end(),wall_id))
        return PlanarTransform{{},0,false,false,intent.joint_translation->offset};
    if (intent.wall_geometry_move)
        for (const auto& target : intent.wall_geometry_move->targets)
            if (target.wall_id==wall_id) return target.rigid_transform;
    return std::nullopt;
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
            if (constraint.bindings.size()>2)
                return "fixed physical arc total ("+constraint.length->original_expression+") on "+
                    std::to_string(constraint.bindings.size()/2)+" connected segments";
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
        case ConstraintRelationKind::tangent:
            return "tangent relation between "+segment_description(entities,constraint.bindings.at(0),constraint.bindings.at(1))+
                " and "+segment_description(entities,constraint.bindings.at(2),constraint.bindings.at(3));
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
    struct Source {
        const Entities& values;
        const DocumentSnapshot* retained{};
        const Entities& entities() const { return values; }
        Revision revision() const { return retained ? retained->revision() : 0; }
    };
    static ConstraintAuthoringPreview build(const DocumentSnapshot& snapshot,
                                             const ConstraintAuthoringIntent& raw_intent);
    static ConstraintAuthoringPreview build(Source snapshot,const ConstraintAuthoringIntent& raw_intent);
    static Command command_for(const DocumentSnapshot& snapshot, const ConstraintAuthoringPreview& preview);
    static Command command_for(const Entities& source,Revision revision,const ConstraintAuthoringPreview& preview,bool retain_joint=true);
};

ConstraintAuthoringPreview ConstraintAuthoringBuilder::build(
    const DocumentSnapshot& snapshot, const ConstraintAuthoringIntent& raw_intent) {
    return build(Source{snapshot.entities(),&snapshot},raw_intent);
}
ConstraintAuthoringPreview ConstraintAuthoringBuilder::build(Source snapshot,const ConstraintAuthoringIntent& raw_intent) {
    ConstraintAuthoringPreview result;
    if (snapshot.retained) result.document_id_=snapshot.retained->document_id();
    result.expected_revision_ = snapshot.revision();
    if (snapshot.retained) result.source_snapshot_digest_=document_snapshot_digest(*snapshot.retained);
    result.candidate_entities_ = snapshot.entities();

    try {
        if (snapshot.retained && !snapshot.retained->is_editable()) {
            invalid(snapshot.retained->read_only_reason().empty() ? "Document is read-only"
                                                        : snapshot.retained->read_only_reason());
        }
        result.normalized_intent_ = normalize_intent(raw_intent);
        if (result.normalized_intent_.joint_translation) {
            auto& move=*result.normalized_intent_.joint_translation;
            std::set<std::string,std::less<>> walls(move.partial_wall_ids.begin(),move.partial_wall_ids.end());
            for (const auto& id:move.rigid_boundary_ids) {
                const auto found=snapshot.entities().find(id);
                if (found==snapshot.entities().end() || !can_recognize_boundary_entity_type(found->second.type))
                    invalid("Joint rigid boundary target does not exist or has the wrong owner type");
                if (found->second.extensions.contains("physical_wall_room"))
                    invalid("Physical-wall rooms require explicit reviewed repair and cannot move independently");
                (void)decode_identified_boundary_entity(found->second);
                if (found->second.properties.contains("wall_measurement_source")) {
                    if (!wall_measurement_source_current(snapshot.entities(),found->second)) invalid("Selected physical measured boundary is stale");
                    const auto perimeter=exterior_corner_perimeter_ids(snapshot.entities(),found->second);
                    walls.insert(perimeter.begin(),perimeter.end());
                }
            }
            move.partial_wall_ids.assign(walls.begin(),walls.end());
            for (const auto& id:move.partial_wall_ids) (void)require_wall(snapshot.entities(),id);
            if (move.partial_wall_ids.size()+move.rigid_boundary_ids.size()+move.rigid_stroke_ids.size()+move.dimension_ids.size()>4096)
                invalid("Expanded joint translation targets exceed their aggregate budget");
            for (const auto& id:move.dimension_ids) {
                const auto found=snapshot.entities().find(id);
                if (found==snapshot.entities().end() || !can_recognize_boundary_dimension_entity_type(found->second.type))
                    invalid("Joint selected callout does not exist or has the wrong owner type");
                const auto decoded=decode_boundary_dimension_entity(found->second);
                if (!decoded.supported()) invalid(decoded.unsupported_reason);
                (void)decoded.dimension->resolve(snapshot.entities().at(decoded.dimension->boundary_id));
            }
        }
        if (result.normalized_intent_.boundary_resize) {
            const auto resize=*result.normalized_intent_.boundary_resize;
            const auto owner=snapshot.entities().find(resize.edit.boundary_id);
            if (owner!=snapshot.entities().end() && owner->second.properties.contains("wall_measurement_source")) {
                if (!resize.exact_length)
                    invalid("Source-derived exterior resize requires the exact entered length quantity");
                result.normalized_intent_.exterior_segment_resize=ExteriorSegmentResizeIntent{
                    resize.edit.boundary_id,resize.edit.target_id,*resize.exact_length,resize.edit.fixed_endpoint,
                    resize.edit.move_connected,resize.move_related_objects};
                (void)encode_exterior_segment_resize(*result.normalized_intent_.exterior_segment_resize);
                result.normalized_intent_.boundary_resize.reset();
            }
        }
        if (result.normalized_intent_.boundary_vertex_move) {
            const auto move = *result.normalized_intent_.boundary_vertex_move;
            const auto owner = snapshot.entities().find(move.edit.boundary_id);
            if (owner != snapshot.entities().end() && owner->second.properties.contains("wall_measurement_source")) {
                result.normalized_intent_.exterior_corner_move = ExteriorCornerMoveIntent{
                    move.edit.boundary_id, move.edit.target_id, move.edit.target_position, move.move_related_objects};
                result.normalized_intent_.boundary_vertex_move.reset();
            }
        }
        const auto& intent = result.normalized_intent_;
        const bool exterior_edit=intent.exterior_corner_move.has_value() || intent.exterior_segment_resize.has_value() ||
            intent.exterior_segment_arc.has_value();
        const auto exterior_boundary_id=intent.exterior_corner_move ? intent.exterior_corner_move->boundary_id :
            intent.exterior_segment_resize ? intent.exterior_segment_resize->boundary_id :
            intent.exterior_segment_arc ? intent.exterior_segment_arc->boundary_id : std::string{};
        const bool move_exterior_related=intent.exterior_corner_move ? intent.exterior_corner_move->move_connected_objects :
            intent.exterior_segment_resize ? intent.exterior_segment_resize->move_connected_objects :
            intent.exterior_segment_arc && intent.exterior_segment_arc->move_connected_objects;
        const BoundaryGeometryEdit* boundary_edit = intent.boundary_resize
            ? &intent.boundary_resize->edit
            : intent.boundary_vertex_move ? &intent.boundary_vertex_move->edit : nullptr;
        const bool move_related_objects = intent.boundary_resize
            ? intent.boundary_resize->move_related_objects
            : intent.boundary_vertex_move && intent.boundary_vertex_move->move_related_objects;
        const auto organization = organize_project(snapshot.entities());
        const auto before_constraints = decode_supported_constraints(snapshot.entities());
        auto candidate = snapshot.entities();
        std::optional<Entities> exterior_physical;
        std::set<std::string, std::less<>> exterior_physical_ids, exterior_owner_ids;
        std::set<std::string, std::less<>> exterior_ring_ids;
        std::vector<ExteriorCornerPhysicalContact> exterior_contacts;
        std::vector<ExteriorCornerPhysicalContact> exterior_t_contacts;
        if (exterior_edit) {
            const auto ring_ids = exterior_corner_perimeter_ids(snapshot.entities(),snapshot.entities().at(exterior_boundary_id));
            exterior_ring_ids.insert(ring_ids.begin(),ring_ids.end());
            exterior_contacts = exterior_corner_physical_contact_graph(snapshot.entities());
            for (const auto& contact : exterior_contacts)
                if (contact.station != 0 && contact.station != 1) exterior_t_contacts.push_back(contact);
            exterior_physical = intent.exterior_corner_move ?
                exterior_corner_physical_entities(snapshot.entities(),*intent.exterior_corner_move) :
                intent.exterior_segment_resize ?
                exterior_segment_resize_physical_entities(snapshot.entities(),*intent.exterior_segment_resize) :
                exterior_segment_arc_physical_entities(snapshot.entities(),*intent.exterior_segment_arc);
            for (const auto& [id, entity] : *exterior_physical)
                if (entity != snapshot.entities().at(id)) exterior_physical_ids.insert(id);
            candidate = *exterior_physical;
            const auto redraws = exterior_wall_measurement_source_updates(snapshot.entities(), candidate, false);
            for (const auto& redraw : redraws) exterior_owner_ids.insert(redraw.boundary_id);
            candidate = edited_boundary_entities_batch(candidate, redraws);
        }
        std::set<std::string, std::less<>> seeds;
        std::set<std::string,std::less<>> joint_source_boundaries;
        if (intent.joint_translation) {
            const auto& move=*intent.joint_translation;
            seeds.insert(move.rigid_boundary_ids.begin(),move.rigid_boundary_ids.end());
            seeds.insert(move.partial_wall_ids.begin(),move.partial_wall_ids.end());
            for (const auto& id:move.rigid_boundary_ids)
                if (snapshot.entities().at(id).properties.contains("wall_measurement_source")) joint_source_boundaries.insert(id);
        }
        seeds.insert(exterior_physical_ids.begin(), exterior_physical_ids.end());
        seeds.insert(exterior_owner_ids.begin(), exterior_owner_ids.end());
        if (exterior_edit) {
            const auto ids = exterior_wall_measurement_source_ids(snapshot.entities().at(exterior_boundary_id));
            seeds.insert(ids.begin(), ids.end());
        }
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
        if (intent.wall_geometry_move.has_value()) {
            for (const auto& target : intent.wall_geometry_move->targets) {
                (void)require_wall(candidate, target.wall_id);
                seeds.insert(target.wall_id);
            }
        }
        if (boundary_edit) {
            seeds.insert(boundary_edit->boundary_id);
        }
        std::map<std::string, ApplyBoundaryConstraintChanges::MeasuredStrokeEdit, std::less<>> selected_stroke_edits;
        const auto admit_stroke = [&](ApplyBoundaryConstraintChanges::MeasuredStrokeEdit edit) {
            const auto found = candidate.find(edit.stroke_id);
            if (found == candidate.end() || found->second.type != "measurement_linework")
                invalid("Measured coordinate intent requires an existing measured stroke");
            const auto decoded = decode_measurement_linework_model(found->second.properties.at("model"));
            if (!decoded.supported()) invalid(decoded.diagnostic);
            auto model = *decoded.model;
            if (edit.authored_edit) model = edited_measurement_linework(model,*edit.authored_edit,edit.authored_length);
            if (edit.rigid_transform) model = transformed_measurement_linework(model,*edit.rigid_transform);
            found->second.properties["model"] = encode_measurement_linework_model(model);
            seeds.insert(edit.stroke_id);
            selected_stroke_edits.emplace(edit.stroke_id,std::move(edit));
        };
        if (intent.measured_stroke_resize) {
            const auto& resize = *intent.measured_stroke_resize;
            admit_stroke({resize.edit.boundary_id,resize.edit,resize.exact_length,std::nullopt,{}});
        }
        if (intent.measured_stroke_vertex_move) {
            const auto& move = *intent.measured_stroke_vertex_move;
            admit_stroke({move.edit.boundary_id,move.edit,std::nullopt,std::nullopt,{}});
        }
        if (intent.measured_stroke_transform)
            for (const auto& target : intent.measured_stroke_transform->targets)
                admit_stroke({target.stroke_id,std::nullopt,std::nullopt,target.transform,{}});
        if (intent.joint_translation)
            for (const auto& id:intent.joint_translation->rigid_stroke_ids)
                admit_stroke({id,std::nullopt,std::nullopt,PlanarTransform{{},0,false,false,intent.joint_translation->offset},{}});
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

        for (const auto& contact : exterior_contacts) {
            adjacency[contact.owner].insert(contact.host);
            adjacency[contact.host].insert(contact.owner);
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
            if (intent.joint_translation && wall_entity.properties.contains("wall_measurement_source")) {
                if (!wall_measurement_source_current(snapshot.entities(),snapshot.entities().at(wall_id)))
                    invalid("Joint connected physical measured boundary is stale and requires explicit repair");
                // Its transient solver coordinates constrain the connected
                // solve, but only final physical walls authorize its redraw.
                // Final persisted relations validate that independent redraw.
                joint_source_boundaries.insert(wall_id);
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
        std::map<std::string, IdentifiedBoundary, std::less<>> measured_strokes;
        std::map<std::string, Vec2, std::less<>> positions;
        std::set<std::string, std::less<>> affected_walls;
        std::map<std::string, WallEndpointBinding, std::less<>> point_bindings;
        SolverConstraintDescriptions constraint_descriptions;
        ConstraintSolveRequest request;
        request.expected_revision = snapshot.revision();
        for (const auto& wall_id : affected) {
            const auto owner = candidate.find(wall_id);
            if (owner == candidate.end()) invalid("Constraint owner does not exist: " + wall_id);
            if (owner->second.type == "measurement_linework") {
                auto stroke = resolve_constraint_segment_owner(snapshot.entities().at(wall_id));
                result.measured_source_completion_ = true;
                for (const auto& edge : stroke.segments)
                    for (const auto role : {WallEndpointRole::start,WallEndpointRole::end}) {
                        WallEndpointBinding binding{wall_id,role,edge.segment_id,
                            role == WallEndpointRole::start ? edge.start_vertex_id : edge.end_vertex_id};
                        const auto id = point_id(binding);
                        const auto position = endpoint_position(edge.segment,role);
                        if (positions.emplace(id,position).second) {
                            point_bindings.emplace(id,binding);
                            request.points.push_back({id,position.x,position.y});
                        }
                    }
                measured_strokes.emplace(wall_id,std::move(stroke));
                continue;
            }
            if (can_recognize_boundary_entity_type(owner->second.type)) {
                auto boundary = decode_identified_boundary_entity(snapshot.entities().at(wall_id));
                const auto initial_boundary = decode_identified_boundary_entity(owner->second);
                WindingInvariant winding;
                winding.orientation = signed_area(boundary_geometry(boundary)) > 0
                    ? WindingOrientation::counter_clockwise : WindingOrientation::clockwise;
                for (const auto& edge : boundary.segments) {
                    WallEndpointBinding binding{wall_id, WallEndpointRole::start,
                        edge.segment_id, edge.start_vertex_id};
                    const auto id = point_id(binding);
                    point_bindings.emplace(id, binding);
                    positions.emplace(id, edge.segment.start);
                    auto initial = edge.segment.start;
                    if (exterior_edit) {
                        const auto initial_edge = std::find_if(initial_boundary.segments.begin(),initial_boundary.segments.end(),
                            [&](const auto& value) { return value.start_vertex_id == edge.start_vertex_id; });
                        if (initial_edge == initial_boundary.segments.end()) invalid("Derived exterior corner lineage is missing");
                        initial = initial_edge->segment.start;
                    }
                    request.points.push_back({id, initial.x, initial.y});
                    winding.loop.push_back(id);
                }
                if (std::all_of(boundary.segments.begin(),boundary.segments.end(),
                    [](const auto& edge) { return edge.segment.sweep_radians==0; }))
                    request.winding_invariants.push_back(std::move(winding));
                boundaries.emplace(wall_id, std::move(boundary));
                continue;
            }
            const auto baseline = read_baseline(require_wall(snapshot.entities(), wall_id));
            old_baselines.emplace(wall_id, baseline);
            affected_walls.insert(wall_id);
            for (const auto role : {WallEndpointRole::start, WallEndpointRole::end}) {
                WallEndpointBinding binding{wall_id, role};
                const auto id = point_id(binding);
                const auto position = endpoint_position(baseline, role);
                point_bindings.emplace(id, binding);
                positions.emplace(id, position);
                const auto initial = exterior_edit
                    ? endpoint_position(read_baseline(owner->second),role) : position;
                request.points.push_back({id, initial.x, initial.y});
            }
        }
        const auto resolve = [&](const WallEndpointBinding& binding) {
            validate_binding(binding);
            const auto boundary = boundaries.find(binding.owner_id);
            const auto stroke = measured_strokes.find(binding.owner_id);
            if (boundary != boundaries.end() || stroke != measured_strokes.end()) {
                const auto& edges = boundary != boundaries.end() ? boundary->second.segments : stroke->second.segments;
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

        std::set<std::pair<std::string,std::string>> physical_joints;
        for (const auto& contact : exterior_contacts) {
            if ((contact.station != 0 && contact.station != 1) ||
                !affected.contains(contact.owner) || !affected.contains(contact.host)) continue;
            const auto first = resolve({contact.owner,contact.start ? WallEndpointRole::start : WallEndpointRole::end});
            const auto second = resolve({contact.host,contact.station == 0 ? WallEndpointRole::start : WallEndpointRole::end});
            const auto pair = std::minmax(first,second);
            if (!physical_joints.emplace(pair.first,pair.second).second) continue;
            const auto id = unique_temporary_id(persistent_ids,physical_joints.size());
            persistent_ids.insert(id);
            request.constraints.push_back(CoincidentConstraint{id,first,second});
            constraint_descriptions.emplace(id,"existing physical wall endpoint joint");
        }

        for (const auto& contact : exterior_t_contacts) {
            if (!affected.contains(contact.owner) || !affected.contains(contact.host)) continue;
            const auto point = resolve({contact.owner,contact.start ? WallEndpointRole::start : WallEndpointRole::end});
            const auto start = resolve({contact.host,WallEndpointRole::start});
            const auto end = resolve({contact.host,WallEndpointRole::end});
            const auto [a,b] = station_coefficients(read_baseline(candidate.at(contact.host)).sweep_radians,contact.station);
            const auto id = unique_temporary_id(persistent_ids,persistent_ids.size());
            persistent_ids.insert(id);
            request.constraints.push_back(AffineStationConstraint{id,point,start,end,a,b});
            constraint_descriptions.emplace(id,"existing physical wall T station");
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

        if (intent.joint_translation) {
            const auto& move=*intent.joint_translation;
            std::set<std::string,std::less<>> selected(move.rigid_boundary_ids.begin(),move.rigid_boundary_ids.end());
            selected.insert(move.rigid_stroke_ids.begin(),move.rigid_stroke_ids.end());
            selected.insert(move.partial_wall_ids.begin(),move.partial_wall_ids.end());
            const PlanarTransform translation{{},0,false,false,move.offset};
            for (const auto& [id,position]:positions) {
                const auto& binding=point_bindings.at(id);
                if (selected.contains(binding.owner_id)) add_fixed(binding,transform_point(position,translation));
                else if (!move.move_connected_objects || snapshot.entities().at(binding.owner_id).extensions.contains("physical_wall_room"))
                    add_fixed(binding,position);
            }
        } else if (exterior_edit) {
            for (const auto& [id, position] : positions) {
                const auto& binding = point_bindings.at(id);
                if (exterior_ring_ids.contains(binding.owner_id))
                    add_fixed(binding, endpoint_position(read_baseline(candidate.at(binding.owner_id)), binding.role));
                else if (exterior_owner_ids.contains(binding.owner_id)) {
                    const auto boundary = decode_identified_boundary_entity(candidate.at(binding.owner_id));
                    const auto edge = std::find_if(boundary.segments.begin(), boundary.segments.end(),
                        [&](const auto& value) { return value.start_vertex_id == binding.vertex_id; });
                    if (edge == boundary.segments.end()) invalid("Derived exterior corner lineage is missing");
                    add_fixed(binding, edge->segment.start);
                } else if (!move_exterior_related)
                    add_fixed(binding, position);
            }
        } else if (intent.wall_geometry_move.has_value()) {
            const auto& move = *intent.wall_geometry_move;
            std::set<std::string, std::less<>> selected_walls;
            std::set<std::string, std::less<>> movable_component;
            for (const auto& target : move.targets) {
                const auto old = old_baselines.at(target.wall_id);
                const auto old_length = segment_length(old);
                if (!std::isfinite(old_length) ||
                    old_length <= default_geometry_tolerance_metres) {
                    invalid("Selected wall has a degenerate or unrepresentable baseline: " +
                            target.wall_id);
                }
                const Segment proposed{target.proposed_start, target.proposed_end,
                    target.rigid_transform ? transform_segment(old,*target.rigid_transform).sweep_radians : old.sweep_radians};
                if (target.rigid_transform) {
                    const auto entry=unchanged_wall_length_entry(snapshot.entities().at(target.wall_id),old,proposed);
                    (void)replay_constraint_wall_edit(snapshot.entities().at(target.wall_id),
                        {target.wall_id,proposed,entry,4,target.rigid_transform});
                }
                const auto proposed_length = segment_length(proposed);
                if (!std::isfinite(proposed_length) ||
                    proposed_length <= default_geometry_tolerance_metres) {
                    invalid("Wall geometry move target baseline is degenerate: " + target.wall_id);
                }
                if (std::abs(proposed_length - old_length) >
                    constraint_linear_tolerance_metres) {
                    invalid("Wall geometry move must preserve physical wall length: " +
                            target.wall_id);
                }
                if (old.sweep_radians != 0.0) {
                    (void)arc_from_chord_angle(proposed.start, proposed.end,
                                               proposed.sweep_radians);
                }
                selected_walls.insert(target.wall_id);
                add_fixed({target.wall_id, WallEndpointRole::start}, proposed.start);
                add_fixed({target.wall_id, WallEndpointRole::end}, proposed.end);
                const auto component = connected_from(target.wall_id);
                movable_component.insert(component.begin(), component.end());
            }
            if (has_upsert && intent.relation_anchor.has_value() &&
                !movable_component.contains(intent.relation_anchor->owner_id)) {
                invalid("Relation anchor is outside the moved wall component");
            }
            for (const auto& [id, position] : positions) {
                const auto& binding = point_bindings.at(id);
                if (selected_walls.contains(binding.owner_id)) {
                    continue;
                }
                if (!move.move_connected_walls ||
                    !movable_component.contains(binding.owner_id)) {
                    add_fixed(binding, position);
                }
            }
        } else if (intent.wall_resize.has_value()) {
            const auto& resize = *intent.wall_resize;
            const auto old = old_baselines.at(resize.wall_id);
            const long double dx = static_cast<long double>(old.end.x) - old.start.x;
            const long double dy = static_cast<long double>(old.end.y) - old.start.y;
            const long double old_length = std::hypot(dx, dy);
            if (!std::isfinite(old_length) || old_length <= default_geometry_tolerance_metres) {
                invalid("Selected wall has a degenerate or unrepresentable baseline");
            }
            Segment target = old;
            if (resize.proposed_endpoint) {
                if (resize.anchored_endpoint == WallResizeAnchor::start)
                    target.end = *resize.proposed_endpoint;
                else
                    target.start = *resize.proposed_endpoint;
                const auto chord_length = std::hypot(target.end.x - target.start.x,
                                                     target.end.y - target.start.y);
                if (!std::isfinite(chord_length) ||
                    chord_length <= constraint_linear_tolerance_metres) {
                    invalid("Wall resize endpoint target baseline is degenerate or unrepresentable");
                }
                const auto physical_length = segment_length(target);
                if (!std::isfinite(physical_length) ||
                    std::abs(physical_length - resize.exact_length.metres) >
                        constraint_linear_tolerance_metres) {
                    invalid("Wall resize endpoint target does not match the exact physical length");
                }
            } else {
                const long double ux = dx / old_length;
                const long double uy = dy / old_length;
                long double length=resize.exact_length.metres;
                if (old.sweep_radians!=0) {
                    PersistentConstraint physical;
                    physical.id="resize-physical-arc";
                    physical.relation=ConstraintRelationKind::fixed_arc_length;
                    physical.bindings={{resize.wall_id,WallEndpointRole::start},{resize.wall_id,WallEndpointRole::end}};
                    physical.length=resize.exact_length;
                    length=constraint_arc_chord_target(physical,candidate);
                }
                if (resize.anchored_endpoint == WallResizeAnchor::start) {
                    target.end = {static_cast<double>(static_cast<long double>(old.start.x) + ux * length),
                                  static_cast<double>(static_cast<long double>(old.start.y) + uy * length)};
                } else {
                    target.start = {static_cast<double>(static_cast<long double>(old.end.x) - ux * length),
                                    static_cast<double>(static_cast<long double>(old.end.y) - uy * length)};
                }
            }
            if (!std::isfinite(target.start.x) || !std::isfinite(target.start.y) ||
                !std::isfinite(target.end.x) || !std::isfinite(target.end.y)) {
                invalid("Requested wall length exceeds the supported coordinate range");
            }
            if (std::abs(segment_length(target)-resize.exact_length.metres)>constraint_linear_tolerance_metres)
                invalid("Requested physical wall length cannot be represented at this coordinate scale");
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
        } else if (!selected_stroke_edits.empty()) {
            std::set<std::string,std::less<>> movable_component;
            for (const auto& [owner_id, edit] : selected_stroke_edits) {
                (void)edit;
                const auto proposed = resolve_constraint_segment_owner(candidate.at(owner_id));
                for (const auto& edge : proposed.segments)
                    for (const auto role : {WallEndpointRole::start,WallEndpointRole::end})
                        add_fixed({owner_id,role,edge.segment_id,
                            role == WallEndpointRole::start ? edge.start_vertex_id : edge.end_vertex_id},
                            endpoint_position(edge.segment,role));
                const auto component = connected_from(owner_id);
                movable_component.insert(component.begin(),component.end());
            }
            if (has_upsert && intent.relation_anchor && !movable_component.contains(intent.relation_anchor->owner_id))
                invalid("Relation anchor is outside the edited measured component");
            const bool move_related = intent.measured_stroke_resize ? intent.measured_stroke_resize->move_related_objects :
                intent.measured_stroke_vertex_move ? intent.measured_stroke_vertex_move->move_related_objects :
                intent.measured_stroke_transform->move_related_objects;
            for (const auto& [id,position] : positions) {
                const auto& binding = point_bindings.at(id);
                if (selected_stroke_edits.contains(binding.owner_id)) continue;
                if (!move_related || !movable_component.contains(binding.owner_id)) add_fixed(binding,position);
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
            (intent.wall_resize.has_value() || intent.wall_geometry_move.has_value() ||
             boundary_edit || !selected_stroke_edits.empty())) {
            const auto& anchor = *intent.relation_anchor;
            add_fixed(anchor, positions.at(resolve(anchor)));
        }

        std::map<std::string, Vec2, std::less<>> solved_points;
        {
            std::size_t temporary_index = 0;
            const auto temporary_pins=intent.joint_translation ? joint_temporary_pins(request,fixed_points) : fixed_points;
            for (const auto& [id, position] : temporary_pins) {
                const auto temporary_id = unique_temporary_id(persistent_ids, temporary_index++);
                request.constraints.push_back(FixedAnchorConstraint{temporary_id, id, position.x, position.y});
                constraint_descriptions.insert_or_assign(temporary_id,
                    "fixed endpoint: " + endpoint_description(candidate, point_bindings.at(id)));
            }
            const auto solver_preview = solve_planar_constraints(request);
            result.degrees_of_freedom_ = solver_preview.degrees_of_freedom;
            result.diagnostics_ = solver_diagnostics(solver_preview, constraint_descriptions);
            if (!solver_preview.accepted()) {
                if (result.diagnostics_.empty()) result.diagnostics_.push_back("Constraint solver rejected the authoring intent");
                return result;
            }
            solved_points.clear();
            for (const auto& solved : solver_preview.points) solved_points.emplace(solved.id, Vec2{solved.x, solved.y});
            for (const auto& [id, position] : fixed_points) {
                const auto solved = solved_points.find(id);
                if (solved == solved_points.end() || !points_near(solved->second, position, constraint_linear_tolerance_metres))
                    invalid("Constraint solver did not preserve an authoring anchor");
                solved->second = position;
            }
        }

        const auto coincident_points = canonicalize_coincident_points(request, positions, solved_points);
        if (intent.wall_resize && intent.wall_resize->proposed_endpoint) {
            const auto& resize = *intent.wall_resize;
            const auto& old = old_baselines.at(resize.wall_id);
            const auto expected_start = resize.anchored_endpoint == WallResizeAnchor::start
                ? old.start : *resize.proposed_endpoint;
            const auto expected_end = resize.anchored_endpoint == WallResizeAnchor::end
                ? old.end : *resize.proposed_endpoint;
            if (!points_exact(solved_points.at(point_id({resize.wall_id, WallEndpointRole::start})), expected_start) ||
                !points_exact(solved_points.at(point_id({resize.wall_id, WallEndpointRole::end})), expected_end)) {
                invalid("Constraint solve did not preserve exact wall resize endpoint targets");
            }
        }
        std::set<std::string,std::less<>> selected_rigid_ids;
        for (const auto& wall_id : affected_walls) {
            const auto old = old_baselines.at(wall_id);
            const auto rigid_transform=selected_wall_rigid_transform(intent,wall_id);
            Segment proposed{
                solved_points.at(point_id({wall_id, WallEndpointRole::start})),
                solved_points.at(point_id({wall_id, WallEndpointRole::end})),
                rigid_transform ? transform_segment(old,*rigid_transform).sweep_radians :
                    exterior_physical_ids.contains(wall_id) ? read_baseline(candidate.at(wall_id)).sweep_radians : old.sweep_radians};
            const bool endpoint_target = intent.wall_resize &&
                intent.wall_resize->wall_id == wall_id && intent.wall_resize->proposed_endpoint;
            if (!rigid_transform && !endpoint_target && baseline_same(old, proposed) &&
                (!coincident_points.contains(point_id({wall_id, WallEndpointRole::start})) ||
                    points_exact(old.start, proposed.start)) &&
                (!coincident_points.contains(point_id({wall_id, WallEndpointRole::end})) ||
                    points_exact(old.end, proposed.end))) {
                proposed = old;
            }
            if (proposed.start.x == old.start.x && proposed.start.y == old.start.y &&
                proposed.end.x == old.end.x && proposed.end.y == old.end.y && proposed.sweep_radians == old.sweep_radians) {
                // A saved constraint can solve an attachment back to its
                // original geometry. Discard its provisional contact redraw.
                if (exterior_edit && !exterior_ring_ids.contains(wall_id))
                    candidate.at(wall_id) = snapshot.entities().at(wall_id);
                continue;
            }
            if (!rigid_transform) validate_endpoint_identity_not_swapped(old, proposed, wall_id);
            auto& wall_entity = candidate.at(wall_id);
            const bool resized = intent.wall_resize.has_value() &&
                intent.wall_resize->wall_id == wall_id;
            const auto& original_wall = snapshot.entities().at(wall_id);
            const auto length_entry = resized
                ? std::optional<Quantity>{intent.wall_resize->exact_length}
                : unchanged_wall_length_entry(original_wall, old, proposed);
            auto proof_rigid_transform=rigid_transform;
            // Historical v4 is selected rigid curve authority. Joint replay
            // independently verifies exact translated straight-wall targets;
            // its lower proof uses the existing ordinary straight dialect.
            if (intent.joint_translation && old.sweep_radians==0.0) proof_rigid_transform.reset();
            const auto proof_version = proof_rigid_transform ? 4ULL : old.sweep_radians == 0.0
                ? 1ULL : length_entry.has_value() ? 3ULL : 2ULL;
            if (exterior_ring_ids.contains(wall_id)) {
                // The perimeter is pinned to the independently reconstructed
                // inverse. Keep its physical construction provenance when the
                // solver returns that same geometry, including a straight origin.
                if (!intent.exterior_segment_arc || !baseline_same(read_baseline(wall_entity), proposed))
                    wall_entity = intent.exterior_segment_arc
                        ? reconstruct_exterior_segment_arc_wall(original_wall, proposed)
                        : reconstruct_exterior_corner_wall(original_wall, proposed);
            }
            else wall_entity = replay_constraint_wall_edit(
                original_wall, {wall_id, proposed, length_entry, proof_version,proof_rigid_transform});
            if (rigid_transform) selected_rigid_ids.insert(wall_id);
            validate_constraint_wall_host(wall_id, candidate);
            result.changed_walls_.push_back({wall_id, old, proposed});
        }

        if (boundary_edit) result.boundary_edits_.push_back(*boundary_edit);
        for (const auto& [id, binding] : point_bindings) {
            const bool selected_joint_boundary=intent.joint_translation && std::binary_search(
                intent.joint_translation->rigid_boundary_ids.begin(),intent.joint_translation->rigid_boundary_ids.end(),binding.owner_id);
            if (!boundaries.contains(binding.owner_id) ||
                (coincident_points.contains(id) || selected_joint_boundary ? points_exact(solved_points.at(id), positions.at(id)) :
                    points_near(solved_points.at(id), positions.at(id))))
                continue;
            if (boundary_edit && binding.owner_id == boundary_edit->boundary_id)
                continue;
            if (exterior_owner_ids.contains(binding.owner_id)) continue;
            if (joint_source_boundaries.contains(binding.owner_id)) continue;
            BoundaryGeometryEdit edit;
            edit.boundary_id = binding.owner_id;
            edit.target_id = binding.vertex_id;
            edit.target_position = solved_points.at(id);
            result.boundary_edits_.push_back(std::move(edit));
        }
        candidate = edited_boundary_entities_batch(candidate, result.boundary_edits_);
        for (const auto& [owner_id, before] : measured_strokes) {
            auto proof = selected_stroke_edits.contains(owner_id) ? selected_stroke_edits.at(owner_id) :
                ApplyBoundaryConstraintChanges::MeasuredStrokeEdit{owner_id,std::nullopt,std::nullopt,std::nullopt,{}};
            const auto current = resolve_constraint_segment_owner(candidate.at(owner_id));
            std::set<std::string,std::less<>> seen;
            for (const auto& edge : current.segments)
                for (const auto role : {WallEndpointRole::start,WallEndpointRole::end}) {
                    const auto& vertex = role == WallEndpointRole::start ? edge.start_vertex_id : edge.end_vertex_id;
                    if (!seen.insert(vertex).second) continue;
                    WallEndpointBinding binding{owner_id,role,edge.segment_id,vertex};
                    const auto id = point_id(binding);
                    const auto solved = solved_points.at(id);
                    const auto previous = endpoint_position(edge.segment,role);
                    if (coincident_points.contains(id) ? points_exact(solved,previous) : points_near(solved,previous))
                        continue;
                    BoundaryGeometryEdit edit;
                    edit.boundary_id = owner_id; edit.target_id = vertex; edit.target_position = solved;
                    proof.vertex_edits.push_back(std::move(edit));
                }
            auto& entity = candidate.at(owner_id);
            const auto model = decode_measurement_linework_model(entity.properties.at("model"));
            entity.properties["model"] = encode_measurement_linework_model(
                edited_measurement_linework_vertices(*model.model,proof.vertex_edits));
            if (entity != snapshot.entities().at(owner_id)) {
                result.measured_stroke_edits_.push_back(std::move(proof));
                result.changed_measured_strokes_.push_back({owner_id,
                    replay_measurement_linework(*decode_measurement_linework_model(snapshot.entities().at(owner_id).properties.at("model")).model),
                    replay_measurement_linework(*decode_measurement_linework_model(entity.properties.at("model")).model)});
            }
            (void)before;
        }
        // Recompute every redraw from the original measured owners and final
        // solved physical geometry; the intermediate derived copies are pins.
        for (const auto& id : exterior_owner_ids) candidate.at(id) = snapshot.entities().at(id);
        std::map<std::string,Vec2,std::less<>> rigid_source_offsets;
        if (intent.joint_translation) for (const auto& id:intent.joint_translation->rigid_boundary_ids)
            if (joint_source_boundaries.contains(id)) rigid_source_offsets.emplace(id,intent.joint_translation->offset);
        result.exterior_source_edits_ = exterior_wall_measurement_source_updates(
            snapshot.entities(), candidate,true,rigid_source_offsets);
        for (const auto& edit : result.exterior_source_edits_) {
            if (std::any_of(result.boundary_edits_.begin(), result.boundary_edits_.end(),
                [&](const auto& authored) { return authored.boundary_id == edit.boundary_id; }))
                invalid("Source-measured owner cannot also receive an authored boundary edit: " + edit.boundary_id);
            boundaries.try_emplace(edit.boundary_id,
                decode_identified_boundary_entity(snapshot.entities().at(edit.boundary_id)));
        }
        candidate = edited_boundary_entities_batch(candidate, result.exterior_source_edits_);
        for (const auto& [id, owner] : snapshot.entities())
            if (owner.properties.contains("wall_measurement_source") &&
                wall_measurement_source_current(snapshot.entities(), owner) &&
                (!candidate.contains(id) || !wall_measurement_source_current(candidate, candidate.at(id))))
                invalid("Constraint authoring would stale the current source walls: " + id);
        for (const auto& [id, before] : boundaries) {
            const auto after = decode_identified_boundary_entity(candidate.at(id));
            if (after != before) result.changed_boundaries_.push_back({before, after});
        }
        if (exterior_edit) validate_exterior_corner_edit_topology(snapshot.entities(),candidate);
        else validate_constraint_edit_topology(snapshot.entities(),candidate,selected_rigid_ids);
        if (exterior_edit) validate_exterior_corner_physical_contacts(snapshot.entities(), candidate);
        if (intent.exterior_segment_resize)
            validate_exterior_segment_resize_result(snapshot.entities(),candidate,*intent.exterior_segment_resize);
        if (intent.exterior_segment_arc)
            validate_exterior_segment_arc_result(snapshot.entities(),candidate,*intent.exterior_segment_arc);
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
            result.exterior_source_edits_.clear();
            result.changed_measured_strokes_.clear();
            result.measured_stroke_edits_.clear();
            result.diagnostics_.push_back("Constraint authoring intent makes no document change");
            return result;
        }

        result.accepted_ = true;
        result.candidate_entities_ = std::move(candidate);
        if (result.measured_source_completion_ || intent.joint_translation) {
            if (snapshot.retained && !intent.joint_translation) {
                const auto completed=Document::preview_command(*snapshot.retained,command_for(*snapshot.retained,result));
                result.candidate_entities_=completed.entities();
            } else if (result.measured_source_completion_)
                result.candidate_entities_=complete_measurement_linework_sources(snapshot.entities(),result.candidate_entities_);
            if (intent.joint_translation) {
                const auto& move=*intent.joint_translation;
                std::set<std::string,std::less<>> rigid(move.rigid_boundary_ids.begin(),move.rigid_boundary_ids.end());
                rigid.insert(move.rigid_stroke_ids.begin(),move.rigid_stroke_ids.end());
                for (const auto& [id,entity]:snapshot.entities()) {
                    if (entity.type!="dimension") continue;
                    const auto decoded=decode_boundary_dimension_entity(entity);
                    if (!decoded.supported()) invalid(decoded.unsupported_reason);
                    const bool rigid_owner=rigid.contains(decoded.dimension->boundary_id);
                    if (!rigid_owner && !std::binary_search(move.dimension_ids.begin(),move.dimension_ids.end(),id)) continue;
                    auto placed=*decoded.dimension;
                    placed.text_position=transform_point(placed.text_position,PlanarTransform{{},0,false,false,move.offset});
                    if (!rigid_owner) { placed.placement=BoundaryDimensionPlacement::manual; placed.automatic_placement_version.reset(); }
                    result.candidate_entities_.at(id)=encode_boundary_dimension_entity(placed,&entity);
                }
                for (const auto& id:move.rigid_boundary_ids) {
                    const auto before=decode_identified_boundary_entity(snapshot.entities().at(id));
                    const auto after=decode_identified_boundary_entity(result.candidate_entities_.at(id));
                    if (before.segments.size()!=after.segments.size()) invalid("Joint translation changed selected boundary topology");
                    for (std::size_t i=0;i<before.segments.size();++i) {
                        const auto& a=before.segments[i]; const auto& b=after.segments[i];
                        const auto expected=transform_segment(a.segment,PlanarTransform{{},0,false,false,move.offset});
                        if (a.segment_id!=b.segment_id || a.start_vertex_id!=b.start_vertex_id || a.end_vertex_id!=b.end_vertex_id ||
                            !points_exact(expected.start,b.segment.start) || !points_exact(expected.end,b.segment.end) ||
                            expected.sweep_radians!=b.segment.sweep_radians) invalid("Joint translation did not preserve exact selected boundary targets");
                    }
                }
            }
            if (intent.exterior_segment_arc)
                validate_exterior_segment_arc_result(snapshot.entities(),result.candidate_entities_,*intent.exterior_segment_arc);
            (void)validate_constraint_integrity(result.candidate_entities_);
            result.changed_boundaries_.clear();
            for (const auto& [id, before] : snapshot.entities()) {
                const auto after = result.candidate_entities_.find(id);
                if (!can_recognize_boundary_entity_type(before.type) || after == result.candidate_entities_.end() ||
                    before == after->second || inspect_boundary_entity_version(before).format != BoundaryEntityFormat::identified_v1)
                    continue;
                const auto original = decode_identified_boundary_entity(before);
                const auto proposed = decode_identified_boundary_entity(after->second);
                if (original != proposed) result.changed_boundaries_.push_back({original,proposed});
            }
        }
        if (snapshot.retained) {
            result.candidate_digest_ = entity_map_digest(result.candidate_entities_);
            result.shown_result_digest_ = digest_shown_result(result);
        }
        return result;
    } catch (const std::exception& error) {
        result.accepted_ = false;
        result.changed_walls_.clear();
        result.changed_boundaries_.clear();
        result.boundary_edits_.clear();
        result.exterior_source_edits_.clear();
        result.changed_measured_strokes_.clear();
        result.measured_stroke_edits_.clear();
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
const std::vector<ConstraintMeasuredStrokeChange>& ConstraintAuthoringPreview::changed_measured_strokes() const noexcept {
    return changed_measured_strokes_;
}

const std::vector<BoundaryGeometryEdit>& ConstraintAuthoringPreview::exterior_source_edits() const noexcept {
    return exterior_source_edits_;
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
            } else if (can_recognize_boundary_entity_type(entity.type) || entity.type == "measurement_linework") {
                auto boundary = resolve_constraint_segment_owner(entity);
                for (const auto& edge : boundary.segments) {
                    add_point({id,WallEndpointRole::start,edge.segment_id,edge.start_vertex_id}, edge.segment.start);
                    if (entity.type == "measurement_linework")
                        add_point({id,WallEndpointRole::end,edge.segment_id,edge.end_vertex_id},edge.segment.end);
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

Command constraint_authoring_verified_command(const DocumentSnapshot& current,
    const ConstraintAuthoringPreview& preview, std::optional<DocumentSnapshot>* candidate) {
    if (!preview.accepted_) {
        invalid("A rejected constraint preview cannot be applied");
    }
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

    const auto command = ConstraintAuthoringBuilder::command_for(current,recomputed);
    const auto verified = Document::preview_command(current,command);
    if (verified.entities() != recomputed.candidate_entities_ || verified.assets() != current.assets())
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "Constraint replay does not reproduce the shown result");
    if (candidate) *candidate = verified;
    return command;
}

Command ConstraintAuthoringBuilder::command_for(const DocumentSnapshot& current,
    const ConstraintAuthoringPreview& recomputed) {
    return command_for(current.entities(),current.revision(),recomputed);
}
Command ConstraintAuthoringBuilder::command_for(const Entities& current,Revision revision,
    const ConstraintAuthoringPreview& recomputed,bool retain_joint) {
    std::vector<EntityChange> changes;
    for (const auto& [id, entity] : current) {
        const auto found = recomputed.candidate_entities_.find(id);
        if (found == recomputed.candidate_entities_.end()) {
            changes.push_back(EntityChange::erase(id));
        } else if (found->second != entity) {
            changes.push_back(EntityChange::upsert(found->second));
        }
    }
    for (const auto& [id, entity] : recomputed.candidate_entities_) {
        if (!current.contains(id)) {
            changes.push_back(EntityChange::upsert(entity));
        }
    }
    if (changes.empty()) {
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "Constraint preview does not contain a document change");
    }
    if (!recomputed.boundary_edits_.empty() || !recomputed.changed_walls_.empty() ||
        !recomputed.exterior_source_edits_.empty() || recomputed.measured_source_completion_) {
        std::vector<EntityChange> constraint_changes;
        for (const auto& change : changes) {
            const auto id = change.kind == EntityChangeKind::upsert
                ? change.entity.id : change.entity_id;
            const auto before = current.find(id);
            const bool was_constraint = before != current.end() &&
                before->second.type == "constraint";
            const bool is_constraint = change.kind == EntityChangeKind::upsert &&
                change.entity.type == "constraint";
            if (was_constraint || is_constraint) {
                constraint_changes.push_back(change);
            }
        }
        ApplyBoundaryConstraintChanges command{
            revision, recomputed.boundary_edits_,
            std::move(constraint_changes), recomputed.normalized_intent_.message};
        if (recomputed.normalized_intent_.joint_translation) {
            const auto& move=*recomputed.normalized_intent_.joint_translation;
            if (retain_joint) { command.joint_translation=move; command.joint_translation_completion=true; }
            std::set<std::string,std::less<>> rigid(move.rigid_boundary_ids.begin(),move.rigid_boundary_ids.end());
            rigid.insert(move.rigid_stroke_ids.begin(),move.rigid_stroke_ids.end());
            for (const auto& [id,entity]:current) {
                if (entity.type!="dimension") continue;
                const auto decoded=decode_boundary_dimension_entity(entity);
                if (!decoded.supported()) invalid(decoded.unsupported_reason);
                if (rigid.contains(decoded.dimension->boundary_id) || std::binary_search(move.dimension_ids.begin(),move.dimension_ids.end(),id))
                    command.dimension_placement_moves.push_back({id,move.offset});
            }
            command.dimension_placement_completion=!command.dimension_placement_moves.empty();
        }
        command.exterior_source_edits = recomputed.exterior_source_edits_;
        command.exterior_source_completion = !recomputed.exterior_source_edits_.empty();
        command.exterior_corner_move = recomputed.normalized_intent_.exterior_corner_move;
        command.exterior_segment_resize = recomputed.normalized_intent_.exterior_segment_resize;
        command.exterior_segment_arc = recomputed.normalized_intent_.exterior_segment_arc;
        command.measured_stroke_edits = recomputed.measured_stroke_edits_;
        command.measured_source_completion = recomputed.measured_source_completion_;
        for (const auto& wall : recomputed.changed_walls_) {
            if (command.exterior_corner_move || command.exterior_segment_resize || command.exterior_segment_arc) {
                const auto& owner_id=command.exterior_corner_move ? command.exterior_corner_move->boundary_id :
                    command.exterior_segment_resize ? command.exterior_segment_resize->boundary_id : command.exterior_segment_arc->boundary_id;
                const auto ids = exterior_corner_perimeter_ids(current,current.at(owner_id));
                if (std::find(ids.begin(),ids.end(),wall.wall_id) != ids.end()) continue;
            }
            const auto& resize = recomputed.normalized_intent_.wall_resize;
            const bool resized = resize && resize->wall_id == wall.wall_id;
            const auto length_entry = resized
                ? std::optional<Quantity>{resize->exact_length}
                : unchanged_wall_length_entry(current.at(wall.wall_id),
                    wall.old_baseline, wall.proposed_baseline);
            const auto rigid_transform=selected_wall_rigid_transform(recomputed.normalized_intent_,wall.wall_id);
            auto proof_rigid_transform=rigid_transform;
            if (recomputed.normalized_intent_.joint_translation && wall.old_baseline.sweep_radians==0.0) proof_rigid_transform.reset();
            const auto proof_version = proof_rigid_transform ? 4ULL : wall.old_baseline.sweep_radians == 0.0
                ? 1ULL : length_entry.has_value() ? 3ULL : 2ULL;
            command.wall_edits.push_back({wall.wall_id, wall.proposed_baseline,
                length_entry, proof_version,proof_rigid_transform});
            command.rigid_wall_transform_completion=command.rigid_wall_transform_completion || proof_rigid_transform.has_value();
        }
        return Command{std::move(command)};
    }
    Command command = ApplyEntityChanges{
        .expected_revision = revision,
        .entity_changes = std::move(changes),
        .message = recomputed.normalized_intent_.message,
    };
    return command;
}

ApplyBoundaryConstraintChanges reconstruct_joint_translation(const Entities& source,const JointTranslationIntent& intent) {
    ConstraintAuthoringIntent authoring; authoring.joint_translation=intent;
    const auto preview=ConstraintAuthoringBuilder::build(ConstraintAuthoringBuilder::Source{source,nullptr},authoring);
    if (!preview.accepted()) invalid(preview.diagnostics().empty() ? "Joint translation reconstruction rejected" : preview.diagnostics().front());
    const auto command=ConstraintAuthoringBuilder::command_for(source,0,preview,false);
    if (!std::holds_alternative<ApplyBoundaryConstraintChanges>(command)) invalid("Joint translation reconstruction has no typed geometry proof");
    return std::get<ApplyBoundaryConstraintChanges>(command);
}

DocumentSnapshot preview_constraint_authoring_snapshot(
    const DocumentSnapshot& source, const ConstraintAuthoringPreview& preview) {
    std::optional<DocumentSnapshot> candidate;
    (void)constraint_authoring_verified_command(source, preview, &candidate);
    return std::move(candidate.value());
}

Revision apply_constraint_authoring(Document& document,
                                     const ConstraintAuthoringPreview& preview) {
    return document.apply(constraint_authoring_verified_command(document.snapshot(), preview, nullptr));
}

}  // namespace sketch
