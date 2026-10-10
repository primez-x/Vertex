#include "sketch/corner_window.hpp"

#include "sketch/document.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr double tolerance = default_geometry_tolerance_metres;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Corner window: " + reason);
}

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        });
}

double number(const Json& value, const char* label) {
    if (!value.is_number()) reject(std::string(label) + " must be a finite number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) reject(std::string(label) + " must be a finite number");
    return result;
}

const Json& required(const Json& properties, const char* key) {
    const auto found = properties.find(key);
    if (found == properties.end()) reject(std::string("missing ") + key);
    return *found;
}

const Json& pair(const Json& properties, const char* key) {
    const auto& value = required(properties, key);
    if (!value.is_array() || value.size() != 2)
        reject(std::string(key) + " requires exactly two entries");
    return value;
}

void validate_value(const CornerWindow& value) {
    std::set<std::string, std::less<>> identities;
    for (const auto& id : {value.id, value.wall_ids[0], value.wall_ids[1],
                           value.opening_ids[0], value.opening_ids[1]})
        if (!valid_id(id) || !identities.insert(id).second)
            reject("owner, hosts and children require distinct bounded identifiers");
    for (const auto width : value.widths)
        if (!std::isfinite(width) || width <= tolerance)
            reject("leg widths must be finite and positive");
    if (!std::isfinite(value.sill) || value.sill < 0.0 ||
        !std::isfinite(value.height) || value.height <= tolerance ||
        !std::isfinite(value.sill + value.height))
        reject("sill and height must define a finite positive opening above the wall bottom");
    validate_opening_assembly(value.assembly);
    if (value.assembly.kind != OpeningAssemblyKind::window ||
        value.assembly.window_layout != WindowLayoutKind::fixed)
        reject("version one requires a fixed glazed window assembly");
    if (value.assembly.glazing_thickness_m <= tolerance)
        reject("both corner legs require positive physical glazing thickness");
    for (const auto width : value.widths)
        if (width - 2.0 * value.assembly.frame_width_m <= tolerance)
            reject("leg width leaves no positive glazing inside the frame");
    if (value.height - 2.0 * value.assembly.frame_width_m <= tolerance)
        reject("height leaves no positive glazing inside the frame");
}

const Entity& target(const Entities& entities, const std::string& id, const char* type) {
    const auto found = entities.find(id);
    if (found == entities.end() || found->second.id != id || found->second.type != type)
        reject(std::string("missing or wrong-type ") + type + " " + id);
    return found->second;
}

void match_dimension(const Json& properties, const char* canonical,
                     const char* legacy, double expected) {
    // The canonical decoder also accepts older scalar names. If both occur,
    // neither is allowed to hide a contradictory retained cut descriptor.
    const auto current = properties.find(canonical);
    const auto old = properties.find(legacy);
    if (current == properties.end() && old == properties.end())
        reject(std::string("child is missing ") + canonical);
    for (const auto* value : {current == properties.end() ? nullptr : &*current,
                             old == properties.end() ? nullptr : &*old})
        if (value && number(*value, canonical) != expected)
            reject(std::string("child ") + canonical + " differs from its owner's derived cut");
}

struct Membership {
    std::string registry;
    bool baseline{};
    std::set<std::string, std::less<>> proposals;
    std::set<std::string, std::less<>> demolitions;
    bool operator==(const Membership&) const = default;
};

using Memberships = std::map<std::string, Membership, std::less<>>;

Memberships memberships(const Entities& entities, const std::set<std::string, std::less<>>& relevant) {
    Memberships result;
    for (const auto& [registry_id, entity] : entities) {
        if (entity.type != "model_phases") continue;
        const auto model = ModelPhases::from_json(required(entity.properties, "model"));
        for (const auto& id : model.entity_ids()) {
            if (!relevant.contains(id)) continue;
            if (!result.emplace(id, Membership{registry_id, false, {}, {}}).second)
                reject("an aggregate participant belongs to multiple phase registries: " + id);
        }
        const auto member = [&](const std::string& id) -> Membership* {
            const auto found = result.find(id);
            return found != result.end() && found->second.registry == registry_id ? &found->second : nullptr;
        };
        for (const auto& id : model.baseline_ids())
            if (auto* value = member(id)) value->baseline = true;
        for (const auto& alternative : model.alternatives()) {
            for (const auto& id : alternative.proposed_ids)
                if (auto* value = member(id)) value->proposals.insert(alternative.id);
            for (const auto& id : alternative.demolished_ids)
                if (auto* value = member(id)) value->demolitions.insert(alternative.id);
        }
    }
    return result;
}

void validate_memberships(const CornerWindow& value, const Memberships& all) {
    const auto owner = all.find(value.id);
    const Membership unphased;
    const auto& expected = owner == all.end() ? unphased : owner->second;
    for (const auto& id : value.opening_ids) {
        const auto child = all.find(id);
        const auto& actual = child == all.end() ? unphased : child->second;
        if (actual != expected) reject("owner and both children must share every saved phase membership");
    }
    for (const auto& id : value.wall_ids) {
        const auto host = all.find(id);
        const auto& actual = host == all.end() ? unphased : host->second;
        if (actual.registry != expected.registry)
            reject("hosts and window must share phase registry ownership");
        if (expected.registry.empty()) continue;
        if (expected.baseline && !actual.baseline)
            reject("a baseline corner window requires baseline hosts");
        for (const auto& alternative : expected.proposals)
            if ((!actual.baseline && !actual.proposals.contains(alternative)) ||
                actual.demolitions.contains(alternative))
                reject("a proposed corner window requires live hosts in its saved alternative");
        // A baseline window survives every alternative unless explicitly
        // demolished there. Removing a host must retire the complete window.
        if (expected.baseline)
            for (const auto& alternative : actual.demolitions)
                if (!expected.demolitions.contains(alternative))
                    reject("host demolition must retire the complete corner window");
    }
}

const Json& optional_property(const Entity& entity, const char* key) {
    static const Json absent;
    const auto found = entity.properties.find(key);
    return found == entity.properties.end() ? absent : *found;
}

void validate_context(const Entity& entity, const Entity& host) {
    // Core does not infer placement or repair memberships. The same raw floor
    // reference necessarily shares its one retained vertical-level binding;
    // equivalent contexts expressed with different raw references refuse.
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "level_id"}) {
        const auto& actual = optional_property(entity, key);
        const auto& expected = optional_property(host, key);
        if (actual != expected || (!actual.is_null() && (!actual.is_string() ||
            !valid_id(actual.get_ref<const std::string&>()))))
            reject("owner, hosts and cuts require identical raw drawing context and level references");
        if (entity.properties.contains(key) && actual.is_null())
            reject("explicit context references cannot be null");
    }
}

