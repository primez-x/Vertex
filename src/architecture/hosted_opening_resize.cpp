#include "sketch/hosted_opening_resize.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;

std::vector<const Entity*> hosted_openings(const DocumentSnapshot& source,
    const std::string& wall_id, const ConstraintPhaseScope& scope) {
    std::vector<const Entity*> result;
    for (const auto& [id,entity] : source.entities()) {
        if (entity.type != "opening" || scope.inactive_owner_ids.contains(id)) continue;
        const auto reference = entity.properties.find("wall_id");
        if (reference == entity.properties.end() || !reference->is_string() ||
            reference->get_ref<const std::string&>() != wall_id) continue;
        std::string host,error;
        if (!read_document_wall_id(entity,host,error)) throw std::invalid_argument(error);
        if (host == wall_id) result.push_back(&entity);
    }
    return result;
}

Wall decode_host(const DocumentSnapshot& source, const std::string& wall_id,
                 const std::vector<const Entity*>& openings) {
    const auto found = source.entities().find(wall_id);
    if (found == source.entities().end())
        throw std::invalid_argument("Hosted opening host wall is missing");
    if (found->second.type != "wall")
        throw std::invalid_argument("Hosted opening host must be a wall");
    Wall wall;
    std::string error;
    const auto resolved = resolve_vertical_placement(source,found->second);
    if (!read_document_wall(resolved,openings,wall,error)) throw std::invalid_argument(error);
    validate_wall_semantics(wall);
    return wall;
}

struct OpeningHost {
    Wall wall;
    std::vector<const Entity*> opening_entities;
    std::size_t selected_index;
};

OpeningHost selected_host(const DocumentSnapshot& source, const std::string& opening_id) {
    const auto scope = constraint_phase_scope(source.entities());
    if (scope.inactive_owner_ids.contains(opening_id))
        throw std::invalid_argument("Hosted opening edit target is inactive in the saved design");
    const auto found = source.entities().find(opening_id);
    if (found == source.entities().end())
        throw std::invalid_argument("Hosted opening edit target is missing");
    if (found->second.type != "opening")
        throw std::invalid_argument("Hosted opening edit requires an opening entity");
    std::string wall_id,error;
    if (!read_document_wall_id(found->second,wall_id,error)) throw std::invalid_argument(error);
    if (scope.inactive_owner_ids.contains(wall_id))
        throw std::invalid_argument("Hosted opening host wall is inactive in the saved design");
    auto openings = hosted_openings(source,wall_id,scope);
    auto wall = decode_host(source,wall_id,openings);
    const auto selected = std::find_if(wall.openings.begin(),wall.openings.end(),
        [&](const HostedOpening& opening) { return opening.id == opening_id; });
    if (selected == wall.openings.end())
        throw std::invalid_argument("Hosted opening edit target is missing from its host");
    const auto index = static_cast<std::size_t>(selected-wall.openings.begin());
    return {std::move(wall),std::move(openings),index};
}

void set_dimension(Json& properties, const char* canonical, const char* alias, double value) {
    properties[canonical] = value;
    if (properties.contains(alias)) properties[alias] = value;
}

// Follow the plan-axis command's receipt rule: changed semantic numbers lose
// only their old input expressions; unknown/opaque records are retained.
void invalidate_changed_quantity_entries(const Entity& before, Entity& after) {
    const auto found = before.properties.find("quantity_entries");
    if (found == before.properties.end()) return;
    if (!found->is_object()) throw std::invalid_argument("Quantity entries must be an object");
    Json retained = *found;
    for (const auto& [pointer,receipt] : found->items()) {
        (void)receipt;
        try {
            const Json::json_pointer path(pointer);
            if (before.properties.contains(path) &&
                (!after.properties.contains(path) || before.properties.at(path) != after.properties.at(path)))
                retained.erase(pointer);
        } catch (const Json::exception&) { /* Preserve opaque legacy receipt pointers. */ }
    }
    after.properties["quantity_entries"] = std::move(retained);
}

} // namespace

