#include "sketch/dxf_architectural_source.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/terrain_surface.hpp"
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
#include "sketch/building_plan_projection.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/roof_entity_codec.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
std::string owner_id(const Json& value) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
        value.get_ref<const std::string&>().size() > 255)
        throw std::invalid_argument("V8 architectural reference identity invalid");
    return value.get<std::string>();
}
void patch(Json& object, const char* slot,
    const std::map<std::string, std::string, std::less<>>& mapping) {
    if (object.contains(slot)) object[slot] = mapping.at(owner_id(object.at(slot)));
}
std::size_t raw_nodes(const Json& value) {
    std::size_t nodes = 0;
    const auto visit = [&](const auto& self, const Json& child, std::size_t depth) -> void {
        if (depth > 24 || ++nodes > 8192) throw std::invalid_argument("V8 architectural raw JSON limit");
        if (child.is_string() && child.get_ref<const std::string&>().size() > 8192)
            throw std::invalid_argument("V8 architectural raw string limit");
        if (child.is_object()) for (const auto& [key, entry] : child.items()) {
            if (key.size() > 8192) throw std::invalid_argument("V8 architectural raw key limit");
            self(self, entry, depth + 1);
        }
        else if (child.is_array()) for (const auto& entry : child) self(self, entry, depth + 1);
    };
    visit(visit, value, 0); return nodes;
}
std::size_t bounded_count(const Json& value, std::size_t maximum) {
    if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument("V8 architectural raw count invalid");
    const auto count = value.get<std::uint64_t>();
    if (!count || count > maximum) throw std::invalid_argument("V8 architectural raw count limit");
    return static_cast<std::size_t>(count);
}
bool legacy_stair_host(const Entity& host) {
    if (host.type != "stair") throw std::invalid_argument("V8 railing actual host is not a stair");
    const auto& properties = host.properties;
    const auto& version = properties.at("version");
    if (!version.is_number_integer()) throw std::invalid_argument("V8 railing host dialect invalid");
    if (version == 1 && properties.at("form") == "straight_stair_flight" &&
        !properties.contains("flights") && !properties.contains("landings")) return true;
    if ((version == 2 || version == 3 || version == 4) && properties.at("form") == "multi_flight_stair" &&
        properties.contains("flights") && properties.contains("landings")) return false;
    throw std::invalid_argument("V8 railing host dialect invalid");
}
std::vector<std::pair<const char*, const char*>> railing_witness_slots(const Entity& source) {
    const auto& version = source.properties.at("version");
    if (!version.is_number_integer()) throw std::invalid_argument("V8 railing dialect invalid");
    if (version == 2 && source.properties.at("form") == "stair_flight_railing")
        return {{"flight_id", "flights"}};
    if (version == 3 && source.properties.at("form") == "stair_landing_railing") {
        const auto& host = source.properties.at("host");
        if (host.at("role") == "connecting") return {{"incoming_flight_id", "flights"},
            {"landing_id", "landings"}, {"outgoing_flight_id", "flights"}};
        if (host.at("role") == "top" && !host.contains("landing_id") && !host.contains("outgoing_flight_id"))
            return {{"incoming_flight_id", "flights"}};
    }
    throw std::invalid_argument("V8 railing host witness dialect invalid");
}
std::size_t independent_compound_edges(const Entity& source, const Entity& catalog) {
    // The complete raw catalog admission runs first. Count the actual selected
    // type's profiles with part multiplicity before expansion or native HLR.
    const auto& model = catalog.properties.at("model");
    std::map<std::string, const Json*, std::less<>> types;
    for (const auto& row : model.at("types")) types.emplace(row.at("id").get<std::string>(), &row);
    std::map<std::string, std::size_t, std::less<>> sizes;
    std::set<std::string, std::less<>> visiting;
    const auto add = [](std::size_t& total, std::size_t amount) {
        if (total > 8192 || amount > 8192 - total) throw std::invalid_argument("V8 independent compound topology limit");
        total += amount;
    };
    const auto visit = [&](const auto& self, const std::string& id, std::size_t depth) -> std::size_t {
        if (depth > 32 || visiting.contains(id)) throw std::invalid_argument("V8 independent compound graph limit");
        if (const auto found = sizes.find(id); found != sizes.end()) return found->second;
        visiting.insert(id);
        const auto& row = *types.at(id);
        std::size_t edges = 0;
        if (row.contains("profiles")) for (const auto& profile : row.at("profiles")) {
            std::size_t segments = profile.at("outer").size();
            for (const auto& hole : profile.at("holes")) segments += hole.size();
            // Every extrusion retains its outer/hole edges; the conservative
            // factor includes cap, side and seam edge families.
            add(edges, segments * 6);
        }
        if (row.contains("parts")) for (const auto& part : row.at("parts"))
            add(edges, self(self, part.at("type_id").template get<std::string>(), depth + 1));
        visiting.erase(id); sizes.emplace(id, edges); return edges;
    };
    return visit(visit, source.properties.at("instance").at("type_id").get<std::string>(), 1);
}

std::string phase_owner_id(const Json& value) {
    if (!value.is_string()) throw std::invalid_argument("phase auxiliary owner identity invalid");
    const auto& id = value.get_ref<const std::string&>();
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    })) throw std::invalid_argument("phase auxiliary owner identity invalid");
    return id;
}

