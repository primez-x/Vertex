#include "sketch/phase_wall_demolition.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/architectural_object_removal.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t entity_limit = 65536, root_limit = 128, affected_limit = 4096;
constexpr std::size_t node_limit = 4 * 1024 * 1024, byte_limit = 64 * 1024 * 1024;
constexpr std::size_t phase_limit = 2000000;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase wall demolition: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters: " + id);
}
const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}
struct Budget {
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) reject("source string/key budget exceeded");
        bytes += value.size();
    }
    void read(const Json& root) {
        std::vector<std::pair<const Json*, std::size_t>> pending{{&root, 0}};
        while (!pending.empty()) {
            const auto [value, depth] = pending.back(); pending.pop_back();
            if (depth > 64 || ++nodes > node_limit) reject("source JSON node/nesting budget exceeded");
            if (value->is_number_float() && !std::isfinite(value->get<double>()))
                reject("source contains a nonfinite scalar");
            if (value->is_string()) text(value->get_ref<const std::string&>());
            if (value->is_binary()) {
                if (value->get_binary().size() > byte_limit - bytes) reject("source binary budget exceeded");
                bytes += value->get_binary().size();
            }
            if (!value->is_structured()) continue;
            if (value->size() > node_limit - nodes || pending.size() > node_limit - nodes - value->size())
                reject("source JSON pending-node budget exceeded");
            if (value->is_object()) for (const auto& [key, child] : value->items()) {
                text(key); pending.emplace_back(&child, depth + 1);
            } else for (const auto& child : *value) pending.emplace_back(&child, depth + 1);
        }
    }
};
// Bound raw source and phase work before any phase/catalog/opening codec.
void bounds(const Entities& source) {
    if (source.size() > entity_limit) reject("source entity budget exceeded");
    Budget budget;
    std::size_t phase_work{}, catalog_rows{};
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
        if (entity.type == "model_phases") {
            const auto model = field(entity.properties, "model");
            const auto members = model ? field(*model, "entity_ids") : nullptr;
            const auto alternatives = model ? field(*model, "alternatives") : nullptr;
            if (!members || !members->is_array() || members->size() > entity_limit ||
                !alternatives || !alternatives->is_array() || alternatives->size() > affected_limit)
                reject("phase registry lacks bounded actual inventory: " + id);
            if (members->size() > (phase_limit - phase_work) / (alternatives->size() + 1))
                reject("aggregate phase state work budget exceeded");
            phase_work += members->size() * (alternatives->size() + 1);
        } else if (entity.type == "assembly_model") {
            const auto model = field(entity.properties, "model");
            for (const auto* key : {"materials", "types", "instances"}) {
                const auto rows = model ? field(*model, key) : nullptr;
                if (!rows || !rows->is_array() || rows->size() > entity_limit - catalog_rows)
                    reject("catalog lacks bounded actual inventory: " + id);
                catalog_rows += rows->size();
            }
        }
    }
}
bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::binary_search(ids.begin(), ids.end(), id);
}
void admit_opening(const Entity& opening) {
    for (const auto& [canonical, alias] : {
            std::pair{"offset_m", "offset"}, std::pair{"width_m", "width"},
            std::pair{"sill_m", "sill"}, std::pair{"height_m", "height"}}) {
        const auto first = field(opening.properties, canonical), second = field(opening.properties, alias);
        const auto chosen = first ? first : second;
        if (!chosen || !chosen->is_number() || !std::isfinite(chosen->get<double>()) ||
            (second && (!second->is_number() || second->get<double>() != chosen->get<double>())))
            reject("opening has missing, nonfinite or conflicting scalar aliases: " + opening.id);
    }
    std::optional<OpeningAssemblyKind> family;
    if (const auto kind = field(opening.properties, "opening_kind")) {
        if (!kind->is_string()) reject("opening kind must be a string: " + opening.id);
        const auto& name = kind->get_ref<const std::string&>();
        family = parse_opening_assembly_kind(name);
        if (!family && name != "opening") reject("unsupported opening kind: " + opening.id);
    }
    if (const auto raw = field(opening.properties, "opening_assembly")) {
        const auto assembly = parse_opening_assembly(*raw);
        if (const auto kind = field(opening.properties, "opening_kind");
            kind && kind->get_ref<const std::string&>() != opening_assembly_kind_name(assembly.kind))
            reject("opening kind and assembly disagree: " + opening.id);
        family = assembly.kind;
    }
    if (const auto operation = field(opening.properties, "door_operation")) {
        (void)decode_door_operation(*operation);
        if (!family || *family != OpeningAssemblyKind::door)
            reject("door operation requires a retained door assembly: " + opening.id);
    }
}
void append_missing(Json& rows, const Ids& additions) {
    Ids retained;
    for (const auto& row : rows) retained.insert(row.get<std::string>());
    for (const auto& id : additions) if (retained.insert(id).second) rows.push_back(id);
}
std::optional<Entity> prepare_registry(const Entities& entities,const std::vector<std::string>& selected_wall_ids,
    bool complete_corners=false,std::vector<std::string>* corner_owner_ids=nullptr) {
    if (selected_wall_ids.size() > root_limit) reject("selected wall count exceeds 128");
    if (selected_wall_ids.empty()) return std::nullopt;
    try {
        bounds(entities);
        const auto scope = constraint_phase_scope(entities);
        std::map<std::string, const PhysicalWallPhaseState*, std::less<>> owners;
        std::map<std::string, ModelPhases, std::less<>> models;
        for (const auto& registry : scope.registries) {
            models.emplace(registry.registry_id, ModelPhases::from_json(
                entities.at(registry.registry_id).properties.at("model")));
            for (const auto& id : registry.registered_entity_ids)
                if (!owners.emplace(id, &registry).second) reject("overlapping actual phase ownership: " + id);
        }
        Ids walls;
        const PhysicalWallPhaseState* destination = nullptr;
        bool ordinary = false;
        for (const auto& id : selected_wall_ids) {
            identity(id);
            if (!walls.insert(id).second) reject("selected wall identity is duplicated: " + id);
            const auto entity = entities.find(id);
            const auto owner = owners.find(id);
            if (entity == entities.end() || entity->second.type != "wall" || owner == owners.end() ||
                !owner->second->alternative_id || !contains(models.at(owner->second->registry_id).baseline_ids(), id)) {
                ordinary = true; continue;
            }
            if (destination && destination != owner->second) reject("selected baseline walls span actual registries");
            destination = owner->second;
        }
        if (!destination) return std::nullopt;
        if (ordinary) reject("selection mixes shared-baseline walls with ordinary/proposed/foreign roots");
        const auto& phases = models.at(destination->registry_id);
        if (phases.active_alternative() != destination->alternative_id)
            reject("destination differs from actual saved alternative");
        const auto active_baseline = [&](const std::string& id, bool unregistered_allowed) {
            const auto found = entities.find(id);
            if (found == entities.end()) reject("affected owner is missing: " + id);
            if (found->second.required) reject("affected owner is required: " + id);
            if (scope.inactive_owner_ids.contains(id)) reject("affected owner is inactive in saved design: " + id);
            const auto owner = owners.find(id);
            if (owner == owners.end()) {
                if (unregistered_allowed) return;
                reject("selected wall has no actual phase membership: " + id);
            }
            const auto state = owner->second->states.find(id);
            if (owner->second != destination || !contains(phases.baseline_ids(), id) ||
                state == owner->second->states.end() || state->second != ModelPhase::existing)
                reject("affected owner requires the same actual active baseline membership: " + id);
        };
        for (const auto& id : walls) active_baseline(id, false);

        Ids demolished = walls, affected_owners = walls, inherited_openings;
        Ids proposed_corner_cuts;
        std::vector<std::string> corners;
        if (complete_corners) {
            // The complete original map supplies reciprocal ownership, exact
            // derived cuts, both hosts and every saved phase alternative.
            validate_corner_window_state(entities);
            Ids closure;
            const auto active_participant = [&](const std::string& id) {
                const auto found=entities.find(id);
                const auto member=owners.find(id);
                if (found==entities.end() || found->second.required || scope.inactive_owner_ids.contains(id) ||
                    member==owners.end() || member->second!=destination)
                    reject("corner participant requires unprotected same-registry active ownership: "+id);
                const auto state=destination->states.find(id);
                if (state==destination->states.end() ||
                    (state->second!=ModelPhase::existing && state->second!=ModelPhase::proposed))
                    reject("corner participant lacks actual saved activity: "+id);
            };
            const auto sole_proposal = [&](const std::string& id) {
                if (contains(phases.baseline_ids(),id)) return false;
                std::size_t proposals{};
                for (const auto& alternative:phases.alternatives()) {
                    if (contains(alternative.demolished_ids,id)) return false;
                    if (!contains(alternative.proposed_ids,id)) continue;
                    if (alternative.id!=*destination->alternative_id) return false;
                    ++proposals;
                }
                return proposals==1;
            };
            for (const auto& [id,entity]:entities) {
                if (entity.type!="corner_window") continue;
                const auto corner=parse_corner_window(entity);
                if (std::none_of(corner.wall_ids.begin(),corner.wall_ids.end(),[&](const auto& host) {
                    return walls.contains(host);
                })) continue;
                if (corners.size()>=1000) reject("affected complete corner owner budget exceeded");
                const bool baseline=contains(phases.baseline_ids(),id);
                for (const auto& participant:{id,corner.opening_ids[0],corner.opening_ids[1],
                        corner.wall_ids[0],corner.wall_ids[1]}) {
                    active_participant(participant);
                    closure.insert(participant);
                    if (closure.size()>affected_limit) reject("affected complete corner inventory exceeds budget");
                }
                for (const auto& participant:{id,corner.opening_ids[0],corner.opening_ids[1]}) {
                    if (baseline) {
                        active_baseline(participant,false);
                        demolished.insert(participant);affected_owners.insert(participant);
                    } else if (!sole_proposal(participant)) {
                        reject("proposed corner retirement requires sole saved-active ownership: "+participant);
                    }
                }
                if (!baseline) proposed_corner_cuts.insert(corner.opening_ids.begin(),corner.opening_ids.end());
                corners.push_back(id);
            }
            // Both the mixed full-source reservation and complete analytical
            // removal admission precede any native-capable replay. Inspectors
            // use these same checks without manufacturing an interim snapshot.
            validate_mixed_wall_removal_source_admission(entities,true,true);
            if (!corners.empty()) preflight_architectural_object_removal(
                entities,corners,{},0,true,false,true,true);
        }
        std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
        for (const auto& [id, entity] : entities) {
            const auto raw_host = field(entity.properties, "wall_id");
            if (!raw_host || !raw_host->is_string() || !walls.contains(raw_host->get_ref<const std::string&>())) continue;
            // Legacy door/window entities are neither phase roles nor admitted
            // host-inherited presentation owners. Never enroll their type.
            if (entity.type != "opening") reject("unsupported affected wall-hosted entity type " + entity.type + ": " + id);
            std::string host, diagnostic;
            if (!read_document_wall_id(entity, host, diagnostic)) reject(id + ": " + diagnostic);
            // Only a complete reciprocally admitted proposed aggregate can
            // bypass baseline parking. The public replay physically retires
            // its owner and both cuts through original-source corner removal.
            if (proposed_corner_cuts.contains(id)) continue;
            active_baseline(id, true);
            admit_opening(entity);
            if (!affected_owners.contains(id) && affected_owners.size() >= affected_limit)
                reject("affected wall/opening inventory exceeds budget");
            affected_owners.insert(id);
            if (owners.contains(id)) demolished.insert(id);
            else inherited_openings.insert(id);
            openings[host].push_back(&entity);
        }
        // The shared document wall codec admits actual semantic cuts against
        // their retained source host. Pure semantic validation, no native body.
        for (const auto& id : walls) {
            Wall wall; std::string diagnostic;
            if (!read_document_wall(entities.at(id), openings[id], wall, diagnostic)) reject(id + ": " + diagnostic);
            validate_wall_semantics(wall);
        }
        for (const auto& [id, entity] : entities) {
            if (entity.type != "assembly_model") continue;
            const auto& raw = entity.properties.at("model");
            bool affected = false;
            for (const auto& row : raw.at("instances")) {
                const auto placement = field(row, "placement");
                const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
                if (host && host->is_string() && affected_owners.contains(host->get_ref<const std::string&>())) affected = true;
            }
            if (!affected) continue;
            active_baseline(id, true);
            const auto catalog = AssemblyModel::from_json(raw);
            for (const auto& row : catalog.instances()) {
                if (!row.placement || !affected_owners.contains(row.placement->host_entity_id)) continue;
                // row.id and type_id are catalog-local identities, never phase
                // entities. Keep every raw row, definition and override exact;
                // supported placement visibility follows its actual host.
                active_baseline(row.placement->host_entity_id, true);
            }
        }

        const auto alternative = std::find_if(phases.alternatives().begin(), phases.alternatives().end(),
            [&](const auto& row) { return row.id == *destination->alternative_id; });
        if (alternative == phases.alternatives().end()) reject("saved active alternative is missing");
        auto replacement = entities.at(destination->registry_id);
        auto& raw_model = replacement.properties.at("model");
        for (auto& row : raw_model.at("alternatives"))
            if (row.at("id") == alternative->id) append_missing(row.at("demolished_ids"), demolished);
        auto alternatives = phases.alternatives();
        for (auto& row : alternatives) if (row.id == alternative->id)
            row.demolished_ids.insert(row.demolished_ids.end(), demolished.begin(), demolished.end());
        // Existing demolished IDs cannot include any newly affected active
        // owner. All arrays retain their raw source order plus sorted additions.
        auto stage = entities;
        stage.at(destination->registry_id) = replacement;
        bounds(stage);
        const auto admitted = ModelPhases::create(phases.entity_ids(), phases.baseline_ids(),
            std::move(alternatives), phases.active_alternative());
        if (ModelPhases::from_json(raw_model).to_json() != admitted.to_json())
            reject("raw registry patch differs from typed phase update");
        const auto after = constraint_phase_scope(stage);
        auto expected_inactive = scope.inactive_owner_ids;
        expected_inactive.insert(demolished.begin(), demolished.end());
        if (after.inactive_owner_ids != expected_inactive) reject("registry patch changed unrelated saved activity");
        // Only actual semantic opening entities inherit inactivity. This is the
        // same host contract used by saved_design_reference_inactive, plan/native
        // visibility and IFC/DXF output; constraint scope stays registry-owned.
        // Verify it against actual source and candidate without adding membership.
        for (const auto& id : inherited_openings) {
            std::string host, diagnostic;
            if (!read_document_wall_id(entities.at(id), host, diagnostic) ||
                scope.inactive_owner_ids.contains(host) || !after.inactive_owner_ids.contains(host) ||
                owners.contains(id) || after.inactive_owner_ids.contains(id) || stage.at(id) != entities.at(id))
                reject("unregistered opening lacks exact inherited host inactivity: " + id);
        }
        if (corner_owner_ids) *corner_owner_ids=std::move(corners);
        return replacement;
    } catch (const Json::exception& error) {
        reject(std::string("malformed source: ") + error.what());
    }
}
} // namespace