void validate_vertical_placement(const Entities& entities, const Entity& host) {
    if (!host.properties.contains("vertical_placement")) return;
    const auto& value = host.properties.at("vertical_placement");
    if (!value.is_object() || value.size() != 3 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.contains("mode") || !value.at("mode").is_string() || !value.contains("offset_m"))
        reject("host vertical placement requires version one, mode and offset_m");
    const auto mode = value.at("mode").get<std::string>();
    const auto offset = number(value.at("offset_m"), "vertical placement offset_m");
    if ((mode != "absolute" && mode != "level") || std::abs(offset) > 1e9 ||
        (mode == "level" && !host.properties.contains("floor_id")))
        reject("host vertical placement requires a supported mode and common floor binding");
    if (mode == "level") {
        const auto& floor_id = host.properties.at("floor_id");
        if (!floor_id.is_string()) reject("level-placed host requires a floor reference");
        const auto& floor = target(entities, floor_id.get<std::string>(), "floor");
        if (!floor.properties.contains("vertical_level_binding") ||
            !host.properties.contains("elevation_m"))
            reject("level-placed host requires a retained floor level binding and elevation_m");
        // Document admission decodes and resolves this known binding and its
        // graph. Keeping the exact common floor avoids a core dependency on
        // organization or a second competing vertical-level decoder.
    }
}

bool may_coexist(const std::string& owner_id, const std::string& other_id,
                 const Memberships& all) {
    const auto owner = all.find(owner_id);
    const auto other = all.find(other_id);
    // Unmanaged records are conservatively present in every saved state.
    if (owner == all.end() || other == all.end() ||
        owner->second.registry != other->second.registry) return true;
    const auto& left = owner->second;
    const auto& right = other->second;
    if (left.baseline && right.baseline) return true;
    if (left.baseline) {
        for (const auto& alternative : right.proposals)
            if (!left.demolitions.contains(alternative)) return true;
    } else if (right.baseline) {
        for (const auto& alternative : left.proposals)
            if (!right.demolitions.contains(alternative)) return true;
    } else {
        for (const auto& alternative : left.proposals)
            if (right.proposals.contains(alternative)) return true;
    }
    return false;
}
} // namespace