// The terrain ceiling accommodates the full 4096-point/8192-triangle dialect.
// Do not reuse the smaller V8 raw ceiling or change that historical contract.
std::size_t phase_raw_nodes(const Json& value) {
    std::size_t nodes = 0;
    const auto visit = [&](const auto& self, const Json& child, std::size_t depth) -> void {
        if (depth > 24 || ++nodes > 65'536)
            throw std::invalid_argument("phase auxiliary raw JSON limit");
        if (child.is_string() && child.get_ref<const std::string&>().size() > 8192)
            throw std::invalid_argument("phase auxiliary raw string limit");
        if (child.is_object()) for (const auto& [key, entry] : child.items()) {
            if (key.size() > 8192) throw std::invalid_argument("phase auxiliary raw key limit");
            self(self, entry, depth + 1);
        }
        else if (child.is_array()) for (const auto& entry : child) self(self, entry, depth + 1);
    };
    visit(visit, value, 0);
    return nodes;
}

std::size_t phase_raw_envelope(const Entity& source) {
    (void)phase_owner_id(Json(source.id));
    if (!source.properties.is_object() || !source.extensions.is_object())
        throw std::invalid_argument("phase auxiliary envelope invalid");
    return phase_raw_nodes(source.properties) + phase_raw_nodes(source.extensions);
}

std::vector<std::string> phase_raw_wall_ids(const Entity& source) {
    const auto& properties = source.properties;
    if (source.type != "wall_join" || !properties.is_object() || properties.size() != 3 ||
        !properties.contains("version") || !properties.contains("style") || !properties.contains("wall_ids") ||
        !properties.at("version").is_number_integer() || properties.at("version") != 1 ||
        properties.at("style") != "fused")
        throw std::invalid_argument("phase auxiliary wall join dialect invalid");
    const auto& rows = properties.at("wall_ids");
    if (!rows.is_array() || rows.size() < 2 || rows.size() > 32)
        throw std::invalid_argument("phase auxiliary wall join raw member limit");
    std::vector<std::string> result;
    std::set<std::string, std::less<>> unique;
    for (const auto& row : rows) {
        auto id = phase_owner_id(row);
        if (!unique.insert(id).second) throw std::invalid_argument("phase auxiliary wall join repeated member");
        result.push_back(std::move(id));
    }
    return result;
}

std::pair<std::size_t, std::size_t> phase_raw_terrain_counts(const Entity& source) {
    const auto& model = source.properties.at("model");
    if (!model.is_object()) throw std::invalid_argument("phase auxiliary terrain model invalid");
    const auto& points = model.at("points");
    const auto& triangles = model.at("triangles");
    if (!points.is_array() || points.size() < 3 || points.size() > TerrainSurface::maximum_points ||
        !triangles.is_array() || triangles.empty() || triangles.size() > TerrainSurface::maximum_triangles)
        throw std::invalid_argument("phase auxiliary terrain raw count limit");
    for (const auto& point : points) if (!point.is_object() || point.size() != 4 ||
        !point.contains("id") || !point.contains("x_m") || !point.contains("y_m") || !point.contains("elevation_m"))
        throw std::invalid_argument("phase auxiliary terrain raw point invalid");
    for (const auto& triangle : triangles) if (!triangle.is_array() || triangle.size() != 3)
        throw std::invalid_argument("phase auxiliary terrain raw triangle invalid");
    return {points.size(), triangles.size()};
}

const Entity& phase_actual_owner(const std::string& id, const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
    const auto found = authored.find(id);
    if (id != source.id && found == authored.end())
        throw std::invalid_argument("phase auxiliary actual member missing");
    const auto& owner = id == source.id ? source : found->second;
    if (owner.id != id) throw std::invalid_argument("phase auxiliary actual owner identity differs");
    return owner;
}

// Source is a candidate replacement, so inspect it once and every other actual
// join. This matches Document's global exclusivity, including inactive members.
void phase_validate_join_graph(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
    std::map<std::string, std::string, std::less<>> ownership;
    const auto check = [&](const Entity& owner) {
        if (owner.type != "wall_join") return;
        (void)phase_raw_envelope(owner);
        (void)phase_raw_wall_ids(owner);
        const auto join = parse_wall_join(owner.properties, owner.id);
        for (const auto& id : join.wall_ids) {
            if (phase_actual_owner(id, source, authored).type != "wall")
                throw std::invalid_argument("phase auxiliary wall join actual member missing/type differs");
            if (!ownership.emplace(id, owner.id).second)
                throw std::invalid_argument("phase auxiliary wall belongs to multiple joins");
        }
    };
    check(source);
    for (const auto& [id, owner] : authored) {
        if (id != owner.id) throw std::invalid_argument("phase auxiliary actual map identity differs");
        if (id != source.id) check(owner);
    }
}

std::map<std::string, std::vector<const Entity*>, std::less<>> phase_wall_openings(
    const std::vector<std::string>& members, const std::map<std::string, Entity, std::less<>>& authored) {
    std::map<std::string, std::vector<const Entity*>, std::less<>> result;
    for (const auto& id : members) result.emplace(id, std::vector<const Entity*>{});
    for (const auto& [id, owner] : authored) {
        if (id != owner.id) throw std::invalid_argument("phase auxiliary actual map identity differs");
        if (owner.type != "opening") continue;
        const auto host = phase_owner_id(owner.properties.at("wall_id"));
        const auto found = result.find(host);
        if (found != result.end()) {
            if (found->second.size() >= 128) throw std::invalid_argument("phase auxiliary raw hosted opening limit");
            found->second.push_back(&owner);
        }
    }
    return result;
}

std::vector<Wall> phase_decode_join_walls(const Entity& source,
    const std::vector<std::string>& members, const std::map<std::string, Entity, std::less<>>& authored) {
    const auto openings = phase_wall_openings(members, authored);
    // Reserve raw shapes for every member before the shared placement pass.
    for (const auto& id : members) {
        const auto& wall = phase_actual_owner(id, source, authored);
        (void)phase_raw_envelope(wall);
        if (wall.properties.contains("layers") && (!wall.properties.at("layers").is_array() ||
            wall.properties.at("layers").size() > 32))
            throw std::invalid_argument("phase auxiliary wall join raw layer limit");
        for (const auto* opening : openings.at(id)) (void)phase_raw_envelope(*opening);
    }
    const auto placed = resolve_vertical_placements(authored, members);
    std::vector<Wall> result;
    result.reserve(members.size());
    for (const auto& id : members) {
        Wall wall;
        std::string error;
        if (!read_document_wall(placed.at(id), openings.at(id), wall, error))
            throw std::invalid_argument(error.empty() ? "phase auxiliary physical wall decode failed" : error);
        validate_wall_semantics(wall);
        result.push_back(std::move(wall));
    }
    return result;
}
}

bool native_dxf_architectural_source_type(std::string_view type) noexcept {
    return type == "slab" || type == "roof" || type == "stair" || type == "railing" ||
        type == "column" || type == "beam" || type == "room" || type == "roof_join" || type == "assembly_instance";
}

std::vector<std::string> native_dxf_architectural_source_catalog_ids(const Entity& source) {
    std::set<std::string> ids;
    for (const auto& reference : architectural_material_source_refs(source)) ids.insert(reference.catalog_id);
    if (source.type == "assembly_instance") ids.insert(owner_id(source.properties.at("assembly_catalog_id")));
    return {ids.begin(), ids.end()};
}

