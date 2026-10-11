#include "sketch/selection_geometry_transform.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/appraisal_area_partition.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
using Entities = std::map<std::string, Entity, std::less<>>;
using Operators = std::map<std::string, PlanarTransform, std::less<>>;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Selected geometry transform: " + reason);
}
bool closed(const Entity& entity) {
    return entity.type == "boundary" || entity.type == "measurement_boundary" ||
        entity.type == "room_boundary";
}
bool geometric(const Entity& entity) {
    return closed(entity) || entity.type == "wall" || entity.type == "measurement_linework";
}
bool exact(const Entity& first, const Entity& second) {
    return first == second && first.properties.dump() == second.properties.dump() &&
        first.extensions.dump() == second.extensions.dump();
}
void budget(std::size_t size) {
    if (size > maximum_selection_geometry_transform_entities)
        invalid("complete dependency graph exceeds the numeric selection limit");
}
void keys(const Json& value, std::initializer_list<std::string_view> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("invalid request shape");
    for (const auto key : expected) if (!value.contains(key)) invalid("missing request field");
}
SelectionGeometryTransformRequest normalized(SelectionGeometryTransformRequest request) {
    if (request.roots.empty()) invalid("select at least one geometric root");
    budget(request.roots.size());
    if (request.message.empty() || request.message.size() > 1024 ||
        request.message.find('\0') != std::string::npos)
        invalid("message must be nonempty UTF-8 of at most 1024 bytes");
    // Strict JSON encoding rejects malformed UTF-8 without changing the text.
    (void)Json(request.message).dump();
    Ids roots;
    for (const auto& root : request.roots) {
        validate_boundary_transform({root.root_id, root.transform});
        if (!roots.insert(root.root_id).second) invalid("selected roots must be unique");
        // Finite parameters alone can still produce an overflowing affine offset.
        for (const auto point : {Vec2{}, Vec2{1, 0}, Vec2{0, 1}}) {
            const auto affine = transform_point(point, root.transform);
            if (!std::isfinite(affine.x) || !std::isfinite(affine.y))
                invalid("captured affine operator overflows");
        }
    }
    std::sort(request.roots.begin(), request.roots.end(), [](const auto& a, const auto& b) {
        return a.root_id < b.root_id;
    });
    return request;
}
std::vector<std::string> deductions(const Entity& entity) {
    const auto found = entity.properties.find("deduction_ids");
    if (found == entity.properties.end()) return {};
    if (!found->is_array()) invalid("deduction_ids must be an array: " + entity.id);
    budget(found->size());
    Ids unique;
    std::vector<std::string> result;
    for (const auto& value : *found) {
        if (!value.is_string()) invalid("deduction must be an entity identity");
        const auto id = value.get<std::string>();
        if (id.empty() || !unique.insert(id).second) invalid("deductions must be unique nonempty identities");
        result.push_back(id);
    }
    return result;
}
Ids room_boundary_wall_ids(const Entity& room) {
    // Transfer and semantic inventories describe the complete context. Only
    // actual outer/hole edge uses confer this room's placement authority.
    const auto descriptor = decode_physical_wall_room_descriptor(room);
    Ids result;
    const auto face = [&](const Json& value) {
        for (const auto& edge : value.at("edges"))
            for (const auto& use : edge.at("source_uses")) {
                result.insert(use.at("owner_id").get<std::string>());
                budget(result.size());
            }
    };
    face(descriptor.source_lineage.at("outer"));
    for (const auto& hole : descriptor.source_lineage.at("holes")) face(hole);
    return result;
}
bool ansi_partition(const DocumentSnapshot& source, const ProjectOrganization& organization,
    const Entity& entity) {
    if (entity.type != "boundary" && entity.type != "measurement_boundary") return false;
    const auto scope = entity.properties.value("calculation_scope", std::string{});
    if (scope == "site") return false;
    const auto floor = organization.nodes.find(entity.properties.value("floor_id", std::string{}));
    const auto property_id = floor != organization.nodes.end() && floor->second.type == "floor"
        ? floor->second.context.property_id : entity.properties.value("property_id", std::string{});
    const auto property = source.entities().find(property_id);
    if (property == source.entities().end() || property->second.type != "property" ||
        property->second.properties.value("calculation_workflow", std::string{}) != "appraisal") return false;
    const auto policy = property->second.properties.find("appraisal_policy");
    if (policy == property->second.properties.end() || !policy->is_object() ||
        policy->value("policy_kind", std::string{}) != "ansi_z765_2021") return false;
    return ansi_appraisal_partition_context(source, entity.id);
}

