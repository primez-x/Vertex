#include "sketch/mixed_wall_opening_removal.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/hosted_opening_removal.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/phase_constraint_authoring.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = MixedWallOpeningRemovalEntities;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t root_limit = 1000, change_limit = 4096, entity_limit = 65536;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t byte_limit = 64 * 1024 * 1024, node_limit = 4 * 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Mixed wall opening removal: " + reason);
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
struct JsonBudget {
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) reject("aggregate JSON string budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || nodes == node_limit) reject("JSON depth/node budget exceeded");
        ++nodes;
        if (value.is_binary() || value.is_discarded()) reject("binary/discarded JSON is unsupported");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("JSON scalar must be finite");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
};
void json_bound(const Json& value) {
    JsonBudget budget; budget.read(value);
    if (value.dump().size() > proof_limit) reject("proof/intent byte budget exceeded");
}
template<class T> void canonical(const std::vector<T>& values) {
    if (!std::is_sorted(values.begin(), values.end()) ||
        std::adjacent_find(values.begin(), values.end()) != values.end())
        reject("selected identities/qualified pairs must be sorted and unique");
}
PhysicalWallJoinRemovalAdditionalIdentities destinations(const MixedWallOpeningRemovalIntent& intent,
    bool reserve_corner_marker=false) {
    PhysicalWallJoinRemovalAdditionalIdentities result;
    Ids fresh;
    for (const auto* mapping : {&intent.wall_additional_identities, &intent.other.roof_additional_identities}) {
        if (mapping->size() > change_limit) reject("destination owner budget exceeded");
        for (const auto& [owner, rows] : *mapping) {
            identity(owner);
            if (rows.empty() || rows.size() > change_limit - fresh.size()) reject("destination slots must be bounded and nonempty");
            if (!result.emplace(owner, rows).second) reject("wall/roof destination owners overlap");
            for (const auto& row : rows) {
                identity(row);
                for (const auto* reserved : {"version", "kind", "expected_revision", "message", "intent", "proof",
                    "mixed_wall_opening_deletion", "apply_entity_changes", "entity_changes", "asset_changes",
                    "wall_ids", "opening_ids", "wall_additional_identities",
                    "other_object_ids", "components", "roof_additional_identities"})
                    if (row == reserved) reject("destination borrows a mixed wall/opening proof token");
                if (reserve_corner_marker && row == "complete_corner_window_consequences")
                    reject("destination borrows the corner-completion mixed wall/opening proof token");
                if (!fresh.insert(row).second) reject("fresh wall/roof destinations overlap");
            }
        }
    }
    for (const auto& [owner, rows] : result) {
        (void)rows;
        if (fresh.contains(owner)) reject("destination borrows a source slot owner");
    }
    for (const auto* roots : {&intent.wall_ids, &intent.opening_ids, &intent.other.object_ids})
        for (const auto& id : *roots) if (fresh.contains(id)) reject("destination borrows a selected owner");
    for (const auto& [catalog, local] : intent.other.components)
        if (fresh.contains(catalog) || fresh.contains(local)) reject("destination borrows a qualified component token");
    return result;
}
void intent_bound(const MixedWallOpeningRemovalIntent& intent) {
    if (intent.wall_ids.empty() || intent.wall_ids.size() > 128 || intent.opening_ids.empty() ||
        intent.opening_ids.size() > root_limit - intent.wall_ids.size() ||
        intent.other.object_ids.size() > root_limit - intent.wall_ids.size() - intent.opening_ids.size() ||
        intent.other.components.size() > root_limit - intent.wall_ids.size() - intent.opening_ids.size() - intent.other.object_ids.size())
        reject("requires actual walls and openings within 1000 aggregate roots/components");
    Ids roots;
    for (const auto* selected : {&intent.wall_ids, &intent.opening_ids, &intent.other.object_ids}) {
        canonical(*selected);
        for (const auto& id : *selected) {
            identity(id);
            if (!roots.insert(id).second) reject("root selection lanes overlap");
        }
    }
    canonical(intent.other.components);
    for (const auto& [catalog, local] : intent.other.components) { identity(catalog); identity(local); }
    (void)destinations(intent);
}
Json intent_json(const MixedWallOpeningRemovalIntent& intent) {
    intent_bound(intent);
    return {{"wall_ids", intent.wall_ids}, {"opening_ids", intent.opening_ids},
        {"wall_additional_identities", intent.wall_additional_identities}, {"other_object_ids", intent.other.object_ids},
        {"components", intent.other.components}, {"roof_additional_identities", intent.other.roof_additional_identities}};
}
MixedWallOpeningRemovalIntent intent_from_json(const Json& value) {
    fields(value, {"wall_ids", "opening_ids", "wall_additional_identities", "other_object_ids", "components", "roof_additional_identities"});
    for (const auto* name : {"wall_ids", "opening_ids", "other_object_ids", "components"})
        if (!value.at(name).is_array()) reject("malformed intent root collections");
    for (const auto* name : {"wall_additional_identities", "roof_additional_identities"})
        if (!value.at(name).is_object()) reject("malformed intent destinations");
    for (const auto& row : value.at("components"))
        if (!row.is_array() || row.size() != 2 || !row[0].is_string() || !row[1].is_string())
            reject("component requires an exact qualified string pair");
    MixedWallOpeningRemovalIntent result;
    result.wall_ids = value.at("wall_ids").get<std::vector<std::string>>();
    result.opening_ids = value.at("opening_ids").get<std::vector<std::string>>();
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
std::string corner_owner(const Entities& actual, const Entity& entity) {
    if (entity.type == "corner_window") return entity.id;
    if (entity.type != "opening" || !entity.properties.contains("corner_window_id")) return {};
    const auto owner_id = entity.properties.at("corner_window_id").get<std::string>();
    const auto owner = actual.find(owner_id);
    if (owner == actual.end() || owner->second.type != "corner_window") reject("managed cut lacks an actual corner owner: " + entity.id);
    const auto corner = parse_corner_window(owner->second);
    if (std::find(corner.opening_ids.begin(), corner.opening_ids.end(), entity.id) == corner.opening_ids.end())
        reject("managed cut is not an actual child of its corner owner: " + entity.id);
    return owner_id;
}
bool wall_covers_corner(const Entities& actual, const std::string& owner, const std::vector<std::string>& walls) {
    const auto corner = parse_corner_window(actual.at(owner));
    return std::any_of(corner.wall_ids.begin(), corner.wall_ids.end(), [&](const auto& wall) {
        return std::binary_search(walls.begin(), walls.end(), wall);
    });
}
void require_retired_corner(const Entities& actual, const Entities& candidate, const std::string& owner) {
    const auto corner = parse_corner_window(actual.at(owner));
    for (const auto& id : {owner, corner.opening_ids[0], corner.opening_ids[1]})
        if (candidate.contains(id)) reject("wall leaf did not retire the complete actual corner aggregate: " + id);
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
bool exact_entities(const Entities& a, const Entities& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [id, entity] : a) {
        const auto found = b.find(id);
        if (found == b.end() || !exact(entity, found->second)) return false;
    }
    return true;
}
void snapshot_bound(const DocumentSnapshot& source, const MixedWallOpeningRemovalIntent& intent) {
    if (!source.is_editable()) reject("captured source is read-only");
    const auto& history = source.history();
    if (history.empty() || history.size() > change_limit || source.revision() >= history.size() ||
        history[source.revision()].revision != source.revision()) reject("captured revision/history is invalid");
    if (source.assets().size() > entity_limit || source.named_revisions().size() > change_limit)
        reject("captured asset/named-revision inventory exceeded");
    if (source.saved_revision_optional() && *source.saved_revision_optional() >= history.size())
        reject("saved revision is outside history");
    for (const auto& [name, revision] : source.named_revisions()) {
        (void)name;
        if (revision >= history.size()) reject("named revision is outside history");
    }
    validate_physical_wall_join_removal_identity_lifetime(source, destinations(intent));
}
ApplyEntityChanges raw_changes(const Entities& actual, const Entities& candidate, Revision revision, const std::string& message) {
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    ApplyEntityChanges result{revision, {}, {}, message};
    for (const auto& [id, entity] : actual) {
        const auto after = candidate.find(id);
        if (after == candidate.end()) result.entity_changes.push_back(EntityChange::erase(id));
        else if (!exact(entity, after->second)) result.entity_changes.push_back(EntityChange::upsert(after->second));
    }
    for (const auto& [id, entity] : candidate) if (!actual.contains(id)) result.entity_changes.push_back(EntityChange::upsert(entity));
    if (result.entity_changes.empty() || result.entity_changes.size() > change_limit) reject("raw change inventory exceeded");
    return result;
}
void raw_bound(const ApplyEntityChanges& command) {
    if (!command.asset_changes.empty() || command.entity_changes.empty() || command.entity_changes.size() > change_limit ||
        command.message.empty() || command.message.size() > 4096) reject("requires a bounded asset-free raw command");
    JsonBudget budget;
    Ids changed;
    for (const auto& change : command.entity_changes) {
        const auto& id = change.kind == EntityChangeKind::erase ? change.entity_id : change.entity.id;
        identity(id);
        if (!changed.insert(id).second) reject("raw child repeats an owner");
        if (change.kind == EntityChangeKind::upsert) {
            budget.text(change.entity.type);
            budget.read(change.entity.properties); budget.read(change.entity.extensions);
        }
    }
    json_bound(command_to_json(Command{command}));
}
Json envelope(const MixedWallOpeningRemovalIntent& intent, const ApplyEntityChanges& command,
    bool complete_corner_window_consequences=false) {
    if (complete_corner_window_consequences) (void)destinations(intent, true);
    Json result{{"version", complete_corner_window_consequences ? 50 : 40}, {"kind", "mixed_wall_opening_deletion"}, {"expected_revision", command.expected_revision},
        {"message", command.message}, {"intent", intent_json(intent)}, {"proof", command_to_json(Command{command})}};
    if (complete_corner_window_consequences) result["complete_corner_window_consequences"] = true;
    json_bound(result);
    return result;
}
void geometry_admission(const DocumentSnapshot& source, const Entities& candidate, const ApplyEntityChanges& command,
    const MixedWallOpeningRemovalIntent& intent) {
    const auto preview = Document::preview_command(source, Command{command});
    if (!exact_entities(preview.entities(), candidate) || preview.assets() != source.assets() ||
        !preview.is_editable() || preview.named_revisions() != source.named_revisions() ||
        preview.saved_revision_optional() != source.saved_revision_optional())
        reject("real raw command admission changed exact candidate/assets/editability/metadata");
    Ids required;
    for (const auto& id : intent.opening_ids)
        required.insert(source.entities().at(id).properties.at("wall_id").get<std::string>());
    for (const auto& change : command.entity_changes) if (change.kind == EntityChangeKind::upsert && physical(change.entity))
        required.insert(change.entity.id);
    std::erase_if(required, [&](const auto& id) { return !candidate.contains(id); });
    validate_architectural_geometry_changes(source, preview, std::vector<std::string>(required.begin(), required.end()));
}
} // namespace