Json native_dxf_architectural_source_dependencies(const Entity& source) {
    Json result{{"roof_ids", Json::array()}, {"stair_id", ""}, {"level_graph_id", ""}};
    if (source.type == "roof_join") {
        const auto& roofs = source.properties.at("roof_ids");
        if (!roofs.is_array() || roofs.empty() || roofs.size() > 32)
            throw std::invalid_argument("V8 roof join raw member limit");
        std::set<std::string> unique;
        for (const auto& id : roofs) if (!unique.insert(owner_id(id)).second)
            throw std::invalid_argument("V8 roof join repeated owner");
        result["roof_ids"] = roofs;
    }
    if (source.type == "railing" && source.properties.contains("host")) {
        const auto& host = source.properties.at("host");
        if (!host.is_object()) throw std::invalid_argument("V8 railing host invalid");
        result["stair_id"] = owner_id(host.at("stair_id"));
    }
    if (source.type == "stair" && source.properties.contains("level_connection")) {
        const auto& connection = source.properties.at("level_connection");
        if (!connection.is_object()) throw std::invalid_argument("V8 stair level connection invalid");
        result["level_graph_id"] = owner_id(connection.at("graph_id"));
    }
    return result;
}

Entity remap_native_dxf_independent_assembly_source(const Entity& source,
    const std::map<std::string, std::string, std::less<>>& bodies,
    const std::map<std::string, std::string, std::less<>>& catalogs) {
    const auto catalog_id = owner_id(source.properties.at("assembly_catalog_id"));
    const std::map<std::string, std::string> mapping{{source.id, bodies.at(source.id)}, {catalog_id, catalogs.at(catalog_id)}};
    // The shared adapter validates both sides and patches only owner IDs.
    // Raw instance numbers, dialect and row order survive every caller.
    return remap_independent_assembly_instance(source, mapping);
}

void remap_native_dxf_architectural_source_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& mapping) {
    (void)native_dxf_architectural_source_dependencies(source);
    auto result = source;
    if (result.type == "roof_join") for (auto& id : result.properties["roof_ids"]) id = mapping.at(owner_id(id));
    if (result.type == "railing" && result.properties.contains("host")) patch(result.properties["host"], "stair_id", mapping);
    source = std::move(result);
}

std::vector<std::string> native_dxf_architectural_child_identity_ids(const Entity& source) {
    std::set<std::string> ids;
    if (source.type == "stair") for (const auto* slot : {"flights", "landings"}) if (source.properties.contains(slot)) {
        const auto& rows = source.properties.at(slot);
        if (!rows.is_array() || rows.size() > 256) throw std::invalid_argument("V8 stair child raw limit");
        for (const auto& row : rows) if (!row.is_object() || !ids.insert(owner_id(row.at("id"))).second)
            throw std::invalid_argument("V8 stair child identity invalid/duplicate");
    }
    return {ids.begin(), ids.end()};
}

void remap_native_dxf_architectural_source_context_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& mapping) {
    auto result = source;
    if (source.type == "stair" && result.properties.contains("level_connection"))
        patch(result.properties["level_connection"], "graph_id", mapping);
    source = std::move(result);
}

void remap_native_dxf_architectural_host_body_aliases(Entity& source, const Entity& authored_host,
    const std::map<std::string, std::string, std::less<>>& mapping) {
    if (source.type != "railing" || !source.properties.contains("host")) return;
    auto result = source;
    auto& host = result.properties["host"];
    if (owner_id(host.at("stair_id")) != authored_host.id)
        throw std::invalid_argument("V8 railing actual host identity differs");
    const auto slots = railing_witness_slots(source);
    if (legacy_stair_host(authored_host)) for (const auto& [slot, roster] : slots) {
        if (std::string_view(roster) != "flights" || owner_id(host.at(slot)) != authored_host.id)
            throw std::invalid_argument("V8 straight stair witness differs from body alias");
        host[slot] = mapping.at(authored_host.id);
    }
    source = std::move(result);
}

void remap_native_dxf_architectural_child_identities(Entity& source,
    const std::map<std::string, std::string, std::less<>>& mapping, const Entity* authored_host) {
    auto result = source;
    const auto children = native_dxf_architectural_child_identity_ids(source);
    std::set<std::string> targets;
    for (const auto& id : children) if (!targets.insert(owner_id(Json(mapping.at(id)))).second)
        throw std::invalid_argument("V8 child mapping not injective");
    if (source.type == "stair") for (const auto* slot : {"flights", "landings"}) if (result.properties.contains(slot))
        for (auto& row : result.properties[slot]) patch(row, "id", mapping);
    if (source.type == "railing" && result.properties.contains("host")) {
        if (!authored_host) throw std::invalid_argument("V8 railing child remap requires actual authored host");
        const auto legacy = legacy_stair_host(*authored_host);
        for (const auto& [slot, roster] : railing_witness_slots(source)) {
            auto& host = result.properties["host"];
            const auto witness = owner_id(host.at(slot));
            if (legacy) {
                // The BODY pass already moved this alias. Never consult a
                // child map, even if an unrelated child has the same spelling.
                if (std::string_view(roster) != "flights" || witness != owner_id(host.at("stair_id")))
                    throw std::invalid_argument("V8 remapped stair body alias differs");
            } else {
                const auto& rows = authored_host->properties.at(roster);
                if (!rows.is_array() || rows.size() > 256 || std::none_of(rows.begin(), rows.end(), [&](const auto& row) {
                    return row.is_object() && row.contains("id") && row.at("id") == witness;
                })) throw std::invalid_argument("V8 railing witness is not a declared host child");
                patch(host, slot, mapping);
            }
        }
    }
    source = std::move(result);
}

std::optional<DrawingContext> native_dxf_architectural_source_context(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored, const ProjectOrganization& organization) {
    if (source.type != "roof_join") return organization.drawing_context(source.id);
    const auto join = parse_roof_join(source.properties, source.id);
    std::optional<DrawingContext> result;
    for (const auto& id : join.roof_ids) {
        const auto found = authored.find(id);
        if (found == authored.end() || found->second.type != "roof")
            throw std::invalid_argument("V8 roof join actual member missing/type differs");
        const auto context = organization.drawing_context(id);
        if (!context || !context->complete()) throw std::invalid_argument("V8 roof join member hierarchy unresolved");
        if (result && *result != *context) throw std::invalid_argument("V8 roof join member contexts conflict");
        result = context;
    }
    return result;
}

