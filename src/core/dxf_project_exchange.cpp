#include "sketch/dxf_project_exchange.hpp"

#include "sketch/annotation_catalog.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"

#include <algorithm>
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

void diagnostic(std::vector<DxfProjectDiagnostic>& output, std::string id,
                std::string kind, std::string code) {
    output.push_back({std::move(id), std::move(kind), std::move(code)});
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    return std::hypot(left.x - right.x, left.y - right.y) <= kGeometryTolerance;
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
            const auto found = document.entities().find(layer_id);
            if (found != document.entities().end() && found->second.type == "layer" &&
                found->second.properties.contains("name") &&
                found->second.properties.at("name").is_string()) {
                layer = found->second.properties.at("name").get<std::string>();
            }
        }
    }
    return valid_layer(std::move(layer), diagnostics, entity.id, entity.type);
}

double normalized_degrees(double radians) {
    auto degrees = radians * 180.0 / std::numbers::pi;
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    return degrees == 0.0 ? 0.0 : degrees;
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
    for (const auto& segment : boundary) {
        const auto bulge = std::tan(segment.sweep_radians * 0.25);
        if (!std::isfinite(bulge) || std::abs(bulge) > 1e12) return std::nullopt;
        result.vertices.push_back({{segment.start.x, segment.start.y},
                                    bulge == 0.0 ? 0.0 : bulge});
    }
    return result;
}

void add_segment_as_dxf(DxfDrawing& drawing, const Segment& segment, std::string layer,
                        std::vector<DxfProjectDiagnostic>& diagnostics,
                        const std::string& source_id, std::string_view source_kind) {
    if (!std::isfinite(segment.sweep_radians) ||
        std::abs(segment.sweep_radians) >= kFullTurn - 1e-10) {
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

std::optional<Segment> native_segment(const Json& value) {
    return read_segment(value);
}

std::optional<Boundary> native_slab_hole(const Json& value) {
    return read_boundary_value(value);
}

void export_native_entity(const DocumentSnapshot& document, const Entity& entity,
                          DxfProjectExportResult& result) {
    const auto layer = layer_for(document, entity, result.diagnostics);
    if (can_recognize_boundary_entity_type(entity.type)) {
        const auto boundary = read_entity_boundary(entity);
        if (!boundary) {
            diagnostic(result.diagnostics, entity.id, entity.type, "boundary_not_representable");
            return;
        }
        add_boundary_as_dxf(result.drawing, *boundary, layer, result.diagnostics,
                            entity.id, entity.type);
        return;
    }
    if (entity.type == "wall") {
        const auto baseline = entity.properties.is_object() && entity.properties.contains("baseline")
            ? native_segment(entity.properties.at("baseline")) : std::nullopt;
        if (!baseline) {
            diagnostic(result.diagnostics, entity.id, entity.type, "wall_baseline_not_representable");
            return;
        }
        // DXF project mapping carries the analytical wall baseline only. The
        // 3D wall envelope, vertical slope, and layer/material stack have no
        // representation in this bounded 2D exchange profile, so make that
        // loss visible in the fidelity report.
        if (entity.properties.contains("thickness_m") ||
            entity.properties.contains("thickness") ||
            entity.properties.contains("height_m") ||
            entity.properties.contains("elevation_m") ||
            entity.properties.contains("slope_rise_m") ||
            entity.properties.contains("layers")) {
            diagnostic(result.diagnostics, entity.id, entity.type,
                       "wall_3d_semantics_not_representable");
        }
        add_segment_as_dxf(result.drawing, *baseline, layer, result.diagnostics,
                           entity.id, entity.type);
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
        if (entity.properties.contains("holes") && entity.properties.at("holes").is_array()) {
            std::size_t index = 0;
            for (const auto& value : entity.properties.at("holes")) {
                const auto hole = native_slab_hole(value);
                if (!hole) {
                    diagnostic(result.diagnostics, entity.id, entity.type, "slab_hole_not_representable");
                } else {
                    add_boundary_as_dxf(result.drawing, *hole, layer, result.diagnostics,
                                        entity.id + ":hole:" + std::to_string(index), "slab_hole");
                }
                ++index;
            }
        }
        return;
    }
    if (entity.type == kAnnotationEntityType) {
        try {
            const auto state = decode_annotation_entity(entity);
            for (const auto& label : state.labels) {
                if (!label.visible) continue;
                result.drawing.labels.push_back({{label.placement.position.x, label.placement.position.y},
                    label.style.text_height_metres, normalized_degrees(label.placement.rotation_radians),
                    label.content, "Annotations"});
            }
            const auto catalog = default_symbol_catalog();
            for (const auto& symbol : state.symbols) {
                if (!symbol.visible) continue;
                const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& item) {
                    return item.id == symbol.symbol_id;
                });
                if (found == catalog.end()) {
                    diagnostic(result.diagnostics, entity.id, entity.type, "symbol_definition_missing");
                    continue;
                }
                try {
                    for (const auto& stroke : placed_symbol_preview(*found, symbol.placement))
                        result.drawing.lines.push_back({{stroke.start.x, stroke.start.y},
                                                        {stroke.end.x, stroke.end.y}, "Symbols"});
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
            // DxfDimension is deliberately limited to a linear measurement.
            // Angle and area dimensions have different analytical semantics;
            // exporting their first segment as a linear dimension would make
            // a successful-looking file lie about the source document.
            if (decoded.dimension->kind != BoundaryDimensionKind::segment_length) {
                diagnostic(result.diagnostics, entity.id, entity.type,
                           "dimension_semantics_not_representable");
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
            const auto resolved = decoded.dimension->resolve(owner->second);
            const auto text = entity.properties.value("display_text", std::string{});
            result.drawing.dimensions.push_back({{resolved.segment.start.x, resolved.segment.start.y},
                {resolved.segment.end.x, resolved.segment.end.y},
                {decoded.dimension->text_position.x, decoded.dimension->text_position.y},
                {decoded.dimension->text_position.x, decoded.dimension->text_position.y},
                decoded.dimension->presentation
                    ? normalized_degrees(decoded.dimension->presentation->rotation_radians) : 0.0,
                text, "Dimensions"});
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

struct InsertTransform {
    DxfPoint origin;
    double scale_x{1};
    double scale_y{1};
    double rotation{};
};

DxfPoint transform_point(DxfPoint point, const InsertTransform& transform) {
    const auto x = point.x * transform.scale_x;
    const auto y = point.y * transform.scale_y;
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
                   std::size_t& counter) {
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
        state.labels.push_back(std::move(item));
    }
}

void import_dimensions(const std::vector<DxfDimension>& dimensions,
                       DxfProjectImportResult& result, AnnotationState& annotations,
                       std::size_t& boundary_counter, std::size_t& label_counter) {
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
                {"text", dimension.text},
                {"annotation_id", label_id}}}}));
        LabelInstance label;
        label.id = label_id;
        label.template_id = "dxf-dimension";
        label.content = dimension.text.empty() ? "Dimension" : dimension.text;
        label.style.text_height_metres = 0.15;
        label.style.stroke_color = "#263241";
        label.style.fill_color = "#FFFFFF";
        label.placement.position = {dimension.text_position.x, dimension.text_position.y};
        label.placement.rotation_radians = radians_from_degrees(dimension.rotation_degrees);
        annotations.labels.push_back(std::move(label));
        diagnostic(result.diagnostics, {}, "DIMENSION", "dimension_associativity_unbound");
    }
}

