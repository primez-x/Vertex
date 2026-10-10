#include "sketch/dxf_project_exchange.hpp"

#include "sketch/annotation_catalog.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/hosted_opening_plan.hpp"
#include "sketch/wall_measurement.hpp"
#ifdef SKETCH_PHYSICAL_ROOMS
#include "sketch/physical_wall_room.hpp"
#endif
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
#include "sketch/architecture.hpp"
#include "sketch/calculations.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;
constexpr double kGeometryTolerance = 1e-7;
constexpr double kFullTurn = 2.0 * std::numbers::pi;
constexpr const char* kManufacturedDepiction = "MANUFACTURED_PLAN_V1";
constexpr const char* kBoundaryDepiction = "BOUNDARY_PLAN_V1";
constexpr const char* kWallSourceContextBinding = "vertex_dxf_wall_source_context_binding";
constexpr const char* kWallSourceHostedOpenings = "vertex_dxf_wall_source_hosted_openings";
constexpr const char* kLineworkDepiction = "LINEWORK_PLAN_V1";
constexpr const char* kMeasuredGraph = "vertex_dxf_measured_graph";
constexpr const char* kResolvedContext = "vertex_dxf_resolved_context";

bool measured_source_member(const Entity& entity) {
    const auto marker = entity.extensions.find("vertex_dxf_boundary");
    return marker != entity.extensions.end() && marker->is_object() && marker->value("version", 0) == 6;
}

bool wall_source_member(const Entity& entity) {
    const auto marker = entity.extensions.find("vertex_dxf_boundary");
    return marker != entity.extensions.end() && marker->is_object() &&
        (marker->value("version", 0) == 5 || marker->value("version", 0) == 6);
}

Json resolved_context_json(const DrawingContext& context) {
    if (!context.complete()) throw std::invalid_argument("native resolved context incomplete");
    return {{"property_id", context.property_id}, {"building_id", context.building_id},
        {"floor_id", context.floor_id}, {"layer_id", context.layer_id}, {"level_id", context.level_id}};
}

DrawingContext read_resolved_context(const Json& context) {
    if (!context.is_object() || context.size() != 5)
        throw std::invalid_argument("invalid native resolved context");
    DrawingContext result;
    for (const auto& [key, destination] : {std::pair{"property_id", &result.property_id},
        std::pair{"building_id", &result.building_id}, std::pair{"floor_id", &result.floor_id},
        std::pair{"layer_id", &result.layer_id}, std::pair{"level_id", &result.level_id}}) {
        const auto value = context.find(key);
        if (value == context.end() || !value->is_string()) throw std::invalid_argument("invalid native resolved context ID");
        *destination = value->get<std::string>();
        if (destination->size() > 128 || (destination->empty() && std::string_view(key) != "level_id") ||
            !std::all_of(destination->begin(), destination->end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == ':';
            })) throw std::invalid_argument("invalid native resolved context ID");
    }
    if (!result.complete()) throw std::invalid_argument("native resolved context incomplete");
    return result;
}

DrawingContext member_resolved_context(const Entity& entity) {
    const auto binding = entity.extensions.find(kWallSourceContextBinding);
    if (binding != entity.extensions.end())
        return read_resolved_context(binding->at(binding->at("destination_context").is_null() ?
            "source_resolved_context" : "destination_resolved_context"));
    const auto& observation = entity.extensions.at(kResolvedContext);
    if (!observation.is_object() || observation.size() != 2 || !observation.at("version").is_number_integer() ||
        observation.at("version") != 1) throw std::invalid_argument("invalid native resolved context observation");
    return read_resolved_context(observation.at("context"));
}

std::vector<std::string> measured_graph_ids(const Entity& area,
    const std::map<std::string, Entity, std::less<>>& graph,
    const std::map<std::string, DrawingContext, std::less<>>& contexts,
    const std::set<std::string, std::less<>>* visible = nullptr) {
    const auto referenced = measurement_linework_source_ids_for_admission(area);
    if (referenced.empty()) return {};
    const bool isolated = std::any_of(referenced.begin(), referenced.end(), [&](const auto& id) {
        return graph.contains(id) && measurement_linework_copy_isolated(graph.at(id));
    });
    if (isolated) return referenced;
    std::vector<std::string> ids;
    for (const auto& [id, owner] : graph)
        if (owner.type == "measurement_linework" && (!visible || visible->contains(id)) &&
            !measurement_linework_copy_isolated(owner) && contexts.at(id) == contexts.at(area.id)) ids.push_back(id);
    return ids;
}

// Recover only the typed retained lineage locations, even when another field
// makes the source record inadmissible. Arbitrary JSON is never an owner link.
std::vector<std::string> recover_measured_source_ids(const Json& extensions) {
    std::set<std::string> ids;
    const auto remember = [&](const Json& uses) {
        if (!uses.is_array()) return;
        for (const auto& edge : uses) if (edge.is_array())
            for (const auto& use : edge) if (use.is_object()) {
                const auto owner = use.find("owner_id");
                if (owner != use.end() && owner->is_string() && !owner->get_ref<const std::string&>().empty() &&
                    owner->get_ref<const std::string&>().size() <= 255) ids.insert(owner->get<std::string>());
            }
    };
    if (!extensions.is_object()) return {};
    if (extensions.contains("measurement_linework_sources")) remember(extensions.at("measurement_linework_sources"));
    const auto group = extensions.find("measurement_linework_group");
    if (group != extensions.end() && group->is_object() && group->contains("members") && group->at("members").is_array())
        for (const auto& member : group->at("members")) remember(member);
    return {ids.begin(), ids.end()};
}

Json direct_source_context(const Entity& entity) {
    Json context = Json::object();
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "phase_id"}) {
        const auto value = entity.properties.find(key);
        if (value == entity.properties.end()) continue;
        if (!value->is_string() || value->get_ref<const std::string&>().empty() ||
            value->get_ref<const std::string&>().size() > 255)
            throw std::invalid_argument("invalid native direct source context");
        context[key] = *value;
    }
    return context;
}

void validate_direct_context(const Json& context) {
    if (!context.is_object()) throw std::invalid_argument("invalid native context object");
    Entity copy{"context", "context", context, false, Json::object()};
    if (direct_source_context(copy) != context)
        throw std::invalid_argument("unknown native direct source context field");
}

std::string wall_source_stair_floor(const Entity& entity) {
    if (!wall_source_member(entity)) return {};
    // Reuse the exact V4 stair schema without expanding V4 destination authority.
    auto copy = entity;
    copy.extensions["vertex_dxf_boundary"]["version"] = 4;
    return native_dxf_boundary_stair_floor_source(copy);
}

std::map<std::string, Entity, std::less<>> wall_source_context_graph(const std::vector<Entity>& members) {
    std::map<std::string, Entity, std::less<>> graph;
    for (const auto& entity : members) {
        auto copy = entity;
        const auto binding = copy.extensions.find(kWallSourceContextBinding);
        if (binding != copy.extensions.end()) {
            const auto& context = binding->at("destination_context").is_null()
                ? binding->at("source_context") : binding->at("destination_context");
            for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "phase_id"})
                copy.properties.erase(key);
            for (const auto& [key, value] : context.items()) copy.properties[key] = value;
            const auto stair = copy.extensions.find("vertex_dxf_stair_floor_binding");
            if (stair != copy.extensions.end() && stair->at("destination_floor_id").is_null())
                copy.properties["appraisal_facts"]["ansi"]["ceiling"]["stair_from_floor_id"] =
                    stair->at("source_floor_id");
        }
        if (!graph.emplace(copy.id, std::move(copy)).second)
            throw std::invalid_argument("duplicate native source identity");
    }
    return graph;
}

std::string block_identity(std::string_view name) {
    std::string result(name);
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return result;
}

void diagnostic(std::vector<DxfProjectDiagnostic>& output, std::string id,
                std::string kind, std::string code) {
    output.push_back({std::move(id), std::move(kind), std::move(code)});
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    // A polyline has one coordinate per shared vertex. Tolerance-only joins
    // cannot be represented without changing an authored endpoint.
    return left.x == right.x && left.y == right.y;
}

Json point_json(Vec2 point) {
    return Json::array({point.x, point.y});
}

Json segment_json(const Segment& segment) {
    return Json{{"start", point_json(segment.start)},
                {"end", point_json(segment.end)},
                {"sweep_radians", segment.sweep_radians}};
}

Json boundary_json(const Boundary& boundary) {
    Json result = Json::array();
    for (const auto& segment : boundary) result.push_back(segment_json(segment));
    return result;
}

std::optional<Vec2> read_point(const Json& value) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() ||
        !value[1].is_number()) return std::nullopt;
    const Vec2 point{value[0].get<double>(), value[1].get<double>()};
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return std::nullopt;
    return point;
}

std::optional<Segment> read_segment(const Json& value) {
    if (!value.is_object() || !value.contains("start") || !value.contains("end") ||
        !value.contains("sweep_radians")) return std::nullopt;
    const auto start = read_point(value.at("start"));
    const auto end = read_point(value.at("end"));
    if (!start || !end || !value.at("sweep_radians").is_number()) return std::nullopt;
    const auto sweep = value.at("sweep_radians").get<double>();
    if (!std::isfinite(sweep)) return std::nullopt;
    return Segment{*start, *end, sweep};
}

std::optional<Boundary> read_boundary_value(const Json& value) {
    if (!value.is_array() || value.empty()) return std::nullopt;
    Boundary result;
    result.reserve(value.size());
    for (const auto& item : value) {
        const auto segment = read_segment(item);
        if (!segment) return std::nullopt;
        result.push_back(*segment);
    }
    return result;
}

std::optional<Boundary> read_entity_boundary(const Entity& entity) {
    try {
        if (can_recognize_boundary_entity_type(entity.type)) {
            const auto version = inspect_boundary_entity_version(entity);
            if (version.format == BoundaryEntityFormat::identified_v1)
                return boundary_geometry(decode_identified_boundary_entity(entity));
            if (version.format == BoundaryEntityFormat::unsupported_version) return std::nullopt;
        }
        if (entity.properties.contains("boundary"))
            return read_boundary_value(entity.properties.at("boundary"));
        if (entity.properties.contains("segments"))
            return read_boundary_value(entity.properties.at("segments"));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

// Admission only reads raw segment values. Identified decoding validates
// topology quadratically and belongs after the complete operation work charge.
std::optional<Boundary> read_entity_boundary_for_admission(const Entity& entity) {
    if (can_recognize_boundary_entity_type(entity.type)) {
        const auto version = inspect_boundary_entity_version(entity);
        if (version.format == BoundaryEntityFormat::unsupported_version) return std::nullopt;
        if (version.format == BoundaryEntityFormat::identified_v1)
            return read_boundary_value(entity.properties.at("segments"));
    }
    if (entity.properties.contains("boundary")) return read_boundary_value(entity.properties.at("boundary"));
    if (entity.properties.contains("segments")) return read_boundary_value(entity.properties.at("segments"));
    return std::nullopt;
}

std::size_t raw_boundary_segment_count(const Entity& entity) {
    const auto version = inspect_boundary_entity_version(entity);
    const auto key = version.format == BoundaryEntityFormat::identified_v1 ? "segments" :
        entity.properties.contains("boundary") ? "boundary" : "segments";
    const auto& values = entity.properties.at(key);
    if (!values.is_array() || values.empty() || values.size() > 512)
        throw std::invalid_argument("V6 raw boundary segment limit");
    std::size_t count = values.size();
    if (const auto holes = entity.properties.find("holes"); holes != entity.properties.end()) {
        if (!holes->is_array()) throw std::invalid_argument("V6 raw holes must be an array");
        for (const auto& hole : *holes) {
            if (!hole.is_array() || hole.empty() || hole.size() > 512 - count)
                throw std::invalid_argument("V6 raw boundary segment limit");
            count += hole.size();
        }
    }
    return count;
}

std::pair<std::size_t, std::size_t> raw_measured_stroke_work(const Entity& entity) {
    const auto& model = entity.properties.at("model");
    if (!model.is_object() || !model.contains("segments") || !model.at("segments").is_array() ||
        model.at("segments").empty() || model.at("segments").size() > 512 ||
        !model.contains("stroke_id") || model.at("stroke_id") != entity.id)
        throw std::invalid_argument("V6 raw stroke schema or owner differs");
    const auto count = model.at("segments").size();
    std::size_t passes = 1;
    for (const auto* key : {"transforms", "operations"}) if (model.contains(key)) {
        const auto& operations = model.at(key);
        if (!operations.is_array()) throw std::invalid_argument("V6 stroke history must be an array");
        for (const auto& operation : operations) {
            std::size_t cost = 1;
            if (operation.is_object() && operation.value("type", std::string{}) == "vertex_batch") {
                const auto& edits = operation.at("edits");
                if (!edits.is_array() || edits.size() >= 250'000)
                    throw std::invalid_argument("V6 vertex batch work limit");
                cost += edits.size();
            }
            if (cost > 250'000 || passes > 250'000 - cost)
                throw std::invalid_argument("V6 stroke history work limit");
            passes += cost;
        }
    }
    return {count, count * passes};
}

std::string valid_layer(std::string value, std::vector<DxfProjectDiagnostic>& diagnostics,
                        const std::string& source_id, std::string_view source_kind) {
    if (value.empty()) return "0";
    if (value.size() > 255 || std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 32 || c > 126;
        })) {
        diagnostic(diagnostics, source_id, std::string(source_kind), "layer_not_representable");
        return "0";
    }
    return value;
}

std::optional<std::string> layer_name(const DocumentSnapshot& document, const std::string& layer_id) {
    const auto found = document.entities().find(layer_id);
    if (found == document.entities().end() || found->second.type != "layer" ||
        !found->second.properties.contains("name") || !found->second.properties.at("name").is_string())
        return std::nullopt;
    const auto name = found->second.properties.at("name").get<std::string>();
    return name.empty() ? std::nullopt : std::optional<std::string>{name};
}

std::string layer_for(const DocumentSnapshot& document, const Entity& entity,
                      std::vector<DxfProjectDiagnostic>& diagnostics) {
    std::string layer;
    if (entity.properties.is_object()) {
        for (const auto* key : {"layer", "layer_name"}) {
            if (entity.properties.contains(key) && entity.properties.at(key).is_string()) {
                layer = entity.properties.at(key).get<std::string>();
                break;
            }
        }
        if (layer.empty() && entity.properties.contains("layer_id") &&
            entity.properties.at("layer_id").is_string()) {
            const auto layer_id = entity.properties.at("layer_id").get<std::string>();
            if (const auto name = layer_name(document, layer_id)) layer = *name;
        }
    }
    return valid_layer(std::move(layer), diagnostics, entity.id, entity.type);
}

std::string annotation_layer_for(const DocumentSnapshot& document, const AnnotationPlacement& placement,
                                 const std::string& child_id, std::string_view fallback,
                                 std::vector<DxfProjectDiagnostic>& diagnostics) {
    if (placement.layer_id.empty()) return std::string(fallback);
    const auto name = layer_name(document, placement.layer_id);
    if (!name) {
        diagnostic(diagnostics, child_id, kAnnotationEntityType, "layer_reference_missing");
        return "0";
    }
    return valid_layer(*name, diagnostics, child_id, kAnnotationEntityType);
}

double normalized_degrees(double radians) {
    auto degrees = radians * 180.0 / std::numbers::pi;
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    return degrees == 0.0 ? 0.0 : degrees;
}

std::string dimension_quantity_text(double quantity, std::string_view suffix,
                                    std::string override_text = {}) {
    if (!std::isfinite(quantity) || quantity < 0.0)
        throw std::invalid_argument("DXF dimension measurement must be finite and nonnegative");
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer),
        quantity == 0.0 ? 0.0 : quantity, std::chars_format::general, 12);
    if (converted.ec != std::errc{})
        throw std::invalid_argument("DXF dimension measurement is not representable");
    const std::string measurement(buffer, converted.ptr);
    if (override_text.empty()) return measurement + std::string(suffix);
    const auto max_text = DxfExchangeLimits{}.max_string_bytes;
    if (override_text.size() > max_text)
        throw std::invalid_argument("DXF dimension text exceeds its transport limit");
    for (std::size_t position = 0; (position = override_text.find("<>", position)) != std::string::npos;) {
        if (measurement.size() > max_text - (override_text.size() - 2))
            throw std::invalid_argument("DXF dimension text exceeds its transport limit");
        override_text.replace(position, 2, measurement);
        position += measurement.size();
    }
    return override_text;
}

std::string dimension_length_text(double metres, std::string override_text = {}) {
    return dimension_quantity_text(metres, " m", std::move(override_text));
}

std::string imported_dimension_text(double metres, const std::string& original,
                                    double source_metres_per_unit) {
    if (!std::isfinite(source_metres_per_unit) || source_metres_per_unit <= 0.0)
        throw std::invalid_argument("DXF dimension source units are invalid");
    // An authored suffix such as '<> ft' belongs to the original drawing
    // units. Geometry becomes SI, but its source text must not relabel metres.
    if (original.empty()) return dimension_length_text(metres);
    return dimension_quantity_text(metres / source_metres_per_unit, "", original);
}

bool linear_dimension_chain(const BoundaryDimension& dimension, const Entity& owner,
                            const BoundaryDimensionResolution& resolved) {
    if (dimension.segment_chain_ids.empty()) return true;
    const auto geometry = resolve_dimension_geometry_owner(owner);
    const auto dx = resolved.segment.end.x - resolved.segment.start.x;
    const auto dy = resolved.segment.end.y - resolved.segment.start.y;
    const auto chord = std::hypot(dx, dy);
    if (!(chord > kGeometryTolerance)) return false;
    for (const auto& id : dimension.segment_chain_ids) {
        const auto edge = std::find_if(geometry.segments.begin(), geometry.segments.end(),
            [&](const auto& item) { return item.segment_id == id; });
        if (edge == geometry.segments.end() || edge->segment.sweep_radians != 0.0) return false;
        const auto ex = edge->segment.end.x - edge->segment.start.x;
        const auto ey = edge->segment.end.y - edge->segment.start.y;
        if ((ex * dx + ey * dy) / chord <= 0.0 ||
            std::abs(ex * dy - ey * dx) / chord > kGeometryTolerance) return false;
    }
    return std::abs(chord - resolved.segment_length()) <= kGeometryTolerance;
}

std::optional<DxfArc> dxf_arc_from_segment(const Segment& segment, std::string layer) {
    if (segment.sweep_radians <= 0.0 || segment.sweep_radians >= kFullTurn - 1e-10) return std::nullopt;
    const auto tangent = std::tan(segment.sweep_radians * 0.5);
    if (!std::isfinite(tangent) || std::abs(tangent) <= std::numeric_limits<double>::epsilon())
        return std::nullopt;
    const auto dx = segment.end.x - segment.start.x;
    const auto dy = segment.end.y - segment.start.y;
    const Vec2 center{(segment.start.x + segment.end.x) * 0.5 - dy * (0.5 / tangent),
                      (segment.start.y + segment.end.y) * 0.5 + dx * (0.5 / tangent)};
    const auto radius = std::hypot(segment.start.x - center.x, segment.start.y - center.y);
    if (!(radius > kGeometryTolerance) || !std::isfinite(radius)) return std::nullopt;
    return DxfArc{{center.x, center.y}, radius,
                  normalized_degrees(std::atan2(segment.start.y - center.y,
                                                segment.start.x - center.x)),
                  normalized_degrees(std::atan2(segment.end.y - center.y,
                                                segment.end.x - center.x)), std::move(layer)};
}

std::optional<DxfPolyline> dxf_polyline_from_boundary(const Boundary& boundary,
                                                       bool force_closed,
                                                       std::string layer) {
    if (boundary.size() < 2) return std::nullopt;
    DxfPolyline result;
    result.layer = std::move(layer);
    result.closed = force_closed && same_point(boundary.back().end, boundary.front().start);
    result.vertices.reserve(boundary.size());
    for (std::size_t index = 0; index < boundary.size(); ++index) {
        const auto& segment = boundary[index];
        if ((index + 1 < boundary.size() &&
             !same_point(segment.end, boundary[index + 1].start)) ||
            !std::isfinite(segment.sweep_radians) ||
            std::abs(segment.sweep_radians) >= kFullTurn)
            return std::nullopt;
        const auto bulge = std::tan(segment.sweep_radians * 0.25);
        if (!std::isfinite(bulge) || std::abs(bulge) > 1e12) return std::nullopt;
        result.vertices.push_back({{segment.start.x, segment.start.y},
                                    bulge == 0.0 ? 0.0 : bulge});
    }
    if (!result.closed)
        result.vertices.push_back({{boundary.back().end.x, boundary.back().end.y}, 0.0});
    return result;
}

void add_segment_as_dxf(DxfDrawing& drawing, const Segment& segment, std::string layer,
                        std::vector<DxfProjectDiagnostic>& diagnostics,
                        const std::string& source_id, std::string_view source_kind) {
    if (!std::isfinite(segment.sweep_radians) ||
        std::abs(segment.sweep_radians) >= kFullTurn) {
        diagnostic(diagnostics, source_id, std::string(source_kind), "arc_sweep_not_representable");
        return;
    }
    if (segment.sweep_radians == 0.0) {
        drawing.lines.push_back({{segment.start.x, segment.start.y},
                                 {segment.end.x, segment.end.y}, std::move(layer)});
        return;
    }
    if (const auto arc = dxf_arc_from_segment(segment, layer); arc) {
        drawing.arcs.push_back(*arc);
        return;
    }
    DxfPolyline fallback;
    fallback.layer = std::move(layer);
    const auto bulge = std::tan(segment.sweep_radians * 0.25);
    if (!std::isfinite(bulge) || std::abs(bulge) > 1e12) {
        diagnostic(diagnostics, source_id, std::string(source_kind), "arc_sweep_not_representable");
        return;
    }
    fallback.vertices = {{{segment.start.x, segment.start.y}, bulge},
                         {{segment.end.x, segment.end.y}, 0.0}};
    drawing.polylines.push_back(std::move(fallback));
    diagnostic(diagnostics, source_id, std::string(source_kind), "arc_exported_as_bulged_polyline");
}

