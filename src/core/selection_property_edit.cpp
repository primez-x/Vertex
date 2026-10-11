#include "sketch/selection_property_edit.hpp"

#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_roof_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_store.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t target_limit = 1000;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
constexpr std::size_t node_limit = 100000;
constexpr std::size_t depth_limit = 64;
// Retain the existing wall/opening full-replay aggregate proof limit while
// staging detached source-owned fields before complete physical admission.
constexpr std::size_t profile_batch_byte_limit = 1024 * 1024;

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Selection Properties preparation: ") + reason);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
std::string identity(const Json& value) {
    if (!value.is_string()) reject("identity must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 128 || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid actual source identity");
    return text;
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid source revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source binding must be a digest");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("source binding must be canonical SHA-256");
}
// Account borrowed JSON before copies, dumps and nested codec traversal.
struct Budget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("encoded byte budget exceeded");
        bytes += count;
    }
    void quoted(std::string_view text) {
        add(2);
        for (const auto c : text) {
            const auto byte = static_cast<unsigned char>(c);
            add(c == '"' || c == '\\' ? 2 : byte < 0x20 ? 6 : 1);
        }
    }
    void visit(const Json& value, std::size_t depth = 0) {
        if (depth > depth_limit || ++nodes > node_limit) reject("JSON complexity budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON value");
        if (value.is_object()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); visit(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) visit(child, depth + 1);
        } else if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
const char* family_name(SelectionPropertyFamily family) {
    switch (family) {
    case SelectionPropertyFamily::wall: return "wall";
    case SelectionPropertyFamily::slab: return "slab";
    case SelectionPropertyFamily::hosted_opening: return "hosted_opening";
    case SelectionPropertyFamily::roof: return "roof";
    case SelectionPropertyFamily::column: return "column";
    case SelectionPropertyFamily::beam: return "beam";
    }
    reject("unsupported selected family");
}
SelectionPropertyFamily family(const Json& value, bool structural) {
    if (value == "wall") return SelectionPropertyFamily::wall;
    if (value == "slab") return SelectionPropertyFamily::slab;
    if (value == "hosted_opening") return SelectionPropertyFamily::hosted_opening;
    if (value == "roof") return SelectionPropertyFamily::roof;
    if (structural && value == "column") return SelectionPropertyFamily::column;
    if (structural && value == "beam") return SelectionPropertyFamily::beam;
    reject("unsupported selected family");
}
const char* source_type(SelectionPropertyFamily value) {
    return value == SelectionPropertyFamily::hosted_opening ? "opening" : family_name(value);
}
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}
bool exact(const Entities& left, const Entities& right) {
    if (left.size() != right.size()) return false;
    auto a = left.begin(), b = right.begin();
    for (; a != left.end(); ++a, ++b)
        if (a->first != b->first || !exact(a->second, b->second)) return false;
    return true;
}
Wall resolved_wall(const Entities& entities, const Entity& entity,
    const std::vector<const Entity*>& openings = {}) {
    Wall result;
    std::string error;
    if (!read_document_wall(resolve_vertical_placement(entities, entity), openings, result, error))
        throw std::invalid_argument("Selection Properties wall " + entity.id + ": " + error);
    validate_wall_semantics(result);
    return result;
}
Entities staged_profile_entities(const Entities& original, const auto& intents,
    auto target_id, auto encoder, auto replay) {
    auto result = original;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encoder(intent).dump().size();
        if (bytes > profile_batch_byte_limit - proof_bytes) reject("profile batch proof byte budget exceeded");
        proof_bytes += bytes;
        const auto& id = target_id(intent);
        result.at(id) = replay(original.at(id), intent);
    }
    return result;
}
void source_metadata(const DocumentSnapshot& source) {
    if (!source.is_editable()) reject("captured source is read-only");
    if (source.history().empty() || source.history().size() > ProjectStore::maximum_revision_count ||
        source.revision() >= source.history().size() ||
        source.history()[source.revision()].revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size()))
        reject("captured source history or save metadata is invalid");
}
void authenticate_source(const DocumentSnapshot& source, const Json& value) {
    source_metadata(source);
    const auto saved = value.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(value.at("source_saved_revision"))};
    if (source.revision() != revision(value.at("expected_revision")) || source.saved_revision_optional() != saved ||
        document_snapshot_digest(source) != value.at("source_snapshot_digest") ||
        document_authoring_source_digest_v2(source) != value.at("source_authoring_digest") ||
        entity_map_digest(source.entities()) != value.at("source_entities_digest"))
        reject("captured full source authority changed");
}