void admit_native_dxf_architectural_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored, NativeDxfWallSourceWorkBudget& budget) {
    if (!native_dxf_architectural_source_type(source.type)) return;
    constexpr std::size_t limit = 67'108'864;
    const auto charge = [&](std::size_t amount) {
        if (budget.architectural_work > limit || amount > limit - budget.architectural_work)
            throw std::invalid_argument("V8 cumulative architectural work limit");
        budget.architectural_work += amount;
    };
    if (!source.properties.is_object() || !source.extensions.is_object())
        throw std::invalid_argument("V8 architectural envelope invalid");
    const auto nodes = raw_nodes(source.properties) + raw_nodes(source.extensions);
    charge(nodes * 32);
    std::size_t complexity = 24;
    if (source.type == "slab" || source.type == "room") {
        const auto& boundary = source.properties.contains("boundary") ? source.properties.at("boundary") : source.properties.at("segments");
        if (!boundary.is_array() || boundary.size() > 128) throw std::invalid_argument("V8 architectural profile limit");
        complexity = boundary.size();
        if (source.properties.contains("holes")) {
            const auto& holes = source.properties.at("holes");
            if (!holes.is_array() || holes.size() > 32) throw std::invalid_argument("V8 architectural hole limit");
            for (const auto& hole : holes) {
                if (!hole.is_array() || hole.size() > 128 - complexity) throw std::invalid_argument("V8 architectural profile limit");
                complexity += hole.size();
            }
        }
        complexity *= 6; // extruded profile edge families, including holes
    } else if (source.type == "stair") {
        complexity = bounded_count(source.properties.at("riser_count"), 256);
        (void)native_dxf_architectural_child_identity_ids(source);
        if (source.properties.contains("flights")) for (const auto& row : source.properties.at("flights"))
            (void)bounded_count(row.at("riser_count"), 256);
        complexity = complexity * 12 + native_dxf_architectural_child_identity_ids(source).size() * 16 + 16;
    } else if (source.type == "railing") {
        const auto& spacing = source.properties.at("post_spacing_m");
        if (!spacing.is_number() || !std::isfinite(spacing.get<double>()) || spacing.get<double>() <= 0)
            throw std::invalid_argument("V8 railing raw post spacing invalid");
        double length = 0;
        if (source.properties.contains("host")) {
            const auto& host = authored.at(owner_id(source.properties.at("host").at("stair_id")));
            if (host.type != "stair") throw std::invalid_argument("V8 railing host type differs");
            charge(raw_nodes(host.properties) * 32);
            const auto host_risers = bounded_count(host.properties.at("riser_count"), 256);
            const auto host_edges = host_risers * 12 + native_dxf_architectural_child_identity_ids(host).size() * 16 + 16;
            if (host_edges > 8192) throw std::invalid_argument("V8 railing host topology limit");
            charge(host_edges * host_edges * 32);
            const auto& going = host.properties.at("going_m"); const auto& width = host.properties.at("width_m");
            if (!going.is_number() || !width.is_number()) throw std::invalid_argument("V8 railing raw host dimensions invalid");
            double maximum_going = going.get<double>(), maximum_width = width.get<double>(), landing_depth = 0;
            if (host.properties.contains("flights")) for (const auto& row : host.properties.at("flights")) {
                if (row.contains("going_m")) maximum_going = std::max(maximum_going, row.at("going_m").get<double>());
                if (row.contains("width_m")) maximum_width = std::max(maximum_width, row.at("width_m").get<double>());
            }
            if (host.properties.contains("landings")) for (const auto& row : host.properties.at("landings"))
                landing_depth += row.at("depth_m").get<double>() + std::abs(row.value("return_gap_m", 0.0));
            if (host.properties.contains("top_landing") && !host.properties.at("top_landing").is_null())
                landing_depth += host.properties.at("top_landing").at("depth_m").get<double>();
            const auto& rise = host.properties.at("total_rise_m");
            if (!rise.is_number() || !std::isfinite(rise.get<double>()) || rise.get<double>() <= 0)
                throw std::invalid_argument("V8 railing raw stair rise invalid");
            length = maximum_going * static_cast<double>(bounded_count(host.properties.at("riser_count"), 256)) +
                std::abs(rise.get<double>()) + maximum_width * 4 + landing_depth * 4;
        } else length = source.properties.at("length_m").get<double>();
        const auto posts = std::ceil(length / spacing.get<double>()) + 2;
        if (!std::isfinite(posts) || posts < 0 || posts > 256) throw std::invalid_argument("V8 railing raw post limit");
        complexity = static_cast<std::size_t>(posts) * 12 + 8;
    } else if (source.type == "roof_join") {
        complexity = 0;
        const auto typed = native_dxf_architectural_source_dependencies(source);
        for (const auto& roof_id : typed.at("roof_ids")) {
            const auto& roof = authored.at(roof_id.get<std::string>());
            if (roof.type != "roof") throw std::invalid_argument("V8 roof join raw host type differs");
            charge(raw_nodes(roof.properties) * 32);
            std::size_t openings = 0;
            if (roof.properties.contains("openings")) {
                const auto& roster = roof.properties.at("openings");
                if (!roster.is_array() || roster.size() > 32) throw std::invalid_argument("V8 roof join raw opening limit");
                openings = roster.size();
            }
            complexity += 64 + openings * 16;
        }
    }
    else if (source.type == "roof" && source.properties.contains("openings")) {
        const auto& openings = source.properties.at("openings");
        if (!openings.is_array() || openings.size() > 32) throw std::invalid_argument("V8 roof opening raw limit");
        complexity += openings.size() * 16;
    }
    if (source.type == "assembly_instance") {
        const auto catalog_id = owner_id(source.properties.at("assembly_catalog_id"));
        const auto catalog = authored.find(catalog_id);
        if (catalog != authored.end()) {
            if (catalog->second.type != "assembly_model") throw std::invalid_argument("V8 independent raw catalog type differs");
            std::size_t roots = 0;
            for (const auto& [id, owner] : authored) {
                (void)id;
                if (owner.type == "assembly_instance" && ++roots > 4096)
                    throw std::invalid_argument("V8 independent raw root limit");
            }
            const auto before = budget.catalog_transfer.consumed_validation_work;
            admit_existing_assembly_catalog_work(catalog->second, budget.catalog_transfer);
            const auto cost = budget.catalog_transfer.consumed_validation_work - before;
            const auto passes = 16 * (roots + 1);
            const auto remaining = budget.catalog_transfer.max_validation_work - budget.catalog_transfer.consumed_validation_work;
            if (cost > remaining / passes) {
                budget.catalog_transfer.consumed_validation_work = budget.catalog_transfer.max_validation_work;
                throw std::invalid_argument("V8 repeated independent expansion work limit");
            }
            budget.catalog_transfer.consumed_validation_work += cost * passes;
            complexity = independent_compound_edges(source, catalog->second);
        }
        // Thin protocol body inventories omit catalogs. The full V8 table
        // validator reserves the same all-root passes before any model decode.
    }
    if (complexity > 8192 || complexity * complexity > limit / 32)
        throw std::invalid_argument("V8 architectural nonlinear work limit");
    charge(complexity * complexity * 32);
    if (budget.segments > 50'000 || complexity > 50'000 - budget.segments)
        throw std::invalid_argument("V8 architectural primitive limit");
    budget.segments += complexity;
    (void)native_dxf_architectural_source_dependencies(source);
}