CornerWindow parse_corner_window(const Entity& entity) {
    if (entity.type != "corner_window" || !entity.properties.is_object())
        reject("requires a corner_window entity with object properties");
    const auto& properties = entity.properties;
    const auto& version = required(properties, "version");
    if (!version.is_number_integer() || version != 1) reject("unsupported version");
    if (properties.contains("door_operation")) reject("cannot carry a door operation");
    CornerWindow value;
    value.id = entity.id;
    const auto& walls = pair(properties, "wall_ids");
    const auto& children = pair(properties, "opening_ids");
    const auto& ends = pair(properties, "at_start");
    const auto& widths = pair(properties, "widths_m");
    for (std::size_t leg = 0; leg < 2; ++leg) {
        if (!walls[leg].is_string() || !children[leg].is_string() || !ends[leg].is_boolean())
            reject("host/child IDs must be strings and selected endpoints must be booleans");
        value.wall_ids[leg] = walls[leg].get<std::string>();
        value.opening_ids[leg] = children[leg].get<std::string>();
        value.at_start[leg] = ends[leg].get<bool>();
        value.widths[leg] = number(widths[leg], "widths_m");
    }
    value.sill = number(required(properties, "sill_m"), "sill_m");
    value.height = number(required(properties, "height_m"), "height_m");
    value.assembly = parse_opening_assembly(required(properties, "opening_assembly"));
    validate_value(value);
    return value;
}

Json corner_window_properties(const CornerWindow& value) {
    validate_value(value);
    return {{"version", 1}, {"wall_ids", value.wall_ids}, {"opening_ids", value.opening_ids},
        {"at_start", value.at_start}, {"widths_m", value.widths}, {"sill_m", value.sill},
        {"height_m", value.height}, {"opening_assembly", opening_assembly_json(value.assembly)}};
}

std::array<HostedOpening, 2> corner_window_cuts(const CornerWindow& value,
                                             const std::array<Wall, 2>& walls) {
    validate_value(value);
    std::array<HostedOpening, 2> cuts;
    std::array<Vec2, 2> endpoints;
    std::array<Vec2, 2> directions;
    for (std::size_t leg = 0; leg < 2; ++leg) {
        const auto& wall = walls[leg];
        if (wall.id != value.wall_ids[leg]) reject("host order differs from wall_ids");
        if (wall.baseline.sweep_radians != 0.0) reject("version one requires straight walls");
        validate_wall_semantics(wall);
        const double dx = wall.baseline.end.x - wall.baseline.start.x;
        const double dy = wall.baseline.end.y - wall.baseline.start.y;
        const double length = std::hypot(dx, dy);
        if (!std::isfinite(length) || value.widths[leg] > length)
            reject("leg width exceeds its host length");
        endpoints[leg] = value.at_start[leg] ? wall.baseline.start : wall.baseline.end;
        const double sign = value.at_start[leg] ? 1.0 : -1.0;
        directions[leg] = {sign * dx / length, sign * dy / length};
        cuts[leg] = {value.opening_ids[leg], value.at_start[leg] ? 0.0 : length - value.widths[leg],
                     value.widths[leg], value.sill, value.height};
        auto checked = wall;
        // A decoded complete host may already contain this exact cut.
        const auto present = std::find_if(checked.openings.begin(), checked.openings.end(),
            [&](const HostedOpening& opening) { return opening.id == cuts[leg].id; });
        if (present == checked.openings.end()) checked.openings.push_back(cuts[leg]);
        else if (*present != cuts[leg]) reject("retained cut differs from the aggregate");
        validate_wall_semantics(checked);
    }
    if (std::hypot(endpoints[0].x - endpoints[1].x, endpoints[0].y - endpoints[1].y) > tolerance)
        reject("selected wall endpoints do not share a corner");
    const auto cross = directions[0].x * directions[1].y - directions[0].y * directions[1].x;
    if (!std::isfinite(cross) || std::abs(cross) <= tolerance)
        reject("host walls must form a noncollinear corner");
    if (std::abs(walls[0].elevation - walls[1].elevation) > tolerance)
        reject("hosts must have the same actual bottom elevation");
    return cuts;
}