// Expand only descendants of actual selected roots. Hard-connected neighbors
// are reserved separately below, never promoted to exact rigid selections.
Operators selected_operators(const DocumentSnapshot& source,
    const SelectionGeometryTransformRequest& request, const ConstraintPhaseScope& scope) {
    Operators operations;
    std::vector<std::string> pending;
    const auto add = [&](const std::string& id, const PlanarTransform& transform) {
        const auto found = source.entities().find(id);
        if (found == source.entities().end() || found->second.id != id || !geometric(found->second))
            invalid("selected root or required geometric source is unavailable: " + id);
        if (scope.inactive_owner_ids.contains(id)) invalid("inactive source cannot be transformed: " + id);
        const auto [retained, inserted] = operations.emplace(id, transform);
        // Exact captured operators keep the solver's own source inheritance
        // contract. Numerically approximate equality cannot grant authority.
        if (!inserted && !(retained->second == transform))
            invalid("shared descendant has inconsistent captured operators: " + id);
        if (inserted) pending.push_back(id);
        budget(operations.size());
    };
    for (const auto& root : request.roots) add(root.root_id, root.transform);
    const auto organization = organize_project(source);
    Ids active;
    for (const auto& [id, entity] : source.entities()) {
        (void)entity;
        if (!scope.inactive_owner_ids.contains(id)) active.insert(id);
    }
    std::optional<std::map<std::string, MeasurementLineworkSourceCheck, std::less<>>> measured_checks;
    std::optional<std::map<std::string, PhysicalWallRoomCheck, std::less<>>> room_checks;
    std::vector<AppraisalPartitionAssignment> partitions;
    std::map<std::string, std::vector<std::string>, std::less<>> deduction_edges;
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto id = pending[cursor];
        const auto& entity = source.entities().at(id);
        const auto transform = operations.at(id);
        if (entity.type == "wall") {
            Wall wall;
            std::string diagnostic;
            if (!read_document_wall(entity, {}, wall, diagnostic)) invalid(diagnostic);
        } else if (entity.type == "measurement_linework") {
            const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
            if (!decoded.supported()) invalid(decoded.diagnostic);
        } else {
            if (inspect_boundary_entity_version(entity).format != BoundaryEntityFormat::identified_v1)
                invalid("boundary requires an explicit identity upgrade: " + id);
            (void)decode_identified_boundary_entity(entity);
            const auto children = deductions(entity);
            deduction_edges.emplace(id, children);
            if (!children.empty()) {
                const bool ansi = ansi_partition(source, organization, entity);
                if (ansi) partitions.push_back({id, children});
                for (const auto& child_id : children) {
                    const auto child = source.entities().find(child_id);
                    if (child == source.entities().end() || !closed(child->second))
                        invalid("deduction is not an existing closed boundary: " + child_id);
                    if (!ansi && !deductions(child->second).empty())
                        invalid("nested deductions require the supported ANSI partition policy");
                    add(child_id, transform);
                }
            }
            if (is_physical_wall_room(entity)) {
                // The room is a source-authenticated selection carrier, never
                // independently editable rigid geometry. Its actual walls move
                // first; explicit room review retains/repairs its destination.
                if (!room_checks) room_checks = physical_wall_room_checks(source);
                const auto current = room_checks->find(id);
                if (current == room_checks->end() || !current->second.current)
                    invalid("physical room requires source refresh before rigid selection: " + id);
                const auto walls = room_boundary_wall_ids(entity);
                if (walls.empty()) invalid("physical room has no complete physical source roster: " + id);
                for (const auto& wall_id : walls) {
                    if (source.entities().at(wall_id).type != "wall") invalid("physical room source has the wrong type");
                    add(wall_id, transform);
                }
            }
            if (entity.properties.contains("wall_measurement_source")) {
                if (!wall_measurement_source_current(source, entity)) invalid("physical measured boundary is stale: " + id);
                for (const auto& wall_id : exterior_wall_measurement_source_ids(entity)) {
                    if (source.entities().at(wall_id).type != "wall") invalid("physical source has the wrong type");
                    add(wall_id, transform);
                }
            }
            const auto measured_sources = measurement_linework_source_ids(entity);
            if (!measured_sources.empty()) {
                if (!measured_checks) measured_checks = measurement_linework_source_checks(source.entities(), &active);
                if (!measurement_linework_source_current(*measured_checks, entity)) invalid("measured boundary is stale: " + id);
                for (const auto& stroke_id : measured_sources) {
                    if (source.entities().at(stroke_id).type != "measurement_linework") invalid("measured source has the wrong type");
                    add(stroke_id, transform);
                }
            }
        }
    }
    // Explicit iterative cycle admission also covers non-ANSI direct graphs.
    Ids complete, visiting;
    for (const auto& [seed, ignored] : deduction_edges) {
        (void)ignored;
        std::vector<std::pair<std::string, bool>> stack{{seed, false}};
        while (!stack.empty()) {
            const auto [id, finish] = stack.back(); stack.pop_back();
            if (finish) { visiting.erase(id); complete.insert(id); continue; }
            if (visiting.contains(id)) invalid("cyclic deduction dependency: " + id);
            if (complete.contains(id)) continue;
            visiting.insert(id); stack.emplace_back(id, true);
            for (const auto& child : deduction_edges.at(id)) stack.emplace_back(child, false);
        }
    }
    (void)prepare_ansi_appraisal_partition_targets(source, partitions);
    for (const auto& [id, entity] : source.entities()) {
        if (entity.type != "corner_window" || scope.inactive_owner_ids.contains(id)) continue;
        const auto corner = parse_corner_window(entity);
        const auto first = operations.find(corner.wall_ids[0]);
        const auto second = operations.find(corner.wall_ids[1]);
        if (first != operations.end() && second != operations.end() && !(first->second == second->second))
            invalid("shared corner-window descendant has inconsistent captured host operators: " + id);
    }
    return operations;
}

