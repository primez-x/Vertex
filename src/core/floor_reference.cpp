#include "sketch/floor_reference.hpp"
#include "sketch/project_organization.hpp"

#include <cmath>
#include <stdexcept>

namespace sketch {
namespace {
void validate(const FloorReferenceSettings& settings) {
    if (settings.source_floor_id.empty())
        throw std::invalid_argument("tracing_reference source_floor_id must be nonempty");
    if (!std::isfinite(settings.opacity) || settings.opacity < 0.05 || settings.opacity > 0.75)
        throw std::invalid_argument("tracing_reference opacity must be between 0.05 and 0.75");
    if (!std::isfinite(settings.offset_m.x) || !std::isfinite(settings.offset_m.y) ||
        std::abs(settings.offset_m.x) > 1e6 || std::abs(settings.offset_m.y) > 1e6)
        throw std::invalid_argument("tracing_reference offset_m must be finite and within +/-1000000 metres");
}
}

FloorReferenceSettings FloorReferenceSettings::from_json(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 5 || !value.contains("version") ||
        !value.contains("source_floor_id") || !value.contains("visible") ||
        !value.contains("opacity") || !value.contains("offset_m") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.at("source_floor_id").is_string() || !value.at("visible").is_boolean() ||
        !value.at("opacity").is_number())
        throw std::invalid_argument("tracing_reference must have exactly the version 1 fields");
    const auto& offset = value.at("offset_m");
    if (!offset.is_object() || offset.size() != 2 || !offset.contains("x") ||
        !offset.contains("y") || !offset.at("x").is_number() || !offset.at("y").is_number())
        throw std::invalid_argument("tracing_reference offset_m must contain exactly numeric x and y");
    FloorReferenceSettings result{value.at("source_floor_id").get<std::string>(),
        value.at("visible").get<bool>(), value.at("opacity").get<double>(),
        {offset.at("x").get<double>(), offset.at("y").get<double>()}};
    validate(result);
    return result;
}

std::optional<FloorReferenceSettings> FloorReferenceSettings::from_entity(const Entity& floor) {
    if (floor.type != "floor" || !floor.properties.is_object())
        throw std::invalid_argument("tracing_reference requires a floor with object properties");
    const auto value = floor.properties.find("tracing_reference");
    if (value == floor.properties.end()) return std::nullopt;
    return from_json(*value);
}

nlohmann::json FloorReferenceSettings::to_json() const {
    validate(*this);
    return {{"version", 1}, {"source_floor_id", source_floor_id}, {"visible", visible},
        {"opacity", opacity}, {"offset_m", {{"x", offset_m.x}, {"y", offset_m.y}}}};
}

std::optional<FloorReferenceSettings> resolve_floor_reference(
    const DocumentSnapshot& snapshot, std::string_view destination_floor_id) {
    const auto& entities = snapshot.entities();
    const auto destination = entities.find(destination_floor_id);
    if (destination == entities.end() || destination->second.type != "floor")
        throw std::invalid_argument("tracing_reference destination floor is missing or has the wrong type");
    const auto settings = FloorReferenceSettings::from_entity(destination->second);
    if (!settings) return std::nullopt;
    if (settings->source_floor_id == destination_floor_id)
        throw std::invalid_argument("tracing_reference source and destination floors must be distinct");
    const auto source = entities.find(settings->source_floor_id);
    if (source == entities.end() || source->second.type != "floor")
        throw std::invalid_argument("tracing_reference source floor is missing or has the wrong type");
    const auto organization = organize_project(snapshot);
    const auto& destination_node = organization.nodes.at(destination->first);
    const auto& source_node = organization.nodes.at(source->first);
    const auto valid = [](const OrganizationNode& node) {
        return node.issues.empty() && !node.context.property_id.empty() &&
            !node.context.building_id.empty() && !node.context.floor_id.empty();
    };
    if (!valid(destination_node) || !valid(source_node))
        throw std::invalid_argument("tracing_reference requires valid floor building/property contexts");
    if (destination_node.context.property_id != source_node.context.property_id ||
        destination_node.context.building_id != source_node.context.building_id)
        throw std::invalid_argument("tracing_reference floors must share the same building and property");
    return settings;
}
}  // namespace sketch
