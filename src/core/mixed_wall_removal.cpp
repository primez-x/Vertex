#include "sketch/mixed_wall_removal.hpp"

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_wall_profile_capture.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/stair_semantics.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = MixedWallRemovalEntities;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t entity_limit = 65536, root_limit = 1000, fresh_limit = 4096;
constexpr std::size_t byte_limit = 64 * 1024 * 1024, node_limit = 4 * 1024 * 1024;
constexpr std::size_t proof_limit = 1024 * 1024, geometry_limit = 262144;
// Reserve repeated actual-source admissions in wall/hosted/join, roof inspection
// and replay, ordinary objects, composition and final validation. This is an
// analytical upper inventory, not permission to reset a budget between lanes.
constexpr std::size_t source_passes = 16, native_passes = 16;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Mixed wall removal: " + reason);
}
void identity(const std::string& value) {
    if (value.empty() || value.size() > 128 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity requires 1..128 supported ASCII characters");
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported proof/intent fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing proof/intent field");
}
struct Budget {
    std::size_t nodes{}, bytes{}, serialized{}, phase{}, rows{}, native{};
    void add(std::size_t& total, std::size_t count, std::size_t limit, const char* reason) {
        if (count > limit - total) reject(reason);
        total += count;
    }
    void text(const std::string& value) { add(bytes, value.size(), byte_limit, "aggregate string budget exceeded"); }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64) reject("JSON nesting budget exceeded");
        add(nodes, 1, node_limit, "aggregate JSON node budget exceeded");
        if (value.is_binary() || value.is_discarded()) reject("binary/discarded JSON is unsupported");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("JSON scalar must be finite");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
    void geometry(std::size_t count) { add(native, count, geometry_limit, "aggregate native/expansion work exceeded"); }
    void product(std::size_t first, std::size_t second) {
        if (first && second > (geometry_limit - native) / first) reject("aggregate native product work exceeded");
        geometry(first * second);
    }
};
void json_bound(const Json& value, std::size_t limit = proof_limit) {
    Budget budget; budget.read(value);
    // Account the tree before recursive serialization or typed decoding.
    if (value.dump().size() > limit) reject("proof/intent byte budget exceeded");
}
void sorted_ids(const std::vector<std::string>& ids) {
    if (!std::is_sorted(ids.begin(), ids.end()) || std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        reject("selected identities must be sorted and unique");
    for (const auto& id : ids) identity(id);
}
PhysicalWallJoinRemovalAdditionalIdentities destinations(const MixedWallRemovalIntent& intent,
    bool reserve_opening_hosted_marker=false) {
    PhysicalWallJoinRemovalAdditionalIdentities result;
    Ids fresh;
    for (const auto* mapping : {&intent.wall_additional_identities, &intent.other.roof_additional_identities}) {
        if (mapping->size() > fresh_limit) reject("destination owner budget exceeded");
        for (const auto& [owner, rows] : *mapping) {
            identity(owner);
            if (rows.empty() || rows.size() > fresh_limit - fresh.size()) reject("destination slots must be bounded and nonempty");
            if (!result.emplace(owner, rows).second) reject("wall/roof destination owners overlap");
            for (const auto& row : rows) {
                identity(row);
                for (const auto* reserved : {"version", "kind", "expected_revision", "message", "intent", "proof",
                    "mixed_wall_deletion", "wall_ids", "wall_additional_identities", "other_object_ids",
                    "components", "roof_additional_identities"})
                    if (row == reserved) reject("destination borrows a mixed-proof envelope token");
                if (reserve_opening_hosted_marker && row == "complete_opening_hosted_removal")
                    reject("destination borrows the opening-hosted mixed-proof token");
                if (!fresh.insert(row).second) reject("fresh wall/roof destinations overlap");
            }
        }
    }
    for (const auto& [owner, rows] : result) {
        (void)rows;
        if (fresh.contains(owner)) reject("destination borrows a source slot owner");
    }
    for (const auto& id : intent.wall_ids) if (fresh.contains(id)) reject("destination borrows a selected wall");
    for (const auto& id : intent.other.object_ids) if (fresh.contains(id)) reject("destination borrows a selected owner");
    return result;
}
void intent_bound(const MixedWallRemovalIntent& intent) {
    if (intent.wall_ids.empty() || intent.wall_ids.size() > 128 ||
        intent.other.object_ids.size() > root_limit - intent.wall_ids.size() ||
        intent.other.components.size() > root_limit - intent.wall_ids.size() - intent.other.object_ids.size() ||
        (intent.other.object_ids.empty() && intent.other.components.empty()))
        reject("requires both actual walls and other selections, with 1..1000 aggregate roots");
    sorted_ids(intent.wall_ids); sorted_ids(intent.other.object_ids);
    for (const auto& id : intent.other.object_ids)
        if (std::binary_search(intent.wall_ids.begin(), intent.wall_ids.end(), id)) reject("wall/other selections overlap");
    if (!std::is_sorted(intent.other.components.begin(), intent.other.components.end()) ||
        std::adjacent_find(intent.other.components.begin(), intent.other.components.end()) != intent.other.components.end())
        reject("qualified components must be sorted and unique");
    for (const auto& [catalog, local] : intent.other.components) { identity(catalog); identity(local); }
    (void)destinations(intent);
}
Json intent_json(const MixedWallRemovalIntent& intent) {
    intent_bound(intent);
    return {{"wall_ids", intent.wall_ids}, {"wall_additional_identities", intent.wall_additional_identities},
        {"other_object_ids", intent.other.object_ids}, {"components", intent.other.components},
        {"roof_additional_identities", intent.other.roof_additional_identities}};
}
MixedWallRemovalIntent intent_from_json(const Json& value) {
    fields(value, {"wall_ids", "wall_additional_identities", "other_object_ids", "components", "roof_additional_identities"});
    if (!value.at("wall_ids").is_array() || !value.at("other_object_ids").is_array() || !value.at("components").is_array() ||
        !value.at("wall_additional_identities").is_object() || !value.at("roof_additional_identities").is_object())
        reject("malformed intent collections");
    for (const auto& row : value.at("components"))
        if (!row.is_array() || row.size() != 2 || !row[0].is_string() || !row[1].is_string())
            reject("component requires an exact qualified string pair");
    MixedWallRemovalIntent result;
    result.wall_ids = value.at("wall_ids").get<std::vector<std::string>>();
    result.wall_additional_identities = value.at("wall_additional_identities").get<PhysicalWallJoinRemovalAdditionalIdentities>();
    result.other.object_ids = value.at("other_object_ids").get<std::vector<std::string>>();
    result.other.components = value.at("components").get<std::vector<std::pair<std::string, std::string>>>();
    result.other.roof_additional_identities = value.at("roof_additional_identities").get<RoofRemovalAdditionalIdentities>();
    if (intent_json(result).dump() != value.dump()) reject("intent is not canonical");
    return result;
}
bool physical(const Entity& entity) {
    return entity.type == "wall" || entity.type == "roof" || entity.type == "slab" || entity.type == "stair" ||
        entity.type == "railing" || entity.type == "column" || entity.type == "beam";
}
bool exact_entity(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() && left.extensions.dump() == right.extensions.dump();
}
bool exact_entities(const Entities& left, const Entities& right) {
    if (left.size() != right.size()) return false;
    for (const auto& [id, entity] : left) {
        const auto found = right.find(id);
        if (found == right.end() || !exact_entity(entity, found->second)) return false;
    }
    return true;
}
void source_bound(const Entities& actual, Budget& budget) {
    if (actual.size() > entity_limit / source_passes) reject("aggregate source entity inventory exceeded");
    for (const auto& [id, entity] : actual) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("requires actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type); budget.read(entity.properties); budget.read(entity.extensions);
        const auto bytes = entity.properties.dump().size() + entity.extensions.dump().size() + Json(entity.type).dump().size() + 2 * id.size() + 128;
        budget.add(budget.serialized, bytes, byte_limit / source_passes, "aggregate serialized source inventory exceeded");
        if (entity.type == "model_phases") {
            const auto& model = entity.properties.at("model");
            const auto& members = model.at("entity_ids"); const auto& alternatives = model.at("alternatives");
            if (!members.is_array() || !alternatives.is_array() || alternatives.size() > fresh_limit ||
                members.size() > (2000000 / source_passes - budget.phase) / (alternatives.size() + 1))
                reject("aggregate phase inventory/work exceeded");
            budget.phase += members.size() * (alternatives.size() + 1);
        } else if (entity.type == "assembly_model") {
            for (const auto* name : {"materials", "types", "instances"}) {
                const auto& rows = entity.properties.at("model").at(name);
                if (!rows.is_array()) reject("catalog requires actual bounded arrays");
                budget.add(budget.rows, rows.size(), entity_limit / source_passes, "aggregate catalog inventory exceeded");
            }
        }
    }
    if (budget.nodes > node_limit / source_passes || budget.bytes > byte_limit / source_passes)
        reject("aggregate source JSON inventory exceeded");
}

// Conservative shared upper inventory: all actual supported physical owners and
// joins, all embedded rows and independent roots. No native building codec,
// shortened source, manufactured admission map or per-lane budget reset occurs.
// Counting unselected geometry trades capacity for an auditable pre-factory bound.
void analytical_work(const Entities& actual, Budget& budget,bool include_manufactured_opening_hosts=false,
    bool complete_corner_catalog_hosts=false) {
    std::map<std::string, std::size_t, std::less<>> opening_counts, costs;
    for (const auto& [id, entity] : actual) {
        (void)id;
        if (entity.type == "opening" || entity.type == "door" || entity.type == "window") {
            const auto host = entity.properties.find("wall_id");
            if (host != entity.properties.end() && host->is_string()) ++opening_counts[host->get<std::string>()];
        }
    }
    for (const auto& [id, entity] : actual) {
        const auto& p = entity.properties;
        if (entity.type == "wall_join" || entity.type == "roof_join") {
            const auto& members = p.at(entity.type == "wall_join" ? "wall_ids" : "roof_ids");
            if (!members.is_array() || members.size() > fresh_limit) reject("join lacks bounded actual members");
            budget.product(native_passes * 2 * members.size(), members.size());
        }
        if (!physical(entity)) continue;
        std::size_t cost = 1;
        if (entity.type == "wall") {
            const auto layers = p.find("layers");
            if (layers != p.end() && (!layers->is_array() || layers->size() > 1024)) reject("wall layer inventory exceeded");
            const auto layer_count = layers == p.end() ? 0 : layers->size();
            const auto openings = opening_counts[id];
            if (openings > fresh_limit) reject("wall opening inventory exceeded");
            cost = (1 + layer_count) * (1 + openings);
            // Every opening may manufacture a fixed frame/three-panel family
            // and rebuild its complete cut host. Historical rows are included.
            budget.product(native_passes * 32 * openings, 1 + cost);
        } else if (entity.type == "roof") {
            const auto openings = p.find("roof_openings");
            if (openings != p.end() && (!openings->is_array() || openings->size() > 1024)) reject("roof opening inventory exceeded");
            cost = 4 + (openings == p.end() ? 0 : openings->size());
        } else if (entity.type == "slab") {
            std::size_t segments{};
            const auto ring = [&](const Json& rows) {
                if (!rows.is_array() || rows.empty() || rows.size() > 4096 - segments) reject("slab footprint inventory exceeded");
                segments += rows.size();
            };
            ring(p.at("boundary"));
            const auto& holes = p.at("holes");
            if (!holes.is_array() || holes.size() > 1024) reject("slab hole inventory exceeded");
            for (const auto& hole : holes) ring(hole);
            const auto layers = p.find("layers");
            if (layers != p.end() && (!layers->is_array() || layers->size() > 1024)) reject("slab layer inventory exceeded");
            cost = segments * std::max<std::size_t>(1, layers == p.end() ? 0 : layers->size());
        } else if (entity.type == "stair") {
            const auto stair = decode_stair_properties(id, p);
            cost = stair.riser_count + stair.landings.size() + 1;
        } else if (entity.type == "railing") {
            const auto rail = decode_railing_properties(id, p);
            const auto host = rail.host ? rail.host->stair_id : rail.landing_host ? rail.landing_host->stair_id : std::string{};
            if (!host.empty()) {
                const auto stair = decode_stair_properties(host, resolve_vertical_placement(actual, actual.at(host)).properties);
                cost = derive_hosted_railing_layout(rail, stair).posts.size() + 1;
            } else {
                const auto posts = std::ceil(rail.length / rail.post_spacing) + 2;
                if (!std::isfinite(posts) || posts < 0 || posts > geometry_limit) reject("railing post inventory exceeded");
                cost = static_cast<std::size_t>(posts);
            }
        }
        budget.product(native_passes, cost); costs.emplace(id, cost);
    }
    if (include_manufactured_opening_hosts) for (const auto& [id, entity] : actual) {
        if (entity.type!="opening") continue;
        // A managed corner leg is only a void. Its owner alone supplies the
        // manufactured body in the explicit completion lane.
        if (complete_corner_catalog_hosts && entity.properties.contains("corner_window_id")) continue;
        const auto host=entity.properties.find("wall_id");
        if (host==entity.properties.end() || !host->is_string()) reject("opening lacks an actual wall identity");
        const auto wall=costs.find(host->get<std::string>());
        if (wall==costs.end() || actual.at(host->get<std::string>()).type!="wall")
            reject("opening lacks an actual supported wall host");
        // A legacy opening copy may manufacture its frame/operation and
        // repeatedly reconstruct the complete layered host. Reserve this body
        // only in the new opening-selection lane; historical v37 stays exact.
        const auto cost=33*(1+wall->second);
        costs.emplace(id,cost);
    }
    Ids inactive_corner_copy_owners;
    if (complete_corner_catalog_hosts) {
        // This aggregate validator, the profile codecs, document wall reader
        // and placement resolvers below are analytical; none creates a solid.
        validate_corner_window_state(actual);
        const auto scope = constraint_phase_scope(actual);
        const auto organization = organize_project(actual);
        const auto context = [&](const Entity& entity) {
            const auto& p = entity.properties;
            const bool scoped = p.contains("property_id") || p.contains("building_id") ||
                p.contains("floor_id") || p.contains("layer_id") || p.contains("level_id") || p.contains("wall_id");
            const auto node = organization.nodes.find(entity.id);
            if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
                reject("corner participant has unresolved actual drawing context: " + entity.id);
        };
        // Decode each reached material catalog once. Source JSON has already
        // shared the enclosing inventory bound; catalog expansion below retains
        // the original cumulative allowance across all rows and independent roots.
        std::map<std::string, Ids, std::less<>> materials;
        const auto material = [&](const std::string& catalog_id, const std::string& material_id) {
            // Local material names follow the catalog codec, not the document
            // owner identifier alphabet.
            identity(catalog_id);
            auto found = materials.find(catalog_id);
            if (found == materials.end()) {
                const auto catalog = actual.find(catalog_id);
                if (catalog == actual.end() || catalog->second.type != "assembly_model")
                    reject("corner material lacks an actual catalog: " + catalog_id);
                const auto model = AssemblyModel::from_json(catalog->second.properties.at("model"));
                Ids rows;
                for (const auto& row : model.materials()) rows.insert(row.id);
                found = materials.emplace(catalog_id, std::move(rows)).first;
            }
            if (!found->second.contains(material_id)) reject("corner material is absent from actual catalog: " + material_id);
        };
        const auto assignment = [&](const Entity& entity) {
            const auto value = entity.properties.find("material_assignment");
            if (value == entity.properties.end()) return;
            if (!value->is_object() || !value->contains("version") || !value->at("version").is_number_integer() ||
                value->at("version") != 1 || !value->contains("catalog_id") || !value->at("catalog_id").is_string() ||
                !value->contains("material_id") || !value->at("material_id").is_string())
                reject("corner participant has an unsupported material assignment: " + entity.id);
            material(value->at("catalog_id").get<std::string>(), value->at("material_id").get<std::string>());
        };
        std::map<std::string, std::vector<const Entity*>, std::less<>> active_openings;
        Ids charged_pocket_hosts;
        for (const auto& [id, entity] : actual) {
            if (entity.type != "opening" || scope.inactive_owner_ids.contains(id)) continue;
            const auto host = entity.properties.find("wall_id");
            if (host != entity.properties.end() && host->is_string())
                active_openings[host->get<std::string>()].push_back(&entity);
        }
        for (const auto& [id, entity] : actual) {
            if (entity.type != "corner_window") continue;
            if (scope.inactive_owner_ids.contains(id)) {
                // Complete saved aggregates were admitted above. Their parked
                // catalog rows remain stored but manufacture no active body,
                // matching native presentation after baseline demolition.
                inactive_corner_copy_owners.insert(id);
                continue;
            }
            const auto owner = parse_corner_window(entity);
            context(entity); assignment(entity);
            const std::array<std::string, 3> site_ids{id, owner.wall_ids[0], owner.wall_ids[1]};
            const auto sites = resolve_site_presentations(actual, site_ids);
            const auto& site = sites.at(id);
            std::array<Wall, 2> hosts;
            std::size_t cost = 33; // Fixed frame/post/panes and source site transform.
            for (std::size_t leg = 0; leg < hosts.size(); ++leg) {
                const auto& wall = actual.at(owner.wall_ids[leg]);
                const auto& child = actual.at(owner.opening_ids[leg]);
                if (scope.inactive_owner_ids.contains(wall.id) || scope.inactive_owner_ids.contains(child.id))
                    reject("corner catalog owner requires both actual active walls and cuts: " + id);
                context(wall); context(child); assignment(wall); assignment(child);
                const auto& placement = sites.at(wall.id);
                if (placement.source_frame != site.source_frame ||
                    placement.forward.translation_m.x != site.forward.translation_m.x ||
                    placement.forward.translation_m.y != site.forward.translation_m.y ||
                    placement.forward.translation_m.z != site.forward.translation_m.z ||
                    placement.forward.rotation_radians != site.forward.rotation_radians)
                    reject("corner catalog owner and hosts require one actual site frame: " + id);
                validate_wall_profile_source_entity(wall);
                const auto& siblings = active_openings[wall.id];
                if (siblings.size() > 128) reject("corner host opening inventory exceeded: " + wall.id);
                for (const auto* sibling : siblings) validate_hosted_opening_profile_entity(*sibling);
                std::string error;
                if (!read_document_wall(resolve_vertical_placement(actual, wall), siblings, hosts[leg], error))
                    reject("corner actual wall codec refused " + wall.id + ": " + error);
                const auto& host = hosts[leg];
                if (host.layers.size() > 32 || host.pocket_recesses.size() > 256)
                    reject("corner host layer/pocket inventory exceeded: " + wall.id);
                validate_wall_semantics(host);
                for (const auto& layer : host.layers) if (layer.material)
                    material(layer.material->catalog_id, layer.material->material_id);
                // The historical wall inventory reserves layered full cuts,
                // but has no pocket term. Add the complete sibling rebuild
                // supplement once per actual corner host, using the same
                // multipass ledger rather than giving this lane a fresh limit.
                if (charged_pocket_hosts.insert(wall.id).second)
                    budget.product(native_passes * 32 * opening_counts[wall.id],
                        (1 + host.layers.size()) * host.pocket_recesses.size());
                // Complete host/cut/pocket work, two clearance builders and
                // reconstruction for this corner's manufactured assembly.
                cost += 66 * std::max<std::size_t>(1, host.layers.size()) *
                    (1 + host.openings.size() + host.pocket_recesses.size());
            }
            const auto cuts = corner_window_cuts(owner, hosts);
            for (std::size_t leg = 0; leg < cuts.size(); ++leg)
                if (std::find(hosts[leg].openings.begin(), hosts[leg].openings.end(), cuts[leg]) == hosts[leg].openings.end())
                    reject("corner catalog cut differs from its actual host: " + id);
            budget.product(native_passes, cost);
            costs.emplace(id, cost);
        }
    }
    AssemblyExpansionBudget expansion_budget;
    expansion_budget.max_nodes = 4096 / native_passes;
    expansion_budget.max_profile_segments = geometry_limit / native_passes;
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model") {
        (void)id;
        const auto model = AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& row : model.instances()) {
            const auto expansion = model.expand(row, expansion_budget);
            if (expansion.profiles.empty() && row.placement) {
                if (inactive_corner_copy_owners.contains(row.placement->host_entity_id)) continue;
                const auto found = costs.find(row.placement->host_entity_id);
                if (found == costs.end()) reject("catalog placement lacks an actual supported host");
                budget.product(native_passes, found->second);
            }
        }
    }
    // Independent instance admission shares this same expansion reservation;
    // do not expand the embedded inventory a second time through the bulk API.
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_instance") {
        (void)id;
        (void)expand_document_assembly_instance(entity, actual, expansion_budget);
    }
    budget.product(native_passes, expansion_budget.consumed_nodes);
    budget.product(native_passes, expansion_budget.consumed_profile_segments);
}
ApplyEntityChanges raw_changes(const Entities& actual, const Entities& candidate, Revision revision, const std::string& message) {
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    ApplyEntityChanges result{revision, {}, {}, message};
    for (const auto& [id, entity] : actual) {
        const auto after = candidate.find(id);
        if (after == candidate.end()) result.entity_changes.push_back(EntityChange::erase(id));
        else if (!exact_entity(entity, after->second)) result.entity_changes.push_back(EntityChange::upsert(after->second));
    }
    for (const auto& [id, entity] : candidate) if (!actual.contains(id)) result.entity_changes.push_back(EntityChange::upsert(entity));
    if (result.entity_changes.empty() || result.entity_changes.size() > fresh_limit) reject("raw change inventory exceeded");
    return result;
}
void snapshot_bound(const DocumentSnapshot& source, const MixedWallRemovalIntent& intent) {
    if (!source.is_editable()) reject("captured source is read-only");
    const auto& history = source.history();
    if (history.empty() || history.size() > 4096 || source.revision() >= history.size() ||
        history[source.revision()].revision != source.revision()) reject("captured revision/history is invalid");
    if (source.assets().size() > entity_limit || source.named_revisions().size() > 4096)
        reject("captured asset/named-revision inventory exceeded");
    if (source.saved_revision_optional() && *source.saved_revision_optional() >= history.size()) reject("saved revision is outside history");
    validate_physical_wall_join_removal_identity_lifetime(source, destinations(intent));
}
void raw_bound(const ApplyEntityChanges& command) {
    if (!command.asset_changes.empty() || command.entity_changes.empty() || command.entity_changes.size() > fresh_limit ||
        command.message.empty() || command.message.size() > 4096) reject("requires a bounded asset-free raw command");
    Budget budget;
    for (const auto& change : command.entity_changes) {
        if (change.kind == EntityChangeKind::erase) identity(change.entity_id);
        else {
            identity(change.entity.id);
            budget.read(change.entity.properties); budget.read(change.entity.extensions);
        }
    }
    json_bound(command_to_json(Command{command}));
}
Json envelope(const MixedWallRemovalIntent& intent, const ApplyEntityChanges& command,
    bool complete_opening_hosted_removal=false) {
    if (complete_opening_hosted_removal) (void)destinations(intent, true);
    Json result{{"version", complete_opening_hosted_removal ? 39 : 37}, {"kind", "mixed_wall_deletion"}, {"expected_revision", command.expected_revision},
        {"message", command.message}, {"intent", intent_json(intent)}, {"proof", command_to_json(Command{command})}};
    if (complete_opening_hosted_removal) result["complete_opening_hosted_removal"] = true;
    json_bound(result);
    return result;
}

