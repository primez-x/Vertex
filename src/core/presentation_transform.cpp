#include "sketch/presentation_transform.hpp"
#include "sketch/annotation_entity_codec.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

void check_source(const DocumentSnapshot& source, Revision expected_revision) {
    if (source.revision() != expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision, "The presentation editing source changed.");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, "The presentation editing source is read-only.");
}

void factor(double value) {
    require(std::isfinite(value) && value > 0.0, "Presentation scale factors must be finite and positive.");
}

void bounded_scale(double value) {
    require(std::isfinite(value) && value >= 0.01 && value <= 100.0,
            "The resized presentation must remain between 0.01x and 100x.");
}

const Entity& owner(const DocumentSnapshot& source, std::string_view id, std::string_view type) {
    const auto found = source.entities().find(id);
    require(found != source.entities().end(), "The presentation owner no longer exists.");
    require(found->second.type == type, "The presentation owner has the wrong entity type.");
    return found->second;
}

struct AnnotationChild {
    const char* collection{};
    std::size_t index{};
    AnnotationPlacement placement;
    double width_scale{1.0};
    double depth_scale{1.0};
};

AnnotationChild child(const AnnotationState& state, std::string_view id) {
    AnnotationChild result;
    std::size_t count = 0;
    for (std::size_t i = 0; i < state.labels.size(); ++i) {
        if (state.labels[i].id != id) continue;
        result = {"labels", i, state.labels[i].placement};
        ++count;
    }
    for (std::size_t i = 0; i < state.symbols.size(); ++i) {
        const auto& symbol = state.symbols[i];
        if (symbol.id != id) continue;
        result = {"symbols", i, symbol.placement, symbol.width_scale, symbol.depth_scale};
        ++count;
    }
    require(count == 1, "The selected annotation must identify exactly one child of its owner.");
    return result;
}

void upgrade_axes(Entity& entity, const AnnotationState& state) {
    auto& raw = entity.properties.at("state");
    if (raw.at("version").get<int>() >= 3) return;
    const auto encoded = encode_annotation_state(state, default_symbol_catalog());
    auto& symbols = raw.at("symbols");
    for (std::size_t i = 0; i < symbols.size(); ++i)
        for (const auto* key : {"definition", "pinned_svg", "width_scale", "depth_scale",
                               "flip_horizontal", "flip_vertical"})
            if (!symbols[i].contains(key)) symbols[i][key] = encoded.at("symbols")[i].at(key);
    raw["version"] = 3;
}

ApplyEntityChanges changed(Revision revision, Entity entity, const char* message) {
    return {revision, {EntityChange::upsert(std::move(entity))}, {}, message};
}

double reference_number(const Entity& entity, const char* key, double fallback) {
    if (!entity.properties.contains(key)) return fallback;
    const auto& raw = entity.properties.at(key);
    require(raw.is_number(), "Reference transform fields must be numeric.");
    const double value = raw.get<double>();
    require(std::isfinite(value), "Reference transform fields must be finite.");
    return value;
}

}  // namespace

ApplyEntityChanges annotation_transform_command(const DocumentSnapshot& source,
    std::string_view owner_id, std::string_view child_id, double relative_scale,
    double rotation_radians, Revision expected_revision) {
    check_source(source, expected_revision);
    factor(relative_scale);
    require(std::isfinite(rotation_radians), "Presentation rotation must be finite.");
    auto candidate = owner(source, owner_id, kAnnotationEntityType);
    const auto selected = child(decode_annotation_entity(candidate), child_id);
    if (relative_scale == 1.0 && rotation_radians == 0.0) return {expected_revision, {}, {}, {}};
    const double scale = selected.placement.scale * relative_scale;
    bounded_scale(scale);
    const double rotation = std::remainder(selected.placement.rotation_radians + rotation_radians,
                                           2.0 * std::numbers::pi);
    require(std::isfinite(rotation), "The resulting annotation rotation must be finite.");
    auto& placement = candidate.properties.at("state").at(selected.collection)
                          .at(selected.index).at("placement");
    if (relative_scale != 1.0) placement["scale"] = scale;
    if (rotation_radians != 0.0) placement["rotation_radians"] = rotation;
    validate_annotation_entity(candidate);
    return changed(expected_revision, std::move(candidate), "Transform annotation");
}