Ids dependency_closure(const DocumentSnapshot& source, const Operators& operations,
    const ConstraintPhaseScope& scope) {
    // Index actual ownership and lineage once. This reserves descendants and
    // reverse consumers for the aggregate cap, but grants them no operator.
    std::map<std::string, Ids, std::less<>> links;
    constexpr std::size_t maximum_index_bytes = 64 * 1024 * 1024;
    constexpr std::size_t maximum_index_edges = 1000000;
    std::size_t index_bytes{}, index_edges{};
    const auto index = [&](const std::string& a, const std::string& b) {
        if (const auto owner = links.find(a); owner != links.end() && owner->second.contains(b)) return;
        // Conservative map/set storage plus retained key text, charged before
        // allocation. The document index and selected closure have distinct
        // bounds; a large unrelated document is not a 4096-member selection.
        const auto bytes = 192 + a.size() + b.size();
        if (index_edges >= maximum_index_edges || bytes > maximum_index_bytes - index_bytes)
            invalid("whole-document dependency index exceeds its resource budget");
        index_bytes += bytes; ++index_edges;
        links[a].insert(b);
    };
    const auto link = [&](const std::string& a, const std::string& b) {
        if (a.empty() || b.empty()) invalid("dependency has an empty identity");
        index(a, b); index(b, a);
    };
    for (const auto& [id, entity] : source.entities()) {
        if (scope.inactive_owner_ids.contains(id)) continue;
        if (closed(entity)) {
            for (const auto& child : deductions(entity)) link(id, child);
            if (entity.properties.contains("wall_measurement_source"))
                for (const auto& wall : exterior_wall_measurement_source_ids(entity)) link(id, wall);
            for (const auto& stroke : measurement_linework_source_ids_for_admission(entity)) link(id, stroke);
            if (is_physical_wall_room(entity))
                for (const auto& wall : room_boundary_wall_ids(entity)) link(id, wall);
        } else if (entity.type == "dimension") {
            const auto target = entity.properties.find("target");
            if (target != entity.properties.end() && target->is_object() && target->contains("entity_id") &&
                target->at("entity_id").is_string()) link(id, target->at("entity_id").get<std::string>());
        } else if (entity.type == "opening") {
            std::string wall, diagnostic;
            if (!read_document_wall_id(entity, wall, diagnostic)) invalid(diagnostic);
            link(id, wall);
        } else if (entity.type == "corner_window") {
            const auto corner = parse_corner_window(entity);
            for (const auto& wall : corner.wall_ids) link(id, wall);
            for (const auto& opening : corner.opening_ids) link(id, opening);
        } else if (entity.type == "constraint") {
            // Scope unsupported semantics from actual declared bindings. The
            // solver retains final admission; no filtered map is supplied.
            const auto bindings = entity.properties.find("bindings");
            if (bindings == entity.properties.end() || !bindings->is_array()) invalid("constraint has indeterminate owner scope");
            for (const auto& binding : *bindings) {
                if (!binding.is_object() || !binding.contains("owner_id") || !binding.at("owner_id").is_string())
                    invalid("constraint has indeterminate owner scope");
                link(id, binding.at("owner_id").get<std::string>());
            }
        }
    }
    const auto contacts = (source.uses_active_phase_constraints() || !scope.registries.empty())
        ? exterior_corner_physical_contact_graph_active_phase(source.entities())
        : exterior_corner_physical_contact_graph(source.entities());
    for (const auto& contact : contacts) link(contact.owner, contact.host);
    Ids result;
    std::vector<std::string> pending;
    const auto reserve = [&](const std::string& id) {
        if (!source.entities().contains(id)) invalid("required dependency is missing: " + id);
        if (scope.inactive_owner_ids.contains(id)) invalid("required dependency is inactive: " + id);
        if (result.insert(id).second) pending.push_back(id);
        budget(result.size());
    };
    for (const auto& [id, transform] : operations) { (void)transform; reserve(id); }
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto id = pending[cursor];
        if (source.entities().at(id).type == "constraint") {
            const auto decoded = decode_constraint_entity(source.entities().at(id));
            if (!decoded.supported()) invalid(decoded.unsupported_reason);
            if (!constraint_participates(*decoded.constraint, scope)) continue;
        } else if (source.entities().at(id).type == "dimension") {
            const auto decoded = decode_boundary_dimension_entity(source.entities().at(id));
            if (!decoded.supported()) invalid(decoded.unsupported_reason);
        }
        if (const auto found = links.find(id); found != links.end())
            for (const auto& related : found->second) reserve(related);
    }
    // Annotation owners hold independent rows: count each touched owner once,
    // without linking its unrelated targets into the geometric selection.
    for (const auto& [id, entity] : source.entities()) {
        if (entity.type != kAnnotationEntityType) continue;
        validate_annotation_entity(entity);
        for (const auto& row : entity.properties.at("state").at("overrides"))
            if (result.contains(row.at("target_id").get<std::string>())) { reserve(id); break; }
    }
    return result;
}