void validate_corner_window_state(const Entities& entities) {
    std::vector<CornerWindow> windows;
    std::set<std::string, std::less<>> relevant;
    std::map<std::string, std::pair<std::string, std::size_t>, std::less<>> children;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "corner_window") continue;
        if (id != entity.id) reject("entity map key differs from owner identity");
        auto value = parse_corner_window(entity);
        relevant.insert(id);
        for (std::size_t leg = 0; leg < 2; ++leg) {
            relevant.insert(value.wall_ids[leg]);
            relevant.insert(value.opening_ids[leg]);
            if (!children.emplace(value.opening_ids[leg], std::pair{id, leg}).second)
                reject("a cut is owned by multiple corner windows");
        }
        windows.push_back(std::move(value));
    }
    // Backlinks remain governed even when the last owner has been deleted.
    for (const auto& [id, entity] : entities) {
        if (!entity.properties.is_object()) continue;
        const bool backlink = entity.properties.contains("corner_window_id");
        const bool indexed = entity.properties.contains("corner_leg");
        if (!backlink && !indexed) continue;
        const auto owned = children.find(id);
        if (entity.id != id || entity.type != "opening" || owned == children.end() || !backlink || !indexed ||
            entity.properties.at("corner_window_id") != owned->second.first ||
            !entity.properties.at("corner_leg").is_number_integer() ||
            entity.properties.at("corner_leg") != owned->second.second)
            reject("orphan, extra or wrongly indexed corner cut " + id);
    }
    if (windows.empty()) return;
    // Index ordinary cuts once. Only pairs that can coexist with this window
    // participate in overlap checks; an inactive replacement remains retained.
    std::map<std::string, std::vector<const Entity*>, std::less<>> hosted;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "opening") continue;
        std::string host_id, error;
        if (read_document_wall_id(entity, host_id, error) && relevant.contains(host_id)) {
            hosted[host_id].push_back(&entity);
            relevant.insert(id);
        }
    }
    const auto phases = memberships(entities, relevant);
    for (const auto& value : windows) {
        const auto& owner = target(entities, value.id, "corner_window");
        std::array<Wall, 2> hosts;
        const auto& first_host = target(entities, value.wall_ids[0], "wall");
        validate_context(owner, first_host);
        if (owner.properties.contains("vertical_placement"))
            reject("the owner inherits its hosts' physical elevation");
        validate_memberships(value, phases);
        for (std::size_t leg = 0; leg < 2; ++leg) {
            const auto& host = target(entities, value.wall_ids[leg], "wall");
            validate_context(host, first_host);
            validate_vertical_placement(entities, host);
            if (optional_property(host, "vertical_placement") != optional_property(first_host, "vertical_placement"))
                reject("hosts require identical retained vertical placement");
            const auto& child = target(entities, value.opening_ids[leg], "opening");
            validate_context(child, first_host);
            const auto& properties = child.properties;
            if (!properties.is_object() || !properties.contains("corner_window_id") ||
                properties.at("corner_window_id") != value.id || !properties.contains("corner_leg") ||
                !properties.at("corner_leg").is_number_integer() || properties.at("corner_leg") != leg ||
                !properties.contains("wall_id") || properties.at("wall_id") != value.wall_ids[leg] ||
                !properties.contains("opening_kind") || properties.at("opening_kind") != "opening" ||
                properties.contains("opening_assembly") || properties.contains("door_operation") ||
                properties.contains("vertical_placement"))
                reject("child must be its owner's indexed bare wall cut");
            std::string error;
            if (!read_document_wall(host, {}, hosts[leg], error)) reject(error);
        }
        const auto cuts = corner_window_cuts(value, hosts);
        for (std::size_t leg = 0; leg < 2; ++leg) {
            const auto& properties = entities.at(value.opening_ids[leg]).properties;
            match_dimension(properties, "offset_m", "offset", cuts[leg].offset);
            match_dimension(properties, "width_m", "width", cuts[leg].width);
            match_dimension(properties, "sill_m", "sill", cuts[leg].sill);
            match_dimension(properties, "height_m", "height", cuts[leg].height);
            const auto& host = entities.at(value.wall_ids[leg]);
            const auto found = hosted.find(host.id);
            if (found == hosted.end()) continue;
            for (const auto* other : found->second) {
                if (other->id == cuts[leg].id || !may_coexist(value.id, other->id, phases)) continue;
                Wall checked;
                std::string error;
                if (!read_document_wall(host, {other}, checked, error)) reject(error);
                checked.openings.push_back(cuts[leg]);
                validate_wall_semantics(checked);
            }
        }
    }
}

} // namespace sketch
