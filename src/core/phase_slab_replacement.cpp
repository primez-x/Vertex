#include "sketch/phase_slab_replacement.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_replacements = 4096;
constexpr std::size_t maximum_authoring_bytes = 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase slab replacement: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
// This bounded scan reserves opaque string values and keys. It is never a
// rewrite mechanism; only codec-qualified slots below acquire remap authority.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    std::size_t node_limit{4 * 1024 * 1024}, byte_limit{64 * 1024 * 1024};
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) reject("source JSON node/nesting budget exceeded");
        const auto reserve = [&](const std::string& text) {
            if (text.size() > byte_limit - bytes) reject("source JSON string budget exceeded");
            bytes += text.size(); values.insert(text);
        };
        if (value.is_string()) reserve(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            reserve(key); read(child, depth + 1);
        }
    }
};
bool touches(const Json& value, const Ids& ids) {
    Strings strings; strings.read(value);
    return std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return strings.values.contains(id); });
}
Strings occupied_strings(const PhaseSlabReplacementEntities& source) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings strings;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) strings.values.insert(key);
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes");
        strings.values.insert(id); strings.values.insert(entity.type);
        strings.read(entity.properties); strings.read(entity.extensions);
    }
    return strings;
}
void diagnostic(PhaseSlabReplacementPlan& plan, const std::string& id, const std::string& reason) {
    const PhaseSlabReplacementDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}
std::string remap(const std::string& id, const PhaseSlabReplacementIdentityMap& identities) {
    const auto found = identities.find(id);
    return found == identities.end() ? id : found->second;
}
void remap_field(Json& value, const char* key, const PhaseSlabReplacementIdentityMap& identities) {
    if (value.contains(key)) value.at(key) = remap(value.at(key).get<std::string>(), identities);
}
bool affected_overlay(const Json& overlay, const Ids& owners) {
    return (overlay.contains("object_id") && owners.contains(overlay.at("object_id").get<std::string>())) ||
        (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null() &&
            owners.contains(overlay.at("dimension_binding").at("object_id").get<std::string>()));
}
Slab actual_slab(const Entity& entity) {
    if (entity.type != "slab") reject("source owner is not a slab: " + entity.id);
    Slab slab; std::string error;
    if (!read_document_slab(entity, slab, error)) reject("unsupported slab " + entity.id + ": " + error);
    return slab;
}
void admit_slabs(const PhaseSlabReplacementEntities& source, const Ids& owners) {
    const auto organization = organize_project(source);
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    const auto material = [&](const std::string& catalog_id, const std::string& material_id) {
        identity(catalog_id); identity(material_id);
        const auto catalog = source.find(catalog_id);
        if (catalog == source.end() || catalog->second.type != "assembly_model")
            reject("slab material must bind an actual assembly catalog: " + catalog_id);
        if (!catalogs.contains(catalog_id)) catalogs.emplace(catalog_id,
            AssemblyModel::from_json(catalog->second.properties.at("model")));
        const auto& materials = catalogs.at(catalog_id).materials();
        if (std::none_of(materials.begin(), materials.end(), [&](const auto& m) { return m.id == material_id; }))
            reject("slab material is absent from its actual catalog: " + material_id);
    };
    for (const auto& id : owners) {
        const auto& entity = source.at(id);
        validate_slab_profile_source_entity(entity);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            reject("slab has unresolved actual drawing context: " + id);
        const auto slab = actual_slab(resolve_vertical_placement(source, entity));
        (void)make_slab(slab);
        if (entity.properties.contains("material_assignment")) {
            const auto& assignment = entity.properties.at("material_assignment");
            if (!assignment.is_object() || !assignment.contains("version") ||
                !assignment.at("version").is_number_integer() || assignment.at("version") != 1)
                reject("unsupported slab material assignment: " + id);
            material(assignment.at("catalog_id").get<std::string>(), assignment.at("material_id").get<std::string>());
        }
        for (const auto& layer : slab.layers) if (layer.material)
            material(layer.material->catalog_id, layer.material->material_id);
    }
}