Entities replay_mixed_wall_opening_removal(const Entities& actual, const MixedWallOpeningRemovalIntent& intent,
    bool active_phase_constraints, bool complete_corner_window_consequences) try {
    intent_bound(intent); json_bound(intent_json(intent));
    // Shared full-source 16-pass bounds reserve wall/join/hosted components,
    // independent opening source/candidate hosts, roof inspection/replay and
    // ordinary components, composers and final admission before any builder.
    // No nested snapshot producer/encoder or fallback replay resets that budget.
    validate_mixed_wall_removal_source_admission(actual, true, complete_corner_window_consequences);
    for (const auto& id : intent.wall_ids) {
        const auto found = actual.find(id);
        if (found == actual.end() || found->second.type != "wall") reject("requires actual wall owner: " + id);
    }
    std::vector<std::string> independent_openings;
    Ids selected_corners;
    for (const auto& id : intent.opening_ids) {
        const auto found = actual.find(id);
        if (found == actual.end() || found->second.type != "opening") reject("requires actual semantic opening: " + id);
        const auto corner = complete_corner_window_consequences ? corner_owner(actual, found->second) : std::string{};
        if (!corner.empty()) {
            selected_corners.insert(corner);
            continue;
        }
        const auto host = found->second.properties.at("wall_id").get<std::string>();
        if (!std::binary_search(intent.wall_ids.begin(), intent.wall_ids.end(), host)) independent_openings.push_back(id);
    }
    bool selected_roof = false;
    for (const auto& id : intent.other.object_ids) {
        const auto found = actual.find(id);
        if (found == actual.end()) reject("requires actual architectural owner: " + id);
        const auto corner = complete_corner_window_consequences ? corner_owner(actual, found->second) : std::string{};
        if (!corner.empty()) selected_corners.insert(corner);
        else if (!physical(found->second) || found->second.type == "wall")
            reject("requires an admitted non-wall architectural owner: " + id);
        selected_roof = selected_roof || found->second.type == "roof";
    }
    if (!selected_roof && !intent.other.roof_additional_identities.empty()) reject("roof destinations lack a selected roof");
    validate_physical_wall_join_removal_identity_lifetime(actual, {}, 0, destinations(intent, complete_corner_window_consequences));
    const auto source_constraints = active_phase_constraints
        ? validate_active_phase_constraint_integrity(actual) : validate_constraint_integrity(actual);
    if (source_constraints) reject(*source_constraints);
    const auto original_aliases = embedded_assembly_presentation_ids(actual);
    for (const auto& key : intent.other.components)
        if (!original_aliases.contains(key)) reject("selected qualified component is absent from actual source");
    std::vector<Entities> candidates;
    candidates.push_back(replay_complete_physical_walls_deletion(actual, intent.wall_ids,
        intent.wall_additional_identities, true, complete_corner_window_consequences, complete_corner_window_consequences));
    // Collapsed roots must actually be retired, not merely share a host token.
    for (const auto& id : intent.opening_ids)
        if ((!complete_corner_window_consequences || corner_owner(actual, actual.at(id)).empty()) &&
            std::binary_search(intent.wall_ids.begin(), intent.wall_ids.end(),
            actual.at(id).properties.at("wall_id").get<std::string>()) && candidates.front().contains(id))
            reject("wall leaf did not retire its selected semantic opening");
    if (!independent_openings.empty()) {
        auto opening = replay_hosted_opening_removal(actual, independent_openings, active_phase_constraints,
            complete_corner_window_consequences);
        if (!opening) reject("independent opening leaf did not admit selection");
        candidates.push_back(std::move(*opening));
    }
    auto other = intent.other;
    if (complete_corner_window_consequences) {
        Ids independent;
        for (const auto& id : other.object_ids)
            if (corner_owner(actual, actual.at(id)).empty()) independent.insert(id);
        for (const auto& owner : selected_corners) {
            if (wall_covers_corner(actual, owner, intent.wall_ids))
                require_retired_corner(actual, candidates.front(), owner);
            else independent.insert(owner);
        }
        other.object_ids.assign(independent.begin(), independent.end());
    }
    for (const auto& candidate : candidates) {
        const auto remaining = embedded_assembly_presentation_ids(candidate);
        std::erase_if(other.components, [&](const auto& key) {
            // Qualified actual row presence alone establishes prior retirement.
            return original_aliases.contains(key) && !remaining.contains(key);
        });
    }
    if (!other.object_ids.empty() || !other.components.empty())
        candidates.push_back(replay_architectural_selection_removal(actual, other, true,
            complete_corner_window_consequences, complete_corner_window_consequences,
            complete_corner_window_consequences, complete_corner_window_consequences));
    auto expected_aliases = original_aliases;
    auto expected_inactive = constraint_phase_scope(actual).inactive_owner_ids;
    for (const auto& candidate : candidates) {
        const auto remaining = embedded_assembly_presentation_ids(candidate);
        for (const auto& [key, alias] : remaining) {
            const auto before = original_aliases.find(key);
            if (before == original_aliases.end() || before->second != alias) reject("leaf changed a surviving component alias");
        }
        for (const auto& [key, alias] : original_aliases) { (void)alias; if (!remaining.contains(key)) expected_aliases.erase(key); }
        const auto scope = constraint_phase_scope(candidate);
        expected_inactive.insert(scope.inactive_owner_ids.begin(), scope.inactive_owner_ids.end());
    }
    auto result = candidates.size() == 1 ? std::move(candidates.front()) :
        compose_ordinary_architectural_removal_candidates(actual, candidates, true, complete_corner_window_consequences);
    if (embedded_assembly_presentation_ids(result) != expected_aliases) reject("composition changed surviving aliases");
    if (constraint_phase_scope(result).inactive_owner_ids != expected_inactive) reject("composition changed inactive ownership");
    if (complete_corner_window_consequences) validate_corner_window_state(result);
    for (const auto& [id, entity] : actual) if (entity.type == "room" || entity.type == "boundary") {
        const auto after = result.find(id);
        if (after == result.end() || !exact(entity, after->second)) reject("removal changed retained room/boundary lineage");
    }
    // Other roots retain their dedicated leaf's qualified roof retention;
    // this composer adds no independent baseline-demolition authority.
    for (const auto* roots : {&intent.wall_ids, &intent.opening_ids})
        for (const auto& id : *roots) if (result.contains(id)) reject("selected actual root survived removal");
    const auto final_constraints = active_phase_constraints
        ? validate_active_phase_constraint_integrity(result) : validate_constraint_integrity(result);
    if (final_constraints) reject(*final_constraints);
    return result;
} catch (const Json::exception& error) {
    reject(std::string("malformed actual source/intent: ") + error.what());
} catch (const Standard_Failure& error) {
    const auto* detail = error.GetMessageString();
    reject(std::string("native actual-source admission failed: ") + (detail ? detail : "Open CASCADE failure"));
}