// The true replay has already admitted the complete source and command. Only
// these bounded source facts establish an older unsupported-admission case;
// failures from an independently attempted old replay must remain visible.
bool needs_opening_hosted_lane(const Entities& actual, const MixedWallRemovalIntent& intent) {
    AssemblyExpansionBudget expansion_budget;
    expansion_budget.max_nodes = 4096 / native_passes;
    expansion_budget.max_profile_segments = geometry_limit / native_passes;
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model") {
        const auto model = AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& row : model.instances()) {
            const auto expansion = model.expand(row, expansion_budget);
            if (!row.placement) continue;
            const auto host = actual.find(row.placement->host_entity_id);
            if (host == actual.end() || host->second.type != "opening") continue;
            if (expansion.profiles.empty() || std::binary_search(intent.other.components.begin(),
                intent.other.components.end(), std::pair{id, row.id})) return true;
            const auto wall = host->second.properties.find("wall_id");
            if (wall != host->second.properties.end() && wall->is_string() &&
                std::binary_search(intent.wall_ids.begin(), intent.wall_ids.end(), wall->get<std::string>())) return true;
        }
    }
    return false;
}
} // namespace

void validate_mixed_wall_removal_source_admission(const Entities& actual,bool include_manufactured_opening_hosts,
    bool complete_corner_catalog_hosts) {
    try {
        Budget budget; source_bound(actual, budget);
        analytical_work(actual, budget,include_manufactured_opening_hosts,complete_corner_catalog_hosts);
    } catch (const Json::exception& error) {
        reject(std::string("malformed actual source admission: ") + error.what());
    }
}