SelectionPropertyEditRequest decode_request(const Json& value) {
    Budget{}.visit(value);
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version") != 1 && value.at("version") != 2)) reject("unsupported request version");
    const bool structural = value.at("version") == 2;
    if (structural)
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "selected", "walls", "slabs", "hosted_openings",
            "roofs", "structural_profiles", "unchanged_policy"});
    else
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "selected", "walls", "slabs", "hosted_openings",
            "roofs", "unchanged_policy"});
    (void)revision(value.at("expected_revision"));
    for (const auto* key : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(key));
    // Undo may leave the saved revision ahead of the captured head. It still
    // has to belong to the complete retained history authenticated below.
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    SelectionPropertyEditRequest result;
    if (value.at("unchanged_policy") == "retain") result.unchanged_policy = SelectionPropertyUnchangedPolicy::retain;
    else if (value.at("unchanged_policy") != "reject") reject("unchanged selection policy must be explicit");
    const auto& selected = value.at("selected");
    if (!selected.is_array() || selected.empty() || selected.size() > target_limit) reject("invalid selected target budget");
    std::map<std::string, SelectionPropertyFamily, std::less<>> declared, covered;
    std::string previous;
    for (const auto& row : selected) {
        fields(row, {"entity_id", "family"});
        const auto id = identity(row.at("entity_id"));
        const auto kind = family(row.at("family"), structural);
        if (!previous.empty() && id <= previous) reject("selected roster must be sorted and unique");
        previous = id;
        declared.emplace(id, kind);
        result.selected.push_back({id, kind});
    }
    const auto lane = [&](const char* key, const char* id_key, SelectionPropertyFamily kind, auto decoder, auto encoder, auto& output) {
        const auto& rows = value.at(key);
        if (!rows.is_array() || rows.size() > target_limit) reject("invalid typed lane budget");
        std::string prior;
        for (const auto& row : rows) {
            auto intent = decoder(row);
            const auto canonical = encoder(intent);
            if (canonical.dump() != row.dump()) reject("typed profile must use its exact canonical codec");
            const auto id = identity(canonical.at(id_key));
            if (!prior.empty() && id <= prior) reject("typed lanes must be sorted and unique");
            prior = id;
            if (!covered.emplace(id, kind).second) reject("typed target overlaps another lane");
            output.push_back(std::move(intent));
        }
    };
    lane("walls", "wall_id", SelectionPropertyFamily::wall, decode_wall_profile_edit_intent, encode_wall_profile_edit_intent, result.walls);
    lane("slabs", "slab_id", SelectionPropertyFamily::slab, decode_slab_profile_edit_intent, encode_slab_profile_edit_intent, result.slabs);
    lane("hosted_openings", "opening_id", SelectionPropertyFamily::hosted_opening,
        decode_hosted_opening_profile_edit_intent, encode_hosted_opening_profile_edit_intent, result.hosted_openings);
    lane("roofs", "roof_id", SelectionPropertyFamily::roof, decode_roof_profile_edit_intent, encode_roof_profile_edit_intent, result.roofs);
    if (structural) {
        const auto& rows = value.at("structural_profiles");
        if (!rows.is_array() || rows.empty() || rows.size() > target_limit) reject("invalid structural lane budget");
        std::string prior;
        for (const auto& row : rows) {
            auto intent = decode_structural_profile_edit_intent(row);
            if (encode_structural_profile_edit_intent(intent).dump() != row.dump())
                reject("structural profile must use its exact canonical codec");
            const auto& id = intent.object_id;
            if (!prior.empty() && id <= prior) reject("typed lanes must be sorted and unique");
            prior = id;
            const auto owner = declared.find(id);
            if (owner == declared.end() ||
                (owner->second != SelectionPropertyFamily::column && owner->second != SelectionPropertyFamily::beam))
                reject("structural lane requires an exactly declared column or beam");
            if (!covered.emplace(id, owner->second).second) reject("typed target overlaps another lane");
            result.structural_profiles.push_back(std::move(intent));
        }
    }
    if (covered != declared) reject("selected roster must exactly match every typed target and family");
    // Enforces portable UTF-8 after bounded traversal; no ignored fields survive.
    if (value.dump().size() > byte_limit) reject("encoded byte budget exceeded");
    return result;
}
Json encode_request(const DocumentSnapshot& source, const SelectionPropertyEditRequest& request) {
    source_metadata(source);
    if (request.selected.empty() || request.selected.size() > target_limit || request.walls.size() > target_limit ||
        request.slabs.size() > target_limit || request.hosted_openings.size() > target_limit || request.roofs.size() > target_limit ||
        request.structural_profiles.size() > target_limit)
        reject("invalid typed target budget");
    if (request.unchanged_policy != SelectionPropertyUnchangedPolicy::reject &&
        request.unchanged_policy != SelectionPropertyUnchangedPolicy::retain) reject("unsupported unchanged policy");
    Json selected = Json::array(), walls = Json::array(), slabs = Json::array(), openings = Json::array(),
        roofs = Json::array(), structural = Json::array();
    for (const auto& target : request.selected)
        selected.push_back({{"entity_id", target.entity_id}, {"family", family_name(target.family)}});
    Budget budget;
    budget.visit(selected);
    const auto lane = [&](const auto& inputs, auto encoder, Json& output) {
        for (const auto& input : inputs) {
            auto row = encoder(input);
            budget.visit(row);
            output.push_back(std::move(row));
        }
    };
    lane(request.walls, encode_wall_profile_edit_intent, walls);
    lane(request.slabs, encode_slab_profile_edit_intent, slabs);
    lane(request.hosted_openings, encode_hosted_opening_profile_edit_intent, openings);
    lane(request.roofs, encode_roof_profile_edit_intent, roofs);
    lane(request.structural_profiles, encode_structural_profile_edit_intent, structural);
    Json result = {{"version", request.structural_profiles.empty() ? 1 : 2}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"selected", std::move(selected)}, {"walls", std::move(walls)}, {"slabs", std::move(slabs)},
        {"hosted_openings", std::move(openings)}, {"roofs", std::move(roofs)},
        {"unchanged_policy", request.unchanged_policy == SelectionPropertyUnchangedPolicy::retain ? "retain" : "reject"}};
    if (!request.structural_profiles.empty()) result["structural_profiles"] = std::move(structural);
    (void)decode_request(result);
    return result;
}
} // namespace