// Remove only qualified live identity slots. Quantity-entry paths use actual
// layer indices, so renaming a layer never changes its receipt pointer/value.
Entity opaque_remainder(Entity entity) {
    // Retirement rows bind historical provenance, not live children. Admit the
    // exact supported envelope before removing only its historical ID slots
    // from this scratch scan; arbitrary receipt siblings remain opaque.
    if (entity.type == "slab" && entity.extensions.contains("slab_layer_stack_retirement")) {
        auto& archive = entity.extensions.at("slab_layer_stack_retirement");
        validate_slab_layer_stack_retirement(archive);
        for (auto& row : archive.at("receipts")) row.erase("layer_id");
    }
    auto& p = entity.properties;
    if (entity.type == "slab" && p.contains("layers")) {
        (void)actual_slab(entity);
        for (auto& layer : p.at("layers")) layer.erase("id");
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model"));
        p.erase("model");
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("object_ids");
            auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& overlay : view.at("overlays")) {
                overlay.erase("id"); overlay.erase("object_id");
                if (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null())
                    overlay.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) row.erase("target_id");
    }
    return entity;
}

void complete_presentation(PhaseSlabReplacementEntities& candidate,
    const PhaseSlabReplacementEntities& source, const Ids& owners,
    const PhaseSlabReplacementIdentityMap& identities) {
    for (const auto& [id, original] : source) {
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            (void)decode_sheet_view_entity(original);
            auto& changed = candidate.at(id);
            for (auto& view : changed.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    auto& rows = view.at("object_ids"); const auto retained = rows;
                    for (const auto& row : retained) if (owners.contains(row.get<std::string>()))
                        rows.push_back(identities.at(row.get<std::string>()));
                }
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null()) {
                    auto& rows = presentation.at("appearance").at("objects"); const auto retained = rows;
                    for (auto row : retained) if (owners.contains(row.at("object_id").get<std::string>())) {
                        remap_field(row, "object_id", identities); rows.push_back(std::move(row));
                    }
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays"); const auto retained = rows;
                    for (auto row : retained) if (affected_overlay(row, owners)) {
                        row.at("id") = identities.at(row.at("id").get<std::string>());
                        remap_field(row, "object_id", identities);
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                            remap_field(row.at("dimension_binding"), "object_id", identities);
                        rows.push_back(std::move(row));
                    }
                }
            }
            validate_sheet_view_entity(changed);
        } else if (original.type == kAnnotationEntityType) {
            validate_annotation_entity(original);
            auto& rows = candidate.at(id).properties.at("state").at("overrides"); const auto retained = rows;
            for (auto row : retained) if (owners.contains(row.at("target_id").get<std::string>())) {
                remap_field(row, "target_id", identities); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}

std::optional<PhaseSlabProfileReplacementRequest> replacement_request(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementEntities& physical,
    const std::vector<std::string>& targets, const std::string& family, const std::string& target_name) {
    const auto unchanged = [&](const auto& id) { return exact(source.at(id), physical.at(id)); };
    if (std::all_of(targets.begin(), targets.end(), unchanged)) return std::nullopt;
    if (std::any_of(targets.begin(), targets.end(), unchanged))
        reject("unchanged slab cannot acquire replacement authority as an extra seed");
    const auto scope = constraint_phase_scope(source);
    std::optional<PhaseSlabProfileReplacementRequest> request;
    std::size_t ordinary = 0;
    for (const auto& id : targets) {
        if (scope.inactive_owner_ids.contains(id)) reject(target_name + " target is inactive: " + id);
        const PhysicalWallPhaseState* membership = nullptr;
        for (const auto& registry : scope.registries)
            if (std::find(registry.registered_entity_ids.begin(), registry.registered_entity_ids.end(), id) != registry.registered_entity_ids.end()) {
                if (membership) reject(target_name + " target has overlapping registry membership");
                membership = &registry;
            }
        if (!membership) { ++ordinary; continue; }
        const auto model = ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), id) != model.baseline_ids().end();
        if (!baseline || !model.active_alternative()) { ++ordinary; continue; }
        if (request && request->registry_id != membership->registry_id)
            reject(family + " span different shared-baseline registries");
        if (!request) request = PhaseSlabProfileReplacementRequest{membership->registry_id, *model.active_alternative(), {}};
        request->seed_slab_ids.push_back(id);
    }
    if (request && ordinary) reject(family + " mix shared-baseline and ordinary/proposed slab owners");
    if (request) std::sort(request->seed_slab_ids.begin(), request->seed_slab_ids.end());
    return request;
}
} // namespace