Entities replay_mixed_wall_removal(const Entities& actual, const MixedWallRemovalIntent& intent,
    bool complete_opening_hosted_removal) {
    try {
        intent_bound(intent); json_bound(intent_json(intent));
        Budget budget; source_bound(actual, budget);
        for (const auto& id : intent.wall_ids) {
            const auto found = actual.find(id);
            if (found == actual.end() || found->second.type != "wall") reject("wall selection requires actual physical owners: " + id);
        }
        bool selected_roof = false;
        for (const auto& id : intent.other.object_ids) {
            const auto found = actual.find(id);
            if (found == actual.end() || !physical(found->second) || found->second.type == "wall")
                reject("other selection requires an admitted architectural owner: " + id);
            selected_roof = selected_roof || found->second.type == "roof";
        }
        if (!selected_roof && !intent.other.roof_additional_identities.empty()) reject("roof destinations lack a selected roof");
        const auto fresh = destinations(intent);
        validate_physical_wall_join_removal_identity_lifetime(actual, {}, 0, fresh);
        // All cumulative native and expansion consequences are analytically
        // reserved before wall, join, roof inspection or component factories.
        analytical_work(actual, budget, complete_opening_hosted_removal);
        // Completion-only proof names must be reserved before either native
        // leaf. Sources that qualify for historical v37 retain its name domain.
        if (complete_opening_hosted_removal && needs_opening_hosted_lane(actual, intent))
            (void)destinations(intent, true);
        const auto original_aliases = embedded_assembly_presentation_ids(actual);
        for (const auto& key : intent.other.components)
            if (!original_aliases.contains(key)) reject("selected qualified component is missing from actual source");
        auto wall = complete_opening_hosted_removal ?
            replay_complete_physical_walls_deletion(actual, intent.wall_ids, intent.wall_additional_identities, true) :
            replay_complete_physical_walls_deletion(actual, intent.wall_ids, intent.wall_additional_identities);
        const auto wall_aliases = embedded_assembly_presentation_ids(wall);
        auto other = intent.other;
        std::erase_if(other.components, [&](const auto& key) {
            // Only actual qualified rows already retired by the wall producer
            // collapse. Alias strings and local IDs are never used as authority.
            return original_aliases.contains(key) && !wall_aliases.contains(key);
        });
        std::vector<Entities> candidates; candidates.push_back(std::move(wall));
        if (!other.object_ids.empty() || !other.components.empty())
            candidates.push_back(complete_opening_hosted_removal ?
                replay_architectural_selection_removal(actual, other, true) :
                replay_architectural_selection_removal(actual, other));
        else if (!other.roof_additional_identities.empty()) reject("roof destinations lack a selected roof");
        auto expected_aliases = original_aliases;
        const auto before_scope = constraint_phase_scope(actual);
        auto expected_inactive = before_scope.inactive_owner_ids;
        for (const auto& candidate : candidates) {
            const auto remaining = embedded_assembly_presentation_ids(candidate);
            for (const auto& [key, alias] : remaining) {
                const auto original = original_aliases.find(key);
                if (original == original_aliases.end() || original->second != alias) reject("leaf changed a surviving component alias");
            }
            for (const auto& [key, alias] : original_aliases) { (void)alias; if (!remaining.contains(key)) expected_aliases.erase(key); }
            const auto scope = constraint_phase_scope(candidate);
            expected_inactive.insert(scope.inactive_owner_ids.begin(), scope.inactive_owner_ids.end());
        }
        auto result = candidates.size() == 1 ? std::move(candidates.front()) :
            compose_ordinary_architectural_removal_candidates(actual, candidates);
        if (embedded_assembly_presentation_ids(result) != expected_aliases) reject("composition changed surviving aliases");
        if (constraint_phase_scope(result).inactive_owner_ids != expected_inactive) reject("composition changed protected inactive ownership");
        for (const auto& [id, entity] : actual) if (entity.type == "boundary" || entity.type == "room") {
            const auto after = result.find(id);
            if (after == result.end() || !exact_entity(entity, after->second)) reject("removal changed retained room/boundary lineage");
        }
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed actual source/intent: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        reject(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}

ApplyEntityChanges prepare_mixed_wall_removal(const DocumentSnapshot& source, const MixedWallRemovalIntent& intent,
    const std::string& message, bool complete_opening_hosted_removal) {
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    intent_bound(intent); snapshot_bound(source, intent);
    auto result = raw_changes(source.entities(), replay_mixed_wall_removal(source.entities(), intent,
        complete_opening_hosted_removal), source.revision(), message);
    // Check bounded canonical retention syntax without repeating native replay.
    // The public encoder independently qualifies the exact preferred authority.
    const bool opening_hosted_proof = complete_opening_hosted_removal &&
        needs_opening_hosted_lane(source.entities(), intent);
    (void)decode_mixed_wall_deletion_review_proof(envelope(intent, result, opening_hosted_proof));
    return result;
}

DecodedMixedWallDeletionReviewProof decode_mixed_wall_deletion_review_proof(const Json& proof) {
    try {
        json_bound(proof);
        if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
            (proof.at("version") != 37 && proof.at("version") != 39)) reject("requires explicit version37/39 mixed-wall proof");
        const bool complete_opening_hosted_removal = proof.at("version") == 39;
        if (complete_opening_hosted_removal) {
            fields(proof, {"version", "kind", "expected_revision", "message", "intent", "proof", "complete_opening_hosted_removal"});
            if (!proof.at("complete_opening_hosted_removal").is_boolean() || proof.at("complete_opening_hosted_removal") != true)
                reject("version39 requires literal true opening-hosted completion");
        } else fields(proof, {"version", "kind", "expected_revision", "message", "intent", "proof"});
        if (proof.at("kind") != "mixed_wall_deletion") reject("requires explicit mixed-wall proof kind");
        auto intent = intent_from_json(proof.at("intent"));
        const auto& child = proof.at("proof");
        fields(child, {"version", "kind", "expected_revision", "message", "entity_changes", "asset_changes"});
        if (!child.at("version").is_number_integer() || child.at("version") != 1 || child.at("kind") != "apply_entity_changes" ||
            !child.at("entity_changes").is_array() || child.at("entity_changes").empty() || child.at("entity_changes").size() > fresh_limit ||
            !child.at("asset_changes").is_array() || !child.at("asset_changes").empty()) reject("requires bounded asset-free raw version-one child");
        auto decoded = command_from_json(child);
        const auto* raw = std::get_if<ApplyEntityChanges>(&decoded);
        if (!raw || raw->message.empty() || raw->message.size() > 4096 || !raw->asset_changes.empty()) reject("invalid raw child");
        Ids changed, erased;
        for (const auto& change : raw->entity_changes) {
            const auto& id = change.kind == EntityChangeKind::erase ? change.entity_id : change.entity.id;
            identity(id);
            if (!changed.insert(id).second) reject("raw child repeats an owner");
            if (change.kind == EntityChangeKind::erase) erased.insert(id);
        }
        for (const auto& id : intent.wall_ids) if (!erased.contains(id)) reject("declared wall is not erased");
        Ids declared_wall_joins, actual_wall_joins;
        for (const auto& [owner, rows] : intent.wall_additional_identities) {
            (void)owner; declared_wall_joins.insert(rows.begin(), rows.end());
        }
        for (const auto& change : raw->entity_changes) if (declared_wall_joins.contains(
            change.kind == EntityChangeKind::erase ? change.entity_id : change.entity.id)) {
            if (change.kind != EntityChangeKind::upsert || change.entity.type != "wall_join")
                reject("declared fresh wall join requires a typed upsert");
            actual_wall_joins.insert(change.entity.id);
        }
        if (actual_wall_joins != declared_wall_joins) reject("declared fresh wall join is missing from child");
        if (envelope(intent, *raw, complete_opening_hosted_removal).dump() != proof.dump()) reject("proof is not canonical or differs from raw child");
        return {*raw, std::move(intent), complete_opening_hosted_removal};
    } catch (const Json::exception& error) { reject(std::string("malformed proof: ") + error.what()); }
}

Json encode_mixed_wall_deletion_review_proof(const DocumentSnapshot& source, const MixedWallRemovalIntent& intent,
    const Command& command, bool complete_opening_hosted_removal) {
    const auto* raw = std::get_if<ApplyEntityChanges>(&command);
    if (!raw || raw->expected_revision != source.revision()) reject("command lacks captured raw revision authority");
    raw_bound(*raw);
    intent_bound(intent); snapshot_bound(source, intent);
    const auto expected = raw_changes(source.entities(), replay_mixed_wall_removal(source.entities(), intent,
        complete_opening_hosted_removal), source.revision(), raw->message);
    if (command_to_json(Command{expected}).dump() != command_to_json(command).dump()) reject("whole raw command differs from independent actual-source replay");
    bool opening_hosted_proof = complete_opening_hosted_removal;
    if (complete_opening_hosted_removal && !needs_opening_hosted_lane(source.entities(), intent)) {
        const auto legacy = raw_changes(source.entities(), replay_mixed_wall_removal(source.entities(), intent),
            source.revision(), raw->message);
        if (command_to_json(Command{legacy}).dump() == command_to_json(command).dump()) opening_hosted_proof = false;
    }
    const auto result = envelope(intent, expected, opening_hosted_proof);
    (void)decode_mixed_wall_deletion_review_proof(result);
    return result;
}

void validate_mixed_wall_deletion_review_source(const Entities& actual, const Entities& candidate,
    const Command& command, const Json& retained_proof) {
    const auto* raw = std::get_if<ApplyEntityChanges>(&command);
    if (!raw) reject("requires a direct raw command");
    raw_bound(*raw);
    const auto decoded = decode_mixed_wall_deletion_review_proof(retained_proof);
    if (command_to_json(Command{decoded.command}).dump() != command_to_json(command).dump()) reject("retained proof differs from whole raw command");
    const auto replayed = replay_mixed_wall_removal(actual, decoded.intent, decoded.complete_opening_hosted_removal);
    const auto expected = raw_changes(actual, replayed, decoded.command.expected_revision, decoded.command.message);
    if (command_to_json(Command{expected}).dump() != command_to_json(command).dump()) reject("raw command differs from independently replayed source consequences");
    if (!exact_entities(candidate, replayed)) reject("whole candidate differs from independent source replay");
}
} // namespace sketch