ApplyEntityChanges prepare_mixed_wall_opening_removal(const DocumentSnapshot& source,
    const MixedWallOpeningRemovalIntent& intent, const std::string& message, bool complete_corner_window_consequences) try {
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    intent_bound(intent); snapshot_bound(source, intent);
    const auto candidate = replay_mixed_wall_opening_removal(source.entities(), intent, source.uses_active_phase_constraints(),
        complete_corner_window_consequences);
    auto result = raw_changes(source.entities(), candidate, source.revision(), message);
    (void)decode_mixed_wall_opening_deletion_review_proof(envelope(intent, result, complete_corner_window_consequences));
    geometry_admission(source, candidate, result, intent);
    return result;
} catch (const Json::exception& error) {
    reject(std::string("malformed captured source: ") + error.what());
} catch (const Standard_Failure& error) {
    const auto* detail = error.GetMessageString();
    reject(std::string("native captured-source admission failed: ") + (detail ? detail : "Open CASCADE failure"));
}

DecodedMixedWallOpeningDeletionReviewProof decode_mixed_wall_opening_deletion_review_proof(const Json& proof) try {
    json_bound(proof);
    if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
        (proof.at("version") != 40 && proof.at("version") != 50))
        reject("requires explicit version40/50 mixed-wall-opening proof");
    const bool complete_corner_window_consequences = proof.at("version") == 50;
    if (complete_corner_window_consequences) {
        fields(proof, {"version", "kind", "expected_revision", "message", "intent", "proof", "complete_corner_window_consequences"});
        if (!proof.at("complete_corner_window_consequences").is_boolean() || proof.at("complete_corner_window_consequences") != true)
            reject("version50 requires literal true corner completion");
    } else fields(proof, {"version", "kind", "expected_revision", "message", "intent", "proof"});
    if (proof.at("kind") != "mixed_wall_opening_deletion") reject("requires explicit mixed-wall-opening proof kind");
    auto intent = intent_from_json(proof.at("intent"));
    const auto& child = proof.at("proof");
    fields(child, {"version", "kind", "expected_revision", "message", "entity_changes", "asset_changes"});
    if (!child.at("version").is_number_integer() || child.at("version") != 1 || child.at("kind") != "apply_entity_changes" ||
        !child.at("entity_changes").is_array() || child.at("entity_changes").empty() || child.at("entity_changes").size() > change_limit ||
        !child.at("asset_changes").is_array() || !child.at("asset_changes").empty())
        reject("requires bounded asset-free raw version-one child");
    const auto decoded = command_from_json(child);
    const auto* raw = std::get_if<ApplyEntityChanges>(&decoded);
    if (!raw) reject("invalid raw child");
    raw_bound(*raw);
    Ids erased;
    for (const auto& change : raw->entity_changes) if (change.kind == EntityChangeKind::erase) erased.insert(change.entity_id);
    for (const auto* roots : {&intent.wall_ids, &intent.opening_ids})
        for (const auto& id : *roots) if (!erased.contains(id)) reject("declared actual root is not erased");
    Ids declared_wall_joins, declared_destinations, actual_destinations;
    for (const auto& [owner, rows] : intent.wall_additional_identities) {
        (void)owner; declared_wall_joins.insert(rows.begin(), rows.end());
    }
    for (const auto& [owner, rows] : destinations(intent, complete_corner_window_consequences)) {
        (void)owner; declared_destinations.insert(rows.begin(), rows.end());
    }
    for (const auto& change : raw->entity_changes) if (declared_destinations.contains(
        change.kind == EntityChangeKind::erase ? change.entity_id : change.entity.id)) {
        if (change.kind != EntityChangeKind::upsert) reject("declared fresh destination requires an upsert");
        if (declared_wall_joins.contains(change.entity.id) && change.entity.type != "wall_join")
            reject("declared fresh wall join requires a typed upsert");
        actual_destinations.insert(change.entity.id);
    }
    if (actual_destinations != declared_destinations) reject("declared fresh wall/roof destination is absent from raw child");
    if (envelope(intent, *raw, complete_corner_window_consequences).dump() != proof.dump())
        reject("proof is not canonical or differs from whole raw child");
    return {*raw, std::move(intent), complete_corner_window_consequences};
} catch (const Json::exception& error) {
    reject(std::string("malformed proof: ") + error.what());
}