nlohmann::json encode_phase_wall_demolition_intent(const PhaseWallDemolitionIntent& intent) {
    identity(intent.registry_id);identity(intent.alternative_id);
    if (intent.wall_ids.empty() || intent.wall_ids.size()>root_limit)
        reject("semantic wall selection requires one to 128 actual roots");
    for (std::size_t index=0;index<intent.wall_ids.size();++index) {
        identity(intent.wall_ids[index]);
        if (index && intent.wall_ids[index-1]>=intent.wall_ids[index])
            reject("semantic wall roots must be ascending and unique");
    }
    Json result{{"version",intent.complete_actual_corner_window_cohorts ? 2 : 1},
        {"registry_id",intent.registry_id},{"alternative_id",intent.alternative_id},{"wall_ids",intent.wall_ids}};
    if (intent.complete_actual_corner_window_cohorts) result["complete_actual_corner_window_cohorts"]=true;
    return result;
}

PhaseWallDemolitionIntent decode_phase_wall_demolition_intent(const Json& value) {
    try {
        const bool complete=value.is_object() && value.contains("version") &&
            value.at("version").is_number_integer() && value.at("version")==2;
        if (!value.is_object() || value.size()!=(complete ? 5 : 4) || !value.contains("version") ||
            !value.at("version").is_number_integer() || (!complete && value.at("version")!=1) ||
            (complete && (!value.contains("complete_actual_corner_window_cohorts") ||
                !value.at("complete_actual_corner_window_cohorts").is_boolean() ||
                value.at("complete_actual_corner_window_cohorts")!=true)) ||
            !value.contains("registry_id") || !value.at("registry_id").is_string() ||
            !value.contains("alternative_id") || !value.at("alternative_id").is_string() ||
            !value.contains("wall_ids") || !value.at("wall_ids").is_array() || value.at("wall_ids").size()>root_limit)
            reject("semantic wall intent requires exact version-one or explicit version-two fields");
        const auto& registry_id=value.at("registry_id").get_ref<const std::string&>();
        const auto& alternative_id=value.at("alternative_id").get_ref<const std::string&>();
        identity(registry_id);identity(alternative_id);
        PhaseWallDemolitionIntent result{registry_id,alternative_id,{}};
        result.complete_actual_corner_window_cohorts=complete;
        for (const auto& id:value.at("wall_ids")) {
            if (!id.is_string()) reject("semantic wall root must be an actual identity");
            if (id.get_ref<const std::string&>().size()>128) reject("semantic wall root identity exceeds budget");
            result.wall_ids.push_back(id.get<std::string>());
        }
        (void)encode_phase_wall_demolition_intent(result);
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed semantic wall intent: ")+error.what()); }
}

std::optional<PhaseWallDemolitionSelection> inspect_phase_wall_demolition_selection(
    const Entities& actual,const std::vector<std::string>& selected_wall_ids,bool complete_actual_corner_window_cohorts) {
    if (selected_wall_ids.empty()) return std::nullopt;
    if (selected_wall_ids.size()>root_limit) reject("selected wall count exceeds 128");
    bounds(actual);
    const auto scope=constraint_phase_scope(actual);
    std::map<std::string,bool,std::less<>> owners;
    for (const auto& registry:scope.registries) {
        const auto model=ModelPhases::from_json(actual.at(registry.registry_id).properties.at("model"));
        for (const auto& id:registry.registered_entity_ids)
            if (!owners.emplace(id,registry.alternative_id.has_value() && contains(model.baseline_ids(),id)).second)
                reject("overlapping actual phase ownership: "+id);
    }
    Ids unique,baseline,ordinary;
    for (const auto& id:selected_wall_ids) {
        identity(id);
        if (!unique.insert(id).second) reject("selected wall identity is duplicated: "+id);
        const auto found=actual.find(id);
        if (found==actual.end() || found->second.type!="wall") reject("selection requires actual physical wall roots: "+id);
        const auto owner=owners.find(id);
        (owner!=owners.end() && owner->second ? baseline : ordinary).insert(id);
    }
    if (baseline.empty()) return std::nullopt;
    std::vector<std::string> baseline_ids(baseline.begin(),baseline.end());
    const auto registry=prepare_registry(actual,baseline_ids,complete_actual_corner_window_cohorts);
    if (!registry) reject("baseline selection no longer has an actual saved choice");
    const auto model=ModelPhases::from_json(registry->properties.at("model"));
    if (!model.active_alternative()) reject("baseline selection no longer has an active alternative");
    return PhaseWallDemolitionSelection{{registry->id,*model.active_alternative(),std::move(baseline_ids),
        complete_actual_corner_window_cohorts},
        std::vector<std::string>(ordinary.begin(),ordinary.end())};
}

Entities replay_phase_wall_demolition_entities(const Entities& actual,const PhaseWallDemolitionIntent& intent) {
    (void)encode_phase_wall_demolition_intent(intent);
    std::vector<std::string> corners;
    const auto registry=prepare_registry(actual,intent.wall_ids,intent.complete_actual_corner_window_cohorts,&corners);
    if (!registry || registry->id!=intent.registry_id)
        reject("semantic wall selection differs from its actual saved registry");
    const auto phases=ModelPhases::from_json(registry->properties.at("model"));
    if (phases.active_alternative()!=std::optional<std::string>{intent.alternative_id})
        reject("semantic wall selection differs from its actual saved alternative");
    auto result=actual;
    if (intent.complete_actual_corner_window_cohorts && !corners.empty()) {
        // The wall and corner leaves independently see the ORIGINAL source.
        // Corner replay owns exact retirement, dependents, catalogs and phase
        // filtering; never grant it the partially parked wall source.
        result=replay_architectural_object_removal(actual,corners,{},true,0,true,false,true,true);
        auto& raw=result.at(registry->id).properties.at("model");
        const auto& wall_raw=registry->properties.at("model");
        for (auto& row:raw.at("alternatives")) if (row.at("id")==intent.alternative_id) {
            Ids additions;
            for (const auto& parked:wall_raw.at("alternatives")) if (parked.at("id")==intent.alternative_id)
                for (const auto& id:parked.at("demolished_ids")) additions.insert(id.get<std::string>());
            append_missing(row.at("demolished_ids"),additions);
        }
        raw=retain_model_phase_source(actual.at(registry->id).properties.at("model"),ModelPhases::from_json(raw));
        // Corner replay must not alter the original selected baseline bodies.
        for (const auto& id:intent.wall_ids) if (result.at(id)!=actual.at(id) ||
            result.at(id).properties.dump()!=actual.at(id).properties.dump() ||
            result.at(id).extensions.dump()!=actual.at(id).extensions.dump())
            reject("complete corner consequence changed a retained baseline wall: "+id);
    } else result.at(registry->id)=*registry;
    if (intent.complete_actual_corner_window_cohorts) {
        bounds(result);
        validate_corner_window_state(result);
        const auto after=constraint_phase_scope(result);
        auto expected_inactive=constraint_phase_scope(actual).inactive_owner_ids;
        for (const auto& alternative:phases.alternatives()) if (alternative.id==intent.alternative_id)
            expected_inactive.insert(alternative.demolished_ids.begin(),alternative.demolished_ids.end());
        if (after.inactive_owner_ids!=expected_inactive)
            reject("complete corner/wall replay changed unrelated saved activity");
        for (const auto& id:intent.wall_ids) if (!after.inactive_owner_ids.contains(id))
            reject("complete replay failed to park an actual selected baseline wall: "+id);
    }
    return result;
}

std::optional<ApplyEntityChanges> prepare_phase_wall_demolition(
    const DocumentSnapshot& source,const std::vector<std::string>& selected_wall_ids,const std::string& message) {
    if (!source.is_editable()) throw DocumentError(DocumentErrorCode::read_only,source.read_only_reason());
    auto registry=prepare_registry(source.entities(),selected_wall_ids);
    if (!registry) return std::nullopt;
    if (message.size()>65536) reject("command message exceeds budget");
    ApplyEntityChanges result{source.revision(),{}, {},message};
    result.entity_changes.push_back(EntityChange::upsert(std::move(*registry)));
    return result;
}

} // namespace sketch