void validate_native_dxf_architectural_source(const Entity& source) {
    if (!native_dxf_architectural_source_type(source.type)) throw std::invalid_argument("V8 architectural family required");
    // These canonical slots have no transfer implementation; opaque metadata is
    // retained, but cannot silently acquire destination document authority.
    for (const auto* slot : {"parent_id", "room_id", "wall_join_id", "refs", "references", "phase_id",
        "phase_binding", "phase_registry_id", "boundary_id", "opening_id", "slab_id", "roof_id", "stair_id",
        "sheet_id", "view_id", "constraint_id", "label_id", "column_id", "beam_id", "railing_id", "host_id",
        "target_id", "entity_id", "source_entity_id", "wall_id"})
        if (!(source.type == "roof_join" && std::string_view(slot) == "roof_id") &&
            (source.properties.contains(slot) || source.properties.contains(std::string(slot) + "s")))
            throw std::invalid_argument("V8 untransported architectural owner reference");
    if (source.type != "roof_join" && source.properties.contains("roof_ids")) throw std::invalid_argument("V8 unexpected roof roster");
    if (source.type != "assembly_instance" && source.properties.contains("assembly_catalog_id"))
        throw std::invalid_argument("V8 unexpected independent catalog reference");
    if (source.type != "stair" && source.properties.contains("level_connection")) throw std::invalid_argument("V8 unexpected level connection");
    if (source.type != "railing" && source.properties.contains("host")) throw std::invalid_argument("V8 unexpected stair host");
    if (source.extensions.contains("roof_join_phase_ownership") || source.extensions.contains("phase_membership"))
        throw std::invalid_argument("V8 architectural phase authoring unavailable");
    (void)native_dxf_architectural_source_dependencies(source);
    (void)native_dxf_architectural_source_catalog_ids(source);
}

Boundary native_dxf_architectural_source_plan(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
    if (source.type == "assembly_instance") {
        AssemblyExpansionBudget budget;
        const auto expansions = expand_document_assembly_instances(authored, budget);
        return project_assembly_plan(expansions.at(source.id));
    }
    const auto placed = source.type == "railing" && source.properties.contains("host") ? source : resolve_vertical_placement(authored, source);
    if (source.type == "slab") {
        Slab slab; std::string error;
        if (!read_document_slab(placed, slab, error)) throw std::invalid_argument(error);
        return project_building_shape_plan(make_slab(slab));
    }
    if (source.type == "room") {
        RoomVolume room; std::string error;
        if (!read_document_room(placed, room, error)) throw std::invalid_argument(error);
        return project_building_shape_plan(make_room_volume(room));
    }
    if (source.type == "roof_join") {
        const auto join = parse_roof_join(source.properties, source.id);
        validate_roof_join_skylights(join, authored);
        std::vector<TopoDS_Shape> shapes;
        for (const auto& id : join.roof_ids) {
            const auto& roof = authored.at(id);
            if (roof.type != "roof") throw std::invalid_argument("V8 roof join owner type differs");
            shapes.push_back(make_roof_shape(decode_roof_entity(resolve_vertical_placement(authored, roof))));
        }
        return project_building_shape_plan(make_roof_join(join, shapes));
    }
    return project_building_plan(decode_building_entity(placed), authored);
#else
    (void)source; (void)authored;
    throw std::invalid_argument("V8 architectural native geometry unavailable");
#endif
}

bool native_dxf_phase_auxiliary_source_type(std::string_view type) noexcept {
    return type == "wall_join" || type == "terrain_surface";
}

std::vector<std::string> native_dxf_phase_auxiliary_source_dependencies(const Entity& source) {
    if (!native_dxf_phase_auxiliary_source_type(source.type))
        throw std::invalid_argument("phase auxiliary source family required");
    (void)phase_raw_envelope(source);
    if (source.type == "terrain_surface") {
        (void)phase_raw_terrain_counts(source);
        return {};
    }
    (void)phase_raw_wall_ids(source);
    return parse_wall_join(source.properties, source.id).wall_ids;
}

void remap_native_dxf_phase_auxiliary_source_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& mapping) {
    const auto members = native_dxf_phase_auxiliary_source_dependencies(source);
    if (source.type == "terrain_surface") {
        // Validate the actual model without serializing it: local point IDs,
        // signed zero, numeric storage and all owner metadata remain untouched.
        (void)TerrainSurface::from_json(source.properties.at("model"));
        return;
    }
    auto result = source;
    std::set<std::string, std::less<>> targets;
    for (const auto& id : members) {
        const auto target = phase_owner_id(Json(mapping.at(id)));
        if (!targets.insert(target).second)
            throw std::invalid_argument("phase auxiliary wall member mapping not injective");
    }
    for (auto& id : result.properties["wall_ids"]) id = mapping.at(phase_owner_id(id));
    (void)native_dxf_phase_auxiliary_source_dependencies(result);
    source = std::move(result);
}

std::optional<DrawingContext> native_dxf_phase_auxiliary_source_context(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored, const ProjectOrganization& organization) {
    const auto members = native_dxf_phase_auxiliary_source_dependencies(source);
    phase_validate_join_graph(source, authored);
    if (source.type == "terrain_surface") {
        // Site terrain can live directly on a property; drawing_context()
        // intentionally exposes only complete floor/layer drawing placements.
        const auto node = organization.nodes.find(source.id);
        if (node == organization.nodes.end() || !node->second.issues.empty() ||
            node->second.context.property_id.empty())
            throw std::invalid_argument("phase auxiliary terrain source hierarchy unresolved");
        const auto& context = node->second.context;
        if (phase_actual_owner(context.property_id, source, authored).type != "property")
            throw std::invalid_argument("phase auxiliary terrain actual property role differs");
        for (const auto& [slot, resolved] : {
            std::pair{"property_id", &context.property_id}, std::pair{"building_id", &context.building_id},
            std::pair{"floor_id", &context.floor_id}, std::pair{"layer_id", &context.layer_id}}) {
            const auto direct = source.properties.find(slot);
            if (direct != source.properties.end() && phase_owner_id(*direct) != *resolved)
                throw std::invalid_argument("phase auxiliary terrain source hierarchy conflicts");
        }
        return context;
    }
    std::optional<DrawingContext> result;
    for (const auto& id : members) {
        const auto context = organization.drawing_context(id);
        if (!context || !context->complete())
            throw std::invalid_argument("phase auxiliary wall join member hierarchy unresolved");
        if (result && *result != *context)
            throw std::invalid_argument("phase auxiliary wall join member contexts conflict");
        result = context;
    }
    return result;
}