void add_boundary_as_dxf(DxfDrawing& drawing, const Boundary& boundary, std::string layer,
                         std::vector<DxfProjectDiagnostic>& diagnostics,
                         const std::string& source_id, std::string_view source_kind) {
    if (boundary.size() >= 2) {
        for (std::size_t index = 0; index + 1 < boundary.size(); ++index) {
            if (same_point(boundary[index].end, boundary[index + 1].start)) continue;
            // Independent primitives retain both coordinates; the importer
            // cannot infer an exact shared-vertex topology from this chain.
            diagnostic(diagnostics, source_id, std::string(source_kind),
                       "boundary_endpoint_connections_not_representable");
            for (const auto& segment : boundary)
                add_segment_as_dxf(drawing, segment, layer, diagnostics, source_id, source_kind);
            return;
        }
        const auto closed = same_point(boundary.back().end, boundary.front().start);
        if (const auto polyline = dxf_polyline_from_boundary(boundary, closed, layer); polyline) {
            drawing.polylines.push_back(*polyline);
            if (!closed) diagnostic(diagnostics, source_id, std::string(source_kind), "open_boundary");
            return;
        }
        diagnostic(diagnostics, source_id, std::string(source_kind), "boundary_arc_not_representable");
        return;
    }
    if (boundary.size() == 1) {
        add_segment_as_dxf(drawing, boundary.front(), std::move(layer), diagnostics,
                           source_id, source_kind);
        return;
    }
    diagnostic(diagnostics, source_id, std::string(source_kind), "empty_boundary");
}

void add_entity_holes_as_dxf(DxfDrawing& drawing, const Entity& entity, const std::string& layer,
                             std::vector<DxfProjectDiagnostic>& diagnostics) {
    const auto holes = entity.properties.find("holes");
    if (holes == entity.properties.end()) return;
    const bool slab = entity.type == "slab";
    const auto invalid_code = slab ? "slab_hole_not_representable" : "boundary_hole_not_representable";
    if (!holes->is_array()) {
        diagnostic(diagnostics, entity.id, entity.type, invalid_code);
        return;
    }
    if (holes->empty()) return;
    // Ordinary curves retain each loop's analytical geometry, but neither
    // transport nor import mapping can bind it as a native hole of its owner.
    diagnostic(diagnostics, entity.id, entity.type, "boundary_hole_association_not_representable");
    std::size_t index = 0;
    for (const auto& value : *holes) {
        const auto hole = read_boundary_value(value);
        if (!hole) {
            diagnostic(diagnostics, entity.id, entity.type, invalid_code);
        } else {
            add_boundary_as_dxf(drawing, *hole, layer, diagnostics,
                               entity.id + ":hole:" + std::to_string(index),
                               slab ? "slab_hole" : "boundary_hole");
        }
        ++index;
    }
}

std::vector<const Entity*> host_openings(const std::map<std::string, Entity, std::less<>>& entities, std::string_view id,
                                       const ConstraintPhaseScope& scope) {
    std::vector<const Entity*> openings;
    for (const auto& [key, entity] : entities) {
        if (scope.inactive_owner_ids.contains(key)) continue;
        if (entity.type == "opening" && entity.properties.is_object() &&
            entity.properties.value("wall_id", std::string{}) == id) openings.push_back(&entity);
    }
    return openings;
}

std::vector<const Entity*> host_openings(const DocumentSnapshot& document, std::string_view id,
                                       const ConstraintPhaseScope& scope) {
    return host_openings(document.entities(), id, scope);
}

Boundary architectural_plan(const std::map<std::string, Entity, std::less<>>& entities, const Entity& entity,
                            const ConstraintPhaseScope& scope) {
    const Entity* host = &entity;
    if (entity.type == "opening") {
        const auto id = entity.properties.at("wall_id").get<std::string>();
        const auto found = entities.find(id);
        if (found == entities.end() || found->second.type != "wall")
            throw std::invalid_argument("missing opening host");
        host = &found->second;
    }
    Wall wall;
    std::string error;
    if (!read_document_wall(*host, host_openings(entities, host->id, scope), wall, error))
        throw std::invalid_argument(error);
    validate_hosted_opening_plan_source(wall);
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
    (void)make_wall(wall);
#endif
    if (entity.type == "wall")
        return wall_plan_footprint(wall.baseline, wall.openings, wall.thickness);
    const auto opening = std::find_if(wall.openings.begin(), wall.openings.end(),
        [&](const auto& value) { return value.id == entity.id; });
    if (opening == wall.openings.end()) throw std::invalid_argument("missing hosted opening");
    const auto kind = entity.properties.value("opening_kind", std::string("opening"));
    if (entity.properties.contains("door_operation") && kind != "door")
        throw std::invalid_argument("door operation requires door opening kind");
    if (entity.properties.contains("opening_assembly")) {
        const auto assembly = parse_opening_assembly(entity.properties.at("opening_assembly"));
        if (opening_assembly_kind_name(assembly.kind) != kind)
            throw std::invalid_argument("opening kind differs from assembly");
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
        const auto operation = kind == "door" && entity.properties.contains("door_operation")
            ? std::optional<DoorOperation>(decode_door_operation(entity.properties.at("door_operation")))
            : std::nullopt;
        return project_hosted_opening_plan(wall, *opening, assembly, operation);
#endif
    }
    if (kind == "door") {
        const auto operation = entity.properties.contains("door_operation")
            ? decode_door_operation(entity.properties.at("door_operation")) : DoorOperation{};
        return door_plan_symbol(wall.baseline, opening->offset, opening->width, operation);
    }
    if (kind == "window")
        return window_plan_symbol(wall.baseline, opening->offset, opening->width, wall.thickness);
    if (kind != "opening") throw std::invalid_argument("unsupported opening kind");
    const auto span = hosted_opening_span(wall.baseline, opening->offset, opening->width);
    auto footprint = wall_plan_footprint(span, {}, wall.thickness);
    // The two jambs plus the directed analytical threshold.
    if (footprint.size() != 4) throw std::invalid_argument("invalid bare opening footprint");
    return {footprint[1], footprint[3], span};
}

DxfBlock architectural_block(const std::map<std::string, Entity, std::less<>>& entities, const Entity& entity,
                              std::string name, std::string layer,
                              std::vector<DxfProjectDiagnostic>& diagnostics,
                              const ConstraintPhaseScope& scope = {}) {
    DxfDrawing plan;
    std::vector<DxfProjectDiagnostic> plan_diagnostics;
    // Export supplies the complete saved-design scope. Import regenerates its
    // detached, phase-free graph with the empty scope and original V1 contract.
    auto geometry = architectural_plan(entities, entity, scope);
    if (geometry.size() > 4096) throw std::invalid_argument("native plan primitive limit");
    // Canonical direction and order make the independently regenerated native
    // plan stable across identity remapping and repeated transport round trips.
    const bool manufactured = entity.type == "opening" && entity.properties.contains("opening_assembly");
    if (manufactured) for (auto& segment : geometry) {
        if (segment.sweep_radians < 0 || (segment.sweep_radians == 0 &&
            (segment.end.x < segment.start.x || (segment.end.x == segment.start.x && segment.end.y < segment.start.y)))) {
            std::swap(segment.start, segment.end);
            segment.sweep_radians = -segment.sweep_radians;
        }
    }
    if (manufactured) std::sort(geometry.begin(), geometry.end(), [](const auto& a, const auto& b) {
        if (a.start.x != b.start.x) return a.start.x < b.start.x;
        if (a.start.y != b.start.y) return a.start.y < b.start.y;
        if (a.end.x != b.end.x) return a.end.x < b.end.x;
        if (a.end.y != b.end.y) return a.end.y < b.end.y;
        return a.sweep_radians < b.sweep_radians;
    });
    for (const auto& segment : geometry)
        add_segment_as_dxf(plan, segment, layer, plan_diagnostics, entity.id, entity.type);
    for (const auto& item : plan_diagnostics)
        if (item.code != "arc_exported_as_bulged_polyline") diagnostics.push_back(item);
    return {std::move(name), {}, std::move(plan.lines), std::move(plan.arcs),
            std::move(plan.polylines), {}, {}};
}

DxfBlock architectural_block(const DocumentSnapshot& document, const Entity& entity,
                            std::string name, std::string layer,
                            std::vector<DxfProjectDiagnostic>& diagnostics,
                            const ConstraintPhaseScope& scope = {}) {
    return architectural_block(document.entities(), entity, std::move(name), std::move(layer), diagnostics, scope);
}

Json native_payload(const DocumentSnapshot& document, const Entity& entity,
                    const ConstraintPhaseScope& scope) {
    Json ids = Json::array();
    if (entity.type == "wall") for (const auto* opening : host_openings(document, entity.id, scope))
        ids.push_back(opening->id);
    Json result = {{"version", 1}, {"id", entity.id}, {"type", entity.type},
            {"properties", entity.properties}, {"extensions", entity.extensions},
            {"hosted_opening_ids", std::move(ids)}};
    if (entity.type == "opening" && entity.properties.contains("opening_assembly"))
        result["depiction"] = kManufacturedDepiction;
    if (measured_source_member(entity)) {
        // V6 admission state belongs to this transfer, never to a later V1
        // architectural carrier after its former consumers were removed.
        for (const auto* key : {"vertex_dxf_boundary", "vertex_dxf_stair_floor_binding",
            kWallSourceContextBinding, kWallSourceHostedOpenings, kMeasuredGraph, kResolvedContext})
            result["extensions"].erase(key);
    }
    return result;
}

Json bounded_native_json(std::string_view bytes) {
    std::vector<std::set<std::string>> keys;
    std::size_t nodes = 0;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 16 || ++nodes > 4096) throw std::invalid_argument("native JSON limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key &&
                 !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("duplicate native JSON key");
        if (value.is_string() && value.get_ref<const std::string&>().size() > 8192)
            throw std::invalid_argument("native JSON string limit");
        return true;
    };
    return Json::parse(bytes, callback);
}

DxfBlock boundary_plan_block(const Entity& entity, std::string name, const std::string& layer,
                             bool proved_appraisal_group = false) {
    if (!wall_source_member(entity) && (proved_appraisal_group ? native_dxf_boundary_has_untransported_source_links(entity) :
        native_dxf_boundary_has_untransported_links(entity)))
        throw std::invalid_argument("native boundary dependent graph is not transported");
    const auto outer = read_entity_boundary(entity);
    if (!outer || outer->size() > 4096 ||
        !same_point(outer->back().end, outer->front().start))
        throw std::invalid_argument("native boundary must be closed");
    std::vector<Boundary> holes;
    std::size_t segment_count = outer->size();
    if (const auto values = entity.properties.find("holes"); values != entity.properties.end()) {
        if (!values->is_array()) throw std::invalid_argument("native holes must be an array");
        for (const auto& value : *values) {
            auto hole = read_boundary_value(value);
            if (!hole || hole->size() > 4096 - segment_count)
                throw std::invalid_argument("native hole primitive limit");
            segment_count += hole->size();
            holes.push_back(std::move(*hole));
        }
    }
    if (const auto error = validate_boundary_holes(*outer, holes))
        throw std::invalid_argument(*error);
    DxfDrawing plan;
    std::vector<DxfProjectDiagnostic> diagnostics;
    add_boundary_as_dxf(plan, *outer, layer, diagnostics, entity.id, entity.type);
    for (const auto& hole : holes)
        add_boundary_as_dxf(plan, hole, layer, diagnostics, entity.id, entity.type);
    if (!diagnostics.empty()) throw std::invalid_argument("native boundary plan is not representable");
    return {std::move(name), {}, std::move(plan.lines), std::move(plan.arcs),
            std::move(plan.polylines), {}, {}};
}

DxfBlock linework_plan_block(const Entity& entity, std::string name, const std::string& layer) {
    const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
    if (!decoded.supported() || decoded.model->stroke_id != entity.id)
        throw std::invalid_argument("native linework model unsupported or owner differs");
    const auto replay = replay_measurement_linework(*decoded.model);
    DxfDrawing plan;
    std::vector<DxfProjectDiagnostic> diagnostics;
    // Independent primitives preserve open strokes, crossings and retracing.
    for (const auto& edge : replay.edges)
        add_segment_as_dxf(plan, edge.segment, layer, diagnostics, entity.id, entity.type);
    if (!diagnostics.empty()) throw std::invalid_argument("native linework plan unavailable");
    return {std::move(name), {}, std::move(plan.lines), std::move(plan.arcs), {}, {}, {}};
}

DxfBlock source_plan_block(const std::map<std::string, Entity, std::less<>>& graph,
    const Entity& entity, std::string name, const std::string& layer,
    std::vector<DxfProjectDiagnostic>& diagnostics) {
    if (entity.type == "measurement_linework") return linework_plan_block(entity, std::move(name), layer);
    if (can_recognize_boundary_entity_type(entity.type)) return boundary_plan_block(entity, std::move(name), layer, true);
    return architectural_block(graph, entity, std::move(name), layer, diagnostics);
}

const char* source_depiction(const Entity& entity) {
    if (entity.type == "measurement_linework") return kLineworkDepiction;
    if (can_recognize_boundary_entity_type(entity.type)) return kBoundaryDepiction;
    if (entity.type == "wall") return "WALL_PLAN_V1";
    return entity.properties.contains("opening_assembly") ? kManufacturedDepiction : "OPENING_PLAN_V1";
}

Entity detached_native_entity(const Entity& source, const std::map<std::string, std::string>& ids,
                              bool proved_stair_floor = false);
bool same_block_geometry(const DxfBlock& a, const DxfBlock& b, bool exact);

bool export_boundary_entity(const DocumentSnapshot& document, const Entity& entity, const std::string& layer,
                            DxfProjectExportResult& result) {
    try {
        auto metadata = native_payload(document, entity, {});
        // V1 remains the unchanged wall/opening contract. V2 admits only a
        // closed native boundary whose entire outer/hole plan can be proved.
        metadata["version"] = 2;
        metadata["depiction"] = kBoundaryDepiction;
        const auto payload = metadata.dump();
        if (payload.size() > 16 * 1024) throw std::invalid_argument("native payload byte limit");
        (void)bounded_native_json(payload);
        if (entity.extensions.contains("physical_wall_room"))
            throw std::invalid_argument("physical room source graph unavailable");
        // A standalone boundary cannot promise editable reconstruction of
        // absent dependent graphs. Organizational bindings alone are detached.
        if (!Document::create({detached_native_entity(entity, {{entity.id, entity.id}})}).snapshot().is_editable())
            throw std::invalid_argument("native boundary schema is not editable");
        auto block = boundary_plan_block(entity,
            "VERTEX_BOUNDARY_" + std::to_string(result.drawing.blocks.size() + 1), layer);
        block.vertex_entity_json = payload;
        DxfDrawing bounded;
        bounded.blocks.push_back(block);
        (void)export_dxf_ascii(bounded);
        result.drawing.inserts.push_back({block.name, {}, 1, 1, 0, layer});
        result.drawing.blocks.push_back(std::move(block));
        return true;
    } catch (const std::exception&) {
        diagnostic(result.diagnostics, entity.id, entity.type, "native_boundary_not_representable");
        return false;
    }
}

Json boundary_group_marker(const std::vector<std::string>& members, int version = 3) {
    return {{"version", version}, {"depiction", kBoundaryDepiction}, {"member_ids", members}};
}

bool boundary_has_stair_declaration(const Entity& entity) {
    (void)native_dxf_boundary_dependency_graph(entity);
    const auto facts = entity.properties.find("appraisal_facts");
    if (facts == entity.properties.end()) return false;
    if (facts->value("boundary_role", std::string{}) == "stair_footprint") return true;
    const auto ansi = facts->find("ansi");
    if (ansi == facts->end()) return false;
    const auto ceiling = ansi->find("ceiling");
    return ceiling != ansi->end() && (ceiling->contains("stair_from_floor_id") ||
        ceiling->value("kind", std::string{}) == "stairs");
}