void import_inserts(const DxfDrawing& drawing, DxfProjectImportResult& result,
                    std::size_t& boundary_counter, std::size_t& label_counter,
                    AnnotationState& annotations) {
    for (const auto& insert : drawing.inserts) {
        const auto block = std::find_if(drawing.blocks.begin(), drawing.blocks.end(),
            [&](const auto& value) { return value.name == insert.block_name; });
        if (block == drawing.blocks.end()) continue;
        const InsertTransform transform{{insert.insertion.x, insert.insertion.y}, insert.scale_x,
            insert.scale_y, radians_from_degrees(insert.rotation_degrees)};
        for (const auto& line : block->lines) {
            const DxfPoint start = transform_point(line.start, transform);
            const DxfPoint end = transform_point(line.end, transform);
            result.entities.push_back(imported_boundary("dxf-boundary-" + std::to_string(++boundary_counter),
                Boundary{{{start.x, start.y}, {end.x, end.y}, 0.0}}, "dxf_insert_line",
                insert.layer, "INSERT"));
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
                Boundary{transformed}, "dxf_insert_arc", insert.layer, "INSERT"));
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
                insert.layer, "INSERT"));
        }
        std::vector<DxfLabel> labels;
        labels.reserve(block->labels.size());
        for (const auto& label : block->labels) {
            const auto position = transform_point(label.position, transform);
            labels.push_back({position, label.height * std::abs(transform.scale_x),
                label.rotation_degrees + insert.rotation_degrees, label.text, insert.layer});
        }
        import_labels(labels, annotations, label_counter);
    }
}

} // namespace

DxfProjectExportResult export_project_dxf(const DocumentSnapshot& document,
                                          const DxfExchangeLimits& limits) {
    DxfProjectExportResult result;
    result.drawing.insertion_units = 6; // SI metres are authoritative in the project model.
    for (const auto& [id, entity] : document.entities()) {
        (void)id;
        export_native_entity(document, entity, result);
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
    const auto parsed = parse_dxf_ascii(bytes, limits);
    DxfProjectImportResult result;
    for (const auto& item : parsed.diagnostics)
        diagnostic(result.diagnostics, {}, item.entity_type, item.code);
    std::size_t boundary_counter = 0;
    import_direct_geometry(parsed.drawing, result, boundary_counter);
    AnnotationState annotations;
    std::size_t label_counter = 0;
    import_labels(parsed.drawing.labels, annotations, label_counter);
    import_dimensions(parsed.drawing.dimensions, result, annotations, boundary_counter, label_counter);
    import_inserts(parsed.drawing, result, boundary_counter, label_counter, annotations);
    if (!annotations.labels.empty()) {
        try {
            result.entities.push_back(make_annotation_entity("dxf-annotations", annotations));
        } catch (const std::exception&) {
            diagnostic(result.diagnostics, "dxf-annotations", "ANNOTATION", "annotation_reconstruction_failed");
        }
    }
    result.source_retention_required = !result.diagnostics.empty();
    return result;
}

} // namespace sketch