Json validate_selection_property_edit_request(const Json& value) {
    (void)decode_request(value);
    return value;
}

SelectionPropertyEditPreparation::SelectionPropertyEditPreparation(const DocumentSnapshot& source, Json request,
    Entities candidate, std::vector<Entities> leaves, std::set<std::string, std::less<>> affected_walls)
    : request_(std::move(request)), preview_(source), leaves_(std::move(leaves)), affected_walls_(std::move(affected_walls)),
      source_digest_(request_.at("source_snapshot_digest").get<std::string>()) {
    auto history = std::make_shared<std::vector<RevisionRecord>>(source.history());
    history->at(source.revision()).entities = std::move(candidate);
    preview_.history_ = std::move(history);
    preview_.editable_ = false;
    preview_.read_only_reason_ = "Selection Properties drawing preview; room review and final admission pending";
    preview_.admission_.reset();
}
const Json& SelectionPropertyEditPreparation::request() const noexcept { return request_; }
const DocumentSnapshot& SelectionPropertyEditPreparation::preview_snapshot() const noexcept { return preview_; }
const std::string& SelectionPropertyEditPreparation::source_snapshot_digest() const noexcept { return source_digest_; }
const std::set<std::string, std::less<>>& SelectionPropertyEditPreparation::affected_wall_ids() const noexcept { return affected_walls_; }
const std::vector<Entities>& SelectionPropertyEditPreparation::original_source_leaf_candidates() const noexcept { return leaves_; }