void validate_boundary_group_source_contexts(const std::vector<Entity>& members,
                                           std::size_t* charged_work = nullptr,
                                           bool preflight_only = false) {
    std::map<std::string, const Entity*, std::less<>> owners;
    std::map<std::string, Boundary, std::less<>> geometry;
    std::size_t segments = 0;
    // Match the isolated candidate's geometry limits before invoking any solid
    // containment operation. Actual inline holes still receive strict topology
    // validation through boundary_plan_block; deductions are inclusive areas.
    for (const auto& entity : members) {
        owners.emplace(entity.id, &entity);
        auto outer = preflight_only ? read_entity_boundary_for_admission(entity) : read_entity_boundary(entity);
        if (!outer) throw std::invalid_argument("source appraisal geometry unavailable");
        std::size_t count = outer->size();
        if (const auto holes = entity.properties.find("holes"); holes != entity.properties.end()) {
            if (!holes->is_array()) throw std::invalid_argument("invalid native holes");
            for (const auto& value : *holes) {
                const auto hole = read_boundary_value(value);
                if (!hole || hole->size() > 512 || count > 512 - hole->size())
                    throw std::invalid_argument("native boundary group geometry limit");
                count += hole->size();
            }
        }
        if (count > 512 || segments > 50'000 - count)
            throw std::invalid_argument("native boundary group geometry limit");
        segments += count;
        geometry.emplace(entity.id, std::move(*outer));
    }
    const auto field = [](const Entity& entity, const char* key) {
        const auto value = entity.properties.find(key);
        if (value == entity.properties.end()) return std::string{};
        if (!value->is_string()) throw std::invalid_argument("invalid source appraisal context");
        return value->get<std::string>();
    };
    const auto source_scope = [&](const Entity& entity) {
        if (entity.properties.contains("calculation_scope")) return field(entity, "calculation_scope");
        const auto classification = entity.properties.contains("measurement_classification")
            ? field(entity, "measurement_classification") : field(entity, "classification");
        return classification == "survey" ? std::string("site") : std::string("building");
    };
    std::uint64_t pairs = charged_work ? *charged_work : 0;
    for (const auto& entity : members) {
        const auto graph = native_dxf_boundary_dependency_graph(entity);
        std::size_t operation_segments = geometry.at(entity.id).size();
        for (const auto& id : graph.at("deduction_ids")) {
            const auto& child = *owners.at(id.get<std::string>());
            const auto floor = field(entity, "floor_id");
            if (floor.empty() || field(child, "floor_id") != floor ||
                field(child, "building_id") != field(entity, "building_id") ||
                field(child, "property_id") != field(entity, "property_id") ||
                source_scope(entity) == "site" || source_scope(child) == "site")
                throw std::invalid_argument("source appraisal deduction context differs");
            const auto count = geometry.at(child.id).size();
            if (operation_segments > 50'000 - count)
                throw std::invalid_argument("native appraisal deduction work limit");
            operation_segments += count;
        }
        const auto work = static_cast<std::uint64_t>(operation_segments) * operation_segments;
        if (work > 250'000 || pairs > 250'000 - work)
            throw std::invalid_argument("native appraisal deduction work limit");
        pairs += work;
    }
    if (charged_work) *charged_work = static_cast<std::size_t>(pairs);
    if (preflight_only) return;
    for (const auto& entity : members) {
        const auto graph = native_dxf_boundary_dependency_graph(entity);
        if (graph.at("deduction_ids").empty()) continue;
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
        // Use the same inclusive containment and union subtraction as the
        // appraisal consumer: edge-sharing and full-parent deductions are valid.
        // This private profile proves geometry only and derives no living-area
        // qualification, measurement observation or reporting confirmation.
        CalculationProfile profile{"native-dxf-geometry", 1, AreaUnit::square_metre, 2,
            {{"geometry", ClassificationRule{}}}};
        MeasurementArea area{entity.id, "native-dxf-context", field(entity, "floor_id"),
            "geometry", geometry.at(entity.id), {}, {1, 1}, AreaScope::building};
        for (const auto& id : graph.at("deduction_ids"))
            area.deductions.push_back({id.get<std::string>(), geometry.at(id.get<std::string>())});
        (void)calculate_area(area, profile);
#else
        throw std::invalid_argument("native appraisal containment engine unavailable");
#endif
    }
}

// Reproduce the existing appraisal_ceiling_geometry_digest binding without
// making the transport library depend on appraisal profile/report authority.
// This is a check of source evidence, never a new observation or certification.
void validate_boundary_group_ceiling_sources(const std::vector<Entity>& members) {
    std::map<std::string, const Entity*, std::less<>> owners;
    for (const auto& entity : members) owners.emplace(entity.id, &entity);
    for (const auto& entity : members) {
        if (!entity.properties.contains("appraisal_facts")) continue;
        const auto& facts = entity.properties.at("appraisal_facts");
        if (!facts.contains("ansi") || !facts.at("ansi").contains("ceiling")) continue;
        const auto& ceiling = facts.at("ansi").at("ceiling");
        if (ceiling.value("kind", std::string{}) != "sloped") continue;
        const auto graph = native_dxf_boundary_dependency_graph(entity);
        if (graph.at("room_boundary_id") != entity.id)
            throw std::invalid_argument("source ceiling room anchor differs");
        const auto outer = read_entity_boundary(entity);
        if (!outer) throw std::invalid_argument("source ceiling geometry unavailable");
        std::map<std::string, Entity, std::less<>> binding;
        binding.emplace("room", Entity{"room", "ceiling_geometry", {{"boundary", boundary_json(*outer)}}, false, Json::object()});
        for (const auto& id : graph.at("deduction_ids")) {
            const auto child = owners.at(id.get<std::string>());
            const auto geometry = read_entity_boundary(*child);
            if (!geometry) throw std::invalid_argument("source ceiling deduction unavailable");
            const auto key = "deduction:" + child->id;
            binding.emplace(key, Entity{key, "ceiling_deduction", {{"boundary", boundary_json(*geometry)}}, false, Json::object()});
        }
        if (!ceiling.contains("source_geometry_sha256") || !ceiling.at("source_geometry_sha256").is_string() ||
            ceiling.at("source_geometry_sha256") != entity_map_digest(binding))
            throw std::invalid_argument("stale source ceiling geometry evidence");
    }
}

bool export_boundary_group(const DocumentSnapshot& document, const std::vector<std::string>& ids,
                           const ConstraintPhaseScope& scope, DxfProjectExportResult& result,
                           NativeDxfWallSourceWorkBudget& wall_source_budget, bool preflight_only = false) {
    try {
        std::vector<Entity> source, detached;
        std::map<std::string, std::string> unchanged;
        int version = 3;
        for (const auto& id : ids) {
            const auto found = document.entities().find(id);
            if (found != document.entities().end() && (found->second.type == "measurement_linework" ||
                found->second.extensions.contains("measurement_linework_sources") ||
                found->second.extensions.contains("measurement_linework_group"))) version = 6;
            else if (version != 6 && found != document.entities().end() && found->second.properties.contains("wall_measurement_source")) version = 5;
            else if (version < 5 && found != document.entities().end() && boundary_has_stair_declaration(found->second)) version = 4;
        }
        std::map<std::string, DrawingContext, std::less<>> captured_contexts;
        std::set<std::string, std::less<>> visible;
        if (version == 6) {
            const auto organization = organize_project(document);
            for (const auto& [id, entity] : document.entities()) {
                if (scope.inactive_owner_ids.contains(id)) continue;
                visible.insert(id);
                if (entity.type == "measurement_linework" || std::binary_search(ids.begin(), ids.end(), id)) {
                    const auto context = organization.drawing_context(id);
                    if (!context) {
                        if (std::binary_search(ids.begin(), ids.end(), id)) throw std::invalid_argument("source measured context unavailable");
                        continue;
                    }
                    captured_contexts.emplace(id, *context);
                }
            }
        }
        const auto marker = boundary_group_marker(ids, version);
        for (const auto& id : ids) {
            const auto found = document.entities().find(id);
            if (found == document.entities().end() || scope.inactive_owner_ids.contains(id) ||
                (version >= 5 ? found->second.type != "boundary" && found->second.type != "measurement_boundary" &&
                    found->second.type != "wall" && found->second.type != "opening" &&
                    !(version == 6 && found->second.type == "measurement_linework") :
                    !can_recognize_boundary_entity_type(found->second.type)))
                throw std::invalid_argument("inactive or unavailable boundary dependency");
            auto entity = found->second;
            // Re-export uses the current reviewed self-floor declaration. Prior
            // destination admission state never becomes a new source binding.
            entity.extensions.erase("vertex_dxf_stair_floor_binding");
            entity.extensions.erase(kWallSourceContextBinding);
            entity.extensions.erase(kResolvedContext);
            entity.extensions.erase(kMeasuredGraph);
            entity.extensions["vertex_dxf_boundary"] = marker;
            if (version == 6) {
                entity.extensions[kResolvedContext] = {{"version", 1}, {"context", resolved_context_json(captured_contexts.at(id))}};
                entity.extensions[kMeasuredGraph] = {{"version", 1}, {"source_ids",
                    measured_graph_ids(entity, document.entities(), captured_contexts, &visible)}};
            }
            if (version >= 5 && entity.type == "wall") {
                std::vector<std::string> hosted;
                for (const auto* opening : host_openings(document, entity.id, scope)) hosted.push_back(opening->id);
                std::sort(hosted.begin(), hosted.end());
                entity.extensions[kWallSourceHostedOpenings] = {{"version", 1}, {"opening_ids", hosted}};
            }
            if (version >= 4) {
                const auto floor_id = version >= 5 ? wall_source_stair_floor(entity) : native_dxf_boundary_stair_floor_source(entity);
                if (!floor_id.empty()) {
                    const auto floor = document.entities().find(floor_id);
                    if (floor == document.entities().end() || floor->second.type != "floor")
                        throw std::invalid_argument("source stair owning floor unavailable");
                }
            }
            source.push_back(std::move(entity));
            unchanged.emplace(id, id);
        }
        // Bound each actual wire envelope before source replay or containment.
        for (const auto& entity : source) {
            auto metadata = native_payload(document, document.entities().at(entity.id), scope);
            metadata["extensions"].erase("vertex_dxf_boundary");
            metadata["extensions"].erase("vertex_dxf_stair_floor_binding");
            metadata["extensions"].erase(kWallSourceContextBinding);
            metadata["extensions"].erase(kWallSourceHostedOpenings);
            metadata["extensions"].erase(kMeasuredGraph);
            metadata["extensions"].erase(kResolvedContext);
            metadata["version"] = version;
            metadata["depiction"] = source_depiction(entity);
            metadata["member_ids"] = ids;
            metadata["dependency_graph"] = version >= 5 ? native_dxf_wall_source_dependency_graph(entity) :
                native_dxf_boundary_dependency_graph(entity);
            if (version == 6) metadata["resolved_context"] = resolved_context_json(captured_contexts.at(entity.id));
            const auto payload = metadata.dump();
            if (payload.size() > 16 * 1024) throw std::invalid_argument("native payload byte limit");
            (void)bounded_native_json(payload);
        }
        if (preflight_only) {
            if (version >= 5) validate_native_dxf_wall_source_groups(source, &wall_source_budget, true);
            return true;
        }
        if (version >= 5) validate_native_dxf_wall_source_groups(source, &wall_source_budget);
        else validate_native_dxf_boundary_groups(source);
        std::vector<Entity> source_boundaries;
        for (const auto& entity : source)
            if (can_recognize_boundary_entity_type(entity.type)) source_boundaries.push_back(entity);
        if (version < 5) validate_boundary_group_source_contexts(source_boundaries);
        validate_boundary_group_ceiling_sources(source_boundaries);
        for (const auto& entity : source) detached.push_back(detached_native_entity(entity, unchanged, version >= 4));
        if (version >= 5) validate_native_dxf_wall_source_groups(detached, &wall_source_budget);
        else validate_native_dxf_boundary_groups(detached);
        const auto detached_document = Document::create(detached).snapshot();
        if (!detached_document.is_editable())
            throw std::invalid_argument("native boundary group schema is not editable");
        DxfDrawing pending;
        for (const auto& entity : source) {
            auto metadata = native_payload(document, document.entities().at(entity.id), scope);
            // The current import marker is admission state, not source graph
            // authority. The carrier below declares its actual new membership.
            metadata["extensions"].erase("vertex_dxf_boundary");
            metadata["extensions"].erase("vertex_dxf_stair_floor_binding");
            metadata["extensions"].erase(kWallSourceContextBinding);
            metadata["extensions"].erase(kWallSourceHostedOpenings);
            metadata["extensions"].erase(kMeasuredGraph);
            metadata["extensions"].erase(kResolvedContext);
            metadata["version"] = version;
            metadata["depiction"] = source_depiction(entity);
            metadata["member_ids"] = ids;
            metadata["dependency_graph"] = version >= 5 ? native_dxf_wall_source_dependency_graph(entity) :
                native_dxf_boundary_dependency_graph(entity);
            if (version == 6) metadata["resolved_context"] = resolved_context_json(captured_contexts.at(entity.id));
            const auto payload = metadata.dump();
            if (payload.size() > 16 * 1024) throw std::invalid_argument("native payload byte limit");
            (void)bounded_native_json(payload);
            const auto layer = layer_for(document, entity, result.diagnostics);
            const auto name = "VERTEX_BOUNDARY_" + std::to_string(result.drawing.blocks.size() + pending.blocks.size() + 1);
            auto block = entity.type == "measurement_linework" ? linework_plan_block(entity, name, layer) :
                can_recognize_boundary_entity_type(entity.type) ? boundary_plan_block(entity, name, layer, true) :
                architectural_block(document, entity, name, layer, result.diagnostics, scope);
            if (version >= 5) {
                const auto& fresh = detached_document.entities().at(entity.id);
                std::vector<DxfProjectDiagnostic> geometry_diagnostics;
                const auto regenerated = source_plan_block(detached_document.entities(), fresh, name, layer, geometry_diagnostics);
                // Exact parity is checked below by the same native comparison
                // used for input carriers; source and detached plans share order.
                if (!geometry_diagnostics.empty() || !same_block_geometry(block, regenerated, true))
                    throw std::invalid_argument("detached V5 plan differs");
            }
#ifndef SKETCH_DXF_NATIVE_GEOMETRY
            if (entity.type == "opening" && entity.properties.contains("opening_assembly"))
                throw std::invalid_argument("manufactured plan geometry unavailable");
#endif
            block.vertex_entity_json = payload;
            pending.inserts.push_back({block.name, {}, 1, 1, 0, layer});
            pending.blocks.push_back(std::move(block));
        }
        (void)export_dxf_ascii(pending);
        result.drawing.blocks.insert(result.drawing.blocks.end(), pending.blocks.begin(), pending.blocks.end());
        result.drawing.inserts.insert(result.drawing.inserts.end(), pending.inserts.begin(), pending.inserts.end());
        return true;
    } catch (const std::exception&) {
        for (const auto& id : ids) diagnostic(result.diagnostics, id, "boundary", "native_boundary_group_not_representable");
        return false;
    }
}

// Build undirected components for the saved active boundary design. An active
// link to a hidden dependency still enters the component and refuses it; an
// unrelated inactive incoming owner is retained evidence, not emitted output.
// Failed components keep every active member on ordinary fallback.
std::pair<std::set<std::string>, std::set<std::string>> export_boundary_groups(
    const DocumentSnapshot& document, const ConstraintPhaseScope& scope, DxfProjectExportResult& result,
    NativeDxfWallSourceWorkBudget& wall_source_budget) {
    std::map<std::string, std::set<std::string>> adjacency;
    std::set<std::string> linked;
    std::map<std::string, DrawingContext, std::less<>> contexts;
    std::set<std::string, std::less<>> visible;
    const bool measured_operation = std::any_of(document.entities().begin(), document.entities().end(), [&](const auto& owner) {
        return !scope.inactive_owner_ids.contains(owner.first) && (owner.second.type == "measurement_linework" ||
            owner.second.extensions.contains("measurement_linework_sources") || owner.second.extensions.contains("measurement_linework_group"));
    });
    wall_source_budget.measured_operation = measured_operation;
    std::optional<ProjectOrganization> organization;
    if (measured_operation) organization = organize_project(document);
    for (const auto& [id, entity] : document.entities()) {
        (void)entity;
        if (scope.inactive_owner_ids.contains(id)) continue;
        visible.insert(id);
        if (organization) if (const auto context = organization->drawing_context(id)) contexts.emplace(id, *context);
    }
    for (const auto& [id, entity] : document.entities()) {
        if (scope.inactive_owner_ids.contains(id) ||
            (!can_recognize_boundary_entity_type(entity.type) && entity.type != "wall" && entity.type != "opening" &&
             entity.type != "measurement_linework")) continue;
        adjacency.try_emplace(id);
        if (entity.type == "measurement_linework") linked.insert(id);
        for (const auto& target : recover_measured_source_ids(entity.extensions)) {
            linked.insert(id); adjacency[id].insert(target); adjacency[target].insert(id);
        }
        try {
            const auto dependencies = native_dxf_wall_source_dependency_ids(entity);
            if (can_recognize_boundary_entity_type(entity.type) &&
                (!dependencies.empty() || boundary_has_stair_declaration(entity))) linked.insert(id);
            for (const auto& target : dependencies) {
                adjacency[id].insert(target); adjacency[target].insert(id);
            }
            for (const auto& target : measured_graph_ids(entity, document.entities(), contexts, &visible)) {
                linked.insert(id); adjacency[id].insert(target); adjacency[target].insert(id);
            }
        } catch (const std::exception&) {
            if (can_recognize_boundary_entity_type(entity.type)) linked.insert(id);
        }
    }
    std::set<std::string> processed, activated, fallback;
    std::vector<std::vector<std::string>> components;
    for (const auto& root : linked) {
        if (processed.contains(root)) continue;
        std::set<std::string> members{root};
        std::vector<std::string> pending{root};
        for (std::size_t i = 0; i < pending.size(); ++i)
            for (const auto& child : adjacency[pending[i]]) if (members.insert(child).second) pending.push_back(child);
        processed.insert(members.begin(), members.end());
        components.emplace_back(members.begin(), members.end());
    }
    // V6 operation admission preflights every component before any nonlinear
    // proof. V1..V5-only operations retain their prior work/schema contract.
    if (measured_operation) for (const auto& ids : components)
        if (!export_boundary_group(document, ids, scope, result, wall_source_budget, true))
            fallback.insert(ids.begin(), ids.end());
    for (const auto& ids : components) {
        if (std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return fallback.contains(id); })) continue;
        auto& destination = export_boundary_group(document, ids, scope, result, wall_source_budget) ? activated : fallback;
        destination.insert(ids.begin(), ids.end());
    }
    return {std::move(activated), std::move(fallback)};
}

void export_architectural_entity(const DocumentSnapshot& document, const Entity& entity,
                                DxfProjectExportResult& result, const ConstraintPhaseScope& scope,
                                bool allow_native = true) {
    try {
        // Reject unbounded metadata before potentially expensive solid/section
        // work. Geometry cannot make an unbounded source into active metadata.
        const auto payload = native_payload(document, entity, scope).dump();
        if (payload.size() > 16 * 1024) throw std::invalid_argument("native payload byte limit");
        (void)bounded_native_json(payload);
        const auto layer = layer_for(document, entity, result.diagnostics);
        auto block = architectural_block(document, entity,
            "VERTEX_PLAN_" + std::to_string(result.drawing.blocks.size() + 1),
            entity.type == "opening" && layer == "0" ? "Openings" : layer, result.diagnostics, scope);
        block.vertex_entity_json = payload;
        if (!allow_native) block.vertex_entity_json.clear();
#ifndef SKETCH_DXF_NATIVE_GEOMETRY
        if (entity.type == "opening" && entity.properties.contains("opening_assembly")) {
            block.vertex_entity_json.clear();
            diagnostic(result.diagnostics, entity.id, entity.type, "manufactured_plan_geometry_unavailable");
        }
#endif
        // Validate independently so an oversized native payload cannot erase
        // unrelated project output. The plan block still survives as fallback.
        DxfDrawing probe;
        probe.blocks.push_back(block);
        try { if (!block.vertex_entity_json.empty()) (void)bounded_native_json(block.vertex_entity_json); (void)export_dxf_ascii(probe); }
        catch (const std::exception&) {
            block.vertex_entity_json.clear();
            diagnostic(result.diagnostics, entity.id, entity.type, "native_metadata_not_representable");
        }
        result.drawing.inserts.push_back({block.name, {}, 1, 1, 0, layer});
        result.drawing.blocks.push_back(std::move(block));
    } catch (const std::exception&) {
        diagnostic(result.diagnostics, entity.id, entity.type, "native_architectural_plan_not_representable");
    }
}

bool annotation_has_inactive_owner(const Entity& entity, const ConstraintPhaseScope& scope) {
    if (scope.inactive_owner_ids.empty()) return false;
    // These are the top-level entity reference fields admitted by Document's
    // collect_references contract. Text, arbitrary nested JSON and extensions
    // are source content, never evidence of an analytical owner relationship.
    const auto inactive = [&](const Json& value) {
        return value.is_string() && scope.inactive_owner_ids.contains(value.get_ref<const std::string&>());
    };
    for (const auto* key : {"assembly_catalog_id", "property_id", "building_id", "floor_id",
                           "layer_id", "boundary_id", "wall_id", "opening_id", "room_id",
                           "slab_id", "roof_id", "stair_id", "sheet_id", "view_id",
                           "constraint_id", "label_id", "column_id", "beam_id", "railing_id",
                           "parent_id", "host_id", "target_id", "entity_id", "source_entity_id"}) {
        const auto single = entity.properties.find(key);
        if (single != entity.properties.end() && inactive(*single)) return true;
        const auto collection = entity.properties.find(std::string(key) + "s");
        if (collection != entity.properties.end() && collection->is_array() &&
            std::any_of(collection->begin(), collection->end(), inactive)) return true;
    }
    for (const auto* key : {"refs", "references"}) {
        const auto collection = entity.properties.find(key);
        if (collection != entity.properties.end() && collection->is_array() &&
            std::any_of(collection->begin(), collection->end(), inactive)) return true;
    }
    return false;
}

void report_boundary_semantics_loss(const Entity& entity, std::string_view imported_classification,
                                    std::vector<DxfProjectDiagnostic>& diagnostics) {
    // Detached import IDs are ordinary policy, but analytical curve records
    // cannot preserve native topology, boundary roles or appraisal facts.
    if (inspect_boundary_entity_version(entity).format == BoundaryEntityFormat::identified_v1)
        diagnostic(diagnostics, entity.id, entity.type, "boundary_topology_not_representable");
    if (entity.type != "boundary")
        diagnostic(diagnostics, entity.id, entity.type, "boundary_type_not_representable");
    for (const auto* key : {"classification", "measurement_classification", "appraisal_category", "appraisal_facts"}) {
        if (!entity.properties.contains(key)) continue;
        if (std::string_view(key) == "classification" && !imported_classification.empty() &&
            entity.properties.at(key).is_string() &&
            entity.properties.at(key).get_ref<const std::string&>() == imported_classification)
            continue;
        diagnostic(diagnostics, entity.id, entity.type, "boundary_classification_not_representable");
        break;
    }
}

