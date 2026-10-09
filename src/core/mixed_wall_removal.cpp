#include "sketch/mixed_wall_removal.hpp"

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/project_organization.hpp"
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
PhysicalWallJoinRemovalAdditionalIdentities destinations(const MixedWallRemovalIntent& intent) {
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
void analytical_work(const Entities& actual, Budget& budget) {
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
    AssemblyExpansionBudget expansion_budget;
    expansion_budget.max_nodes = 4096 / native_passes;
    expansion_budget.max_profile_segments = geometry_limit / native_passes;
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model") {
        (void)id;
        const auto model = AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& row : model.instances()) {
            const auto expansion = model.expand(row, expansion_budget);
            if (expansion.profiles.empty() && row.placement) {
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
Json envelope(const MixedWallRemovalIntent& intent, const ApplyEntityChanges& command) {
    Json result{{"version", 37}, {"kind", "mixed_wall_deletion"}, {"expected_revision", command.expected_revision},
        {"message", command.message}, {"intent", intent_json(intent)}, {"proof", command_to_json(Command{command})}};
    json_bound(result);
    return result;
}
} // namespace

void validate_mixed_wall_removal_source_admission(const Entities& actual) {
    try {
        Budget budget; source_bound(actual, budget); analytical_work(actual, budget);
    } catch (const Json::exception& error) {
        reject(std::string("malformed actual source admission: ") + error.what());
    }
}

Entities replay_mixed_wall_removal(const Entities& actual, const MixedWallRemovalIntent& intent) {
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
        analytical_work(actual, budget);
        const auto original_aliases = embedded_assembly_presentation_ids(actual);
        for (const auto& key : intent.other.components)
            if (!original_aliases.contains(key)) reject("selected qualified component is missing from actual source");
        auto wall = replay_complete_physical_walls_deletion(actual, intent.wall_ids, intent.wall_additional_identities);
        const auto wall_aliases = embedded_assembly_presentation_ids(wall);
        auto other = intent.other;
        std::erase_if(other.components, [&](const auto& key) {
            // Only actual qualified rows already retired by the wall producer
            // collapse. Alias strings and local IDs are never used as authority.
            return original_aliases.contains(key) && !wall_aliases.contains(key);
        });
        std::vector<Entities> candidates; candidates.push_back(std::move(wall));
        if (!other.object_ids.empty() || !other.components.empty())
            candidates.push_back(replay_architectural_selection_removal(actual, other));
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
    const std::string& message) {
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    intent_bound(intent); snapshot_bound(source, intent);
    auto result = raw_changes(source.entities(), replay_mixed_wall_removal(source.entities(), intent), source.revision(), message);
    // Canonical bounded retention, without publishing or rewriting rooms.
    (void)decode_mixed_wall_deletion_review_proof(envelope(intent, result));
    return result;
}

DecodedMixedWallDeletionReviewProof decode_mixed_wall_deletion_review_proof(const Json& proof) {
    try {
        json_bound(proof);
        fields(proof, {"version", "kind", "expected_revision", "message", "intent", "proof"});
        if (!proof.at("version").is_number_integer() || proof.at("version") != 37 || proof.at("kind") != "mixed_wall_deletion")
            reject("requires explicit version37 mixed-wall proof");
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
        if (envelope(intent, *raw).dump() != proof.dump()) reject("proof is not canonical or differs from raw child");
        return {*raw, std::move(intent)};
    } catch (const Json::exception& error) { reject(std::string("malformed proof: ") + error.what()); }
}

Json encode_mixed_wall_deletion_review_proof(const DocumentSnapshot& source, const MixedWallRemovalIntent& intent,
    const Command& command) {
    const auto* raw = std::get_if<ApplyEntityChanges>(&command);
    if (!raw || raw->expected_revision != source.revision()) reject("command lacks captured raw revision authority");
    raw_bound(*raw);
    intent_bound(intent); snapshot_bound(source, intent);
    const auto expected = raw_changes(source.entities(), replay_mixed_wall_removal(source.entities(), intent), source.revision(), raw->message);
    if (command_to_json(Command{expected}).dump() != command_to_json(command).dump()) reject("whole raw command differs from independent actual-source replay");
    const auto result = envelope(intent, expected);
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
    const auto replayed = replay_mixed_wall_removal(actual, decoded.intent);
    const auto expected = raw_changes(actual, replayed, decoded.command.expected_revision, decoded.command.message);
    if (command_to_json(Command{expected}).dump() != command_to_json(command).dump()) reject("raw command differs from independently replayed source consequences");
    if (!exact_entities(candidate, replayed)) reject("whole candidate differs from independent source replay");
}
} // namespace sketch
