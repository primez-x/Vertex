#include "sketch/presentation_transform.hpp"
#include "sketch/annotation_entity_codec.hpp"

#include <cmath>
#include <map>
#include <numbers>
#include <set>
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

bool reference_boolean(const Entity& entity, const char* key, bool fallback) {
    if (!entity.properties.contains(key)) return fallback;
    const auto& raw = entity.properties.at(key);
    require(raw.is_boolean(), "Reference transform flags must be boolean.");
    return raw.get<bool>();
}

struct ReferencePlacement {
    Vec2 position;
    double rotation_radians{};
    bool flip_vertical{};
};

ReferencePlacement reference_placement(const DocumentSnapshot& source, const Entity& entity) {
    require(entity.properties.is_object(), "Reference properties must be an object.");
    const auto asset = [&](const char* key, bool required) {
        if (!entity.properties.contains(key)) {
            require(!required, "The reference source asset ID is missing.");
            return;
        }
        const auto& raw = entity.properties.at(key);
        require(raw.is_string(), "Reference asset IDs must be strings.");
        const auto& id = raw.get_ref<const std::string&>();
        require(!id.empty() && id.size() <= 256 && source.assets().contains(id),
                "The reference asset is missing or invalid.");
    };
    asset("asset_id", true);
    asset("render_asset_id", false);
    require(entity.properties.contains("position_m"), "Reference position is required.");
    const auto& raw = entity.properties.at("position_m");
    require(raw.is_array() && raw.size() == 2 && raw[0].is_number() && raw[1].is_number(),
            "Reference position must contain two numeric metre coordinates.");
    const Vec2 position{raw[0].get<double>(), raw[1].get<double>()};
    require(std::isfinite(position.x) && std::isfinite(position.y), "Reference position must be finite.");
    // Omitted legacy fields use the scene's defined defaults. Present but
    // malformed values are refused, never replaced with fallback values.
    factor(reference_number(entity, "metres_per_source_unit", 0.01));
    factor(reference_number(entity, "scale", 1.0));
    const auto intensity = reference_number(entity, "intensity", 1.0);
    require(intensity >= 0.0 && intensity <= 1.0, "Reference intensity must be between zero and one.");
    (void)reference_boolean(entity, "visible", true);
    (void)reference_boolean(entity, "flip_horizontal", false);
    const auto rotation = reference_number(entity, "rotation_degrees", 0.0);
    return {position, rotation * (std::numbers::pi / 180.0),
            reference_boolean(entity, "flip_vertical", false)};
}

bool rigid_identity(const PlanarTransform& transform) {
    require(std::isfinite(transform.pivot.x) && std::isfinite(transform.pivot.y) &&
                std::isfinite(transform.offset.x) && std::isfinite(transform.offset.y) &&
                std::isfinite(transform.rotation_radians),
            "Presentation group transform requires finite parameters.");
    const auto rotation = std::remainder(transform.rotation_radians, 2.0 * std::numbers::pi);
    // Two global reflections are a half-turn, cancelling only a half-turn
    // rotation. Canonicalization identifies identity intent only: nonidentity
    // replay retains the exact requested transform_point operation order.
    const bool linear_identity = (!transform.flip_horizontal && !transform.flip_vertical && rotation == 0.0) ||
        (transform.flip_horizontal && transform.flip_vertical && std::abs(rotation) == std::numbers::pi);
    return linear_identity && transform.offset.x == 0.0 && transform.offset.y == 0.0;
}

double rigid_orientation(double rotation, const PlanarTransform& linear) {
    // symbol_transform and drawReference both apply local flips BEFORE R(theta).
    // Retaining local X flip therefore requires the transformed UNFLIPPED X
    // direction here; the determinant change is carried by local Y alone.
    const auto x_axis = transform_point({std::cos(rotation), std::sin(rotation)}, linear);
    const auto result = std::atan2(x_axis.y, x_axis.x);
    require(std::isfinite(result), "The resulting presentation orientation must be finite.");
    return result;
}

}  // namespace

