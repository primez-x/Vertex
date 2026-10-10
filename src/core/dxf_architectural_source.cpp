#include "sketch/dxf_architectural_source.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/roof_join_semantics.hpp"
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
    auto result = remap_independent_assembly_instance(source, mapping);
    // The existing adapter validates the exact root/catalog mapping. Retain
    // raw numbers, dialect and opaque instance fields in this source contract.
    result.properties = source.properties;
    result.properties["assembly_catalog_id"] = catalogs.at(catalog_id);
    result.properties["instance"]["id"] = result.id;
    return result;
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
} // namespace sketch