ApplyEntityChanges annotation_axis_resize_command(const DocumentSnapshot& source,
    std::string_view owner_id, std::string_view child_id, double scale_x, double scale_y,
    Vec2 source_anchor, Revision expected_revision) {
    check_source(source, expected_revision);
    factor(scale_x);
    factor(scale_y);
    require(std::isfinite(source_anchor.x) && std::isfinite(source_anchor.y),
            "The annotation resize anchor must be finite.");
    auto candidate = owner(source, owner_id, kAnnotationEntityType);
    auto state = decode_annotation_entity(candidate);
    const auto selected = child(state, child_id);
    require(std::string_view(selected.collection) == "symbols", "Only symbols support independent axis resize.");
    if (scale_x == 1.0 && scale_y == 1.0) return {expected_revision, {}, {}, {}};
    const double width = selected.width_scale * scale_x;
    const double depth = selected.depth_scale * scale_y;
    // Independent physical dimensions retain the codec's finite-positive
    // domain; the uniform placement-scale authoring limit is a different value.
    factor(width);
    factor(depth);
    const double c = std::cos(selected.placement.rotation_radians);
    const double s = std::sin(selected.placement.rotation_radians);
    const double dx = selected.placement.position.x - source_anchor.x;
    const double dy = selected.placement.position.y - source_anchor.y;
    const double x = (c * dx + s * dy) * scale_x;
    const double y = (-s * dx + c * dy) * scale_y;
    const Vec2 position{source_anchor.x + c * x - s * y, source_anchor.y + s * x + c * y};
    require(std::isfinite(position.x) && std::isfinite(position.y), "The resized annotation position must be finite.");
    // Supply the new dimensions when filling required legacy symbol fields.
    state.symbols[selected.index].width_scale = width;
    state.symbols[selected.index].depth_scale = depth;
    upgrade_axes(candidate, state);
    auto& raw = candidate.properties.at("state").at("symbols").at(selected.index);
    raw.at("placement")["x"] = position.x;
    raw.at("placement")["y"] = position.y;
    if (scale_x != 1.0) raw["width_scale"] = width;
    if (scale_y != 1.0) raw["depth_scale"] = depth;
    validate_annotation_entity(candidate);
    return changed(expected_revision, std::move(candidate), "Resize symbol dimensions");
}

ApplyEntityChanges reference_transform_command(const DocumentSnapshot& source,
    std::string_view reference_id, double relative_scale, double rotation_radians,
    Revision expected_revision) {
    check_source(source, expected_revision);
    factor(relative_scale);
    require(std::isfinite(rotation_radians), "Presentation rotation must be finite.");
    auto candidate = owner(source, reference_id, "reference_asset");
    require(candidate.properties.is_object(), "Reference properties must be an object.");
    const double current_scale = reference_number(candidate, "scale", 1.0);
    factor(current_scale);
    const double current_rotation = reference_number(candidate, "rotation_degrees", 0.0);
    if (relative_scale == 1.0 && rotation_radians == 0.0) return {expected_revision, {}, {}, {}};
    const double scale = current_scale * relative_scale;
    bounded_scale(scale);
    const double rotation = std::remainder(current_rotation +
        rotation_radians * 180.0 / std::numbers::pi, 360.0);
    require(std::isfinite(rotation), "The resulting reference rotation must be finite.");
    if (relative_scale != 1.0) candidate.properties["scale"] = scale;
    if (rotation_radians != 0.0) candidate.properties["rotation_degrees"] = rotation;
    return changed(expected_revision, std::move(candidate), "Transform reference");
}

}  // namespace sketch