void verify_corner_callout_completion(const Entities& source, const Entities& candidate,
    const Operators& operations, const Ids& dependencies) {
    for (const auto& id : dependencies) {
        const auto& entity = source.at(id);
        if (entity.type != "dimension") continue;
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported()) invalid(decoded.unsupported_reason);
        const auto owner = source.find(decoded.dimension->boundary_id);
        if (owner == source.end() || owner->second.type != "corner_window") continue;
        const auto corner = parse_corner_window(owner->second);
        const auto first = operations.find(corner.wall_ids[0]);
        const auto second = operations.find(corner.wall_ids[1]);
        if (first == operations.end() || second == operations.end()) continue;
        if (!(first->second == second->second))
            invalid("corner-window callout hosts have inconsistent captured operators: " + id);
        (void)resolve_current_boundary_dimension(*decoded.dimension, source);
        (void)resolve_current_boundary_dimension(*decoded.dimension, candidate);
        auto placed = *decoded.dimension;
        placed.text_position = transform_point(placed.text_position, first->second);
        // Compare the complete source-derived entity. Automatic placement and
        // opaque siblings survive just as they do for ordinary rigid owners.
        const auto expected = encode_boundary_dimension_entity(placed, &entity);
        const auto actual = candidate.find(id);
        if (actual == candidate.end() || !exact(expected, actual->second))
            invalid("rigid corner-window callout differs from its captured source result: " + id);
    }
}