void export_native_entity(const DocumentSnapshot& document, const Entity& entity,
                          DxfProjectExportResult& result, const ConstraintPhaseScope& scope,
                          bool allow_boundary_native = true,
                          NativeDxfWallSourceWorkBudget* source_budget = nullptr) {
    if (entity.type == "wall" || entity.type == "opening") {
        export_architectural_entity(document, entity, result, scope, allow_boundary_native);
        return;
    }
    if ((entity.type == kAnnotationEntityType || entity.type == "label") &&
        annotation_has_inactive_owner(entity, scope)) {
        diagnostic(result.diagnostics, entity.id, entity.type, "inactive_design_owner_not_exported");
        return;
    }
    const auto layer = layer_for(document, entity, result.diagnostics);
    if (entity.type == "measurement_linework") {
        try {
            if (source_budget) {
                const auto [count, replay_work] = raw_measured_stroke_work(entity);
                if (source_budget->segments > 50'000 - count || source_budget->source_work > 250'000 ||
                    replay_work > (250'000 - source_budget->source_work) / 16)
                    throw std::invalid_argument("V6 fallback replay work limit");
                source_budget->segments += count;
                source_budget->source_work += replay_work * 16;
            }
            const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
            if (!decoded.supported()) {
                diagnostic(result.diagnostics, entity.id, entity.type, "measurement_linework_model_unsupported");
                return;
            }
            const auto replay = replay_measurement_linework(*decoded.model);
            Boundary geometry;
            geometry.reserve(replay.edges.size());
            for (const auto& edge : replay.edges) geometry.push_back(edge.segment);
            if (geometry.size() == 1) {
                add_segment_as_dxf(result.drawing, geometry.front(), layer,
                    result.diagnostics, entity.id, entity.type);
            } else if (const auto polyline = dxf_polyline_from_boundary(geometry, replay.closed, layer)) {
                result.drawing.polylines.push_back(*polyline);
            } else {
                diagnostic(result.diagnostics, entity.id, entity.type, "measurement_linework_geometry_not_representable");
                return;
            }
            // Plain DXF carries exact analytical geometry, but cannot carry the
            // native receipt expressions, topology IDs, or future extensions.
            diagnostic(result.diagnostics, entity.id, entity.type, "measurement_linework_inputs_not_representable");
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, entity.id, entity.type, "measurement_linework_geometry_not_representable");
        }
        return;
    }
    if (can_recognize_boundary_entity_type(entity.type)) {
        if (source_budget) {
            try {
                const auto count = raw_boundary_segment_count(entity);
                const auto work = count * count * 16;
                if (source_budget->segments > 50'000 - count || source_budget->source_work > 250'000 ||
                    work > 250'000 - source_budget->source_work)
                    throw std::invalid_argument("V6 fallback topology work limit");
                source_budget->segments += count;
                source_budget->source_work += work;
            } catch (const std::exception&) {
                diagnostic(result.diagnostics, entity.id, entity.type, "boundary_not_representable");
                return;
            }
        }
        const auto boundary = read_entity_boundary(entity);
        if (!boundary) {
            diagnostic(result.diagnostics, entity.id, entity.type, "boundary_not_representable");
            return;
        }
        if (allow_boundary_native && boundary->size() >= 2 && same_point(boundary->back().end, boundary->front().start) &&
            export_boundary_entity(document, entity, layer, result)) return;
        const auto line_count = result.drawing.lines.size();
        const auto arc_count = result.drawing.arcs.size();
        const auto polyline_count = result.drawing.polylines.size();
        add_boundary_as_dxf(result.drawing, *boundary, layer, result.diagnostics,
                            entity.id, entity.type);
        // A source CAD primitive classification can survive when the same
        // single primitive is emitted. Native area classifications cannot.
        std::string_view imported_classification;
        const auto lines_added = result.drawing.lines.size() - line_count;
        const auto arcs_added = result.drawing.arcs.size() - arc_count;
        const auto polylines_added = result.drawing.polylines.size() - polyline_count;
        if (lines_added == 1 && arcs_added == 0 && polylines_added == 0)
            imported_classification = "dxf_line";
        else if (lines_added == 0 && arcs_added == 1 && polylines_added == 0)
            imported_classification = "dxf_arc";
        else if (lines_added == 0 && arcs_added == 0 && polylines_added == 1)
            imported_classification = result.drawing.polylines.back().closed
                ? "dxf_polyline_closed" : "dxf_polyline_open";
        add_entity_holes_as_dxf(result.drawing, entity, layer, result.diagnostics);
        report_boundary_semantics_loss(entity, imported_classification, result.diagnostics);
        return;
    }
    if (entity.type == "slab") {
        const auto boundary = entity.properties.is_object() && entity.properties.contains("boundary")
            ? read_boundary_value(entity.properties.at("boundary")) : std::nullopt;
        if (!boundary) {
            diagnostic(result.diagnostics, entity.id, entity.type, "slab_boundary_not_representable");
            return;
        }
        // The footprint and explicit hole loops are representable. Thickness,
        // elevation, kind, and material layers are native 3D semantics and
        // must remain visible as a fidelity limitation instead of vanishing.
        if (entity.properties.contains("thickness_m") ||
            entity.properties.contains("thickness") ||
            entity.properties.contains("elevation_m") ||
            entity.properties.contains("element_kind") ||
            entity.properties.contains("layers")) {
            diagnostic(result.diagnostics, entity.id, entity.type,
                       "slab_3d_semantics_not_representable");
        }
        add_boundary_as_dxf(result.drawing, *boundary, layer, result.diagnostics,
                            entity.id, entity.type);
        add_entity_holes_as_dxf(result.drawing, entity, layer, result.diagnostics);
        return;
    }
    if (entity.type == kAnnotationEntityType) {
        try {
            const auto state = decode_annotation_entity(entity);
            for (const auto& label : state.labels) {
                if (!label.visible) continue;
                result.drawing.labels.push_back({{label.placement.position.x, label.placement.position.y},
                    label.style.text_height_metres, normalized_degrees(label.placement.rotation_radians),
                    label.content, annotation_layer_for(document, label.placement, label.id, "Annotations", result.diagnostics)});
            }
            const auto catalog = default_symbol_catalog();
            for (const auto& symbol : state.symbols) {
                if (!symbol.visible) continue;
                const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& item) {
                    return item.id == symbol.symbol_id;
                });
                if (found == catalog.end() && !symbol.definition) {
                    diagnostic(result.diagnostics, entity.id, entity.type, "symbol_definition_missing");
                    continue;
                }
                try {
                    const auto definition = resolved_symbol_definition(symbol, catalog);
                    const auto symbol_layer = annotation_layer_for(document, symbol.placement, symbol.id, "Symbols", result.diagnostics);
                    for (const auto& stroke : transformed_symbol_preview(definition, symbol))
                        result.drawing.lines.push_back({{stroke.start.x, stroke.start.y},
                                                        {stroke.end.x, stroke.end.y}, symbol_layer});
                } catch (const std::exception&) {
                    diagnostic(result.diagnostics, entity.id, entity.type, "symbol_not_representable");
                }
            }
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, entity.id, entity.type, "annotation_not_representable");
        }
        return;
    }
    if (entity.type == "dimension") {
        try {
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) {
                diagnostic(result.diagnostics, entity.id, entity.type, "dimension_semantics_unsupported");
                return;
            }
            if (scope.inactive_owner_ids.contains(decoded.dimension->boundary_id)) {
                diagnostic(result.diagnostics, entity.id, entity.type, "inactive_design_owner_not_exported");
                return;
            }
            if (decoded.dimension->presentation &&
                !decoded.dimension->presentation->visible) {
                // Visibility is presentation state. Keep hidden dimensions
                // out of exported drawing output just as the desktop canvas
                // and sheet renderer do.
                return;
            }
            const auto owner = document.entities().find(decoded.dimension->boundary_id);
            if (owner == document.entities().end()) {
                diagnostic(result.diagnostics, entity.id, entity.type, "dimension_owner_missing");
                return;
            }
            // Export uses the actual current document inventory. Entity-only
            // resolution cannot admit physical-room targets or their holes.
            const auto resolved = resolve_current_boundary_dimension(*decoded.dimension, document);
            const auto text = entity.properties.value("display_text", std::string{});
            const auto text_rotation = decoded.dimension->presentation
                ? normalized_degrees(decoded.dimension->presentation->rotation_radians) : 0.0;
            const auto callout = [&](std::string name, double quantity, std::string_view suffix,
                                     const char* fidelity) {
                if (text != " ") {
                    name += ": " + dimension_quantity_text(quantity, suffix, text);
                    if (name.size() > DxfExchangeLimits{}.max_string_bytes)
                        throw std::invalid_argument("DXF dimension callout exceeds its transport limit");
                    result.drawing.labels.push_back({{decoded.dimension->text_position.x,
                        decoded.dimension->text_position.y}, 0.15, text_rotation, std::move(name), "Dimensions"});
                }
                diagnostic(result.diagnostics, entity.id, entity.type, fidelity);
            };
            if (resolved.kind == BoundaryDimensionKind::area) {
                callout("Area", resolved.area(), " m2", "dimension_area_exported_as_quantity_callout");
                return;
            }
            if (resolved.kind == BoundaryDimensionKind::angle) {
                if (!resolved.angle_geometry)
                    throw std::invalid_argument("DXF angular dimension has no admitted tangent geometry");
                const auto& geometry = *resolved.angle_geometry;
                auto first = geometry.first_direction;
                auto second = geometry.second_direction;
                auto start = std::atan2(first.y, first.x);
                auto sweep = std::fmod(std::atan2(second.y, second.x) - start + kFullTurn, kFullTurn);
                // Native angle targets measure the smaller angle between the
                // admitted vertex tangents, including curved source edges.
                if (sweep > std::numbers::pi) {
                    std::swap(first, second);
                    start = std::atan2(first.y, first.x);
                    sweep = kFullTurn - sweep;
                }
                const auto boundary = resolve_dimension_geometry_owner(owner->second);
                const auto secondary = std::find_if(boundary.segments.begin(), boundary.segments.end(),
                    [&](const auto& item) { return item.segment_id == decoded.dimension->secondary_segment_id; });
                if (secondary == boundary.segments.end())
                    throw std::invalid_argument("DXF angular dimension is missing its admitted second edge");
                const auto first_length = segment_length(resolved.segment);
                const auto second_length = segment_length(secondary->segment);
                auto radius = std::hypot(decoded.dimension->text_position.x - geometry.vertex.x,
                                         decoded.dimension->text_position.y - geometry.vertex.y);
                if (radius <= kGeometryTolerance)
                    radius = std::max(0.3, std::min(first_length, second_length) * 0.5);
                const auto ray_length = std::min(std::min(first_length, second_length), radius * 0.7);
                const DxfPoint first_point{geometry.vertex.x + first.x * ray_length,
                                           geometry.vertex.y + first.y * ray_length};
                const DxfPoint second_point{geometry.vertex.x + second.x * ray_length,
                                            geometry.vertex.y + second.y * ray_length};
                const DxfPoint arc_point{geometry.vertex.x + radius * std::cos(start + sweep * 0.5),
                                         geometry.vertex.y + radius * std::sin(start + sweep * 0.5)};
                result.drawing.angular_dimensions.push_back({first_point, second_point,
                    {geometry.vertex.x, geometry.vertex.y}, arc_point,
                    {decoded.dimension->text_position.x, decoded.dimension->text_position.y},
                    text_rotation, text, "Dimensions"});
                return;
            }
            if (!linear_dimension_chain(*decoded.dimension, owner->second, resolved)) {
                // A bent/curved multi-edge total has no single straight or
                // circular dimension target. Keep its actual quantity as a
                // clearly named callout instead of measuring its endpoint chord.
                callout("Length", resolved.segment_length(), " m",
                    "dimension_chain_exported_as_total_callout");
                return;
            }
            if (resolved.segment.sweep_radians != 0.0) {
                auto measured = resolved.segment;
                if (measured.sweep_radians < 0.0) {
                    std::swap(measured.start, measured.end);
                    measured.sweep_radians = -measured.sweep_radians;
                }
                const auto arc = dxf_arc_from_segment(measured, "Dimensions");
                if (!arc) {
                    diagnostic(result.diagnostics, entity.id, entity.type, "dimension_arc_not_representable");
                    return;
                }
                auto dimension_radius = std::hypot(decoded.dimension->text_position.x - arc->center.x,
                                                   decoded.dimension->text_position.y - arc->center.y);
                if (dimension_radius <= kGeometryTolerance) dimension_radius = arc->radius + 0.3;
                // Keep the arc definition inside the actual measured CCW span.
                // Manual text placement cannot select its complementary arc.
                const auto midpoint_angle = arc->start_degrees * std::numbers::pi / 180.0 +
                    measured.sweep_radians * 0.5;
                const DxfPoint arc_position{arc->center.x + dimension_radius * std::cos(midpoint_angle),
                                           arc->center.y + dimension_radius * std::sin(midpoint_angle)};
                result.drawing.arc_dimensions.push_back({{measured.start.x, measured.start.y},
                    {measured.end.x, measured.end.y}, arc->center, arc_position,
                    {decoded.dimension->text_position.x, decoded.dimension->text_position.y},
                    text_rotation, text, "Dimensions"});
                return;
            }
            result.drawing.dimensions.push_back({{resolved.segment.start.x, resolved.segment.start.y},
                {resolved.segment.end.x, resolved.segment.end.y},
                {decoded.dimension->text_position.x, decoded.dimension->text_position.y},
                {decoded.dimension->text_position.x, decoded.dimension->text_position.y},
                normalized_degrees(std::atan2(resolved.segment.end.y - resolved.segment.start.y,
                                               resolved.segment.end.x - resolved.segment.start.x)),
                text, "Dimensions", true, text_rotation});
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, entity.id, entity.type, "dimension_not_representable");
        }
        return;
    }
    if (entity.type == "label") {
        const auto content = entity.properties.value("content", entity.properties.value("text", std::string{}));
        const auto position = entity.properties.value("position", Json::array());
        const auto point = read_point(position);
        if (content.empty() || !point) {
            diagnostic(result.diagnostics, entity.id, entity.type, "label_not_representable");
            return;
        }
        const auto height = entity.properties.value("height_m", 0.15);
        result.drawing.labels.push_back({{point->x, point->y}, height, 0.0, content, "Annotations"});
        return;
    }
    // Project scaffolding and architectural objects without a 2D exchange
    // representation are intentionally reported. Their native source remains
    // intact because this function never mutates the snapshot.
    if (entity.type != "property" && entity.type != "building" && entity.type != "floor" &&
        entity.type != "layer" && entity.type != "sheet" && entity.type != "view" &&
        entity.type != "sheet_view_model" && entity.type != "reference_asset")
        diagnostic(result.diagnostics, entity.id, entity.type, "entity_not_representable");
}

Json source_extension(const std::string& layer, std::string_view primitive) {
    return Json{{"format", "dxf"}, {"version", "R2013"}, {"layer", layer},
                {"primitive", primitive}};
}

Entity imported_boundary(std::string id, const Boundary& boundary, std::string classification,
                         std::string layer, std::string primitive,
                         Json extra_extensions = Json::object()) {
    if (!extra_extensions.is_object()) {
        throw std::invalid_argument("DXF import extension metadata must be an object");
    }
    Json extensions{{"dxf_source", source_extension(layer, primitive)}};
    for (const auto& [key, value] : extra_extensions.items()) {
        if (key == "dxf_source") {
            throw std::invalid_argument("DXF import extension cannot replace dxf_source");
        }
        extensions[key] = value;
    }
    return Entity{std::move(id), "boundary",
                  Json{{"boundary", boundary_json(boundary)},
                       {"classification", std::move(classification)}},
                  false,
                  std::move(extensions)};
}

double radians_from_degrees(double degrees) { return degrees * std::numbers::pi / 180.0; }

Segment arc_segment(const DxfArc& arc) {
    const auto start = radians_from_degrees(arc.start_degrees);
    const auto end = radians_from_degrees(arc.end_degrees);
    auto sweep = end - start;
    if (sweep <= 0.0) sweep += kFullTurn;
    return Segment{{arc.center.x + arc.radius * std::cos(start),
                    arc.center.y + arc.radius * std::sin(start)},
                   {arc.center.x + arc.radius * std::cos(end),
                    arc.center.y + arc.radius * std::sin(end)},
                   sweep};
}

Boundary polyline_boundary(const DxfPolyline& polyline) {
    Boundary result;
    if (polyline.vertices.size() < 2) return result;
    const auto count = polyline.closed ? polyline.vertices.size() : polyline.vertices.size() - 1;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto next = (index + 1) % polyline.vertices.size();
        const auto& first = polyline.vertices[index];
        const auto& second = polyline.vertices[next];
        result.push_back({{first.point.x, first.point.y}, {second.point.x, second.point.y},
                          4.0 * std::atan(first.bulge)});
    }
    return result;
}

bool circle_geometry_representable(const Boundary& boundary, DxfPoint expected_center, double expected_radius) {
    if (!std::isfinite(expected_center.x) || !std::isfinite(expected_center.y) ||
        !std::isfinite(expected_radius) || !(expected_radius > 0) || boundary.size() != 2)
        return false;
    // Closure must survive exactly, independently of the validator's contact
    // tolerance. Validate native analytical arcs before recovering their circle.
    if (boundary[0].end.x != boundary[1].start.x || boundary[0].end.y != boundary[1].start.y ||
        boundary[1].end.x != boundary[0].start.x || boundary[1].end.y != boundary[0].start.y ||
        !validate_boundary(boundary, kGeometryTolerance).empty())
        return false;
    for (const auto& segment : boundary) {
        if (std::abs(segment.sweep_radians) != std::numbers::pi ||
            segment.sweep_radians != boundary.front().sweep_radians)
            return false;
        const auto dx = segment.end.x - segment.start.x;
        const auto dy = segment.end.y - segment.start.y;
        const auto chord = std::hypot(dx, dy);
        const auto half_sweep = segment.sweep_radians * 0.5;
        const auto radius = chord / (2.0 * std::sin(std::abs(half_sweep)));
        const auto offset = chord / (2.0 * std::tan(half_sweep));
        const DxfPoint center{(segment.start.x + segment.end.x) * 0.5 - (dy / chord) * offset,
                              (segment.start.y + segment.end.y) * 0.5 + (dx / chord) * offset};
        if (!std::isfinite(radius) || !std::isfinite(center.x) || !std::isfinite(center.y) ||
            std::abs(radius - expected_radius) > kGeometryTolerance ||
            std::hypot(center.x - expected_center.x, center.y - expected_center.y) > kGeometryTolerance)
            return false;
    }
    return true;
}

std::optional<Boundary> circle_boundary(const DxfCircle& circle) {
    // A single full-turn segment has coincident endpoints and is not a native
    // arc. Two analytical semicircles retain the complete closed geometry.
    const Vec2 right{circle.center.x + circle.radius, circle.center.y};
    const Vec2 left{circle.center.x - circle.radius, circle.center.y};
    Boundary boundary{{right, left, std::numbers::pi}, {left, right, std::numbers::pi}};
    if (!circle_geometry_representable(boundary, circle.center, circle.radius)) return std::nullopt;
    return boundary;
}

struct InsertTransform {
    DxfPoint origin;
    double scale_x{1};
    double scale_y{1};
    double rotation{};
    DxfPoint base;
};

DxfPoint transform_point(DxfPoint point, const InsertTransform& transform) {
    const auto x = (point.x - transform.base.x) * transform.scale_x;
    const auto y = (point.y - transform.base.y) * transform.scale_y;
    const auto c = std::cos(transform.rotation);
    const auto s = std::sin(transform.rotation);
    return {transform.origin.x + c * x - s * y, transform.origin.y + s * x + c * y};
}

std::optional<Boundary> transformed_boundary(const Boundary& source,
                                             const InsertTransform& transform) {
    const auto uniform = std::abs(std::abs(transform.scale_x) - std::abs(transform.scale_y)) <= 1e-10;
    for (const auto& segment : source) {
        if (segment.sweep_radians != 0.0 && !uniform) return std::nullopt;
    }
    Boundary result = source;
    const auto reflected = transform.scale_x * transform.scale_y < 0.0;
    for (auto& segment : result) {
        const auto start = transform_point({segment.start.x, segment.start.y}, transform);
        const auto end = transform_point({segment.end.x, segment.end.y}, transform);
        segment.start = {start.x, start.y};
        segment.end = {end.x, end.y};
        if (reflected) segment.sweep_radians = -segment.sweep_radians;
    }
    return result;
}

void import_direct_geometry(const DxfDrawing& drawing, DxfProjectImportResult& result,
                            std::size_t& counter) {
    for (const auto& line : drawing.lines) {
        const Boundary boundary{{{line.start.x, line.start.y}, {line.end.x, line.end.y}, 0.0}};
        result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++counter),
            boundary, "dxf_line", line.layer, "LINE"));
    }
    for (const auto& arc : drawing.arcs) {
        const Boundary boundary{arc_segment(arc)};
        result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++counter),
            boundary, "dxf_arc", arc.layer, "ARC"));
    }
    for (const auto& circle : drawing.circles) {
        const auto boundary = circle_boundary(circle);
        if (!boundary) {
            diagnostic(result.diagnostics, {}, "CIRCLE", "circle_geometry_not_representable");
            continue;
        }
        result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++counter),
            *boundary, "dxf_circle", circle.layer, "CIRCLE"));
    }
    for (const auto& polyline : drawing.polylines) {
        const auto boundary = polyline_boundary(polyline);
        if (boundary.empty()) {
            diagnostic(result.diagnostics, {}, "LWPOLYLINE", "polyline_not_representable");
            continue;
        }
        result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++counter),
            boundary, polyline.closed ? "dxf_polyline_closed" : "dxf_polyline_open",
            polyline.layer, "LWPOLYLINE"));
    }
    for (const auto& hatch : drawing.hatches) {
        Boundary boundary;
        if (hatch.boundary.size() >= 2) {
            for (std::size_t index = 0; index < hatch.boundary.size(); ++index) {
                const auto next = (index + 1) % hatch.boundary.size();
                boundary.push_back({{hatch.boundary[index].x, hatch.boundary[index].y},
                                    {hatch.boundary[next].x, hatch.boundary[next].y}, 0.0});
            }
        }
        if (boundary.empty()) {
            diagnostic(result.diagnostics, {}, "HATCH", "hatch_not_representable");
            continue;
        }
        result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++counter),
            boundary, hatch.solid ? "dxf_hatch_solid" : "dxf_hatch", hatch.layer, "HATCH"));
    }
}

void import_labels(const std::vector<DxfLabel>& labels, AnnotationState& state,
                   std::size_t& counter, Json& source_layers) {
    for (const auto& label : labels) {
        LabelInstance item;
        item.id = "dxf-label-" + std::to_string(++counter);
        item.template_id = "dxf";
        item.content = label.text;
        item.style.text_height_metres = label.height;
        item.style.stroke_color = "#263241";
        item.style.fill_color = "#FFFFFF";
        item.placement.position = {label.position.x, label.position.y};
        item.placement.rotation_radians = radians_from_degrees(label.rotation_degrees);
        source_layers[item.id] = label.layer;
        state.labels.push_back(std::move(item));
    }
}

void import_dimensions(const std::vector<DxfDimension>& dimensions,
                       DxfProjectImportResult& result, AnnotationState& annotations,
                       std::size_t& boundary_counter, std::size_t& label_counter,
                       Json& source_layers, double source_metres_per_unit) {
    for (const auto& dimension : dimensions) {
        const Boundary extension{{{dimension.extension_start.x, dimension.extension_start.y},
                                  {dimension.extension_end.x, dimension.extension_end.y}, 0.0}};
        const auto boundary_id = "dxf-boundary-" + std::to_string(++boundary_counter);
        const auto label_id = "dxf-dimension-" + std::to_string(++label_counter);
        result.entities.push_back(imported_boundary(
            boundary_id, extension, "dxf_dimension_extension", dimension.layer, "DIMENSION",
            Json{{"dxf_dimension", Json{
                {"dimension_line", Json::array({dimension.dimension_line.x,
                                                 dimension.dimension_line.y})},
                {"text_position", Json::array({dimension.text_position.x,
                                                dimension.text_position.y})},
                {"rotation_degrees", dimension.rotation_degrees},
                {"aligned", dimension.aligned},
                {"text_rotation_degrees", dimension.text_rotation_degrees},
                {"text_height", dimension.text_height},
                {"source_metres_per_unit", source_metres_per_unit},
                {"text", dimension.text},
                {"annotation_id", label_id}}}}));
        LabelInstance label;
        label.id = label_id;
        label.template_id = "dxf-dimension";
        const auto dx = dimension.extension_end.x - dimension.extension_start.x;
        const auto dy = dimension.extension_end.y - dimension.extension_start.y;
        const auto angle = radians_from_degrees(dimension.rotation_degrees);
        const auto measurement = dimension.aligned ? std::hypot(dx, dy)
            : std::abs(dx * std::cos(angle) + dy * std::sin(angle));
        label.content = imported_dimension_text(measurement, dimension.text, source_metres_per_unit);
        label.style.text_height_metres = dimension.text_height;
        label.style.stroke_color = "#263241";
        label.style.fill_color = "#FFFFFF";
        label.placement.position = {dimension.text_position.x, dimension.text_position.y};
        label.placement.rotation_radians = radians_from_degrees(dimension.text_rotation_degrees);
        source_layers[label.id] = dimension.layer;
        annotations.labels.push_back(std::move(label));
        diagnostic(result.diagnostics, {}, "DIMENSION", "dimension_associativity_unbound");
    }
}