HostedOpeningResizeFrame hosted_opening_resize_frame(const DocumentSnapshot& source,
                                                     const std::string& opening_id) {
    const auto host = selected_host(source,opening_id);
    const auto& wall = host.wall;
    const auto& opening = wall.openings[host.selected_index];
    const double dx=wall.baseline.end.x-wall.baseline.start.x;
    const double dy=wall.baseline.end.y-wall.baseline.start.y;
    const double length=segment_length(wall.baseline);
    const auto span = hosted_opening_span(wall.baseline,opening.offset,opening.width);
    const double angle = std::atan2(dy,dx)+wall.baseline.sweep_radians*
        ((opening.offset+opening.width*.5)/length-.5);
    if (!std::isfinite(angle)) throw std::invalid_argument("Hosted opening tangent exceeds numeric range");
    return {span.start,span.end,wall.thickness,angle,opening.width,opening.height,
            wall.baseline,opening.offset};
}

ApplyEntityChanges hosted_opening_width_resize_command(const DocumentSnapshot& source,
    const std::string& opening_id, double relative_width_scale, bool keep_start_jamb) {
    if (!std::isfinite(relative_width_scale) || relative_width_scale <= 0)
        throw std::invalid_argument("Hosted opening width scale must be finite and positive");
    const auto host = selected_host(source,opening_id);
    const auto& opening = host.wall.openings[host.selected_index];
    const double width = opening.width*relative_width_scale;
    const double offset = keep_start_jamb ? opening.offset : opening.offset+opening.width-width;
    if (!std::isfinite(width) || !std::isfinite(offset) || width <= 0)
        throw std::invalid_argument("Hosted opening resize dimensions exceed the supported numeric range");

    const auto& original = source.entities().at(opening_id);
    Entity candidate = original;
    if (relative_width_scale != 1) {
        set_dimension(candidate.properties,"width_m","width",width);
        set_dimension(candidate.properties,"offset_m","offset",offset);
        invalidate_changed_quantity_entries(original,candidate);
    }
    // The scalar owner copy is the exact property-merge result, including
    // aliases, retained receipts, identity and opaque metadata/extensions.
    ApplyEntityChanges command{source.revision(),{}, {},"Resize hosted opening width"};
    if (candidate != original)
        command.entity_changes.push_back(EntityChange::upsert(std::move(candidate)));
    // Even a no-op must prove source editability and complete host geometry.
    const auto preview = Document::preview_command(source,command);
    validate_architectural_geometry_changes(source,preview,{opening_id});
    return command;
}

ApplyEntityChanges hosted_opening_offset_command(const DocumentSnapshot& source,
    const std::string& opening_id, double offset_metres) {
    if (!std::isfinite(offset_metres) || offset_metres < 0)
        throw std::invalid_argument("Hosted opening offset must be finite and nonnegative");
    const auto host = selected_host(source,opening_id);
    const auto& opening = host.wall.openings[host.selected_index];
    const double end = offset_metres+opening.width;
    if (!std::isfinite(end))
        throw std::invalid_argument("Hosted opening end station exceeds the supported numeric range");
    // Compare start stations so (length-width)+width cannot reject the exact
    // end stop merely because the addition rounds upward by one ulp.
    if (offset_metres != opening.offset && offset_metres > segment_length(host.wall.baseline)-opening.width)
        throw std::invalid_argument("Hosted opening extends beyond its host baseline");

    const auto& original = source.entities().at(opening_id);
    Entity candidate = original;
    if (offset_metres != opening.offset) {
        set_dimension(candidate.properties,"offset_m","offset",offset_metres);
        invalidate_changed_quantity_entries(original,candidate);
    }
    ApplyEntityChanges command{source.revision(),{}, {},"Move hosted opening along host"};
    if (candidate != original)
        command.entity_changes.push_back(EntityChange::upsert(std::move(candidate)));
    // Required admission includes every active sibling and manufactured assembly,
    // also when the station is unchanged and no entity change is emitted.
    const auto preview = Document::preview_command(source,command);
    validate_architectural_geometry_changes(source,preview,{opening_id});
    return command;
}
} // namespace sketch