bool PhaseSlabReplacementPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) { return d.blocking; });
}

PhaseSlabReplacementPlan inspect_phase_slab_replacement_plan(
    const PhaseSlabReplacementEntities& source, const std::vector<std::string>& seeds,
    const std::string& registry_id, const std::string& alternative_id) {
    try {
        identity(registry_id); identity(alternative_id); (void)occupied_strings(source);
        if (!source.contains(registry_id) || source.at(registry_id).type != "model_phases")
            reject("registry must be an actual model_phases entity");
        const auto model = ModelPhases::from_json(source.at(registry_id).properties.at("model"));
        if (model.active_alternative() != std::optional<std::string>{alternative_id})
            reject("alternative must be the actual saved active selection");
        const auto scope = constraint_phase_scope(source);
        std::map<std::string, std::string, std::less<>> memberships;
        for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, registry.registry_id).second) reject("overlapping all-registry model membership: " + id);
        PhaseSlabReplacementPlan plan;
        plan.registry_id = registry_id; plan.alternative_id = alternative_id; plan.seed_slab_ids = seeds;
        if (seeds.empty() || seeds.size() > maximum_replacements) reject("requires bounded nonempty explicit slab seeds");
        std::sort(plan.seed_slab_ids.begin(), plan.seed_slab_ids.end());
        if (std::adjacent_find(plan.seed_slab_ids.begin(), plan.seed_slab_ids.end()) != plan.seed_slab_ids.end())
            reject("slab seeds must be unique");
        const Ids owners(plan.seed_slab_ids.begin(), plan.seed_slab_ids.end());
        const Ids baseline(model.baseline_ids().begin(), model.baseline_ids().end());
        for (const auto& id : owners) {
            identity(id);
            const auto member = memberships.find(id);
            if (!source.contains(id) || source.at(id).type != "slab" || !baseline.contains(id) ||
                scope.inactive_owner_ids.contains(id) || member == memberships.end() || member->second != registry_id)
                reject("seed must be an active actual baseline slab in the selected registry: " + id);
        }
        Ids children, ambiguous;
        std::map<std::string, std::string, std::less<>> child_owners;
        const auto reserve_child = [&](const std::string& child, const std::string& owner, bool required) {
            identity(child);
            if (source.contains(child) || !child_owners.emplace(child, owner).second) ambiguous.insert(child);
            if (required) children.insert(child);
        };
        for (const auto& [id, entity] : source) {
            try {
                if (entity.type == "slab" && entity.properties.contains("layers")) {
                    const auto slab = actual_slab(entity);
                    for (const auto& layer : slab.layers) reserve_child(layer.id, id, owners.contains(id));
                } else if (entity.type == kSheetViewEntityType) {
                    const auto views = decode_sheet_view_entity(entity);
                    for (const auto& view : views.views()) for (const auto& overlay : view.overlays)
                        reserve_child(overlay.id, id + ":" + view.id, owners.contains(overlay.object_id) ||
                            (overlay.dimension_binding && owners.contains(overlay.dimension_binding->object_id)));
                }
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, owners) || touches(entity.extensions, owners))
                    diagnostic(plan, id, "affected child roster is unsupported: " + std::string(error.what()));
            }
        }
        for (const auto& child : children) if (ambiguous.contains(child))
            diagnostic(plan, child, "copied child identity aliases another retained owner or actual entity");
        if (owners.size() + children.size() > maximum_replacements) reject("replacement entity/child budget exceeded");
        Ids affected = owners; affected.insert(children.begin(), children.end());
        for (const auto& [id, entity] : source) {
            try {
                const auto remainder = opaque_remainder(entity);
                if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                    diagnostic(plan, id, "affected reference has no qualified replacement codec; original remains preserved");
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, affected) || touches(entity.extensions, affected))
                    diagnostic(plan, id, "affected typed record is unsupported: " + std::string(error.what()));
            }
        }
        try { admit_slabs(source, owners); }
        catch (const std::exception& error) { diagnostic(plan, registry_id, "source slab admission failed: " + std::string(error.what())); }
        plan.required_entity_ids.assign(owners.begin(), owners.end());
        plan.required_child_ids.assign(children.begin(), children.end());
        std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
            return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
        });
        return plan;
    } catch (const Json::exception& error) { reject(std::string("malformed typed source: ") + error.what()); }
}