void import_arc_dimensions(const std::vector<DxfArcDimension>& dimensions,
                           DxfProjectImportResult& result, AnnotationState& annotations,
                           std::size_t& boundary_counter, std::size_t& label_counter,
                           Json& source_layers, double source_metres_per_unit) {
    for (const auto& dimension : dimensions) {
        const auto radius = std::hypot(dimension.extension_start.x - dimension.center.x,
                                       dimension.extension_start.y - dimension.center.y);
        const auto start_angle = std::atan2(dimension.extension_start.y - dimension.center.y,
                                           dimension.extension_start.x - dimension.center.x);
        const auto end_angle = std::atan2(dimension.extension_end.y - dimension.center.y,
                                         dimension.extension_end.x - dimension.center.x);
        auto sweep = end_angle - start_angle;
        if (sweep <= 0.0) sweep += kFullTurn;
        const Segment measured{{dimension.extension_start.x, dimension.extension_start.y},
            {dimension.center.x + radius * std::cos(end_angle),
             dimension.center.y + radius * std::sin(end_angle)}, sweep};
        const auto measured_length = segment_length(measured);
        const auto boundary_id = "dxf-boundary-" + std::to_string(++boundary_counter);
        const auto label_id = "dxf-dimension-" + std::to_string(++label_counter);
        result.entities.push_back(imported_boundary(boundary_id, Boundary{measured},
            "dxf_arc_dimension_extension", dimension.layer, "ARC_DIMENSION",
            Json{{"dxf_arc_dimension", Json{
                {"center", Json::array({dimension.center.x, dimension.center.y})},
                {"extension_start", Json::array({dimension.extension_start.x, dimension.extension_start.y})},
                {"extension_end", Json::array({dimension.extension_end.x, dimension.extension_end.y})},
                {"dimension_arc", Json::array({dimension.dimension_arc.x, dimension.dimension_arc.y})},
                {"text_position", Json::array({dimension.text_position.x, dimension.text_position.y})},
                {"text_rotation_degrees", dimension.text_rotation_degrees},
                {"text_height", dimension.text_height},
                {"source_metres_per_unit", source_metres_per_unit},
                {"text", dimension.text},
                {"annotation_id", label_id}}}}));
        LabelInstance label;
        label.id = label_id;
        label.template_id = "dxf-dimension";
        label.content = imported_dimension_text(measured_length, dimension.text, source_metres_per_unit);
        label.style.text_height_metres = dimension.text_height;
        label.style.stroke_color = "#263241";
        label.style.fill_color = "#FFFFFF";
        label.placement.position = {dimension.text_position.x, dimension.text_position.y};
        label.placement.rotation_radians = radians_from_degrees(dimension.text_rotation_degrees);
        source_layers[label.id] = dimension.layer;
        annotations.labels.push_back(std::move(label));
        diagnostic(result.diagnostics, boundary_id, "ARC_DIMENSION", "dimension_associativity_unbound");
    }
}

void import_angular_dimensions(const std::vector<DxfAngularDimension>& dimensions,
                               DxfProjectImportResult& result, AnnotationState& annotations,
                               std::size_t& boundary_counter, std::size_t& label_counter,
                               Json& source_layers) {
    for (const auto& dimension : dimensions) {
        const auto start = std::atan2(dimension.extension_start.y - dimension.vertex.y,
                                      dimension.extension_start.x - dimension.vertex.x);
        const auto end = std::atan2(dimension.extension_end.y - dimension.vertex.y,
                                    dimension.extension_end.x - dimension.vertex.x);
        auto sweep = end - start;
        if (sweep <= 0.0) sweep += kFullTurn;
        const Boundary rays{
            {{dimension.extension_start.x, dimension.extension_start.y},
             {dimension.vertex.x, dimension.vertex.y}, 0.0},
            {{dimension.vertex.x, dimension.vertex.y},
             {dimension.extension_end.x, dimension.extension_end.y}, 0.0}};
        const auto boundary_id = "dxf-boundary-" + std::to_string(++boundary_counter);
        const auto label_id = "dxf-dimension-" + std::to_string(++label_counter);
        result.entities.push_back(imported_boundary(boundary_id, rays,
            "dxf_angular_dimension_rays", dimension.layer, "DIMENSION",
            Json{{"dxf_angular_dimension", Json{
                {"vertex", Json::array({dimension.vertex.x, dimension.vertex.y})},
                {"extension_start", Json::array({dimension.extension_start.x, dimension.extension_start.y})},
                {"extension_end", Json::array({dimension.extension_end.x, dimension.extension_end.y})},
                {"dimension_arc", Json::array({dimension.dimension_arc.x, dimension.dimension_arc.y})},
                {"text_position", Json::array({dimension.text_position.x, dimension.text_position.y})},
                {"text_rotation_degrees", dimension.text_rotation_degrees},
                {"text_height", dimension.text_height},
                {"text", dimension.text},
                {"annotation_id", label_id}}}}));
        LabelInstance label;
        label.id = label_id;
        label.template_id = "dxf-dimension";
        // Angle quantities do not scale when linear source units become metres.
        label.content = dimension_quantity_text(sweep * 180.0 / std::numbers::pi, " deg", dimension.text);
        label.style.text_height_metres = dimension.text_height;
        label.style.stroke_color = "#263241";
        label.style.fill_color = "#FFFFFF";
        label.placement.position = {dimension.text_position.x, dimension.text_position.y};
        label.placement.rotation_radians = radians_from_degrees(dimension.text_rotation_degrees);
        source_layers[label.id] = dimension.layer;
        annotations.labels.push_back(std::move(label));
        diagnostic(result.diagnostics, boundary_id, "DIMENSION", "dimension_associativity_unbound");
    }
}

void import_inserts(const DxfDrawing& drawing, const std::set<std::size_t>& native_inserts,
                    DxfProjectImportResult& result,
                    std::size_t& boundary_counter, std::size_t& label_counter,
                    AnnotationState& annotations, Json& source_layers) {
    for (std::size_t insert_index = 0; insert_index < drawing.inserts.size(); ++insert_index) {
        if (native_inserts.contains(insert_index)) continue;
        const auto& insert = drawing.inserts[insert_index];
        const auto block = std::find_if(drawing.blocks.begin(), drawing.blocks.end(),
            [&](const auto& value) { return block_identity(value.name) == block_identity(insert.block_name); });
        if (block == drawing.blocks.end()) continue;
        const InsertTransform transform{{insert.insertion.x, insert.insertion.y}, insert.scale_x,
            insert.scale_y, radians_from_degrees(insert.rotation_degrees), block->base};
        const auto effective_layer = [&](const std::string& layer) -> const std::string& {
            return layer.empty() || layer == "0" ? insert.layer : layer;
        };
        for (const auto& line : block->lines) {
            const DxfPoint start = transform_point(line.start, transform);
            const DxfPoint end = transform_point(line.end, transform);
            result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++boundary_counter),
                Boundary{{{start.x, start.y}, {end.x, end.y}, 0.0}}, "dxf_insert_line",
                effective_layer(line.layer), "INSERT"));
        }
        for (const auto& arc : block->arcs) {
            if (std::abs(std::abs(transform.scale_x) - std::abs(transform.scale_y)) > 1e-10) {
                diagnostic(result.diagnostics, insert.block_name, "INSERT", "nonuniform_arc_scale");
                continue;
            }
            auto transformed = arc_segment(arc);
            transformed.start = {transform_point({transformed.start.x, transformed.start.y}, transform).x,
                                 transform_point({transformed.start.x, transformed.start.y}, transform).y};
            const auto transformed_end = transform_point({transformed.end.x, transformed.end.y}, transform);
            transformed.end = {transformed_end.x, transformed_end.y};
            if (transform.scale_x * transform.scale_y < 0.0) transformed.sweep_radians = -transformed.sweep_radians;
            result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++boundary_counter),
                Boundary{transformed}, "dxf_insert_arc", effective_layer(arc.layer), "INSERT"));
        }
        for (const auto& circle : block->circles) {
            if (std::abs(transform.scale_x) != std::abs(transform.scale_y)) {
                diagnostic(result.diagnostics, insert.block_name, "INSERT", "nonuniform_circle_scale");
                continue;
            }
            const auto center = transform_point(circle.center, transform);
            // CIRCLE has no authored angular endpoints. Construct its diameter
            // after placement so recentering/upscaling can recover geometry
            // that cannot be represented around the source block's coordinates.
            const auto signed_radius = circle.radius * transform.scale_x;
            const auto dx = signed_radius * std::cos(transform.rotation);
            const auto dy = signed_radius * std::sin(transform.rotation);
            const Vec2 right{center.x + dx, center.y + dy};
            const Vec2 left{center.x - dx, center.y - dy};
            const auto sweep = transform.scale_x * transform.scale_y < 0 ? -std::numbers::pi : std::numbers::pi;
            const Boundary transformed{{right, left, sweep}, {left, right, sweep}};
            if (!circle_geometry_representable(transformed, center, std::abs(signed_radius))) {
                diagnostic(result.diagnostics, insert.block_name, "INSERT", "circle_geometry_not_representable");
                continue;
            }
            result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++boundary_counter),
                transformed, "dxf_insert_circle", effective_layer(circle.layer), "INSERT"));
        }
        for (const auto& polyline : block->polylines) {
            auto source = polyline_boundary(polyline);
            const auto transformed = transformed_boundary(source, transform);
            if (!transformed) {
                diagnostic(result.diagnostics, insert.block_name, "INSERT", "nonuniform_polyline_arc_scale");
                continue;
            }
            result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++boundary_counter),
                *transformed, polyline.closed ? "dxf_insert_polyline_closed" : "dxf_insert_polyline_open",
                effective_layer(polyline.layer), "INSERT"));
        }
        std::vector<DxfLabel> labels;
        labels.reserve(block->labels.size());
        for (const auto& label : block->labels) {
            const auto position = transform_point(label.position, transform);
            labels.push_back({position, label.height * std::abs(transform.scale_x),
                label.rotation_degrees + insert.rotation_degrees, label.text, effective_layer(label.layer)});
        }
        import_labels(labels, annotations, label_counter, source_layers);
    }
}

std::optional<double> metres_per_source_unit(int units) {
    switch (units) {
    case 1: return 0.0254; // Inches.
    case 2: return 0.3048; // Feet.
    case 4: return 0.001;  // Millimetres.
    case 5: return 0.01;   // Centimetres.
    case 6: return 1.0;    // Metres.
    case 7: return 1000.0; // Kilometres.
    default: return std::nullopt;
    }
}

struct NativeCandidate {
    Entity entity;
    std::vector<std::string> hosted_ids;
    std::size_t insert_index{};
    const DxfBlock* block{};
    int version{1};
    std::vector<std::string> member_ids;
};

NativeCandidate decode_native_candidate(const DxfBlock& block, std::size_t insert_index, const Json& payload) {
    std::set<std::string> expected{"version", "id", "type", "properties", "extensions", "hosted_opening_ids"};
    std::set<std::string> actual;
    if (!payload.is_object()) throw std::invalid_argument("native payload must be object");
    const bool measured_source = payload.contains("version") && payload.at("version").is_number_integer() && payload.at("version") == 6;
    const bool wall_source = payload.contains("version") && payload.at("version").is_number_integer() &&
        (payload.at("version") == 5 || measured_source);
    const bool linework = measured_source && payload.value("type", std::string{}) == "measurement_linework";
    if (measured_source) expected.insert("resolved_context");
    const bool boundary = (payload.contains("version") && payload.at("version").is_number_integer() &&
        (payload.at("version") == 2 || payload.at("version") == 3 || payload.at("version") == 4)) ||
        (wall_source && payload.contains("type") && (payload.at("type") == "boundary" || payload.at("type") == "measurement_boundary"));
    const bool group = wall_source || (boundary && (payload.at("version") == 3 || payload.at("version") == 4));
    if (group) { expected.insert("member_ids"); expected.insert("dependency_graph"); }
    if (boundary) {
        expected.insert("depiction");
        if (!payload.contains("depiction") || payload.at("depiction") != kBoundaryDepiction)
            throw std::invalid_argument("boundary depiction contract missing");
    }
    const bool manufactured = payload.contains("properties") && payload.at("properties").is_object() &&
        payload.at("properties").contains("opening_assembly");
    if (manufactured) {
        expected.insert("depiction");
        if (!payload.contains("depiction") || payload.at("depiction") != kManufacturedDepiction)
            throw std::invalid_argument("manufactured depiction contract missing");
#ifndef SKETCH_DXF_NATIVE_GEOMETRY
        throw std::invalid_argument("manufactured plan geometry unavailable");
#endif
    }
    if (wall_source && !boundary && !manufactured) {
        expected.insert("depiction");
        const auto depiction = linework ? kLineworkDepiction : payload.value("type", std::string{}) == "wall" ? "WALL_PLAN_V1" : "OPENING_PLAN_V1";
        if (!payload.contains("depiction") || payload.at("depiction") != depiction)
            throw std::invalid_argument("V5 architecture depiction contract missing");
    }
    for (const auto& [key, value] : payload.items()) { (void)value; actual.insert(key); }
    if (actual != expected || !payload.at("version").is_number_integer() ||
        (!boundary && !wall_source && payload.at("version") != 1) ||
        !payload.at("id").is_string() || !payload.at("type").is_string() ||
        !payload.at("properties").is_object() || !payload.at("extensions").is_object() ||
        !payload.at("hosted_opening_ids").is_array()) throw std::invalid_argument("invalid native payload schema");
    NativeCandidate candidate{{payload.at("id").get<std::string>(), payload.at("type").get<std::string>(),
                               payload.at("properties"), false, payload.at("extensions")}, {}, insert_index, &block};
    if (candidate.entity.id.empty() || candidate.entity.id.size() > 255 ||
        (boundary ? !can_recognize_boundary_entity_type(candidate.entity.type) :
         candidate.entity.type != "wall" && candidate.entity.type != "opening" && !linework) ||
        (manufactured && candidate.entity.type != "opening"))
        throw std::invalid_argument("native entity type/id not allowed");
    std::set<std::string> hosted;
    for (const auto& id : payload.at("hosted_opening_ids")) {
        if (!id.is_string() || id.get_ref<const std::string&>().empty() ||
            !hosted.insert(id.get<std::string>()).second) throw std::invalid_argument("invalid hosted ID list");
        candidate.hosted_ids.push_back(id.get<std::string>());
    }
    if ((candidate.entity.type == "opening" || boundary || linework) && !candidate.hosted_ids.empty())
        throw std::invalid_argument("native entity cannot host children");
    if (boundary && candidate.entity.extensions.contains("physical_wall_room"))
        throw std::invalid_argument("physical room source graph unavailable");
    candidate.version = payload.at("version").get<int>();
    if (candidate.entity.extensions.contains(kMeasuredGraph) || candidate.entity.extensions.contains(kResolvedContext))
        throw std::invalid_argument("native source cannot carry measured admission state");
    if ((candidate.version == 4 || wall_source) && candidate.entity.extensions.contains("vertex_dxf_stair_floor_binding"))
        throw std::invalid_argument("native source cannot carry destination floor binding");
    if (wall_source && candidate.entity.extensions.contains(kWallSourceContextBinding))
        throw std::invalid_argument("native source cannot carry destination context binding");
    if (wall_source && candidate.entity.extensions.contains(kWallSourceHostedOpenings))
        throw std::invalid_argument("native source cannot carry hosted admission state");
    if (wall_source && candidate.entity.type == "wall") {
        auto ids = candidate.hosted_ids;
        std::sort(ids.begin(), ids.end());
        candidate.entity.extensions[kWallSourceHostedOpenings] = {{"version", 1}, {"opening_ids", ids}};
    }
    if (measured_source) {
        (void)read_resolved_context(payload.at("resolved_context"));
        candidate.entity.extensions[kResolvedContext] = {{"version", 1}, {"context", payload.at("resolved_context")}};
        candidate.entity.extensions[kMeasuredGraph] = {{"version", 1},
            {"source_ids", payload.at("dependency_graph").at("measurement_graph_ids")}};
        // The exact graph codec selects V6 by its admission marker.
        candidate.entity.extensions["vertex_dxf_boundary"] = boundary_group_marker({}, 6);
    }
    if (group) {
        if (!payload.at("member_ids").is_array() ||
            payload.at("dependency_graph") != (wall_source ? native_dxf_wall_source_dependency_graph(candidate.entity) :
                native_dxf_boundary_dependency_graph(candidate.entity)))
            throw std::invalid_argument("native boundary dependency graph differs");
        for (const auto& id : payload.at("member_ids")) {
            if (!id.is_string() || id.get_ref<const std::string&>().empty() || id.get_ref<const std::string&>().size() > 255)
                throw std::invalid_argument("invalid boundary group identity");
            candidate.member_ids.push_back(id.get<std::string>());
        }
        if (candidate.member_ids.empty() || !std::is_sorted(candidate.member_ids.begin(), candidate.member_ids.end()) ||
            std::adjacent_find(candidate.member_ids.begin(), candidate.member_ids.end()) != candidate.member_ids.end() ||
            !std::binary_search(candidate.member_ids.begin(), candidate.member_ids.end(), candidate.entity.id))
            throw std::invalid_argument("invalid boundary group membership");
    }
    return candidate;
}

bool same_block_geometry(const DxfBlock& a, const DxfBlock& b, bool exact = false) {
    const auto near = [exact](double x, double y) {
        return exact ? x == y : std::abs(x - y) <= kGeometryTolerance;
    };
    const auto point = [&](DxfPoint x, DxfPoint y) { return near(x.x, y.x) && near(x.y, y.y); };
    if (!a.labels.empty() || !b.labels.empty() || a.lines.size() != b.lines.size() ||
        a.arcs.size() != b.arcs.size() || a.polylines.size() != b.polylines.size() ||
        a.circles.size() != b.circles.size()) return false;
    for (std::size_t i = 0; i < a.lines.size(); ++i)
        if (!point(a.lines[i].start, b.lines[i].start) || !point(a.lines[i].end, b.lines[i].end) ||
            a.lines[i].layer != b.lines[i].layer) return false;
    for (std::size_t i = 0; i < a.arcs.size(); ++i)
        if (!point(a.arcs[i].center, b.arcs[i].center) || !near(a.arcs[i].radius, b.arcs[i].radius) ||
            !near(a.arcs[i].start_degrees, b.arcs[i].start_degrees) ||
            !near(a.arcs[i].end_degrees, b.arcs[i].end_degrees) || a.arcs[i].layer != b.arcs[i].layer) return false;
    for (std::size_t i = 0; i < a.circles.size(); ++i)
        if (!point(a.circles[i].center, b.circles[i].center) || !near(a.circles[i].radius, b.circles[i].radius) ||
            a.circles[i].layer != b.circles[i].layer) return false;
    for (std::size_t i = 0; i < a.polylines.size(); ++i) {
        const auto& x = a.polylines[i]; const auto& y = b.polylines[i];
        if (x.closed != y.closed || x.layer != y.layer || x.vertices.size() != y.vertices.size()) return false;
        for (std::size_t j = 0; j < x.vertices.size(); ++j)
            if (!point(x.vertices[j].point, y.vertices[j].point) || !near(x.vertices[j].bulge, y.vertices[j].bulge)) return false;
    }
    return true;
}

// Bindings to absent project scaffolding remain inert source evidence. Only
// boundary topology and wall/opening host relationships become active here.
Entity detached_native_entity(const Entity& source, const std::map<std::string, std::string>& ids,
                              bool proved_stair_floor) {
    const bool wall_source = wall_source_member(source);
    const auto stair_floor = wall_source ? wall_source_stair_floor(source) :
        proved_stair_floor ? native_dxf_boundary_stair_floor_source(source) : std::string{};
    Entity result = can_recognize_boundary_entity_type(source.type) && ids.at(source.id) != source.id
        ? remap_boundary_owner_identity(source, ids.at(source.id)) : source;
    result.id = ids.at(source.id);
    if (source.type == "measurement_linework" && result.id != source.id)
        result.properties["model"] = remap_measurement_linework_owner_identity(source.properties.at("model"), result.id);
    if (!result.extensions.contains("vertex_dxf_source"))
        result.extensions["vertex_dxf_source"] = {{"version", 1}, {"id", source.id},
            {"properties", source.properties}, {"extensions", source.extensions}};
    if (wall_source) {
        result.extensions[kWallSourceContextBinding] = {{"version", 1},
            {"source_context", direct_source_context(source)}, {"destination_context", nullptr}};
        if (measured_source_member(source)) {
            auto& binding = result.extensions[kWallSourceContextBinding];
            binding["version"] = 2;
            binding["source_resolved_context"] = resolved_context_json(member_resolved_context(source));
            binding["destination_resolved_context"] = nullptr;
            result.extensions.erase(kResolvedContext);
        }
        result.properties.erase("phase_id");
        result.properties.erase("layer");
        result.properties.erase("layer_name");
    }
    for (const auto* key : {"floor_id", "layer_id", "building_id", "property_id"})
        result.properties.erase(key);
    if (!stair_floor.empty()) {
        result.properties["appraisal_facts"]["ansi"]["ceiling"]["stair_from_floor_id"] = "";
        result.extensions["vertex_dxf_stair_floor_binding"] = {{"version", 1},
            {"source_floor_id", stair_floor}, {"destination_floor_id", nullptr}};
    }
    if (can_recognize_boundary_entity_type(source.type)) {
        // Legacy names are organizational bindings too. A reviewed desktop
        // destination must not be overridden on later export by a source name.
        result.properties.erase("layer");
        result.properties.erase("layer_name");
    } else {
        if (!wall_source) result.properties.erase("vertical_placement");
        for (const auto* key : {"level_connection", "material_assignment",
                               "wall_join_id", "room_id"}) result.properties.erase(key);
        if (result.properties.contains("layers") && result.properties.at("layers").is_array())
            for (auto& layer : result.properties["layers"])
                if (layer.is_object()) layer.erase("material_assignment");
    }
    if (source.type == "wall") {
        Wall decoded;
        std::string error;
        if (!read_document_wall(source, {}, decoded, error)) throw std::invalid_argument(error);
        result.properties["thickness_m"] = decoded.thickness;
        result.properties["height_m"] = decoded.height;
        result.properties["elevation_m"] = decoded.elevation;
        if (decoded.slope_rise) result.properties["slope_rise_m"] = *decoded.slope_rise;
        if (decoded.top_gradient_m_per_m)
            result.properties["top_plane"] = wall_top_plane_json(*decoded.top_gradient_m_per_m);
        for (const auto* key : {"thickness", "height", "elevation", "slope_rise"}) result.properties.erase(key);
    } else if (source.type == "opening") {
        if (!wall_source) result.properties["wall_id"] = ids.at(source.properties.at("wall_id").get<std::string>());
        for (const auto* key : {"offset", "width", "sill", "height"}) {
            const auto canonical = std::string(key) + "_m";
            if (!result.properties.contains(canonical) && result.properties.contains(key))
                result.properties[canonical] = result.properties.at(key);
            result.properties.erase(key);
        }
    }
    return result;
}