Json encode_mixed_wall_opening_deletion_review_proof(const DocumentSnapshot& source,
    const MixedWallOpeningRemovalIntent& intent, const Command& command, bool complete_corner_window_consequences) {
    const auto* raw = std::get_if<ApplyEntityChanges>(&command);
    if (!raw || raw->expected_revision != source.revision()) reject("command lacks captured raw revision authority");
    raw_bound(*raw); intent_bound(intent); snapshot_bound(source, intent);
    const auto candidate = replay_mixed_wall_opening_removal(source.entities(), intent, source.uses_active_phase_constraints(),
        complete_corner_window_consequences);
    const auto expected = raw_changes(source.entities(), candidate, source.revision(), raw->message);
    if (command_to_json(Command{expected}).dump() != command_to_json(command).dump())
        reject("whole raw command differs from independent actual-source replay");
    const auto result = envelope(intent, expected, complete_corner_window_consequences);
    (void)decode_mixed_wall_opening_deletion_review_proof(result);
    return result;
}

void validate_mixed_wall_opening_deletion_review_source(const Entities& actual, const Entities& candidate,
    const Command& command, const Json& retained_proof, bool active_phase_constraints) {
    const auto* raw = std::get_if<ApplyEntityChanges>(&command);
    if (!raw) reject("requires a direct raw command");
    raw_bound(*raw);
    const auto decoded = decode_mixed_wall_opening_deletion_review_proof(retained_proof);
    if (command_to_json(Command{decoded.command}).dump() != command_to_json(command).dump())
        reject("retained proof differs from whole raw command");
    const auto replayed = replay_mixed_wall_opening_removal(actual, decoded.intent, active_phase_constraints,
        decoded.complete_corner_window_consequences);
    const auto expected = raw_changes(actual, replayed, decoded.command.expected_revision, decoded.command.message);
    if (command_to_json(Command{expected}).dump() != command_to_json(command).dump())
        reject("raw command differs from independent source consequences");
    if (!exact_entities(candidate, replayed)) reject("whole candidate differs from independent source replay");
}
} // namespace sketch