PhaseSlabReplacementResult replay_phase_slab_replacement(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementPlan& plan,
    const PhaseSlabReplacementIdentityMap& identities, const std::vector<SlabProfileEditIntent>& profiles,
    const std::vector<SlabLayerStackEditIntent>& stacks) {
    try {
        if (profiles.empty() == stacks.empty()) reject("requires exactly one nonempty profile or stack edit family");
        const auto derived = inspect_phase_slab_replacement_plan(source, plan.seed_slab_ids, plan.registry_id, plan.alternative_id);
        if (derived != plan) reject("supplied plan differs from actual source discovery");
        if (!derived.ready()) reject("replacement has unresolved affected dependencies");
        const Ids seeds(plan.seed_slab_ids.begin(), plan.seed_slab_ids.end());
        Ids targets;
        const auto add_target = [&](const auto& intent) {
            if (!seeds.contains(intent.slab_id) || !targets.insert(intent.slab_id).second)
                reject(stacks.empty() ? "profiles require unique explicit seed slabs" : "stacks require unique explicit seed slabs");
        };
        for (const auto& profile : profiles) add_target(profile);
        for (const auto& stack : stacks) add_target(stack);
        if (targets != seeds) reject(stacks.empty() ? "slab seeds must exactly match authored profile targets" :
            "slab seeds must exactly match authored stack targets");
        const auto physical = stacks.empty() ? replay_slab_profile_entities(source, profiles) :
            replay_slab_layer_stack_entities(source, stacks);
        for (const auto& id : targets) if (exact(source.at(id), physical.at(id)))
            reject("unchanged slab cannot acquire replacement authority as an extra seed");
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (identities.size() != expected.size()) reject("requires complete exact entity/child mapping");
        const auto occupied = occupied_strings(source);
        Ids fresh;
        for (const auto& [old_id, new_id] : identities) {
            identity(old_id); identity(new_id);
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        Ids new_layers;
        for (const auto& stack : stacks) {
            const auto before = actual_slab(source.at(stack.slab_id));
            Ids existing;
            for (const auto& layer : before.layers) existing.insert(layer.id);
            for (const auto& row : stack.layers) if (!existing.contains(row.layer_id)) {
                identity(row.layer_id);
                if (occupied.values.contains(row.layer_id) || !fresh.insert(row.layer_id).second)
                    reject("new stack layer identity collision: " + row.layer_id);
                new_layers.insert(row.layer_id);
            }
            const auto remainder = opaque_remainder(physical.at(stack.slab_id));
            if (touches(remainder.properties, expected) || touches(remainder.extensions, expected))
                reject("proposed stack metadata has an unqualified affected reference: " + stack.slab_id);
        }
        admit_slabs(physical, seeds);
        PhaseSlabReplacementResult result{source, identities, {}};
        for (const auto& id : plan.required_entity_ids) {
            auto copy = physical.at(id);
            copy.id = identities.at(id);
            if (copy.properties.contains("layers")) for (auto& layer : copy.properties.at("layers")) {
                const auto layer_id = layer.at("id").get<std::string>();
                // Existing source children use the complete reserved mapping;
                // newly authored rows already declare their fresh actual IDs.
                if (new_layers.contains(layer_id)) continue;
                layer.at("id") = identities.at(layer_id);
            }
            const auto copy_id = copy.id;
            if (!result.entities.emplace(copy_id, std::move(copy)).second) reject("copy insertion collides");
        }
        const auto model = ModelPhases::from_json(source.at(plan.registry_id).properties.at("model"));
        auto model_ids = model.entity_ids(); auto alternatives = model.alternatives();
        const auto target = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& a) { return a.id == plan.alternative_id; });
        if (target == alternatives.end()) reject("target alternative disappeared");
        for (const auto& id : plan.required_entity_ids) {
            model_ids.push_back(identities.at(id)); target->proposed_ids.push_back(identities.at(id)); target->demolished_ids.push_back(id);
        }
        const auto final_model = ModelPhases::create(model_ids, model.baseline_ids(), alternatives, model.active_alternative());
        auto raw = source.at(plan.registry_id).properties.at("model");
        for (const auto& id : plan.required_entity_ids) raw.at("entity_ids").push_back(identities.at(id));
        for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == plan.alternative_id)
            for (const auto& id : plan.required_entity_ids) {
                alternative.at("proposed_ids").push_back(identities.at(id)); alternative.at("demolished_ids").push_back(id);
            }
        if (ModelPhases::from_json(raw).to_json() != final_model.to_json())
            reject("retained registry reconstruction differs from typed update");
        result.entities.at(plan.registry_id).properties.at("model") = std::move(raw);
        complete_presentation(result.entities, source, seeds, identities);
        Ids copies;
        for (const auto& id : seeds) copies.insert(identities.at(id));
        admit_slabs(result.entities, copies);
        (void)constraint_phase_scope(result.entities);
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed typed replay: ") + error.what()); }
}