std::set<std::size_t> import_native_graphs(const DxfDrawing& drawing, bool source_is_metres,
                                         DxfProjectImportResult& result) {
    std::map<std::string, NativeCandidate> candidates;
    std::set<std::string> duplicate_ids;
    std::set<std::string> declared_ids;
    std::set<std::string> rejected_boundary_group_ids;
    std::set<std::size_t> activated;
    std::vector<Entity> original_measured_inventory;
    bool original_measured_context_incomplete = false;
    for (std::size_t i = 0; i < drawing.inserts.size(); ++i) {
        const auto& insert = drawing.inserts[i];
        const auto block = std::find_if(drawing.blocks.begin(), drawing.blocks.end(),
            [&](const auto& value) { return block_identity(value.name) == block_identity(insert.block_name); });
        if (block == drawing.blocks.end() || block->vertex_entity_json.empty()) continue;
        Json payload;
        try {
            payload = bounded_native_json(block->vertex_entity_json);
            // Inventory recoverable declarations before full admission. A
            // malformed graph or placement still conflicts with another copy.
            if (payload.is_object() && payload.contains("version") &&
                payload.at("version").is_number_integer() &&
                (payload.at("version") == 1 || payload.at("version") == 2 || payload.at("version") == 3 ||
                 payload.at("version") == 4 || payload.at("version") == 5 || payload.at("version") == 6) &&
                payload.contains("id") && payload.at("id").is_string()) {
                const auto id = payload.at("id").get<std::string>();
                if (!id.empty() && id.size() <= 255 && !declared_ids.insert(id).second)
                    duplicate_ids.insert(id);
            }
            // Recover the original source scope before full carrier admission.
            // A refused standalone stroke still participates in an unisolated
            // area's layer inventory; destination CAD-layer choices cannot
            // remove it from the source proof.
            if (payload.is_object() && ((payload.contains("version") && payload.at("version") == 6) ||
                (payload.contains("type") && payload.at("type") == "measurement_linework"))) {
                try {
                    Entity observation{payload.at("id").get<std::string>(), payload.at("type").get<std::string>(),
                        payload.at("properties"), false, payload.at("extensions")};
                    if (observation.id.empty() || observation.id.size() > 255 ||
                        !observation.properties.is_object() || !observation.extensions.is_object())
                        throw std::invalid_argument("incomplete original V6 inventory");
                    (void)read_resolved_context(payload.at("resolved_context"));
                    observation.extensions.erase("vertex_dxf_boundary");
                    observation.extensions.erase(kWallSourceContextBinding);
                    observation.extensions.erase(kMeasuredGraph);
                    observation.extensions[kResolvedContext] = {{"version", 1}, {"context", payload.at("resolved_context")}};
                    original_measured_inventory.push_back(std::move(observation));
                } catch (const std::exception&) {
                    original_measured_context_incomplete = true;
                }
            }
            auto candidate = decode_native_candidate(*block, i, payload);
            const auto source_id = candidate.entity.id;
            if (candidate.version == 2) {
                // A standalone record cannot supply its active appraisal links.
                // Recoverable incoming links still prevent a referenced V3 group
                // from being promoted as though that declaration did not exist.
                const auto dependencies = native_dxf_wall_source_dependency_ids(candidate.entity);
                rejected_boundary_group_ids.insert(dependencies.begin(), dependencies.end());
            }
            if (!source_is_metres || insert.insertion.x != 0 || insert.insertion.y != 0 ||
                insert.scale_x != 1 || insert.scale_y != 1 || insert.rotation_degrees != 0 ||
                block->base.x != 0 || block->base.y != 0)
                throw std::invalid_argument("native placement/units differs");
            if (!candidates.emplace(source_id, std::move(candidate)).second) duplicate_ids.insert(source_id);
        } catch (const std::exception& error) {
            // Unreadable native metadata cannot certify a complete measured
            // source inventory. V1..V5-only imports retain their prior path.
            if (payload.is_null()) original_measured_context_incomplete = true;
            if (payload.is_object() && payload.contains("version") &&
                payload.at("version").is_number_integer() &&
                (payload.at("version") == 1 || payload.at("version") == 2 || payload.at("version") == 3 || payload.at("version") == 4 || payload.at("version") == 5 || payload.at("version") == 6)) {
                const auto remember = [&](const Json& value) {
                    if (value.is_string()) {
                        const auto& id = value.get_ref<const std::string&>();
                        if (!id.empty() && id.size() <= 255) rejected_boundary_group_ids.insert(id);
                    }
                };
                const auto array = [&](const Json& object, const char* key) {
                    if (object.is_object() && object.contains(key) && object.at(key).is_array())
                        for (const auto& id : object.at(key)) remember(id);
                };
                if (payload.contains("id")) remember(payload.at("id"));
                if (payload.contains("extensions"))
                    for (const auto& id : recover_measured_source_ids(payload.at("extensions"))) rejected_boundary_group_ids.insert(id);
                array(payload, "member_ids");
                const auto references = [&](const Json& graph) {
                    array(graph, "deduction_ids"); array(graph, "below_5ft_deduction_ids");
                    array(graph, "wall_source_ids"); array(graph, "hosted_opening_ids");
                    array(graph, "measurement_source_ids"); array(graph, "measurement_graph_ids");
                    if (graph.is_object() && graph.contains("room_boundary_id")) remember(graph.at("room_boundary_id"));
                    if (graph.is_object() && graph.contains("wall_id")) remember(graph.at("wall_id"));
                };
                array(payload, "hosted_opening_ids");
                if (payload.contains("dependency_graph")) references(payload.at("dependency_graph"));
                if (payload.contains("properties") && payload.at("properties").is_object()) {
                    const auto& properties = payload.at("properties");
                    array(properties, "deduction_ids");
                    if (properties.contains("wall_id")) remember(properties.at("wall_id"));
                    const auto walls = properties.find("wall_measurement_source");
                    if (walls != properties.end() && walls->is_object() && walls->contains("walls") && walls->at("walls").is_array())
                        for (const auto& wall : walls->at("walls"))
                            if (wall.is_object() && wall.contains("id")) remember(wall.at("id"));
                    const auto facts = properties.find("appraisal_facts");
                    if (facts != properties.end() && facts->is_object()) {
                        const auto ansi = facts->find("ansi");
                        if (ansi != facts->end() && ansi->is_object() && ansi->contains("ceiling"))
                            references(ansi->at("ceiling"));
                    }
                }
            }
            diagnostic(result.diagnostics, block->name, "BLOCK",
                std::string_view(error.what()) == "manufactured plan geometry unavailable"
                    ? "manufactured_plan_geometry_unavailable" : "native_metadata_not_activated");
        }
    }
    std::set<std::string> allocated_ids;
    for (const auto& [id, candidate] : candidates) { (void)candidate; allocated_ids.insert(id); }
    std::set<std::string> processed_groups;
    NativeDxfWallSourceWorkBudget wall_source_budget;
    std::set<std::string> preflight_refused;
    wall_source_budget.measured_operation = std::any_of(candidates.begin(), candidates.end(),
        [](const auto& entry) { return entry.second.version == 6; });
    const auto with_original_inventory = [&](std::vector<Entity> source) {
        std::set<std::string> members;
        for (const auto& member : source) members.insert(member.id);
        if (original_measured_context_incomplete)
            throw std::invalid_argument("original V6 source context inventory incomplete");
        for (const auto& observation : original_measured_inventory)
            if (!members.contains(observation.id)) source.push_back(observation);
        return source;
    };
    if (wall_source_budget.measured_operation) {
        std::set<std::string> preflight_seen;
        for (const auto& [id, candidate] : candidates) {
            if (candidate.version < 5 || preflight_seen.contains(id)) continue;
            preflight_seen.insert(candidate.member_ids.begin(), candidate.member_ids.end());
            try {
                std::vector<Entity> source;
                for (const auto& member_id : candidate.member_ids) {
                    const auto& item = candidates.at(member_id);
                    if (item.version != candidate.version || item.member_ids != candidate.member_ids ||
                        duplicate_ids.contains(member_id) || rejected_boundary_group_ids.contains(member_id))
                        throw std::invalid_argument("partial V6 preflight graph");
                    auto entity = item.entity;
                    entity.extensions["vertex_dxf_boundary"] = boundary_group_marker(candidate.member_ids, candidate.version);
                    source.push_back(std::move(entity));
                }
                validate_native_dxf_wall_source_groups(candidate.version == 6 ? with_original_inventory(std::move(source)) : source,
                    &wall_source_budget, true);
            } catch (const std::exception&) {
                preflight_refused.insert(candidate.member_ids.begin(), candidate.member_ids.end());
            }
        }
    }
    for (const auto& [id, candidate] : candidates) {
        if ((candidate.version != 3 && candidate.version != 4 && candidate.version != 5 && candidate.version != 6) || processed_groups.contains(id)) continue;
        // Mark the attempted declaration, but conflicting members are still
        // checked below. No member is published until every proof succeeds.
        processed_groups.insert(candidate.member_ids.begin(), candidate.member_ids.end());
        try {
            if (preflight_refused.contains(id)) throw std::invalid_argument("native source operation preflight refused");
            std::vector<const NativeCandidate*> group;
            std::vector<Entity> source;
            std::map<std::string, std::string> ids;
            std::map<std::string, std::string, std::less<>> typed_ids;
            const auto marker = boundary_group_marker(candidate.member_ids, candidate.version);
            for (const auto& member_id : candidate.member_ids) {
                const auto found = candidates.find(member_id);
                if (found == candidates.end() || duplicate_ids.contains(member_id) ||
                    rejected_boundary_group_ids.contains(member_id) || found->second.version != candidate.version ||
                    found->second.member_ids != candidate.member_ids)
                    throw std::invalid_argument("partial or mismatched native boundary group");
                group.push_back(&found->second);
                auto entity = found->second.entity;
                entity.extensions["vertex_dxf_boundary"] = marker;
                source.push_back(std::move(entity));
                auto fresh = make_stable_id();
                while (!allocated_ids.insert(fresh).second) fresh = make_stable_id();
                ids.emplace(member_id, fresh); typed_ids.emplace(member_id, std::move(fresh));
            }
            // Any incoming V3/V4 declaration must share this exact complete group.
            for (const auto& [other_id, other] : candidates) {
                if (std::binary_search(candidate.member_ids.begin(), candidate.member_ids.end(), other_id)) continue;
                for (const auto& target : native_dxf_wall_source_dependency_ids(other.entity))
                    if (std::binary_search(candidate.member_ids.begin(), candidate.member_ids.end(), target))
                        throw std::invalid_argument("incoming native boundary graph differs");
                for (const auto& target : other.member_ids)
                    if (std::binary_search(candidate.member_ids.begin(), candidate.member_ids.end(), target))
                        throw std::invalid_argument("overlapping native boundary graph");
            }
            if (candidate.version >= 5)
                validate_native_dxf_wall_source_groups(candidate.version == 6 ? with_original_inventory(source) : source,
                    &wall_source_budget);
            else validate_native_dxf_boundary_groups(source);
            if (candidate.version >= 5) {
                for (const auto* item : group) if (item->entity.type == "wall") {
                    std::set<std::string> actual, expected(item->hosted_ids.begin(), item->hosted_ids.end());
                    for (const auto* child : group)
                        if (child->entity.type == "opening" && child->entity.properties.at("wall_id") == item->entity.id)
                            actual.insert(child->entity.id);
                    if (actual != expected) throw std::invalid_argument("partial V5 hosted opening declaration");
                }
            }
            std::vector<Entity> source_boundaries;
            for (const auto& entity : source) if (can_recognize_boundary_entity_type(entity.type)) source_boundaries.push_back(entity);
            if (candidate.version < 5) validate_boundary_group_source_contexts(source_boundaries);
            validate_boundary_group_ceiling_sources(source_boundaries);
            std::vector<Entity> detached;
            for (const auto& entity : source) {
                auto fresh = detached_native_entity(entity, ids, candidate.version >= 4);
                remap_native_dxf_boundary_dependency_ids(fresh, typed_ids);
                detached.push_back(std::move(fresh));
            }
            if (candidate.version >= 5) validate_native_dxf_wall_source_groups(detached, &wall_source_budget);
            else validate_native_dxf_boundary_groups(detached);
            const auto document = Document::create(detached).snapshot();
            if (!document.is_editable()) throw std::invalid_argument("native boundary group schema is not editable");
            for (std::size_t i = 0; i < group.size(); ++i) {
                const auto& item = *group[i];
                const auto& block = *item.block;
                const auto layer = !block.lines.empty() ? block.lines.front().layer :
                    !block.arcs.empty() ? block.arcs.front().layer :
                    !block.polylines.empty() ? block.polylines.front().layer : std::string("0");
                const auto& fresh = document.entities().at(ids.at(item.entity.id));
                std::vector<DxfProjectDiagnostic> geometry_diagnostics;
                if (candidate.version >= 5) {
                    const auto source_graph = wall_source_context_graph(source);
                    const auto& original = source_graph.at(item.entity.id);
                    const auto source_plan = source_plan_block(source_graph, original, block.name, layer, geometry_diagnostics);
                    if (!geometry_diagnostics.empty() || !same_block_geometry(block, source_plan, true))
                        throw std::invalid_argument("native V5 source plan differs");
                }
                const auto expected = source_plan_block(document.entities(), fresh, block.name, layer, geometry_diagnostics);
                if (!geometry_diagnostics.empty() || !same_block_geometry(block, expected, true))
                    throw std::invalid_argument("native group geometry differs");
                const auto effective_layer = layer == "0" ? drawing.inserts.at(item.insert_index).layer : layer;
                detached[i].extensions["dxf_source"] = source_extension(effective_layer, "INSERT");
            }
            for (const auto* item : group) activated.insert(item->insert_index);
            result.entities.insert(result.entities.end(), detached.begin(), detached.end());
            for (const auto* item : group) {
                const auto& properties = item->entity.properties;
                const bool sloped = properties.contains("appraisal_facts") &&
                    properties.at("appraisal_facts").contains("ansi") &&
                    properties.at("appraisal_facts").at("ansi").contains("ceiling") &&
                    properties.at("appraisal_facts").at("ansi").at("ceiling").value("kind", std::string{}) == "sloped";
                if (candidate.version >= 5 && properties.contains("phase_id"))
                    diagnostic(result.diagnostics, item->entity.id, item->entity.type, "source_phase_not_transported");
                if (sloped || properties.contains("appraisal_reporting"))
                    diagnostic(result.diagnostics, item->entity.id, item->entity.type, "source_confirmation_required");
            }
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, id, candidate.entity.type, "native_boundary_group_not_activated");
        }
    }
    for (const auto& [id, candidate] : candidates) {
        if (!can_recognize_boundary_entity_type(candidate.entity.type) || candidate.version == 3 || candidate.version == 4 || candidate.version == 5 || candidate.version == 6) continue;
        try {
            if (duplicate_ids.contains(id) || rejected_boundary_group_ids.contains(id) || processed_groups.contains(id))
                throw std::invalid_argument("conflicting native identity declaration");
            auto fresh = make_stable_id();
            while (!allocated_ids.insert(fresh).second) fresh = make_stable_id();
            auto entity = detached_native_entity(candidate.entity, {{id, fresh}});
            // Missing dependent graphs are never removed to force admission.
            // Document validation and a regenerated complete plan prove this
            // detached boundary before any candidate is published.
            const auto document = Document::create({entity}).snapshot();
            if (!document.is_editable()) throw std::invalid_argument("native boundary schema is not editable");
            const auto& block = *candidate.block;
            const auto layer = !block.lines.empty() ? block.lines.front().layer :
                !block.arcs.empty() ? block.arcs.front().layer :
                !block.polylines.empty() ? block.polylines.front().layer : std::string("0");
            const auto expected = boundary_plan_block(document.entities().at(fresh), block.name, layer);
            if (!same_block_geometry(block, expected, true))
                throw std::invalid_argument("native boundary geometry differs");
            const auto effective_layer = layer == "0" ? drawing.inserts.at(candidate.insert_index).layer : layer;
            entity.extensions["dxf_source"] = source_extension(effective_layer, "INSERT");
            entity.extensions["vertex_dxf_boundary"] = {{"version", 2}, {"depiction", kBoundaryDepiction}};
            result.entities.push_back(std::move(entity));
            activated.insert(candidate.insert_index);
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, id, candidate.entity.type, "native_boundary_not_activated");
        }
    }
    for (const auto& [id, wall] : candidates) {
        if (wall.entity.type != "wall" || wall.version >= 5) continue;
        try {
            std::vector<const NativeCandidate*> group{&wall};
            std::set<std::string> expected(wall.hosted_ids.begin(), wall.hosted_ids.end()), actual;
            for (const auto& [child_id, child] : candidates) {
                if (child.entity.type == "opening" && child.entity.properties.value("wall_id", std::string{}) == id) {
                    actual.insert(child_id); group.push_back(&child);
                }
            }
            if (expected != actual) throw std::invalid_argument("partial native host graph");
            std::map<std::string, std::string> ids;
            std::vector<Entity> detached;
            for (const auto* item : group) {
                if (duplicate_ids.contains(item->entity.id) || rejected_boundary_group_ids.contains(item->entity.id) ||
                    processed_groups.contains(item->entity.id))
                    throw std::invalid_argument("conflicting native identity declaration");
                auto fresh = make_stable_id();
                while (!allocated_ids.insert(fresh).second) fresh = make_stable_id();
                ids.emplace(item->entity.id, std::move(fresh));
            }
            for (const auto* item : group) {
                auto entity = detached_native_entity(item->entity, ids);
                entity.extensions["dxf_source"] = source_extension(drawing.inserts.at(item->insert_index).layer, "INSERT");
                detached.push_back(std::move(entity));
            }
            const auto document = Document::create(detached).snapshot();
            for (const auto* item : group) {
                const auto& block = *item->block;
                const auto layer = !block.lines.empty() ? block.lines.front().layer :
                    !block.arcs.empty() ? block.arcs.front().layer :
                    !block.polylines.empty() ? block.polylines.front().layer : std::string("0");
                std::vector<DxfProjectDiagnostic> geometry_diagnostics;
                const auto expected_block = architectural_block(document, document.entities().at(ids.at(item->entity.id)),
                    block.name, layer, geometry_diagnostics);
                if (!geometry_diagnostics.empty() || !same_block_geometry(block, expected_block))
                    throw std::invalid_argument("native geometry differs");
            }
            for (const auto* item : group) activated.insert(item->insert_index);
            result.entities.insert(result.entities.end(), detached.begin(), detached.end());
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, id, "wall", "native_host_graph_not_activated");
        }
    }
    for (const auto& [id, candidate] : candidates)
        if (!activated.contains(candidate.insert_index))
            diagnostic(result.diagnostics, id, candidate.entity.type, "native_metadata_visual_fallback");
    return activated;
}

void normalize_drawing_to_metres(DxfDrawing& drawing, double factor) {
    const auto length = [factor](double& value) {
        value *= factor;
        if (!std::isfinite(value))
            throw std::invalid_argument("DXF source-unit conversion exceeds finite metre range");
    };
    const auto point = [&](DxfPoint& value) { length(value.x); length(value.y); };
    // The direct drawing and each block share these primitive families. Only
    // their linear fields change; bulges, rotations and insert scales do not.
    const auto primitives = [&](auto& contents) {
        for (auto& line : contents.lines) { point(line.start); point(line.end); }
        for (auto& arc : contents.arcs) { point(arc.center); length(arc.radius); }
        for (auto& circle : contents.circles) { point(circle.center); length(circle.radius); }
        for (auto& polyline : contents.polylines)
            for (auto& vertex : polyline.vertices) point(vertex.point);
        for (auto& label : contents.labels) { point(label.position); length(label.height); }
    };
    primitives(drawing);
    for (auto& dimension : drawing.dimensions) {
        point(dimension.extension_start);
        point(dimension.extension_end);
        point(dimension.dimension_line);
        point(dimension.text_position);
        length(dimension.text_height);
    }
    for (auto& dimension : drawing.arc_dimensions) {
        point(dimension.extension_start);
        point(dimension.extension_end);
        point(dimension.center);
        point(dimension.dimension_arc);
        point(dimension.text_position);
        length(dimension.text_height);
    }
    for (auto& dimension : drawing.angular_dimensions) {
        point(dimension.extension_start);
        point(dimension.extension_end);
        point(dimension.vertex);
        point(dimension.dimension_arc);
        point(dimension.text_position);
        length(dimension.text_height);
    }
    for (auto& hatch : drawing.hatches)
        for (auto& vertex : hatch.boundary) point(vertex);
    for (auto& block : drawing.blocks) { point(block.base); primitives(block); }
    for (auto& insert : drawing.inserts) point(insert.insertion);
    drawing.insertion_units = 6;
}