void admit_native_dxf_phase_auxiliary_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored, NativeDxfWallSourceWorkBudget& budget) {
    if (!native_dxf_phase_auxiliary_source_type(source.type)) return;
    constexpr std::size_t limit = 67'108'864;
    const auto charge = [&](std::size_t amount) {
        if (budget.architectural_work > limit)
            throw std::invalid_argument("cumulative phase auxiliary architectural work limit");
        if (amount > limit - budget.architectural_work) {
            budget.architectural_work = limit;
            throw std::invalid_argument("cumulative phase auxiliary architectural work limit");
        }
        budget.architectural_work += amount;
    };
    charge(1); // Refuse exhausted attempts before any raw traversal.
    const auto admit_raw = [&](const Entity& owner) {
        std::size_t nodes = 0;
        try { nodes = phase_raw_envelope(owner); }
        catch (...) {
            // A failed traversal can reach either raw ceiling. Its inspection
            // still consumes the ledger, preventing uncharged retry work.
            charge(2 * 65'536 * 32);
            throw;
        }
        charge(nodes * 32);
        return nodes;
    };
    (void)admit_raw(source);
    // Bound the complete actual-map scans before inspecting join ownership,
    // opening rosters or resolving vertical placement. Failed work is retained.
    if (authored.size() > (limit - budget.architectural_work) / 128)
        throw std::invalid_argument("phase auxiliary actual graph work limit");
    charge(authored.size() * 128);
    std::map<std::string, std::string, std::less<>> ownership;
    const auto admit_join = [&](const Entity& owner) {
        if (owner.type != "wall_join") return;
        if (owner.id != source.id) (void)admit_raw(owner);
        for (const auto& id : phase_raw_wall_ids(owner)) {
            if (phase_actual_owner(id, source, authored).type != "wall")
                throw std::invalid_argument("phase auxiliary wall join raw actual member missing/type differs");
            if (!ownership.emplace(id, owner.id).second)
                throw std::invalid_argument("phase auxiliary wall belongs to multiple joins");
        }
    };
    admit_join(source);
    for (const auto& [id, owner] : authored) {
        if (id != owner.id) throw std::invalid_argument("phase auxiliary actual map identity differs");
        if (id != source.id) admit_join(owner);
    }
    std::size_t primitives = 0;
    if (source.type == "terrain_surface") {
        const auto [points, triangles] = phase_raw_terrain_counts(source);
        primitives = triangles * 3;
        // Validation, detached remapping and plan_edges insert triangle edges
        // into ordered trees. Reserve four passes at the maximum tree depth.
        std::size_t depth = 1;
        for (auto remaining = primitives; remaining > 1; remaining /= 2) ++depth;
        charge(points * 128 + primitives * (depth + 1) * 128);
    } else {
        const auto members = phase_raw_wall_ids(source);
        const auto openings = phase_wall_openings(members, authored);
        bool level_placement = false;
        for (const auto& id : members) {
            const auto& wall = phase_actual_owner(id, source, authored);
            (void)admit_raw(wall);
            if (const auto placement = wall.properties.find("vertical_placement");
                placement != wall.properties.end() && placement->is_object() &&
                placement->value("mode", Json()) == "level") level_placement = true;
            if (!wall.properties.contains("baseline") || !wall.properties.at("baseline").is_object())
                throw std::invalid_argument("phase auxiliary wall join requires actual physical baseline");
            std::size_t layers = 1;
            if (const auto found = wall.properties.find("layers"); found != wall.properties.end()) {
                if (!found->is_array() || found->size() > 32)
                    throw std::invalid_argument("phase auxiliary wall join raw layer limit");
                layers = std::max<std::size_t>(1, found->size());
            }
            for (const auto* opening : openings.at(id)) (void)admit_raw(*opening);
            // Each layer builds cap/side/seam edges and every hosted cut.
            // Include curved/sloping cuts before union connectivity and HLR.
            const auto edges = layers * (48 + openings.at(id).size() * 32);
            if (edges > 8192 || primitives > 8192 - edges)
                throw std::invalid_argument("phase auxiliary wall join topology limit");
            primitives += edges;
        }
        if (level_placement) {
            // organize_project replays every floor binding, even unrelated
            // floors. Reserve full support before any placement/graph codec.
            std::size_t floors = 0, maximum_level_work = 0;
            for (const auto& [id, owner] : authored) {
                (void)id;
                if (owner.type == "property" || owner.type == "building" || owner.type == "floor" || owner.type == "layer") {
                    (void)admit_raw(owner);
                    if (owner.type == "floor" && owner.properties.contains("vertical_level_binding")) ++floors;
                }
                if (owner.type != "vertical_levels") continue;
                const auto nodes = admit_raw(owner);
                const auto& model = owner.properties.at("model");
                if (!model.is_object() || !model.at("levels").is_array() || !model.at("links").is_array() ||
                    model.at("levels").size() > 4096 || model.at("links").size() > 8192)
                    throw std::invalid_argument("phase auxiliary raw vertical graph limit");
                const auto levels = model.at("levels").size(), links = model.at("links").size();
                std::size_t depth = 1;
                for (auto remaining = levels + links + 1; remaining > 1; remaining /= 2) ++depth;
                // Upper bound includes all links as connected and all level
                // searches; no decoded graph chooses a cheaper admission path.
                const auto cost = nodes + (levels + links + 1) * depth * 4 + links * (links + 2 * levels) + levels;
                maximum_level_work = std::max(maximum_level_work, cost);
            }
            // Shared batch placement organizes once. Four passes cover source
            // validation/projection and detached validation/projection.
            const auto passes = 4 * (floors + members.size() + 1);
            if (maximum_level_work > (limit - budget.architectural_work) / 32 / passes)
                throw std::invalid_argument("phase auxiliary vertical placement replay work limit");
            charge(maximum_level_work * passes * 32);
        }
        if (primitives * primitives > limit / 32)
            throw std::invalid_argument("phase auxiliary wall join nonlinear work limit");
        charge(primitives * primitives * 32);
    }
    if (budget.segments > 50'000 || primitives > 50'000 - budget.segments)
        throw std::invalid_argument("phase auxiliary primitive limit");
    budget.segments += primitives;
}

void validate_native_dxf_phase_auxiliary_source(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
    const auto members = native_dxf_phase_auxiliary_source_dependencies(source);
    phase_validate_join_graph(source, authored);
    if (source.type == "terrain_surface") {
        (void)TerrainSurface::from_json(source.properties.at("model"));
        return;
    }
    (void)phase_decode_join_walls(source, members, authored);
}

Boundary native_dxf_phase_auxiliary_source_plan(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
    const auto members = native_dxf_phase_auxiliary_source_dependencies(source);
    phase_validate_join_graph(source, authored);
    if (source.type == "terrain_surface")
        return TerrainSurface::from_json(source.properties.at("model")).plan_edges();
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
    const auto join = parse_wall_join(source.properties, source.id);
    const auto walls = phase_decode_join_walls(source, members, authored);
    const auto fused_shape = make_wall_join(join, walls);
    return project_building_shape_plan(fused_shape);
#else
    (void)members;
    throw std::invalid_argument("phase auxiliary wall join native geometry unavailable");
#endif
}

namespace {
bool corner_marked(const Entity& source) {
    return source.properties.is_object() &&
        (source.properties.contains("corner_window_id") || source.properties.contains("corner_leg"));
}

std::vector<std::string> raw_corner_members(const Entity& source) {
    (void)phase_owner_id(Json(source.id));
    if (!source.properties.is_object() || !source.extensions.is_object())
        throw std::invalid_argument("corner source envelope invalid");
    const auto& properties = source.properties;
    if (source.type != "corner_window") {
        if (source.type != "opening" || !corner_marked(source) ||
            !properties.contains("corner_window_id") || !properties.contains("corner_leg") ||
            !properties.at("corner_leg").is_number_integer() ||
            (properties.at("corner_leg") != 0 && properties.at("corner_leg") != 1) ||
            !properties.contains("wall_id") || !properties.contains("opening_kind") ||
            properties.at("opening_kind") != "opening" || properties.contains("opening_assembly") ||
            properties.contains("door_operation") || properties.contains("vertical_placement"))
            throw std::invalid_argument("corner source child requires indexed bare cut");
        const auto owner = phase_owner_id(properties.at("corner_window_id"));
        const auto wall = phase_owner_id(properties.at("wall_id"));
        if (owner == source.id || wall == source.id || owner == wall)
            throw std::invalid_argument("corner source child identities overlap");
        return {owner};
    }
    if (!properties.contains("version") || !properties.at("version").is_number_integer() ||
        properties.at("version") != 1 || properties.contains("corner_window_id") ||
        properties.contains("corner_leg") || properties.contains("door_operation") ||
        properties.contains("vertical_placement"))
        throw std::invalid_argument("corner source owner dialect invalid");
    std::set<std::string, std::less<>> members{source.id};
    for (const auto* slot : {"wall_ids", "opening_ids"}) {
        const auto& values = properties.at(slot);
        if (!values.is_array() || values.size() != 2)
            throw std::invalid_argument("corner source requires exactly two hosts and cuts");
        for (const auto& value : values)
            if (!members.insert(phase_owner_id(value)).second)
                throw std::invalid_argument("corner source identities overlap");
    }
    const auto& ends = properties.at("at_start");
    const auto& widths = properties.at("widths_m");
    if (!ends.is_array() || ends.size() != 2 || !ends[0].is_boolean() || !ends[1].is_boolean() ||
        !widths.is_array() || widths.size() != 2 || !widths[0].is_number() || !widths[1].is_number() ||
        !properties.at("sill_m").is_number() || !properties.at("height_m").is_number() ||
        !properties.at("opening_assembly").is_object())
        throw std::invalid_argument("corner source raw shape invalid");
    members.erase(source.id);
    return {members.begin(), members.end()};
}

void require_actual_corner(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
    const auto found = authored.find(source.id);
    if (source.type != "corner_window" || found == authored.end() || found->second.id != source.id ||
        found->second.type != source.type || found->second.required != source.required ||
        found->second.properties.dump() != source.properties.dump() ||
        found->second.extensions.dump() != source.extensions.dump())
        throw std::invalid_argument("corner source requires its actual authored owner");
}
}

std::vector<std::string> native_dxf_corner_window_source_dependencies(const Entity& source) {
    return raw_corner_members(source);
}

void remap_native_dxf_corner_window_source_dependencies(Entity& source,
    const std::map<std::string, std::string, std::less<>>& mapping) {
    if (source.type != "corner_window" && !corner_marked(source)) return;
    // The caller has already moved the owner identity. An allocated target may
    // legally reuse another original source spelling; do not validate a mixed
    // source/destination namespace as though it were the final actual graph.
    auto original = source;
    std::optional<std::string> original_id;
    for (const auto& [id, target] : mapping) if (target == source.id) {
        if (original_id) throw std::invalid_argument("corner source owner mapping not injective");
        original_id = phase_owner_id(Json(id));
    }
    if (!original_id) throw std::invalid_argument("corner source mapped owner identity missing");
    original.id = *original_id;
    const auto members = raw_corner_members(original);
    auto result = source;
    std::set<std::string, std::less<>> targets{phase_owner_id(Json(source.id))};
    for (const auto& id : members)
        if (!targets.insert(phase_owner_id(Json(mapping.at(id)))).second)
            throw std::invalid_argument("corner source destination identities overlap");
    if (source.type == "corner_window") {
        for (const auto* slot : {"wall_ids", "opening_ids"})
            for (auto& id : result.properties[slot]) id = mapping.at(phase_owner_id(id));
    } else {
        result.properties["corner_window_id"] = mapping.at(phase_owner_id(source.properties.at("corner_window_id")));
        const auto wall = phase_owner_id(Json(mapping.at(phase_owner_id(original.properties.at("wall_id")))));
        if (!targets.insert(wall).second) throw std::invalid_argument("corner source child destination identities overlap");
        auto checked = result;
        checked.properties["wall_id"] = wall;
        (void)raw_corner_members(checked);
    }
    if (source.type == "corner_window") (void)raw_corner_members(result);
    source = std::move(result);
}

std::optional<DrawingContext> native_dxf_corner_window_source_context(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored, const ProjectOrganization& organization) {
    require_actual_corner(source, authored);
    const auto members = raw_corner_members(source);
    const auto result = organization.drawing_context(source.id);
    if (!result || !result->complete())
        throw std::invalid_argument("corner source owner hierarchy unresolved");
    for (const auto& id : members) {
        const auto found = authored.find(id);
        const auto context = organization.drawing_context(id);
        const bool host = source.properties.at("wall_ids")[0] == id || source.properties.at("wall_ids")[1] == id;
        if (found == authored.end() || found->second.id != id || found->second.type != (host ? "wall" : "opening") ||
            !context || *context != *result)
            throw std::invalid_argument("corner source actual member hierarchy/type differs");
    }
    return result;
}

void admit_native_dxf_corner_window_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored, NativeDxfWallSourceWorkBudget& budget,
    bool native_geometry) {
    if (source.type != "corner_window") return;
    constexpr std::size_t limit = 67'108'864;
    const auto charge = [&](std::size_t amount) {
        if (budget.architectural_work > limit || amount > limit - budget.architectural_work) {
            budget.architectural_work = limit;
            throw std::invalid_argument("cumulative corner source architectural work limit");
        }
        budget.architectural_work += amount;
    };
    charge(1);
    const auto admit_raw = [&](const Entity& owner) {
        std::size_t nodes = 0;
        try { nodes = phase_raw_envelope(owner); }
        catch (...) { charge(2 * 65'536 * 32); throw; }
        charge(nodes * 32);
        return nodes;
    };
    (void)admit_raw(source);
    require_actual_corner(source, authored);
    (void)raw_corner_members(source);
    // The global corner validator reads every owner, indexed cut and registry,
    // including inactive saved states. Placement also reads the shared hierarchy.
    if (authored.size() > limit / 128) throw std::invalid_argument("corner source actual graph size limit");
    charge(authored.size() * 128);
    std::size_t corner_count = 0, raw_openings = 0, phase_work = 0, floors = 0, level_work = 0;
    for (const auto& [id, owner] : authored) {
        if (id != owner.id) throw std::invalid_argument("corner source actual map identity differs");
        if (owner.type == "corner_window") ++corner_count;
        if (owner.type == "opening") ++raw_openings;
        if (owner.type != "corner_window" && owner.type != "wall" && owner.type != "opening" &&
            owner.type != "model_phases" && owner.type != "vertical_levels" && owner.type != "property" &&
            owner.type != "building" && owner.type != "floor" && owner.type != "layer") continue;
        const auto nodes = id == source.id ? 0 : admit_raw(owner);
        if (owner.type == "corner_window") (void)raw_corner_members(owner);
        if (corner_marked(owner)) (void)raw_corner_members(owner);
        if (owner.type == "model_phases") {
            const auto& model = owner.properties.at("model");
            const auto& members = model.at("entity_ids");
            const auto& alternatives = model.at("alternatives");
            if (!members.is_array() || members.size() > 16'384 || !alternatives.is_array() || alternatives.size() > 256)
                throw std::invalid_argument("corner source saved phase work limit");
            const auto amount = (members.size() + 1) * (alternatives.size() + 1) * 32;
            if (amount > limit || phase_work > limit - amount)
                throw std::invalid_argument("corner source cumulative saved phase work limit");
            phase_work += amount;
        }
        if (owner.type == "floor" && owner.properties.contains("vertical_level_binding")) ++floors;
        if (owner.type == "vertical_levels") {
            const auto& model = owner.properties.at("model");
            const auto& levels = model.at("levels"); const auto& links = model.at("links");
            if (!levels.is_array() || levels.size() > 4096 || !links.is_array() || links.size() > 8192)
                throw std::invalid_argument("corner source raw level graph limit");
            const auto cost = nodes + links.size() * (links.size() + 2 * levels.size()) +
                (levels.size() + links.size() + 1) * 64;
            level_work = std::max(level_work, cost);
        }
    }
    if (corner_count > limit / 1024 || raw_openings > limit / 1024 / std::max<std::size_t>(1, corner_count))
        throw std::invalid_argument("corner source global ownership work limit");
    charge(corner_count * (raw_openings + 1) * 1024);
    charge(phase_work);
    const auto passes = (native_geometry ? 4 : 1) * (floors + 3);
    if (level_work > limit / 32 / passes) throw std::invalid_argument("corner source level placement work limit");
    charge(level_work * passes * 32);
    std::vector<std::string> hosts;
    for (const auto& id : source.properties.at("wall_ids")) hosts.push_back(phase_owner_id(id));
    const auto openings = phase_wall_openings(hosts, authored);
    std::size_t primitives = 96; // Joined frame, post and two distinct panes.
    for (const auto& id : hosts) {
        const auto& wall = authored.at(id);
        if (wall.type != "wall" || !wall.properties.contains("baseline") || !wall.properties.at("baseline").is_object())
            throw std::invalid_argument("corner source requires actual physical host baselines");
        std::size_t layers = 1;
        if (const auto rows = wall.properties.find("layers"); rows != wall.properties.end()) {
            if (!rows->is_array() || rows->size() > 32) throw std::invalid_argument("corner source host layer limit");
            layers = std::max<std::size_t>(1, rows->size());
        }
        // Every opening may also derive a pocket recess from its operation.
        // Recesses are decoded from actual openings, never from opaque wall
        // metadata; reserve both cut and recess edge families before decoding.
        const auto edges = layers * (48 + openings.at(id).size() * 64);
        if (edges > 8192 || primitives > 8192 - edges) throw std::invalid_argument("corner source host topology limit");
        charge(edges * 32); // Structural decoding of the actual host roster.
        primitives += edges;
    }
    if (!native_geometry) return;
    if (primitives > limit / 32 / primitives) throw std::invalid_argument("corner source nonlinear geometry work limit");
    charge(primitives * primitives * 32);
    if (budget.segments > 50'000 || primitives > 50'000 - budget.segments)
        throw std::invalid_argument("corner source cumulative primitive limit");
    budget.segments += primitives;
}

void validate_native_dxf_corner_window_source(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
    require_actual_corner(source, authored);
    (void)raw_corner_members(source);
    validate_corner_window_state(authored);
}

Boundary native_dxf_corner_window_source_plan(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored) {
    validate_native_dxf_corner_window_source(source, authored);
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
    const auto value = parse_corner_window(source);
    const std::vector<std::string> members(value.wall_ids.begin(), value.wall_ids.end());
    const auto decoded = phase_decode_join_walls(source, members, authored);
    const std::array<Wall, 2> walls{decoded.at(0), decoded.at(1)};
    const auto cuts = corner_window_cuts(value, walls);
    return project_building_shape_plan(make_corner_window(walls, cuts, value.assembly));
#else
    throw std::invalid_argument("corner source manufactured native geometry unavailable");
#endif
}
} // namespace sketch