std::optional<PhaseSlabProfileReplacementRequest> phase_slab_profile_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabProfileEditIntent>& profiles) {
    const auto physical = replay_slab_profile_entities(source, profiles);
    std::vector<std::string> targets;
    for (const auto& profile : profiles) targets.push_back(profile.slab_id);
    return replacement_request(source, physical, targets, "profiles", "profile");
}

std::optional<PhaseSlabProfileReplacementRequest> phase_slab_layer_stack_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabLayerStackEditIntent>& stacks) {
    const auto physical = replay_slab_layer_stack_entities(source, stacks);
    std::vector<std::string> targets;
    for (const auto& stack : stacks) targets.push_back(stack.slab_id);
    return replacement_request(source, physical, targets, "stacks", "stack");
}

nlohmann::json encode_phase_slab_replacement_authoring(const PhaseSlabReplacementAuthoring& authoring) {
    identity(authoring.registry_id); identity(authoring.alternative_id);
    const bool stack_edit = !authoring.slab_stacks.empty();
    if (authoring.seed_slab_ids.empty() || authoring.seed_slab_ids.size() > maximum_replacements ||
        authoring.identities.empty() || authoring.identities.size() > maximum_replacements ||
        authoring.slab_profiles.size() > maximum_replacements || authoring.slab_stacks.size() > maximum_replacements ||
        authoring.slab_profiles.empty() == authoring.slab_stacks.empty())
        reject("authoring requires bounded nonempty seeds, mapping and exactly one edit family");
    Ids seeds, targets, fresh;
    for (const auto& id : authoring.seed_slab_ids) { identity(id); if (!seeds.insert(id).second) reject("duplicate authoring seed"); }
    for (const auto& [old_id, new_id] : authoring.identities) {
        identity(old_id); identity(new_id);
        if (old_id == new_id || !fresh.insert(new_id).second) reject("authoring identities must be fresh and injective");
    }
    for (const auto& id : seeds) if (!authoring.identities.contains(id)) reject("authoring seed has no proposed identity");
    const auto* family = stack_edit ? "slab_stacks" : "slab_profiles";
    Json result{{"version", stack_edit ? 2 : 1}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
        {"seed_slab_ids", authoring.seed_slab_ids}, {"identities", authoring.identities}, {family, Json::array()}};
    auto bytes = result.dump().size();
    if (bytes > maximum_authoring_bytes) reject("authoring byte budget exceeded");
    auto& rows = result.at(family);
    const auto append = [&](const auto& intent, Json encoded) {
        if (!seeds.contains(intent.slab_id) || !targets.insert(intent.slab_id).second)
            reject(stack_edit ? "authoring stacks require unique seed targets" : "authoring profiles require unique seed targets");
        const auto added = encoded.dump().size() + (rows.empty() ? 0 : 1);
        if (added > maximum_authoring_bytes - bytes) reject("authoring byte budget exceeded");
        bytes += added; rows.push_back(std::move(encoded));
    };
    for (const auto& profile : authoring.slab_profiles) append(profile, encode_slab_profile_edit_intent(profile));
    for (const auto& stack : authoring.slab_stacks) append(stack, encode_slab_layer_stack_edit_intent(stack));
    if (targets != seeds) reject(stack_edit ? "authoring seeds must exactly match stack targets" :
        "authoring seeds must exactly match profile targets");
    return result;
}