void preflight_project_expansion(const DxfDrawing& drawing, const DxfExchangeLimits& limits) {
    // Transport budgets bound stored definitions, not their INSERT expansion.
    // The mapper separately caps work records (including annotation children)
    // and analytical geometry: one line/arc segment, two circle segments,
    // polyline/hatch vertices, label anchors and four/five dimension anchors.
    struct Work {
        std::size_t records{};
        std::size_t geometry{};
        bool annotations{};
    };
    const auto add = [](std::size_t& total, std::size_t amount, std::size_t cap) {
        if (amount > cap - total)
            throw std::invalid_argument("dxf_project_expansion_limit_exceeded");
        total += amount;
    };
    const auto primitives = [&](const auto& contents) {
        Work work;
        const auto raw_cap = std::numeric_limits<std::size_t>::max();
        for (const auto count : {contents.lines.size(), contents.arcs.size(), contents.circles.size(),
                                 contents.polylines.size(), contents.labels.size()})
            add(work.records, count, raw_cap);
        add(work.geometry, contents.lines.size(), raw_cap);
        add(work.geometry, contents.arcs.size(), raw_cap);
        add(work.geometry, contents.circles.size(), raw_cap);
        add(work.geometry, contents.circles.size(), raw_cap);
        for (const auto& polyline : contents.polylines)
            add(work.geometry, polyline.vertices.size(), raw_cap);
        add(work.geometry, contents.labels.size(), raw_cap);
        work.annotations = !contents.labels.empty();
        return work;
    };
    Work total;
    const auto append = [&](const Work& work) {
        add(total.records, work.records, limits.max_entities);
        add(total.geometry, work.geometry, limits.max_vertices);
        total.annotations = total.annotations || work.annotations;
    };
    append(primitives(drawing));
    // Dimensions produce both a boundary candidate and an annotation child.
    for (const auto& dimension : drawing.dimensions) {
        (void)dimension;
        append({2, 4, true});
    }
    for (const auto& dimension : drawing.arc_dimensions) {
        (void)dimension;
        append({2, 5, true});
    }
    for (const auto& dimension : drawing.angular_dimensions) {
        (void)dimension;
        append({2, 5, true});
    }
    for (const auto& hatch : drawing.hatches)
        append({1, hatch.boundary.size(), false});

    std::map<std::string, Work, std::less<>> blocks;
    for (const auto& block : drawing.blocks) {
        auto work = primitives(block);
        if (!block.vertex_entity_json.empty()) {
            // Reserve activation as well as fallback before inspecting native
            // metadata. Generated plan parity is still checked independently.
            add(work.records, 1, std::numeric_limits<std::size_t>::max());
            add(work.geometry, 1, std::numeric_limits<std::size_t>::max());
        }
        blocks.emplace(block_identity(block.name), work);
    }
    for (const auto& insert : drawing.inserts) {
        const auto block = blocks.find(block_identity(insert.block_name));
        if (block == blocks.end()) throw std::invalid_argument("invalid_or_excessive_dxf");
        append(block->second);
    }
    // Children share one native annotation-state container.
    if (total.annotations) add(total.records, 1, limits.max_entities);
}

} // namespace

Json native_dxf_wall_source_dependency_graph(const Entity& entity) {
    const bool boundary = can_recognize_boundary_entity_type(entity.type);
    auto graph = boundary ? native_dxf_boundary_dependency_graph(entity) :
        native_dxf_boundary_dependency_graph(Entity{"dependencies", "boundary", Json::object(), false, Json::object()});
    graph["wall_source_ids"] = Json::array();
    graph["wall_id"] = "";
    if (entity.properties.contains("wall_measurement_source")) {
        auto ids = exterior_wall_measurement_source_ids(entity);
        std::sort(ids.begin(), ids.end());
        graph["wall_source_ids"] = ids;
    }
    if (entity.type == "opening") {
        const auto host = entity.properties.find("wall_id");
        if (host == entity.properties.end() || !host->is_string() ||
            host->get_ref<const std::string&>().empty() || host->get_ref<const std::string&>().size() > 255)
            throw std::invalid_argument("invalid native wall-source opening host");
        graph["wall_id"] = *host;
    }
    if (measured_source_member(entity)) {
        graph["measurement_source_ids"] = measurement_linework_source_ids_for_admission(entity);
        const auto& declaration = entity.extensions.at(kMeasuredGraph);
        if (!declaration.is_object() || declaration.size() != 2 || !declaration.at("version").is_number_integer() ||
            declaration.at("version") != 1 || !declaration.at("source_ids").is_array())
            throw std::invalid_argument("invalid V6 measured graph declaration");
        graph["measurement_graph_ids"] = declaration.at("source_ids");
    }
    return graph;
}

std::vector<std::string> native_dxf_wall_source_dependency_ids(const Entity& entity) {
    const auto graph = native_dxf_wall_source_dependency_graph(entity);
    std::set<std::string> ids;
    for (const auto* key : {"deduction_ids", "below_5ft_deduction_ids", "wall_source_ids"})
        for (const auto& value : graph.at(key)) ids.insert(value.get<std::string>());
    for (const auto* key : {"room_boundary_id", "wall_id"})
        if (graph.at(key) != "") ids.insert(graph.at(key).get<std::string>());
    for (const auto& id : measurement_linework_source_ids_for_admission(entity)) ids.insert(id);
    if (measured_source_member(entity))
        for (const auto& id : graph.at("measurement_graph_ids")) {
            if (!id.is_string() || id.get_ref<const std::string&>().empty() || id.get_ref<const std::string&>().size() > 255)
                throw std::invalid_argument("invalid V6 measured graph owner");
            ids.insert(id.get<std::string>());
        }
    // These are recoverable typed incoming physical-room references only.
    // Reaching such an owner rejects V5; its unsupported descriptor never gains
    // transport authority and no arbitrary nested owner_id search is used.
    const auto physical = entity.extensions.find("physical_wall_room");
    if (physical != entity.extensions.end() && physical->is_object()) {
        const auto remember = [&](const Json& record, const char* key) {
            const auto value = record.find(key);
            if (value != record.end() && value->is_string() && !value->get_ref<const std::string&>().empty() &&
                value->get_ref<const std::string&>().size() <= 255) ids.insert(value->get<std::string>());
        };
        remember(*physical, "selected_wall_id");
        const auto lineage = physical->find("source_lineage");
        if (lineage != physical->end() && lineage->is_object()) {
            const auto sources = lineage->find("physical_sources");
            if (sources != lineage->end() && sources->is_array())
                for (const auto& source : *sources) if (source.is_object()) remember(source, "owner_id");
        }
    }
    return {ids.begin(), ids.end()};
}

void validate_native_dxf_wall_source_member(const Entity& entity) {
    if (!wall_source_member(entity)) throw std::invalid_argument("V5 marker required");
    const auto& marker = entity.extensions.at("vertex_dxf_boundary");
    if (!marker.at("version").is_number_integer() || marker.size() != 3 ||
        marker.value("depiction", std::string{}) != kBoundaryDepiction ||
        !marker.contains("member_ids") || !marker.at("member_ids").is_array() ||
        marker.at("member_ids").empty() || marker.at("member_ids").size() > 4096 ||
        (entity.type != "boundary" && entity.type != "measurement_boundary" &&
         entity.type != "wall" && entity.type != "opening" &&
         !(measured_source_member(entity) && entity.type == "measurement_linework")) || !entity.properties.is_object())
        throw std::invalid_argument("invalid V5 member schema");
    std::vector<std::string> members;
    for (const auto& value : marker.at("member_ids")) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
            value.get_ref<const std::string&>().size() > 255)
            throw std::invalid_argument("invalid V5 member identity");
        members.push_back(value.get<std::string>());
    }
    if (!std::is_sorted(members.begin(), members.end()) ||
        std::adjacent_find(members.begin(), members.end()) != members.end() ||
        !std::binary_search(members.begin(), members.end(), entity.id))
        throw std::invalid_argument("invalid V5 membership");
    for (const auto* key : {"physical_wall_room", "measurement_linework_sources", "measurement_linework_group"})
        if (entity.extensions.contains(key) && (std::string_view(key) == "physical_wall_room" || !measured_source_member(entity)))
            throw std::invalid_argument("untransported native source graph");
    if (!measured_source_member(entity) && (entity.extensions.contains(kMeasuredGraph) || entity.extensions.contains(kResolvedContext)))
        throw std::invalid_argument("V5 cannot carry V6 admission state");
    if (measured_source_member(entity)) {
        const auto graph = native_dxf_wall_source_dependency_graph(entity);
        std::vector<std::string> inventory;
        for (const auto& id : graph.at("measurement_graph_ids")) {
            if (!id.is_string() || id.get_ref<const std::string&>().empty() || id.get_ref<const std::string&>().size() > 255)
                throw std::invalid_argument("invalid V6 inventory identity");
            inventory.push_back(id.get<std::string>());
        }
        if (inventory.size() > 4096 || !std::is_sorted(inventory.begin(), inventory.end()) ||
            std::adjacent_find(inventory.begin(), inventory.end()) != inventory.end() ||
            (measurement_linework_source_ids_for_admission(entity).empty() && !inventory.empty()))
            throw std::invalid_argument("invalid V6 inventory");
        const auto resolved = member_resolved_context(entity);
        const auto direct = direct_source_context(entity);
        const auto resolved_json = resolved_context_json(resolved);
        for (const auto& [key, value] : direct.items())
            if (resolved_json.contains(key) && resolved_json.at(key) != value)
                throw std::invalid_argument("V6 context contradicts direct placement");
        (void)measurement_linework_copy_isolated(entity);
    }
    for (const auto* key : {"parent_id", "room_id", "wall_join_id", "level_connection",
                           "material_assignment", "assembly_catalog_id", "refs", "references"})
        if (entity.properties.contains(key)) throw std::invalid_argument("untransported V5 dependency");
    for (const auto* key : {"boundary_id", "opening_id", "slab_id", "roof_id", "stair_id", "sheet_id", "view_id",
                           "constraint_id", "label_id", "column_id", "beam_id", "railing_id", "host_id", "target_id",
                           "entity_id", "source_entity_id", "assembly_catalog_id", "room_id", "wall_id", "parent_id"}) {
        if (std::string_view(key) == "wall_id" && entity.type == "opening") continue;
        if (entity.properties.contains(key) || entity.properties.contains(std::string(key) + "s"))
            throw std::invalid_argument("untransported V5 owner reference");
    }
    if (const auto placement = entity.properties.find("vertical_placement"); placement != entity.properties.end()) {
        if (!placement->is_object() || placement->size() != 3 || !placement->contains("version") ||
            !placement->at("version").is_number_integer() || placement->at("version") != 1 ||
            !placement->contains("mode") || placement->at("mode") != "absolute" ||
            !placement->contains("offset_m") || !placement->at("offset_m").is_number() ||
            !std::isfinite(placement->at("offset_m").get<double>()) ||
            std::abs(placement->at("offset_m").get<double>()) > 1e9)
            throw std::invalid_argument("V5 only transports absolute vertical placement");
    }
    if (entity.properties.contains("layers") && entity.properties.at("layers").is_array())
        for (const auto& layer : entity.properties.at("layers"))
            if (layer.is_object() && layer.contains("material_assignment"))
                throw std::invalid_argument("untransported V5 material dependency");
    if (entity.type == "wall" || entity.type == "opening" || entity.type == "measurement_linework")
        for (const auto* key : {"deduction_ids", "appraisal_facts", "wall_measurement_source"})
            if (entity.properties.contains(key)) throw std::invalid_argument("invalid V5 architecture relationship");
    const auto hosted = entity.extensions.find(kWallSourceHostedOpenings);
    if (entity.type == "wall") {
        if (hosted == entity.extensions.end() || !hosted->is_object() || hosted->size() != 2 ||
            !hosted->contains("version") || !hosted->at("version").is_number_integer() || hosted->at("version") != 1 ||
            !hosted->contains("opening_ids") || !hosted->at("opening_ids").is_array() || hosted->at("opening_ids").size() > 4096)
            throw std::invalid_argument("V5 hosted opening declaration required");
        std::vector<std::string> ids;
        for (const auto& value : hosted->at("opening_ids")) {
            if (!value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size() > 255)
                throw std::invalid_argument("invalid V5 hosted opening identity");
            ids.push_back(value.get<std::string>());
        }
        if (!std::is_sorted(ids.begin(), ids.end()) || std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            throw std::invalid_argument("invalid V5 hosted opening order");
    } else if (hosted != entity.extensions.end()) throw std::invalid_argument("V5 only walls declare hosted openings");
    (void)native_dxf_wall_source_dependency_graph(entity);
    (void)wall_source_stair_floor(entity);
    const auto binding = entity.extensions.find(kWallSourceContextBinding);
    if (binding == entity.extensions.end()) { (void)direct_source_context(entity); return; }
    if (!binding->is_object() || binding->size() != (measured_source_member(entity) ? 5 : 3) || !binding->contains("version") ||
        !binding->at("version").is_number_integer() || binding->at("version") != (measured_source_member(entity) ? 2 : 1) ||
        !binding->contains("source_context") || !binding->contains("destination_context"))
        throw std::invalid_argument("invalid V5 context binding");
    validate_direct_context(binding->at("source_context"));
    if (measured_source_member(entity)) {
        if (entity.extensions.contains(kResolvedContext)) throw std::invalid_argument("V6 pending resolved observation must detach");
        const auto resolved = resolved_context_json(read_resolved_context(binding->at("source_resolved_context")));
        for (const auto& [key, value] : binding->at("source_context").items())
            if (resolved.contains(key) && resolved.at(key) != value) throw std::invalid_argument("V6 source context contradiction");
        if (binding->at("destination_context").is_null() != binding->at("destination_resolved_context").is_null())
            throw std::invalid_argument("V6 resolved binding state differs");
    }
    const auto& destination = binding->at("destination_context");
    if (destination.is_null()) {
        if (!direct_source_context(entity).empty()) throw std::invalid_argument("pending V5 context must detach");
    } else {
        validate_direct_context(destination);
        if (destination.contains("phase_id") || direct_source_context(entity) != destination ||
            !destination.contains("floor_id") || !destination.contains("layer_id"))
            throw std::invalid_argument("bound V5 direct context differs");
    }
    if (entity.properties.contains("layer") || entity.properties.contains("layer_name"))
        throw std::invalid_argument("V5 legacy layer must detach");
    const auto source = entity.extensions.find("vertex_dxf_source");
    if (source == entity.extensions.end() || !source->is_object() || !source->contains("properties") ||
        !source->at("properties").is_object()) throw std::invalid_argument("V5 context source evidence missing");
    // vertex_dxf_source may be older retained provenance after a reviewed
    // re-transfer. The dedicated source_context is the immediate observation;
    // complete raw graph currentness proves it without refreshing older evidence.
}

void remap_native_dxf_wall_source_dependency_ids(Entity& entity,
    const std::map<std::string, std::string, std::less<>>& ids) {
    // The caller already changed owner identity; membership still names old IDs.
    if (!wall_source_member(entity)) throw std::invalid_argument("V5 remapping requires marker");
    auto result = entity;
    std::vector<std::string> members;
    for (const auto& id : result.extensions.at("vertex_dxf_boundary").at("member_ids"))
        members.push_back(ids.at(id.get<std::string>()));
    std::sort(members.begin(), members.end());
    result.extensions["vertex_dxf_boundary"]["member_ids"] = members;
    if (measured_source_member(result)) {
        std::vector<std::string> inventory;
        for (const auto& id : result.extensions.at(kMeasuredGraph).at("source_ids"))
            inventory.push_back(ids.at(id.get<std::string>()));
        std::sort(inventory.begin(), inventory.end());
        result.extensions[kMeasuredGraph]["source_ids"] = inventory;
        result = remap_measurement_linework_source_references(result, ids);
    }
    if (result.type == "wall") {
        std::vector<std::string> hosted;
        for (const auto& id : result.extensions.at(kWallSourceHostedOpenings).at("opening_ids"))
            hosted.push_back(ids.at(id.get<std::string>()));
        std::sort(hosted.begin(), hosted.end());
        result.extensions[kWallSourceHostedOpenings]["opening_ids"] = hosted;
    }
    const auto replace = [&](Json& object, const char* key, bool array) {
        if (!object.contains(key)) return;
        const auto remap = [&](Json& value) { if (value != "") value = ids.at(value.get<std::string>()); };
        if (array) for (auto& value : object.at(key)) remap(value); else remap(object.at(key));
    };
    replace(result.properties, "deduction_ids", true);
    if (result.properties.contains("appraisal_facts") && result.properties.at("appraisal_facts").contains("ansi") &&
        result.properties.at("appraisal_facts").at("ansi").contains("ceiling")) {
        auto& ceiling = result.properties["appraisal_facts"]["ansi"]["ceiling"];
        replace(ceiling, "below_5ft_deduction_ids", true);
        replace(ceiling, "room_boundary_id", false);
        if (ceiling.value("kind", std::string{}) == "sloped") {
            ceiling["room_boundary_id"] = "";
            ceiling["complete_room_observed"] = false;
        }
    }
    if (result.type == "opening") replace(result.properties, "wall_id", false);
    if (result.properties.contains("wall_measurement_source")) {
        std::map<std::string, std::string, std::less<>> unchanged_contexts;
        for (const auto& record : result.properties.at("wall_measurement_source").at("walls"))
            for (const auto& [key, value] : record.at("context").items()) {
                (void)key; const auto id = value.get<std::string>(); unchanged_contexts.emplace(id, id);
            }
        result = remap_exterior_wall_measurement_source_references(result, ids, unchanged_contexts);
    }
    entity = std::move(result);
}

