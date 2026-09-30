#include "sketch/hosted_opening_resize.hpp"

#include "sketch/architectural_document_adapter.hpp"
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
                                          const std::string& wall_id) {
    std::vector<const Entity*> result;
    for (const auto& [id,entity] : source.entities()) {
        (void)id;
        if (entity.type != "opening") continue;
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
    const auto found = source.entities().find(opening_id);
    if (found == source.entities().end())
        throw std::invalid_argument("Hosted opening resize target is missing");
    if (found->second.type != "opening")
        throw std::invalid_argument("Hosted opening resize requires an opening entity");
    std::string wall_id,error;
    if (!read_document_wall_id(found->second,wall_id,error)) throw std::invalid_argument(error);
    auto openings = hosted_openings(source,wall_id);
    auto wall = decode_host(source,wall_id,openings);
    const auto selected = std::find_if(wall.openings.begin(),wall.openings.end(),
        [&](const HostedOpening& opening) { return opening.id == opening_id; });
    if (selected == wall.openings.end())
        throw std::invalid_argument("Hosted opening resize target is missing from its host");
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

void validate_generated_host(const DocumentSnapshot& preview, const std::string& wall_id) {
    const auto openings = hosted_openings(preview,wall_id);
    const auto wall = decode_host(preview,wall_id,openings);
    (void)make_wall(wall);
    for (std::size_t i=0; i<openings.size(); ++i) {
        const auto& entity = *openings[i];
        const auto kind = entity.properties.find("opening_kind");
        const auto explicit_assembly = entity.properties.find("opening_assembly");
        std::optional<OpeningAssembly> assembly;
        if (explicit_assembly != entity.properties.end()) assembly = parse_opening_assembly(*explicit_assembly);
        else if (kind != entity.properties.end() && kind->is_string()) {
            const auto parsed = parse_opening_assembly_kind(kind->get<std::string>());
            if (parsed) assembly = default_opening_assembly(*parsed);
        }
        if (!assembly) continue; // Generic wall cuts have no manufactured frame.
        std::optional<DoorOperation> operation;
        if (entity.properties.contains("door_operation"))
            operation = decode_door_operation(entity.properties.at("door_operation"));
        (void)make_opening_assembly(wall,wall.openings[i],*assembly,operation);
    }

    // The resized cut also participates in each source wall's derived join.
    for (const auto& [id,entity] : preview.entities()) {
        if (entity.type != "wall_join") continue;
        const auto join = parse_wall_join(entity.properties,id);
        if (std::find(join.wall_ids.begin(),join.wall_ids.end(),wall_id) == join.wall_ids.end()) continue;
        std::vector<Wall> members;
        for (const auto& member_id : join.wall_ids)
            members.push_back(decode_host(preview,member_id,hosted_openings(preview,member_id)));
        (void)make_wall_join(join,members);
    }
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
    ArchitecturalOperation operation{ArchitecturalAction::property_edit,opening_id,{}, {},{},std::nullopt};
    // A property transaction retains exact entity identity/extensions and
    // does not replace typed receipts or derived proofs elsewhere in history.
    for (const auto& [key,value] : candidate.properties.items())
        if (!original.properties.contains(key) || original.properties.at(key) != value)
            operation.properties.emplace(key,value.dump());
    ApplyEntityChanges command{source.revision(),{}, {},"Resize hosted opening width"};
    if (!operation.properties.empty()) {
        const auto transaction = ArchitecturalTransaction::create("hosted-opening-width-resize",
            std::to_string(source.revision()),{opening_id},{std::move(operation)},command.message);
        command = architectural_transaction_command(source,transaction,source.revision());
    }
    // Even a no-op must prove source editability and complete host geometry.
    const auto preview = Document::preview_command(source,command);
    validate_generated_host(preview,host.wall.id);
    return command;
}
} // namespace sketch