PhaseSlabReplacementAuthoring decode_phase_slab_replacement_authoring(const nlohmann::json& value) {
    try {
        if (!value.is_object() || value.size() != 6 || !value.contains("version") ||
            !value.at("version").is_number_integer() || (value.at("version") != 1 && value.at("version") != 2) ||
            !value.contains("registry_id") || !value.contains("alternative_id") || !value.contains("seed_slab_ids") ||
            !value.contains("identities") || !value.at("identities").is_object() || !value.at("seed_slab_ids").is_array())
            reject("authoring must contain exactly the six supported versioned fields");
        const bool stack_edit = value.at("version") == 2;
        const auto* family = stack_edit ? "slab_stacks" : "slab_profiles";
        if (!value.contains(family) || !value.at(family).is_array())
            reject("authoring edit family must exactly match its version");
        if (value.at("seed_slab_ids").size() > maximum_replacements || value.at("identities").size() > maximum_replacements ||
            value.at(family).size() > maximum_replacements)
            reject("authoring budget exceeded");
        Strings budget; budget.node_limit = maximum_authoring_bytes; budget.byte_limit = maximum_authoring_bytes;
        budget.read(value);
        if (value.dump().size() > maximum_authoring_bytes) reject("authoring byte budget exceeded");
        PhaseSlabReplacementAuthoring result;
        result.registry_id = value.at("registry_id").get<std::string>();
        result.alternative_id = value.at("alternative_id").get<std::string>();
        result.seed_slab_ids = value.at("seed_slab_ids").get<std::vector<std::string>>();
        result.identities = value.at("identities").get<PhaseSlabReplacementIdentityMap>();
        if (stack_edit) for (const auto& stack : value.at(family))
            result.slab_stacks.push_back(decode_slab_layer_stack_edit_intent(stack));
        else for (const auto& profile : value.at(family))
            result.slab_profiles.push_back(decode_slab_profile_edit_intent(profile));
        const auto canonical = encode_phase_slab_replacement_authoring(result);
        if (canonical != value || canonical.dump() != value.dump()) reject("authoring differs from its canonical typed encoding");
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed authoring: ") + error.what()); }
}

PhaseSlabReplacementEntities replay_phase_slab_replacement_authoring(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementAuthoring& authoring) {
    const auto encoded = encode_phase_slab_replacement_authoring(authoring);
    const auto canonical = encode_phase_slab_replacement_authoring(decode_phase_slab_replacement_authoring(encoded));
    if (canonical != encoded || canonical.dump() != encoded.dump()) reject("authoring typed round trip differs");
    const auto plan = inspect_phase_slab_replacement_plan(source, authoring.seed_slab_ids, authoring.registry_id, authoring.alternative_id);
    return replay_phase_slab_replacement(source, plan, authoring.identities, authoring.slab_profiles, authoring.slab_stacks).entities;
}

} // namespace sketch