void validate_native_dxf_wall_source_groups(const std::vector<Entity>& entities,
    NativeDxfWallSourceWorkBudget* work_budget, bool preflight_only) {
    std::map<std::string, const Entity*, std::less<>> owners;
    for (const auto& entity : entities)
        if (!owners.emplace(entity.id, &entity).second) throw std::invalid_argument("duplicate imported identity");
    std::set<std::string> checked;
    struct PendingSourceCheck {
        std::map<std::string, Entity, std::less<>> graph;
        std::map<std::string, Entity, std::less<>> measured_inventory;
        std::vector<Entity> boundaries;
        std::map<std::string, DrawingContext, std::less<>> contexts;
        bool measured{};
    };
    std::vector<PendingSourceCheck> pending_checks;
    // A response may contain many independent components. Charge all of them
    // before any containment or exterior-source replay, rather than granting
    // each component a fresh expensive-work allowance.
    std::size_t segments = work_budget ? work_budget->segments : 0;
    std::size_t source_work = work_budget ? work_budget->source_work : 0;
    if (segments > 50'000 || source_work > 250'000)
        throw std::invalid_argument("V5 cumulative work limit");
    const bool measured_operation = std::any_of(entities.begin(), entities.end(), measured_source_member);
    if (measured_operation || (work_budget && work_budget->measured_operation)) {
        // Charge raw topology pairs for every component before any identified
        // boundary decoder can run. The multiplier covers source/detached
        // decoding, plans, identity remapping and editable-document admission.
        for (const auto& entity : entities) if (wall_source_member(entity) &&
            can_recognize_boundary_entity_type(entity.type)) {
            const auto count = raw_boundary_segment_count(entity);
            const auto work = count * count * 16;
            if (work > 250'000 - source_work) throw std::invalid_argument("V6 cumulative topology work limit");
            source_work += work;
            if (work_budget) work_budget->source_work = source_work;
        }
    }
    std::map<std::string, Entity, std::less<>> complete_measured_inventory;
    std::map<std::string, DrawingContext, std::less<>> complete_contexts;
    std::map<std::string, DrawingContext, std::less<>> source_layers;
    if (measured_operation) {
        std::vector<Entity> observed;
        for (const auto& entity : entities) if (measured_source_member(entity) ||
            entity.type == "measurement_linework" || entity.extensions.contains(kResolvedContext)) {
            const auto context = member_resolved_context(entity);
            const auto [layer, inserted] = source_layers.emplace(context.layer_id, context);
            if (!inserted && layer->second != context)
                throw std::invalid_argument("V6 source contexts disagree within one drawing layer");
            const auto direct = direct_source_context(entity);
            const auto resolved = resolved_context_json(context);
            for (const auto& [key, value] : direct.items())
                if (resolved.contains(key) && resolved.at(key) != value)
                    throw std::invalid_argument("V6 original inventory contradicts direct placement");
            complete_contexts.emplace(entity.id, context);
            observed.push_back(entity);
        }
        complete_measured_inventory = wall_source_context_graph(observed);
    }
    for (const auto& root : entities) {
        if (!wall_source_member(root) || checked.contains(root.id)) continue;
        validate_native_dxf_wall_source_member(root);
        const auto& marker = root.extensions.at("vertex_dxf_boundary");
        std::vector<Entity> members;
        for (const auto& id : marker.at("member_ids")) {
            const auto found = owners.find(id.get<std::string>());
            if (found == owners.end() || checked.contains(found->first) ||
                !wall_source_member(*found->second) || found->second->extensions.at("vertex_dxf_boundary") != marker)
                throw std::invalid_argument("partial or overlapping V5 graph");
            validate_native_dxf_wall_source_member(*found->second);
            members.push_back(*found->second);
        }
        const auto graph = wall_source_context_graph(members);
        const bool measured = measured_source_member(root);
        std::map<std::string, DrawingContext, std::less<>> contexts;
        if (measured) contexts = complete_contexts;
        std::map<std::string, std::set<std::string>> adjacency;
        std::map<std::string, std::vector<std::string>> deductions;
        std::map<std::string, std::size_t> incoming;
        std::vector<Entity> boundaries;
        bool live_source = false;
        std::size_t measured_segments = 0;
        std::size_t measured_replay_work = 0;
        std::optional<int> binding_state;
        for (const auto& member : members) {
            const auto binding = member.extensions.find(kWallSourceContextBinding);
            const int state = binding == member.extensions.end() ? 0 : binding->at("destination_context").is_null() ? 1 : 2;
            if (binding_state && *binding_state != state) throw std::invalid_argument("mixed V5 binding state");
            binding_state = state;
            incoming.try_emplace(member.id, 0);
            const auto dependencies = native_dxf_wall_source_dependency_graph(member);
            for (const auto& id : native_dxf_wall_source_dependency_ids(member)) {
                if (!graph.contains(id)) throw std::invalid_argument("missing V5 dependency");
                adjacency[member.id].insert(id); adjacency[id].insert(member.id);
            }
            if (member.type == "wall") {
                std::set<std::string> expected, actual;
                for (const auto& id : member.extensions.at(kWallSourceHostedOpenings).at("opening_ids"))
                    expected.insert(id.get<std::string>());
                for (const auto& [id, child] : graph)
                    if (child.type == "opening" && child.properties.at("wall_id") == member.id) actual.insert(id);
                if (actual != expected) throw std::invalid_argument("partial V5 hosted opening closure");
                Wall wall; std::string error;
                if (!read_document_wall(member, {}, wall, error)) throw std::invalid_argument(error);
                if (segments >= 50'000) throw std::invalid_argument("V5 geometry limit");
                ++segments;
            } else if (member.type == "opening") {
                const auto& host = graph.at(dependencies.at("wall_id").get<std::string>());
                if (host.type != "wall") throw std::invalid_argument("V5 opening host type differs");
                const auto child_context = direct_source_context(graph.at(member.id));
                const auto host_context = direct_source_context(host);
                if (child_context.contains("floor_id") && (!host_context.contains("floor_id") ||
                    child_context.at("floor_id") != host_context.at("floor_id")))
                    throw std::invalid_argument("V5 opening host floor differs");
            } else if (member.type == "measurement_linework") {
                const auto [count, replay_work] = raw_measured_stroke_work(member);
                // Includes decode's replay, exact plans, raw owner remapping and
                // editable-document admission between source/detached proofs.
                if (segments > 50'000 - count || replay_work > (250'000 - source_work) / 16)
                    throw std::invalid_argument("V6 cumulative replay work limit");
                segments += count; measured_segments += count;
                source_work += replay_work * 16;
                measured_replay_work += replay_work * 16;
            } else {
                auto boundary = read_entity_boundary_for_admission(member);
                if (!boundary) throw std::invalid_argument("V5 boundary geometry missing");
                std::size_t count = boundary->size();
                if (const auto holes = member.properties.find("holes"); holes != member.properties.end()) {
                    if (!holes->is_array()) throw std::invalid_argument("V5 holes must be an array");
                    for (const auto& value : *holes) {
                        const auto hole = read_boundary_value(value);
                        if (!hole || hole->size() > 512 || count > 512 - hole->size())
                            throw std::invalid_argument("V5 boundary geometry limit");
                        count += hole->size();
                    }
                }
                if (count > 512 || segments > 50'000 - count)
                    throw std::invalid_argument("V5 boundary geometry limit");
                segments += count;
                auto proof_boundary = graph.at(member.id);
                if (measured) {
                    const auto context = resolved_context_json(contexts.at(member.id));
                    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
                        proof_boundary.properties[key] = context.at(key);
                    const auto expected = measured_graph_ids(member, complete_measured_inventory, contexts);
                    if (dependencies.at("measurement_graph_ids") != Json(expected))
                        throw std::invalid_argument("partial V6 applicable measured graph");
                    for (const auto& source_id : dependencies.at("measurement_source_ids"))
                        if (!graph.contains(source_id.get<std::string>()) || graph.at(source_id.get<std::string>()).type != "measurement_linework")
                            throw std::invalid_argument("V6 measured source owner missing or type differs");
                    if (!expected.empty()) live_source = true;
                }
                boundaries.push_back(std::move(proof_boundary));
                if (dependencies.at("room_boundary_id") != "" && dependencies.at("room_boundary_id") != member.id)
                    throw std::invalid_argument("V5 ceiling owner differs");
                for (const auto& id : dependencies.at("deduction_ids")) {
                    const auto target = id.get<std::string>();
                    if (graph.at(target).type != "boundary" && graph.at(target).type != "measurement_boundary")
                        throw std::invalid_argument("V5 deduction type differs");
                    deductions[member.id].push_back(target); ++incoming[target];
                }
                for (const auto& id : dependencies.at("below_5ft_deduction_ids")) {
                    const auto& child = graph.at(id.get<std::string>());
                    if (std::find(dependencies.at("deduction_ids").begin(), dependencies.at("deduction_ids").end(), id) ==
                            dependencies.at("deduction_ids").end() || !child.properties.contains("appraisal_facts") ||
                        child.properties.at("appraisal_facts").value("boundary_role", std::string{}) != "other_void" ||
                        !native_dxf_boundary_dependency_graph(child).at("deduction_ids").empty())
                        throw std::invalid_argument("V5 ceiling exclusion must be an other-void deduction leaf");
                }
                const auto source_count = dependencies.at("wall_source_ids").size();
                if (source_count) {
                    const auto& source = member.properties.at("wall_measurement_source");
                    const auto passes = source.at("version") == 2 ? source.at("translations").size() + 2 : 2;
                    if (source_count > 2048 || passes > 250'000 ||
                        source_count * source_count > (250'000 - source_work) / passes)
                        throw std::invalid_argument("V5 wall-source work limit");
                    source_work += source_count * source_count * passes;
                    for (const auto& id : dependencies.at("wall_source_ids"))
                        if (graph.at(id.get<std::string>()).type != "wall")
                            throw std::invalid_argument("V5 source wall type differs");
                    live_source = true;
                }
            }
        }
        if (!live_source && (!measured || members.size() != 1 || members.front().type != "measurement_linework"))
            throw std::invalid_argument("native group requires a live source or standalone stroke");
        if (measured) {
            // Bound graph contacts and worst-case grouped-face/use matching
            // before invoking any linework decoder or source graph checker.
            if (measured_segments > 2048) throw std::invalid_argument("V6 graph source segment limit");
            const auto charge = [&](std::size_t amount) {
                if (amount > 250'000 - source_work) throw std::invalid_argument("V6 cumulative matching/replay work limit");
                source_work += amount;
            };
            for (const auto& boundary : boundaries) if (!measurement_linework_source_ids_for_admission(boundary).empty()) {
                const auto contacts = measured_segments * measured_segments;
                charge(contacts);
                std::size_t source_uses = 0;
                const auto face_work = [&](const Json& uses, bool all_faces) {
                    if (!uses.is_array()) throw std::invalid_argument("V6 retained face must be an array");
                    const auto edges = uses.size();
                    for (const auto& edge : uses) {
                        if (!edge.is_array() || edge.size() > 250'000 - source_uses)
                            throw std::invalid_argument("V6 retained source use limit");
                        source_uses += edge.size();
                    }
                    // An arrangement of N straight/circular-arc primitives has
                    // at most N^2+1 faces. A matched K-edge face can carry at
                    // most K*N uses. Charge both orientations and every start,
                    // including failed signature/interval comparisons.
                    const auto weight = edges * (measured_segments + 1);
                    const auto attempts = 2 * edges + 1;
                    const auto faces = all_faces ? contacts + 1 : std::size_t{1};
                    if (weight > 250'000 || attempts > 250'000 || faces > 250'000 ||
                        weight > 250'000 / attempts || weight * attempts + 1 > 250'000 / faces)
                        throw std::invalid_argument("V6 face matching work limit");
                    charge(faces * (weight * attempts + 1));
                };
                const bool grouped = boundary.extensions.contains("measurement_linework_group");
                face_work(boundary.extensions.at("measurement_linework_sources"), !grouped);
                if (grouped) for (const auto& member : boundary.extensions.at("measurement_linework_group").at("members"))
                    face_work(member, true);
                // Source validation caches are per consumer; ordinary >16-owner
                // consumers can decode repeatedly. Charge every retained use,
                // plus a graph rebuild, against the complete raw history work.
                if (measured_replay_work && source_uses + 1 > (250'000 - source_work) / measured_replay_work)
                    throw std::invalid_argument("V6 repeated consumer replay work limit");
                charge(measured_replay_work * (source_uses + 1));
            }
        }
        for (const auto& other : entities) {
            if (graph.contains(other.id)) continue;
            const auto dependencies = can_recognize_boundary_entity_type(other.type) ||
                other.type == "wall" || other.type == "opening" || other.type == "measurement_linework" ? native_dxf_wall_source_dependency_ids(other) :
                std::vector<std::string>{};
            for (const auto& target : dependencies)
                if (graph.contains(target)) throw std::invalid_argument("incoming V5 dependency outside component");
            if (wall_source_member(other)) for (const auto& id : other.extensions.at("vertex_dxf_boundary").at("member_ids"))
                if (graph.contains(id.get<std::string>())) throw std::invalid_argument("overlapping V5 membership");
        }
        std::vector<std::string> ready;
        for (const auto& [id, count] : incoming) if (!count) ready.push_back(id);
        std::size_t visited = 0;
        for (std::size_t i = 0; i < ready.size(); ++i) {
            ++visited; for (const auto& child : deductions[ready[i]]) if (!--incoming.at(child)) ready.push_back(child);
        }
        if (visited != members.size()) throw std::invalid_argument("cyclic V5 deductions");
        std::set<std::string> connected{root.id}; ready = {root.id};
        for (std::size_t i = 0; i < ready.size(); ++i)
            for (const auto& id : adjacency[ready[i]]) if (connected.insert(id).second) ready.push_back(id);
        if (connected.size() != members.size()) throw std::invalid_argument("disconnected V5 component");
        validate_boundary_group_source_contexts(boundaries, &source_work, true);
        auto proof_inventory = graph;
        if (measured) for (const auto& [id, entity] : complete_measured_inventory)
            if (entity.type == "measurement_linework") proof_inventory.try_emplace(id, entity);
        pending_checks.push_back({graph, std::move(proof_inventory), std::move(boundaries), std::move(contexts), measured});
        checked.insert(connected.begin(), connected.end());
    }
    if (work_budget) {
        work_budget->segments = segments;
        work_budget->source_work = source_work;
        work_budget->measured_operation = work_budget->measured_operation || measured_operation;
    }
    if (preflight_only) return;
    for (const auto& pending : pending_checks) {
        if (pending.measured) {
            for (const auto& [id, member] : pending.graph) if (member.type == "measurement_linework") {
                (void)id;
                const auto decoded = decode_measurement_linework_model(member.properties.at("model"));
                if (!decoded.supported()) throw std::invalid_argument("V6 measured source model unsupported");
                (void)replay_measurement_linework(*decoded.model);
            }
            const auto checks = measurement_linework_source_checks_with_contexts(pending.measured_inventory, pending.contexts);
            for (const auto& boundary : pending.boundaries)
                if (!measurement_linework_source_current(checks, boundary)) throw std::invalid_argument("stale V6 measured source graph");
        }
        validate_boundary_group_source_contexts(pending.boundaries);
        for (const auto& [id, member] : pending.graph) {
            (void)id;
            if (member.properties.contains("wall_measurement_source") &&
                !wall_measurement_source_current(pending.graph, member))
                throw std::invalid_argument("stale V5 exterior wall source");
        }
    }
}

void bind_native_dxf_wall_source_destinations(std::vector<Entity>& entities,
    const std::map<std::string, DrawingContext, std::less<>>& actual_contexts) {
    auto staged = entities;
    NativeDxfWallSourceWorkBudget work_budget;
    auto* shared_work = std::any_of(staged.begin(), staged.end(), measured_source_member) ? &work_budget : nullptr;
    // Desktop may already have assigned floor/layer; validate the untouched
    // pending graph by removing only those reviewed assignments in a copy.
    auto pending = staged;
    // Existing destination strokes are observations of the reviewed destination,
    // not original-source evidence. Source closure has already been proved in
    // the mapper/broker. Include these outsiders only in the final destination
    // inventory, without mixing their actual contexts into the pending proof.
    std::erase_if(pending, [](const Entity& entity) {
        return entity.type == "measurement_linework" && !wall_source_member(entity);
    });
    for (auto& entity : pending) if (wall_source_member(entity)) {
        const auto actual = actual_contexts.find(entity.id);
        if (actual == actual_contexts.end() || !actual->second.complete())
            throw std::invalid_argument("V5 reviewed destination context missing");
        const auto binding = entity.extensions.find(kWallSourceContextBinding);
        if (binding == entity.extensions.end() || !binding->at("destination_context").is_null())
            throw std::invalid_argument("V5 destination binding requires pending state");
        for (const auto* key : {"floor_id", "layer_id"}) {
            const auto assigned = entity.properties.find(key);
            const auto& expected = std::string_view(key) == "floor_id" ? actual->second.floor_id : actual->second.layer_id;
            if (assigned != entity.properties.end() && *assigned != expected)
                throw std::invalid_argument("V5 explicit reviewed destination differs");
            entity.properties.erase(key);
        }
    }
    validate_native_dxf_wall_source_groups(pending, shared_work);
    for (auto& entity : staged) if (entity.type == "measurement_linework" && !wall_source_member(entity)) {
        const auto actual = actual_contexts.find(entity.id);
        if (actual == actual_contexts.end() || !actual->second.complete())
            throw std::invalid_argument("V6 destination stroke observation context missing");
        entity.extensions[kResolvedContext] = {{"version", 1},
            {"context", resolved_context_json(actual->second)}};
    }
    for (auto& entity : staged) if (wall_source_member(entity)) {
        const auto& context = actual_contexts.at(entity.id);
        auto& binding = entity.extensions.at(kWallSourceContextBinding);
        Json destination = Json::object();
        const auto& original = binding.at("source_context");
        if (original.contains("property_id")) destination["property_id"] = context.property_id;
        if (original.contains("building_id")) destination["building_id"] = context.building_id;
        destination["floor_id"] = context.floor_id; destination["layer_id"] = context.layer_id;
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "phase_id"}) entity.properties.erase(key);
        for (const auto& [key, value] : destination.items()) entity.properties[key] = value;
        binding["destination_context"] = destination;
        if (measured_source_member(entity)) binding["destination_resolved_context"] = resolved_context_json(context);
        const auto stair = entity.extensions.find("vertex_dxf_stair_floor_binding");
        if (stair != entity.extensions.end()) {
            entity.properties["appraisal_facts"]["ansi"]["ceiling"]["stair_from_floor_id"] = context.floor_id;
            (*stair)["destination_floor_id"] = context.floor_id;
        }
    }
    std::map<std::string, Entity, std::less<>> final_graph;
    for (const auto& entity : staged) final_graph.emplace(entity.id, entity);
    for (const auto& entity : staged) if (measured_source_member(entity) &&
        !measurement_linework_source_ids_for_admission(entity).empty()) {
        const auto expected = measured_graph_ids(entity, final_graph, actual_contexts);
        if (entity.extensions.at(kMeasuredGraph).at("source_ids") != Json(expected))
            throw std::invalid_argument("reviewed V6 destination changes applicable measured graph");
    }
    for (auto& entity : staged) if (wall_source_member(entity) && entity.properties.contains("wall_measurement_source"))
        for (auto& record : entity.properties.at("wall_measurement_source").at("walls"))
            record.at("context") = direct_source_context(final_graph.at(record.at("id").get<std::string>()));
    validate_native_dxf_wall_source_groups(staged, shared_work);
    entities = std::move(staged);
}

DxfProjectExportResult export_project_dxf(const DocumentSnapshot& document,
                                          const DxfExchangeLimits& limits) {
    DxfProjectExportResult result;
    result.drawing.insertion_units = 6; // SI metres are authoritative in the project model.
    ConstraintPhaseScope scope;
    try {
        // Evaluate every registry against the complete source before deriving
        // output. A filtered Document would lose retained membership evidence.
        scope = constraint_phase_scope(document.entities());
    } catch (const std::exception&) {
        diagnostic(result.diagnostics, {}, "PROJECT", "active_design_scope_not_representable");
        return result;
    }
    // Hosted objects cannot be visible without their actual active wall, even
    // when the opening itself has no registry membership.
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "opening" || !entity.properties.is_object()) continue;
        const auto host_id = entity.properties.find("wall_id");
        if (host_id == entity.properties.end() || !host_id->is_string()) continue;
        const auto host = document.entities().find(host_id->get_ref<const std::string&>());
        if (host != document.entities().end() && host->second.type == "wall" &&
            scope.inactive_owner_ids.contains(host->first)) scope.inactive_owner_ids.insert(id);
    }
#ifdef SKETCH_PHYSICAL_ROOMS
    const auto physical_rooms = physical_wall_room_checks(document);
#endif
    NativeDxfWallSourceWorkBudget wall_source_budget;
    const bool measured_operation = std::any_of(document.entities().begin(), document.entities().end(), [&](const auto& owner) {
        return !scope.inactive_owner_ids.contains(owner.first) && (owner.second.type == "measurement_linework" ||
            owner.second.extensions.contains("measurement_linework_sources") || owner.second.extensions.contains("measurement_linework_group"));
    });
    const auto [native_boundaries, fallback_boundaries] = export_boundary_groups(document, scope, result, wall_source_budget);
    for (const auto& [id, entity] : document.entities()) {
        if (native_boundaries.contains(id)) continue;
        if (scope.inactive_owner_ids.contains(id)) {
            diagnostic(result.diagnostics, id, entity.type, "inactive_design_evidence_not_representable");
            continue;
        }
        if (entity.type == "room_boundary" && entity.extensions.contains("physical_wall_room")) {
#ifdef SKETCH_PHYSICAL_ROOMS
            const auto found = physical_rooms.find(id);
            if (found == physical_rooms.end() || !found->second.current) {
                diagnostic(result.diagnostics, id, entity.type, "physical_room_source_stale");
                continue;
            }
            const auto layer = layer_for(document, entity, result.diagnostics);
            add_boundary_as_dxf(result.drawing, found->second.boundary, layer, result.diagnostics, id, entity.type);
            for (const auto& hole : found->second.holes)
                add_boundary_as_dxf(result.drawing, hole, layer, result.diagnostics, id, entity.type);
            if (!found->second.holes.empty())
                diagnostic(result.diagnostics, id, entity.type, "boundary_hole_association_not_representable");
            report_boundary_semantics_loss(entity, {}, result.diagnostics);
            diagnostic(result.diagnostics, id, entity.type, "physical_room_source_evidence_not_representable");
#else
            diagnostic(result.diagnostics, id, entity.type, "physical_room_runtime_unavailable");
#endif
            continue;
        }
        export_native_entity(document, entity, result, scope, !fallback_boundaries.contains(id),
            measured_operation ? &wall_source_budget : nullptr);
    }
    // Validate the complete mapped drawing before returning it. The caller can
    // still inspect diagnostics; an invalid mapped record is never serialized.
    try {
        (void)export_dxf_ascii(result.drawing, limits);
    } catch (const std::exception&) {
        diagnostic(result.diagnostics, {}, "PROJECT", "mapped_drawing_not_serializable");
        result.drawing = {};
        result.drawing.insertion_units = 6;
    }
    return result;
}

DxfProjectImportResult import_project_dxf(std::string_view bytes,
                                          const DxfExchangeLimits& limits) {
    auto parsed = parse_dxf_ascii(bytes, limits);
    DxfProjectImportResult result;
    for (const auto& item : parsed.diagnostics)
        diagnostic(result.diagnostics, {}, item.entity_type, item.code);
    const auto factor = metres_per_source_unit(parsed.drawing.insertion_units);
    if (!factor) {
        diagnostic(result.diagnostics, {}, "HEADER",
                   parsed.drawing.insertion_units == 0 ? "source_units_unspecified" : "source_units_unsupported");
        result.source_retention_required = true;
        return result;
    }
    const bool source_is_metres = parsed.drawing.insertion_units == 6;
    preflight_project_expansion(parsed.drawing, limits);
    normalize_drawing_to_metres(parsed.drawing, *factor);
    const auto native_inserts = import_native_graphs(parsed.drawing, source_is_metres, result);
    std::size_t boundary_counter = 0;
    import_direct_geometry(parsed.drawing, result, boundary_counter);
    AnnotationState annotations;
    Json annotation_layers = Json::object();
    std::size_t label_counter = 0;
    import_labels(parsed.drawing.labels, annotations, label_counter, annotation_layers);
    import_dimensions(parsed.drawing.dimensions, result, annotations, boundary_counter, label_counter, annotation_layers, *factor);
    import_arc_dimensions(parsed.drawing.arc_dimensions, result, annotations, boundary_counter, label_counter, annotation_layers, *factor);
    import_angular_dimensions(parsed.drawing.angular_dimensions, result, annotations, boundary_counter, label_counter, annotation_layers);
    import_inserts(parsed.drawing, native_inserts, result, boundary_counter, label_counter, annotations, annotation_layers);
    if (!annotations.labels.empty()) {
        try {
            auto entity = make_annotation_entity("dxf-annotations", annotations);
            entity.extensions["dxf_annotation_layers"] = std::move(annotation_layers);
            result.entities.push_back(std::move(entity));
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, "dxf-annotations", "ANNOTATION", "annotation_reconstruction_failed");
        }
    }
    result.source_retention_required = !result.diagnostics.empty();
    return result;
}

} // namespace sketch