std::vector<SelectionGeometryRoomReviewRequirement> room_requirements(
    const DocumentSnapshot& source, const Entities& candidate, const Operators& operations, Ids& dependencies) {
    Ids changed_walls;
    for (const auto& [id, entity] : source.entities()) {
        const auto after = candidate.find(id);
        if (after == candidate.end() || after->second.type != entity.type)
            invalid("rigid transformation cannot remove or replace a source identity");
        if (!exact(entity, after->second)) {
            dependencies.insert(id); budget(dependencies.size());
            if (entity.type == "wall") changed_walls.insert(id);
        }
    }
    if (candidate.size() != source.entities().size()) invalid("rigid transformation cannot add identities");
    if (changed_walls.empty() && std::none_of(operations.begin(), operations.end(), [&](const auto& entry) {
            const auto& transform = entry.second;
            return is_physical_wall_room(source.entities().at(entry.first)) &&
                (transform.rotation_radians != 0 || transform.flip_horizontal || transform.flip_vertical ||
                    transform.offset.x != 0 || transform.offset.y != 0);
        })) return {};
    const auto organization = organize_project(source);
    std::vector<SelectionGeometryRoomReviewRequirement> result;
    for (const auto& id : active_physical_wall_room_ids(source.entities())) {
        const auto& room = source.entities().at(id);
        const auto context = organization.drawing_context(id);
        if (!context || !context->complete()) invalid("retained physical room has unresolved drawing context");
        const auto lineage = validate_retained_physical_wall_room_lineage(room, *context);
        std::vector<PhysicalWallRoomDimensionPlacement> placements;
        if (const auto operation = operations.find(id); operation != operations.end())
            for (const auto& [dimension_id, entity] : source.entities()) {
                if (entity.type != "dimension") continue;
                const auto decoded = decode_boundary_dimension_entity(entity);
                if (!decoded.supported()) invalid(decoded.unsupported_reason);
                if (decoded.dimension->boundary_id != id) continue;
                const auto before = decoded.dimension->text_position;
                const auto after = transform_point(before, operation->second);
                const Vec2 offset{after.x - before.x, after.y - before.y};
                if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) invalid("room callout displacement overflows");
                if (offset.x != 0 || offset.y != 0) placements.push_back({dimension_id, offset});
            }
        const auto selected_room = operations.find(id);
        const bool selected_room_callouts = selected_room != operations.end() &&
            (selected_room->second.rotation_radians != 0 || selected_room->second.flip_horizontal ||
                selected_room->second.flip_vertical || selected_room->second.offset.x != 0 || selected_room->second.offset.y != 0);
        // A selected room's independent area callout can move even if its
        // source walls are invariant and it has no saved dimension entity.
        if (!selected_room_callouts && placements.empty() && std::none_of(lineage.source_owner_ids.begin(), lineage.source_owner_ids.end(),
                [&](const auto& wall) { return changed_walls.contains(wall); })) continue;
        validate_physical_wall_room_dimension_placements(source.entities(), placements);
        if (!exact(room, candidate.at(id))) invalid("geometry stage cannot silently repair a physical room");
        auto group = std::find_if(result.begin(), result.end(), [&](const auto& entry) {
            return entry.context == *context &&
                std::abs(entry.effective_elevation_m - lineage.effective_elevation_m) <= default_geometry_tolerance_metres;
        });
        if (group == result.end()) {
            result.push_back({*context, lineage.effective_elevation_m, {}, {}, {}, {}});
            group = std::prev(result.end());
        }
        group->retained_room_ids.push_back(id);
        group->source_wall_ids.insert(group->source_wall_ids.end(), lineage.source_owner_ids.begin(), lineage.source_owner_ids.end());
        if (const auto operation = operations.find(id); operation != operations.end())
            group->room_transformations.push_back({id, operation->second});
        group->dimension_placements.insert(group->dimension_placements.end(), placements.begin(), placements.end());
        dependencies.insert(id); budget(dependencies.size());
    }
    for (auto& group : result) {
        std::sort(group.source_wall_ids.begin(), group.source_wall_ids.end());
        group.source_wall_ids.erase(std::unique(group.source_wall_ids.begin(), group.source_wall_ids.end()), group.source_wall_ids.end());
        std::sort(group.dimension_placements.begin(), group.dimension_placements.end(), [](const auto& a, const auto& b) {
            return a.dimension_id < b.dimension_id;
        });
    }
    return result;
}
} // namespace