SelectionPropertyEditPreparation prepare_selection_property_edit(const DocumentSnapshot& source, const SelectionPropertyEditRequest& request) {
    return replay_selection_property_edit_preparation(source, encode_request(source, request));
}

SelectionPropertyEditPreparation replay_selection_property_edit_preparation(const DocumentSnapshot& source, const Json& value) {
    const auto request = decode_request(value);
    authenticate_source(source, value);
    const auto& original = source.entities();
    const auto scope = constraint_phase_scope(original);
    if (const auto diagnostic = validate_active_phase_constraint_integrity(original)) throw std::invalid_argument(*diagnostic);
    for (const auto& target : request.selected) {
        const auto found = original.find(target.entity_id);
        if (found == original.end() || found->second.id != found->first || found->second.type != source_type(target.family))
            reject("selected family requires its actual qualified owner");
        if (scope.inactive_owner_ids.contains(target.entity_id)) reject("selected owner is inactive in the saved design");
    }
    std::set<std::string, std::less<>> affected_walls;
    for (const auto& intent : request.walls) affected_walls.insert(intent.wall_id);
    for (const auto& intent : request.hosted_openings) {
        const auto host = original.find(intent.wall_id);
        if (host == original.end() || host->second.id != host->first || host->second.type != "wall" ||
            scope.inactive_owner_ids.contains(intent.wall_id)) reject("hosted opening requires its actual active wall");
        validate_hosted_opening_profile_entity(original.at(intent.opening_id));
        if (intent.materialize_default_door_assembly &&
            resolved_wall(original, host->second, {&original.at(intent.opening_id)}).baseline.sweep_radians != 0)
            reject("default-door materialization requires a straight actual original wall");
        affected_walls.insert(intent.wall_id);
    }
    validate_active_wall_physical_dependencies(original, affected_walls, true);
    std::vector<Entities> leaves;
    std::set<std::string, std::less<>> invariant_targets;
    const auto add = [&](Entities candidate, const auto& intents, auto target_id) {
        for (const auto& intent : intents) {
            const auto& id = target_id(intent);
            if (exact(original.at(id), candidate.at(id))) {
                if (request.unchanged_policy == SelectionPropertyUnchangedPolicy::reject)
                    reject("unchanged selected owner requires explicit retain policy");
                invariant_targets.insert(id);
            }
        }
        leaves.push_back(std::move(candidate));
    };
    bool depth_changed = false;
    if (!request.walls.empty()) {
        auto wall_contribution = staged_profile_entities(original, request.walls,
            [](const WallProfileEditIntent& intent) -> const std::string& { return intent.wall_id; },
            encode_wall_profile_edit_intent, replay_wall_profile_entity);
        for (const auto& intent : request.walls)
            depth_changed = depth_changed ||
                resolved_wall(original, original.at(intent.wall_id)).thickness !=
                resolved_wall(wall_contribution, wall_contribution.at(intent.wall_id)).thickness;
        add(std::move(wall_contribution), request.walls,
            [](const WallProfileEditIntent& intent) -> const std::string& { return intent.wall_id; });
    }
    if (!request.slabs.empty()) add(replay_slab_profile_entities(original, request.slabs), request.slabs,
        [](const SlabProfileEditIntent& intent) -> const std::string& { return intent.slab_id; });
    if (!request.hosted_openings.empty()) add(staged_profile_entities(original, request.hosted_openings,
        [](const HostedOpeningProfileEditIntent& intent) -> const std::string& { return intent.opening_id; },
        encode_hosted_opening_profile_edit_intent, replay_hosted_opening_profile_entity), request.hosted_openings,
        [](const HostedOpeningProfileEditIntent& intent) -> const std::string& { return intent.opening_id; });
    std::vector<RoofEditIntent> roof_dependency_checks;
    for (const auto& profile : request.roofs) {
        RoofEditIntent edit;
        edit.roof_id = profile.roof_id;
        edit.profile = profile;
        roof_dependency_checks.push_back(std::move(edit));
    }
    if (!request.roofs.empty()) {
        auto roof_candidate = replay_roof_profile_entities(original, request.roofs);
        // The existing composite admits actual saved-active roof joins and
        // cross-member skylight solids. Its profile-only authority must derive
        // exactly the same map as the retained scalar profile replay.
        if (!exact(roof_candidate, replay_roof_edit_entities(original, roof_dependency_checks)))
            reject("roof physical dependency replay differs from scalar authority");
        add(std::move(roof_candidate), request.roofs,
            [](const RoofProfileEditIntent& intent) -> const std::string& { return intent.roof_id; });
    }
    if (!request.structural_profiles.empty())
        add(replay_structural_profile_entities(original, request.structural_profiles), request.structural_profiles,
            [](const StructuralProfileEditIntent& intent) -> const std::string& { return intent.object_id; });
    auto candidate = compose_mixed_selection_edit_candidates(original, leaves);
    // Admit the complete shared-host state, never an intermediate wall-only or
    // opening-only state. Original-source admission above cannot be repaired
    // by this fit check, and no fit result supplies authored scalar authority.
    validate_active_wall_physical_dependencies(candidate, affected_walls, true);
    if (depth_changed) {
        const auto edits = exterior_wall_measurement_source_updates_active_phase(original, candidate, false);
        auto completed = edited_boundary_entities_batch(candidate, edits);
        for (const auto& target : request.selected)
            if (!exact(candidate.at(target.entity_id), completed.at(target.entity_id)))
                reject("derived measurement completion changed a typed selected owner");
        // Preserve these authenticated consequences in the wall contribution
        // for enclosing composition. The other lanes still contain only their
        // own original-source authority; no completed map becomes a new source.
        for (const auto& [id, entity] : candidate)
            if (!exact(entity, completed.at(id))) leaves.front().at(id) = completed.at(id);
        candidate = std::move(completed);
    }
    for (const auto& id : invariant_targets)
        if (!exact(original.at(id), candidate.at(id))) reject("composed consequences changed an invariant selected owner");
    // Full replays are final idempotence/physical checks only. They cannot grant
    // new fields: every result must equal the source-derived complete map.
    // Default-door materialization is a one-time original-source precondition,
    // already proved above. The final fit check uses the now explicit assembly
    // without claiming that the final map is another implicit-door source.
    auto final_opening_checks = request.hosted_openings;
    for (auto& intent : final_opening_checks) intent.materialize_default_door_assembly = false;
    validate_active_wall_physical_dependencies(candidate, affected_walls, true);
    if (!exact(candidate, replay_wall_profile_entities(candidate, request.walls, false, true)) ||
        !exact(candidate, replay_slab_profile_entities(candidate, request.slabs)) ||
        !exact(candidate, replay_hosted_opening_profile_entities(candidate, final_opening_checks, false)) ||
        !exact(candidate, replay_roof_profile_entities(candidate, request.roofs)) ||
        !exact(candidate, replay_roof_edit_entities(candidate, roof_dependency_checks)) ||
        !exact(candidate, replay_structural_profile_entities(candidate, request.structural_profiles)))
        reject("combined physical replay introduced another consequence");
    std::set<std::string, std::less<>> retained_baselines;
    for (const auto& [id, entity] : original) if (entity.type == "model_phases") {
        (void)id;
        const auto model = ModelPhases::from_json(entity.properties.at("model"));
        if (model.active_alternative()) retained_baselines.insert(model.baseline_ids().begin(), model.baseline_ids().end());
    }
    for (const auto& [id, entity] : original)
        if ((entity.type == "room" || scope.inactive_owner_ids.contains(id) || retained_baselines.contains(id)) &&
            !exact(entity, candidate.at(id)))
            reject("preparation changed a retained room, inactive owner or baseline owner");
    // Wall/opening profiles retain rooms for explicit enclosing completion.
    // With no such dependencies, no deferred room residual admission exists.
    if (affected_walls.empty())
        if (const auto diagnostic = validate_active_phase_constraint_integrity(candidate)) throw std::invalid_argument(*diagnostic);
    return {source, value, std::move(candidate), std::move(leaves), std::move(affected_walls)};
}

void require_selection_property_edit_source_current(const DocumentSnapshot& source, const SelectionPropertyEditPreparation& prepared) {
    (void)decode_request(prepared.request());
    authenticate_source(source, prepared.request());
}

} // namespace sketch