ApplyEntityChanges presentation_group_transform_command(const DocumentSnapshot& source,
    std::span<const PresentationAnnotationTarget> annotations,
    std::span<const std::string> reference_ids, const PlanarTransform& requested,
    Revision expected_revision) {
    check_source(source, expected_revision);
    require(annotations.size() <= maximum_presentation_group_targets &&
                reference_ids.size() <= maximum_presentation_group_targets - annotations.size() &&
                (!annotations.empty() || !reference_ids.empty()),
            "Presentation group selection must contain between one and 1000 targets.");
    const bool identity = rigid_identity(requested);
    const auto& transform = requested;
    const PlanarTransform linear{{}, transform.rotation_radians,
                                transform.flip_horizontal, transform.flip_vertical, {}};
    const bool oriented = linear.rotation_radians != 0.0 || linear.flip_horizontal || linear.flip_vertical;
    const bool reflected = linear.flip_horizontal != linear.flip_vertical;

    std::map<std::string, std::set<std::string>> selected;
    for (const auto& target : annotations) {
        require(!target.owner_id.empty() && target.owner_id.size() <= 256 &&
                    !target.child_id.empty() && target.child_id.size() <= 256,
                "Presentation target IDs must be bounded and nonempty.");
        require(selected[target.owner_id].insert(target.child_id).second,
                "The presentation group contains a duplicate annotation target.");
    }
    struct AnnotationEdit {
        Entity entity;
        AnnotationState state;
        std::vector<AnnotationChild> children;
    };
    std::vector<AnnotationEdit> edits;
    for (const auto& [owner_id, ids] : selected) {
        auto entity = owner(source, owner_id, kAnnotationEntityType);
        auto state = decode_annotation_entity(entity); // Includes model/version and identity validation.
        std::vector<AnnotationChild> children;
        std::map<std::string,std::size_t> matches;
        for (std::size_t i = 0; i < state.labels.size(); ++i)
            if (ids.contains(state.labels[i].id)) {
                ++matches[state.labels[i].id];
                children.push_back({"labels", i, state.labels[i].placement});
            }
        for (std::size_t i = 0; i < state.symbols.size(); ++i) {
            const auto& symbol = state.symbols[i];
            if (ids.contains(symbol.id)) {
                ++matches[symbol.id];
                children.push_back({"symbols", i, symbol.placement, symbol.width_scale, symbol.depth_scale});
            }
        }
        for (const auto& id : ids)
            require(matches[id] == 1, "A selected annotation child is missing or ambiguous.");
        edits.push_back({std::move(entity), std::move(state), std::move(children)});
    }
    std::set<std::string> unique_references;
    std::vector<std::pair<Entity, ReferencePlacement>> references;
    for (const auto& id : reference_ids) {
        require(!id.empty() && id.size() <= 256 && unique_references.insert(id).second,
                "Reference target IDs must be bounded, nonempty and unique.");
        auto entity = owner(source, id, "reference_asset");
        auto placement = reference_placement(source, entity);
        references.emplace_back(std::move(entity), placement);
    }
    // Identity never repairs or upgrades a source, but must refuse every invalid
    // selected owner/child/reference above just like a nonidentity request.
    if (identity) return {expected_revision, {}, {}, {}};

    ApplyEntityChanges command{expected_revision, {}, {}, "Transform presentation group"};
    for (auto& edit : edits) {
        for (const auto& selected_child : edit.children) {
            const bool symbol = std::string_view(selected_child.collection) == "symbols";
            if (symbol && reflected) upgrade_axes(edit.entity, edit.state);
            auto& raw = edit.entity.properties.at("state").at(selected_child.collection).at(selected_child.index);
            auto& placement = raw.at("placement");
            const auto position = transform_point(selected_child.placement.position, transform);
            if (position.x != selected_child.placement.position.x) placement["x"] = position.x;
            if (position.y != selected_child.placement.position.y) placement["y"] = position.y;
            if (oriented) {
                const auto rotation = rigid_orientation(selected_child.placement.rotation_radians, linear);
                if (rotation != selected_child.placement.rotation_radians) placement["rotation_radians"] = rotation;
            }
            if (symbol && reflected)
                raw["flip_vertical"] = !edit.state.symbols[selected_child.index].flip_vertical;
            // Labels intentionally have no local mirror flags: glyphs remain
            // readable while their anchor and baseline follow the shared edit.
        }
        validate_annotation_entity(edit.entity);
        if (edit.entity != source.entities().at(edit.entity.id))
            command.entity_changes.push_back(EntityChange::upsert(std::move(edit.entity)));
    }
    for (auto& [entity, placement] : references) {
        const auto position = transform_point(placement.position, transform);
        if (position.x != placement.position.x) entity.properties.at("position_m")[0] = position.x;
        if (position.y != placement.position.y) entity.properties.at("position_m")[1] = position.y;
        if (oriented) {
            const auto rotation = rigid_orientation(placement.rotation_radians, linear) * (180.0 / std::numbers::pi);
            if (rotation != reference_number(entity, "rotation_degrees", 0.0))
                entity.properties["rotation_degrees"] = rotation;
        }
        if (reflected) entity.properties["flip_vertical"] = !placement.flip_vertical;
        if (entity != source.entities().at(entity.id))
            command.entity_changes.push_back(EntityChange::upsert(std::move(entity)));
    }
    return command;
}

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