SelectionGeometryTransformPreparation::SelectionGeometryTransformPreparation(
    SelectionGeometryTransformRequest request, std::string digest,
    std::vector<std::string> dependencies, Command command, DocumentSnapshot candidate,
    std::vector<SelectionGeometryRoomReviewRequirement> room_reviews, bool changed)
    : request_(std::move(request)), source_digest_(std::move(digest)),
      dependency_ids_(std::move(dependencies)), command_(std::move(command)),
      candidate_(std::move(candidate)), room_reviews_(std::move(room_reviews)), changed_(changed) {}
const SelectionGeometryTransformRequest& SelectionGeometryTransformPreparation::request() const noexcept { return request_; }
const std::string& SelectionGeometryTransformPreparation::source_snapshot_digest() const noexcept { return source_digest_; }
const std::vector<std::string>& SelectionGeometryTransformPreparation::dependency_ids() const noexcept { return dependency_ids_; }
const Command& SelectionGeometryTransformPreparation::geometry_command() const noexcept { return command_; }
const DocumentSnapshot& SelectionGeometryTransformPreparation::geometry_snapshot() const noexcept { return candidate_; }
const std::vector<SelectionGeometryRoomReviewRequirement>& SelectionGeometryTransformPreparation::required_room_reviews() const noexcept { return room_reviews_; }
bool SelectionGeometryTransformPreparation::makes_change() const noexcept { return changed_; }

Json encode_selection_geometry_transform_request(const SelectionGeometryTransformRequest& input) {
    const auto request = normalized(input);
    Json roots = Json::array();
    for (const auto& root : request.roots) {
        auto transform = encode_boundary_transform({root.root_id, root.transform});
        transform.erase("version"); transform.erase("boundary_id");
        roots.push_back({{"root_id", root.root_id}, {"transform", std::move(transform)}});
    }
    Json result{{"version", 1}, {"expected_revision", request.expected_revision},
        {"roots", std::move(roots)}, {"message", request.message}};
    if (result.dump().size() > 1024 * 1024) invalid("request exceeds the one MiB wire budget");
    return result;
}
SelectionGeometryTransformRequest decode_selection_geometry_transform_request(const Json& value) {
    keys(value, {"version", "expected_revision", "roots", "message"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.at("expected_revision").is_number_integer() || value.at("expected_revision") < 0 ||
        !value.at("roots").is_array() || !value.at("message").is_string()) invalid("invalid request field types");
    budget(value.at("roots").size());
    SelectionGeometryTransformRequest result;
    result.expected_revision = value.at("expected_revision").get<Revision>();
    result.message = value.at("message").get<std::string>();
    for (const auto& root : value.at("roots")) {
        keys(root, {"root_id", "transform"});
        if (!root.at("root_id").is_string()) invalid("root_id must be a string");
        keys(root.at("transform"), {"pivot", "rotation_radians", "flip_horizontal", "flip_vertical", "offset"});
        auto encoded = root.at("transform");
        encoded["version"] = 1; encoded["boundary_id"] = root.at("root_id");
        const auto decoded = decode_boundary_transform(encoded);
        result.roots.push_back({decoded.boundary_id, decoded.transform});
    }
    result = normalized(std::move(result));
    // Shape/primitive admission precedes serialization, so arbitrary nested
    // values cannot reach the wire-budget traversal.
    if (value.dump().size() > 1024 * 1024) invalid("request exceeds the one MiB wire budget");
    return result;
}

SelectionGeometryTransformPreparation prepare_selection_geometry_transform(
    const DocumentSnapshot& source, const SelectionGeometryTransformRequest& input) {
    if (!source.is_editable()) invalid("captured document is read-only");
    const auto request = normalized(input);
    if (request.expected_revision != source.revision())
        throw DocumentError(DocumentErrorCode::stale_revision, "Selected geometry transform revision is stale");
    const auto scope = constraint_phase_scope(source.entities());
    const auto operations = selected_operators(source, request, scope);
    auto dependencies = dependency_closure(source, operations, scope);
    JointTranslationIntent joint;
    joint.per_owner_rigid_completion = true;
    joint.corner_window_dimension_completion = true;
    for (const auto& [id, transform] : operations) {
        const auto& entity = source.entities().at(id);
        if (is_physical_wall_room(entity)) continue; // Review owns this carrier.
        if (entity.type == "wall") joint.partial_wall_ids.push_back(id);
        else if (entity.type == "measurement_linework") joint.rigid_stroke_ids.push_back(id);
        else joint.rigid_boundary_ids.push_back(id);
        joint.owner_transformations.push_back({id, transform});
    }
    ConstraintAuthoringIntent intent;
    intent.joint_translation = std::move(joint); intent.message = request.message;
    const auto preview = preview_constraint_authoring(source, intent);
    Command command = ApplyEntityChanges{source.revision(), {}, {}, request.message};
    auto candidate = source;
    bool changed = false;
    if (preview.accepted()) {
        // Rigid previews may be accepted even when their full result is exact
        // identity. Their command producer intentionally requires a change.
        // Authenticate through that complete preview before returning a no-op.
        if (entity_map_digest(preview.candidate_entities()) != entity_map_digest(source.entities())) {
            // Existing friend producer is found by ADL on the sealed preview.
            std::optional<DocumentSnapshot> verified;
            command = constraint_authoring_verified_command(source, preview, &verified);
            candidate = std::move(verified.value());
            changed = true;
        }
    } else if (preview.diagnostics().size() != 1 ||
        preview.diagnostics().front() != "Constraint authoring intent makes no document change" ||
        entity_map_digest(preview.candidate_entities()) != entity_map_digest(source.entities())) {
        invalid(preview.diagnostics().empty() ? "rigid solve rejected" : preview.diagnostics().front());
    }
    verify_corner_callout_completion(source.entities(), candidate.entities(), operations, dependencies);
    auto rooms = room_requirements(source, candidate.entities(), operations, dependencies);
    // A geometry-invariant operation may still move a selected room's owned
    // callouts. Review then starts directly from the unchanged original source;
    // it does not need a fabricated nonempty geometry proof.
    if (!rooms.empty() && changed && !is_physical_wall_room_geometry_review_command(command))
        invalid("affected physical rooms require a supported detached geometry-review command");
    if (candidate.assets() != source.assets()) invalid("rigid transform cannot change source assets");
    return {request, document_snapshot_digest(source),
        std::vector<std::string>(dependencies.begin(), dependencies.end()), std::move(command),
        std::move(candidate), std::move(rooms), changed};
}

SelectionGeometryTransformPreparation replay_selection_geometry_transform(
    const DocumentSnapshot& source, const SelectionGeometryTransformPreparation& prepared) {
    if (document_snapshot_digest(source) != prepared.source_snapshot_digest())
        throw DocumentError(DocumentErrorCode::stale_revision, "Selected geometry transform source snapshot changed");
    return prepare_selection_geometry_transform(source, prepared.request());
}
} // namespace sketch
