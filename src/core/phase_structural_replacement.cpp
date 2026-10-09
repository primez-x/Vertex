#include "sketch/phase_structural_replacement.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/structural_hosted_components.hpp"

#include <algorithm>
#include <cctype>
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
    throw std::invalid_argument("Phase structural replacement: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
void source_child_identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isspace(c);
    })) reject("source overlay identity must be nonblank and at most 128 bytes");
}
void source_hosted_instance_identity(const std::string& id) {
    if (id.size() > maximum_authoring_bytes) reject("source hosted instance identity exceeds authoring byte budget");
    if (id.empty() || std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }))
        reject("source hosted instance identity must be nonblank");
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
// Read-only reservation of all strings and keys, including unknown future
// identities. This scan never confers reference or rewrite authority.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    std::size_t node_limit{4 * 1024 * 1024}, byte_limit{64 * 1024 * 1024};
    void reserve(const std::string& value) {
        if (value.size() > byte_limit - bytes) reject("JSON string/key budget exceeded");
        bytes += value.size(); values.insert(value);
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) reject("JSON node/nesting budget exceeded");
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
Strings occupied_strings(const PhaseStructuralReplacementEntities& actual) {
    if (actual.size() > maximum_entities) reject("source entity budget exceeded");
    Strings strings;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) strings.reserve(key);
    for (const auto& [id, entity] : actual) {
        if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes");
        strings.reserve(id); strings.reserve(entity.type);
        strings.read(entity.properties); strings.read(entity.extensions);
        if (entity.type == "assembly_model" && entity.properties.contains("model")) {
            const auto& model = entity.properties.at("model");
            if (model.is_object() && model.contains("instances") && model.at("instances").is_array())
                for (const auto& row : model.at("instances"))
                    if (row.is_object() && row.contains("id") && row.at("id").is_string())
                        strings.reserve(id + ":instance:" + row.at("id").get<std::string>());
        }
    }
    return strings;
}
void diagnostic(PhaseStructuralReplacementPlan& plan, const std::string& id, const std::string& reason) {
    const PhaseStructuralReplacementDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}

struct Memberships {
    std::map<std::string, ModelPhases, std::less<>> models;
    std::map<std::string, std::string, std::less<>> owner_registry;
};
Memberships memberships(const PhaseStructuralReplacementEntities& actual) {
    Memberships result;
    for (const auto& [registry_id, entity] : actual) {
        if (entity.type != "model_phases") continue;
        auto model = ModelPhases::from_json(entity.properties.at("model"));
        for (const auto& id : model.entity_ids()) {
            const auto owner = actual.find(id);
            if (owner == actual.end() || !is_model_phase_entity_type(owner->second.type))
                reject("registry references missing or unsupported actual owner: " + id);
            if (!result.owner_registry.emplace(id, registry_id).second)
                reject("overlapping all-registry membership: " + id);
        }
        result.models.emplace(registry_id, std::move(model));
    }
    return result;
}
bool baseline(const ModelPhases& model, const std::string& id) {
    return std::find(model.baseline_ids().begin(), model.baseline_ids().end(), id) != model.baseline_ids().end();
}
void active_owner(const ModelPhases& model, const std::string& id) {
    const auto states = model.active_state();
    const auto state = states.find(id);
    if (state == states.end() || state->second == ModelPhase::demolished)
        reject("changed structural target is inactive: " + id);
}
std::optional<PhaseStructuralEditReplacementRequest> classify(
    const PhaseStructuralReplacementEntities& actual, const PhaseStructuralReplacementEntities& physical,
    const std::vector<StructuralObjectEditIntent>& edits) {
    Ids changed, targets;
    for (const auto& edit : edits) {
        if (!targets.insert(edit.object_id).second) reject("duplicate structural edit target");
        if (!actual.contains(edit.object_id) || !physical.contains(edit.object_id)) reject("missing actual edit target");
        if (!exact(actual.at(edit.object_id), physical.at(edit.object_id))) changed.insert(edit.object_id);
    }
    if (changed.empty()) return std::nullopt;
    const auto scope = memberships(actual);
    std::optional<PhaseStructuralEditReplacementRequest> result;
    Ids changed_registries;
    for (const auto& id : changed) {
        const auto membership = scope.owner_registry.find(id);
        if (membership == scope.owner_registry.end()) continue;
        changed_registries.insert(membership->second);
        const auto& model = scope.models.at(membership->second);
        active_owner(model, id);
        if (!baseline(model, id) || !model.active_alternative()) continue;
        if (result && result->registry_id != membership->second)
            reject("changed structural baseline targets span foreign registries");
        if (!result) result = PhaseStructuralEditReplacementRequest{membership->second, *model.active_alternative(), {}};
        result->seed_object_ids.push_back(id);
    }
    if (result && std::any_of(changed_registries.begin(), changed_registries.end(), [&](const auto& id) {
        return id != result->registry_id;
    })) reject("replacement cohort includes a changed foreign registry owner");
    return result;
}

void admit_structural(const PhaseStructuralReplacementEntities& actual, const Ids& owners) {
    const auto organization = organize_project(actual);
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    for (const auto& id : owners) {
        const auto& entity = actual.at(id);
        validate_structural_object_source_entity(entity);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            reject("structural object has unresolved actual drawing context: " + id);
        (void)decode_building_entity(resolve_vertical_placement(actual, entity));
        if (!entity.properties.contains("material_assignment")) continue;
        const auto& assignment = entity.properties.at("material_assignment");
        if (!assignment.is_object() || !assignment.contains("version") ||
            !assignment.at("version").is_number_integer() || assignment.at("version") != 1 ||
            !assignment.contains("catalog_id") || !assignment.at("catalog_id").is_string() ||
            !assignment.contains("material_id") || !assignment.at("material_id").is_string())
            reject("unsupported structural material assignment: " + id);
        const auto catalog_id = assignment.at("catalog_id").get<std::string>();
        const auto material_id = assignment.at("material_id").get<std::string>();
        const auto catalog = actual.find(catalog_id);
        if (catalog == actual.end() || catalog->second.type != "assembly_model")
            reject("structural material must bind an actual assembly catalog: " + catalog_id);
        if (!catalogs.contains(catalog_id)) catalogs.emplace(catalog_id,
            AssemblyModel::from_json(catalog->second.properties.at("model")));
        const auto& materials = catalogs.at(catalog_id).materials();
        if (std::none_of(materials.begin(), materials.end(), [&](const auto& value) { return value.id == material_id; }))
            reject("structural material is absent from its actual catalog: " + material_id);
    }
}

bool affected_overlay(const Json& overlay, const Ids& owners) {
    return (overlay.contains("object_id") && owners.contains(overlay.at("object_id").get<std::string>())) ||
        (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null() &&
            owners.contains(overlay.at("dimension_binding").at("object_id").get<std::string>()));
}
void remap_field(Json& value, const char* key, const PhaseStructuralReplacementIdentityMap& identities) {
    if (!value.contains(key)) return;
    const auto found = identities.find(value.at(key).get<std::string>());
    if (found != identities.end()) value.at(key) = found->second;
}

// Omit only validated live reference slots in an admission scratch copy. No
// raw source row is changed, and no arbitrary string is interpreted as a link.
Entity opaque_remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model"));
        auto& model = p.at("model");
        model.erase("entity_ids"); model.erase("baseline_ids");
        for (auto& alternative : model.at("alternatives")) {
            alternative.erase("demolished_ids"); alternative.erase("proposed_ids");
        }
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
        for (auto& row : p.at("state").at("overrides"))
            if (row.at("target_kind") == "object") row.erase("target_id");
    }
    return entity;
}

void complete_presentation(PhaseStructuralReplacementEntities& candidate,
    const PhaseStructuralReplacementEntities& actual, const Ids& owners,
    const PhaseStructuralReplacementIdentityMap& identities) {
    for (const auto& [id, original] : actual) {
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
            for (auto row : retained) if (row.at("target_kind") == "object" &&
                owners.contains(row.at("target_id").get<std::string>())) {
                remap_field(row, "target_id", identities); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}

void admit_demolition_dependencies(const PhaseStructuralReplacementEntities& actual, const Ids& seeds) {
    AssemblyExpansionBudget budget;
    for (const auto& [id, entity] : actual) {
        if (entity.type != "assembly_model" || !entity.properties.contains("model")) continue;
        const auto& raw = entity.properties.at("model");
        if (!raw.is_object() || !raw.contains("instances") || !raw.at("instances").is_array()) continue;
        const bool affected = std::any_of(raw.at("instances").begin(), raw.at("instances").end(), [&](const Json& row) {
            if (!row.is_object() || !row.contains("placement") || !row.at("placement").is_object()) return false;
            const auto& placement = row.at("placement");
            return placement.contains("host_entity_id") && placement.at("host_entity_id").is_string() &&
                seeds.contains(placement.at("host_entity_id").get<std::string>());
        });
        if (!affected) continue;
        try {
            const auto catalog = AssemblyModel::from_json(raw);
            for (const auto& instance : catalog.instances())
                if (instance.placement && seeds.contains(instance.placement->host_entity_id))
                    (void)catalog.expand(instance, budget);
            // Both expanded profiles and legacy host copies are scoped by the
            // actual placement host in native publication and schedules. Their
            // retained raw placements become inactive with the demolished host.
            // Independent document assembly instances forbid host placements.
        } catch (const std::exception& error) {
            reject("affected hosted assembly cannot establish typed host scope: " + id + ": " + error.what());
        }
    }
}

PhaseStructuralReplacementEntities replay_demolition(const PhaseStructuralReplacementEntities& actual,
    const PhaseStructuralReplacementAuthoring& authoring) {
    const auto request = phase_structural_demolition_request(actual, authoring.seed_object_ids);
    if (!request || request->registry_id != authoring.registry_id || request->alternative_id != authoring.alternative_id ||
        Ids(request->seed_object_ids.begin(), request->seed_object_ids.end()) !=
            Ids(authoring.seed_object_ids.begin(), authoring.seed_object_ids.end()))
        reject("demolition seeds differ from actual active baseline membership");
    const Ids seeds(request->seed_object_ids.begin(), request->seed_object_ids.end());
    admit_demolition_dependencies(actual, seeds);
    const auto before_scope = constraint_phase_scope(actual);
    const auto& original = actual.at(request->registry_id);
    const auto model = ModelPhases::from_json(original.properties.at("model"));
    const auto selected = std::find_if(model.alternatives().begin(), model.alternatives().end(), [&](const auto& row) {
        return row.id == request->alternative_id;
    });
    if (selected == model.alternatives().end()) reject("actual active demolition alternative is absent");
    auto updated = *selected;
    updated.demolished_ids.insert(updated.demolished_ids.end(), request->seed_object_ids.begin(), request->seed_object_ids.end());
    const auto expected = model.with_updated_alternative(std::move(updated));
    auto raw = original.properties.at("model");
    bool appended{};
    for (auto& row : raw.at("alternatives")) if (row.at("id") == request->alternative_id) {
        for (const auto& id : request->seed_object_ids) row.at("demolished_ids").push_back(id);
        appended = true;
    }
    if (!appended || ModelPhases::from_json(raw).to_json() != expected.to_json())
        reject("retained demolition registry differs from its typed additive update");
    auto candidate = actual;
    candidate.at(request->registry_id).properties.at("model") = std::move(raw);
    const auto after_scope = constraint_phase_scope(candidate);
    auto expected_inactive = before_scope.inactive_owner_ids;
    expected_inactive.insert(seeds.begin(), seeds.end());
    if (after_scope.inactive_owner_ids != expected_inactive)
        reject("demolition registry append did not exclusively park the selected actual owners");
    for (const auto& [id, entity] : actual)
        if (id != request->registry_id && !exact(entity, candidate.at(id)))
            reject("registry-only demolition changed a retained physical or presentation row");
    return candidate;
}
} // namespace

bool PhaseStructuralReplacementPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.blocking; });
}

PhaseStructuralReplacementPlan inspect_phase_structural_replacement_plan(
    const PhaseStructuralReplacementEntities& actual, const std::vector<std::string>& seeds,
    const std::string& registry_id, const std::string& alternative_id, bool complete_hosted) {
    try {
        identity(registry_id); identity(alternative_id); (void)occupied_strings(actual);
        const auto scope = memberships(actual);
        const auto registry = scope.models.find(registry_id);
        if (registry == scope.models.end()) reject("registry must be an actual model_phases entity");
        const auto& model = registry->second;
        if (model.active_alternative() != std::optional<std::string>{alternative_id})
            reject("alternative must be the actual saved active selection");
        if (seeds.empty() || seeds.size() > maximum_replacements) reject("requires bounded nonempty structural seeds");
        PhaseStructuralReplacementPlan plan;
        plan.registry_id = registry_id; plan.alternative_id = alternative_id; plan.seed_object_ids = seeds;
        std::sort(plan.seed_object_ids.begin(), plan.seed_object_ids.end());
        if (std::adjacent_find(plan.seed_object_ids.begin(), plan.seed_object_ids.end()) != plan.seed_object_ids.end())
            reject("structural seeds must be unique");
        const Ids owners(plan.seed_object_ids.begin(), plan.seed_object_ids.end());
        for (const auto& id : owners) {
            identity(id);
            const auto member = scope.owner_registry.find(id);
            if (!actual.contains(id) || (actual.at(id).type != "column" && actual.at(id).type != "beam") ||
                !baseline(model, id) || member == scope.owner_registry.end() || member->second != registry_id)
                reject("seed must be an actual baseline structural owner in the selected registry: " + id);
            active_owner(model, id);
        }
        Ids presentation_owners = owners, catalogs, render_aliases;
        if (complete_hosted) {
            const auto hosted = inspect_structural_hosted_components(actual, plan.seed_object_ids);
            for (const auto& item : hosted.diagnostics) diagnostic(plan, item.entity_id, item.reason);
            catalogs.insert(hosted.catalog_ids.begin(), hosted.catalog_ids.end());
            plan.required_hosted_instance_ids = hosted.instance_ids;
            for (const auto& [key, alias] : hosted.original_presentation_ids) {
                (void)key; presentation_owners.insert(alias);
            }
            if (hosted.ready()) for (const auto& [key, alias] : embedded_assembly_presentation_ids(actual)) {
                (void)key; render_aliases.insert(alias);
            }
        }
        Ids children, ambiguous;
        // Use a structural pair, not a delimiter-composed owner string: IDs
        // may contain ':', and distinct view entities may reuse local view IDs.
        using OverlayOwner = std::pair<std::string, std::string>;
        std::map<std::string, OverlayOwner, std::less<>> child_owners;
        for (const auto& [id, entity] : actual) {
            try {
                if (entity.type == kSheetViewEntityType) {
                    const auto decoded = decode_sheet_view_entity(entity);
                    for (const auto& view : decoded.views()) for (const auto& overlay : view.overlays) {
                        source_child_identity(overlay.id);
                        if (actual.contains(overlay.id) || render_aliases.contains(overlay.id) ||
                            !child_owners.emplace(overlay.id, OverlayOwner{id, view.id}).second)
                            ambiguous.insert(overlay.id);
                        if (presentation_owners.contains(overlay.object_id) ||
                            (overlay.dimension_binding && presentation_owners.contains(overlay.dimension_binding->object_id)))
                            children.insert(overlay.id);
                    }
                } else if (!complete_hosted && entity.type == "assembly_model") {
                    const auto catalog = AssemblyModel::from_json(entity.properties.at("model"));
                    for (const auto& instance : catalog.instances())
                        if (instance.placement && owners.contains(instance.placement->host_entity_id))
                            diagnostic(plan, id, "hosted assembly instance requires qualified catalog/instance copy and placement replay: " + instance.id);
                }
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, presentation_owners) || touches(entity.extensions, presentation_owners))
                    diagnostic(plan, id, "affected child/catalog codec is unsupported: " + std::string(error.what()));
            }
        }
        for (const auto& child : children) if (ambiguous.contains(child))
            diagnostic(plan, child, "copied overlay identity aliases an actual entity, render alias or qualified overlay owner");
        if (owners.size() + catalogs.size() + children.size() + plan.required_hosted_instance_ids.size() > maximum_replacements)
            reject("replacement entity/child/hosted budget exceeded");
        Ids affected = presentation_owners; affected.insert(children.begin(), children.end());
        for (const auto& [id, entity] : actual) {
            try {
                const auto remainder = complete_hosted && catalogs.contains(id)
                    ? structural_hosted_catalog_opaque_remainder(entity) : opaque_remainder(entity);
                if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                    diagnostic(plan, id, "affected reference has no qualified structural replacement codec; original remains preserved");
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, affected) || touches(entity.extensions, affected))
                    diagnostic(plan, id, "affected typed record is unsupported: " + std::string(error.what()));
            }
        }
        try { admit_structural(actual, owners); }
        catch (const std::exception& error) { diagnostic(plan, registry_id, "source structural admission failed: " + std::string(error.what())); }
        Ids required = owners; required.insert(catalogs.begin(), catalogs.end());
        plan.required_entity_ids.assign(required.begin(), required.end());
        plan.required_child_ids.assign(children.begin(), children.end());
        std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
            return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
        });
        return plan;
    } catch (const Json::exception& error) { reject(std::string("malformed typed source: ") + error.what()); }
}

std::optional<PhaseStructuralEditReplacementRequest> phase_structural_edit_replacement_request(
    const PhaseStructuralReplacementEntities& actual, const std::vector<StructuralObjectEditIntent>& edits) {
    try {
        (void)occupied_strings(actual);
        const auto physical = replay_structural_object_edit_entities(actual, edits);
        return classify(actual, physical, edits);
    } catch (const Json::exception& error) { reject(std::string("malformed structural edit source: ") + error.what()); }
}

std::optional<PhaseStructuralEditReplacementRequest> phase_structural_demolition_request(
    const PhaseStructuralReplacementEntities& actual, const std::vector<std::string>& seed_object_ids) {
    try {
        if (seed_object_ids.size() > maximum_replacements) reject("demolition target budget exceeded");
        if (seed_object_ids.empty()) return std::nullopt;
        (void)occupied_strings(actual);
        const auto scope = memberships(actual);
        Ids selected;
        std::size_t ordinary{};
        std::optional<PhaseStructuralEditReplacementRequest> result;
        for (const auto& id : seed_object_ids) {
            identity(id);
            if (!selected.insert(id).second) reject("duplicate demolition target");
            const auto found = actual.find(id);
            if (found == actual.end() || (found->second.type != "column" && found->second.type != "beam"))
                reject("demolition target must be an actual column or beam: " + id);
            validate_structural_object_source_entity(found->second);
            const auto member = scope.owner_registry.find(id);
            if (member == scope.owner_registry.end()) { ++ordinary; continue; }
            const auto& model = scope.models.at(member->second);
            active_owner(model, id);
            if (!baseline(model, id) || !model.active_alternative()) { ++ordinary; continue; }
            if (result && result->registry_id != member->second)
                reject("demolition targets span foreign saved registries");
            if (!result) result = PhaseStructuralEditReplacementRequest{member->second, *model.active_alternative(), {}};
            result->seed_object_ids.push_back(id);
        }
        if (!result) return std::nullopt;
        if (ordinary) reject("demolition requires only active shared-baseline structural owners from one saved registry");
        admit_structural(actual, selected);
        std::sort(result->seed_object_ids.begin(), result->seed_object_ids.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed structural demolition source: ") + error.what()); }
}

nlohmann::json encode_phase_structural_replacement_authoring(const PhaseStructuralReplacementAuthoring& authoring) {
    identity(authoring.registry_id); identity(authoring.alternative_id);
    if (authoring.demolition) {
        if (authoring.complete_hosted || !authoring.hosted_instance_identities.empty() ||
            !authoring.identities.empty() || !authoring.edits.empty() || authoring.seed_object_ids.empty() ||
            authoring.seed_object_ids.size() > maximum_replacements)
            reject("demolition authoring requires bounded seeds and no identity or physical edit authority");
        Ids seeds;
        for (const auto& id : authoring.seed_object_ids) {
            identity(id); if (!seeds.insert(id).second) reject("duplicate demolition authoring seed");
        }
        Json result{{"version", 2}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
            {"seed_object_ids", authoring.seed_object_ids}, {"demolition", true}};
        if (result.dump().size() > maximum_authoring_bytes) reject("demolition authoring byte budget exceeded");
        return result;
    }
    if (!authoring.complete_hosted && !authoring.hosted_instance_identities.empty())
        reject("v1 cannot author qualified hosted identities");
    if (authoring.seed_object_ids.empty() || authoring.seed_object_ids.size() > maximum_replacements ||
        authoring.identities.empty() || authoring.identities.size() > maximum_replacements ||
        authoring.edits.empty() || authoring.edits.size() > maximum_replacements)
        reject("authoring requires bounded nonempty seeds, identities and edits");
    if (authoring.hosted_instance_identities.size() > maximum_replacements - authoring.identities.size())
        reject("authoring entity/child/hosted identity budget exceeded");
    Ids seeds, fresh, targets;
    for (const auto& id : authoring.seed_object_ids) {
        identity(id); if (!seeds.insert(id).second) reject("duplicate authoring seed");
    }
    for (const auto& [old_id, new_id] : authoring.identities) {
        source_child_identity(old_id); identity(new_id);
        if (old_id == new_id || !fresh.insert(new_id).second) reject("authoring identities must be fresh and injective");
    }
    Json hosted_rows = Json::array();
    std::size_t hosted_bytes{};
    for (const auto& [key, new_id] : authoring.hosted_instance_identities) {
        identity(key.first); source_hosted_instance_identity(key.second); identity(new_id);
        const auto size = key.first.size() + key.second.size() + new_id.size();
        if (size > maximum_authoring_bytes - hosted_bytes) reject("qualified hosted identity byte budget exceeded");
        hosted_bytes += size;
        if (key.second == new_id || !fresh.insert(new_id).second)
            reject("qualified hosted identities must be fresh and injective across all mappings");
        hosted_rows.push_back({{"catalog_id", key.first}, {"instance_id", key.second}, {"proposed_instance_id", new_id}});
    }
    Json rows = Json::array();
    std::size_t bytes{};
    for (const auto& edit : authoring.edits) {
        if (!targets.insert(edit.object_id).second) reject("authoring requires unique structural edit targets");
        auto row = encode_structural_object_edit_intent(edit);
        const auto size = row.dump().size() + 1;
        if (size > maximum_authoring_bytes - bytes) reject("authoring byte budget exceeded");
        bytes += size; rows.push_back(std::move(row));
    }
    for (const auto& id : seeds) if (!authoring.identities.contains(id) || !targets.contains(id))
        reject("every authoring seed requires its mapped identity and actual typed edit");
    Json result{{"version", authoring.complete_hosted ? 3 : 1}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
        {"seed_object_ids", authoring.seed_object_ids}, {"identities", authoring.identities}, {"edits", std::move(rows)}};
    if (authoring.complete_hosted) {
        result["complete_hosted"] = true;
        result["hosted_instance_identities"] = std::move(hosted_rows);
    }
    Strings budget; budget.node_limit = maximum_authoring_bytes; budget.byte_limit = maximum_authoring_bytes;
    budget.read(result);
    if (result.dump().size() > maximum_authoring_bytes) reject("authoring byte budget exceeded");
    return result;
}

PhaseStructuralReplacementAuthoring decode_phase_structural_replacement_authoring(const nlohmann::json& value) {
    try {
        if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() && value.at("version") == 2) {
            if (value.size() != 5 || !value.contains("registry_id") || !value.at("registry_id").is_string() ||
                !value.contains("alternative_id") || !value.at("alternative_id").is_string() ||
                !value.contains("seed_object_ids") || !value.at("seed_object_ids").is_array() ||
                !value.contains("demolition") || !value.at("demolition").is_boolean() || value.at("demolition") != true)
                reject("demolition authoring must contain exactly its five v2 fields with demolition:true");
            if (value.at("seed_object_ids").size() > maximum_replacements) reject("demolition authoring target budget exceeded");
            Strings budget; budget.node_limit = maximum_authoring_bytes; budget.byte_limit = maximum_authoring_bytes;
            budget.read(value);
            if (value.dump().size() > maximum_authoring_bytes) reject("demolition authoring byte budget exceeded");
            PhaseStructuralReplacementAuthoring result;
            result.registry_id = value.at("registry_id").get<std::string>();
            result.alternative_id = value.at("alternative_id").get<std::string>();
            result.seed_object_ids = value.at("seed_object_ids").get<std::vector<std::string>>();
            result.demolition = true;
            const auto canonical = encode_phase_structural_replacement_authoring(result);
            if (canonical != value || canonical.dump() != value.dump()) reject("demolition authoring differs from its canonical typed encoding");
            return result;
        }
        const bool hosted = value.is_object() && value.contains("version") &&
            value.at("version").is_number_integer() && value.at("version") == 3;
        if (!value.is_object() || value.size() != (hosted ? 8U : 6U) || !value.contains("version") ||
            !value.at("version").is_number_integer() || (!hosted && value.at("version") != 1) ||
            !value.contains("registry_id") || !value.at("registry_id").is_string() ||
            !value.contains("alternative_id") || !value.at("alternative_id").is_string() ||
            !value.contains("seed_object_ids") || !value.at("seed_object_ids").is_array() ||
            !value.contains("identities") || !value.at("identities").is_object() ||
            !value.contains("edits") || !value.at("edits").is_array())
            reject("authoring must contain exactly its six v1 fields or eight v3 fields");
        if (hosted && (!value.contains("complete_hosted") || !value.at("complete_hosted").is_boolean() ||
            value.at("complete_hosted") != true || !value.contains("hosted_instance_identities") ||
            !value.at("hosted_instance_identities").is_array()))
            reject("v3 requires complete_hosted:true and a qualified hosted identity array");
        if (value.at("seed_object_ids").size() > maximum_replacements ||
            value.at("identities").size() > maximum_replacements || value.at("edits").size() > maximum_replacements)
            reject("authoring item budget exceeded");
        if (hosted && value.at("hosted_instance_identities").size() > maximum_replacements - value.at("identities").size())
            reject("authoring entity/child/hosted identity budget exceeded");
        Strings budget; budget.node_limit = maximum_authoring_bytes; budget.byte_limit = maximum_authoring_bytes;
        budget.read(value);
        if (value.dump().size() > maximum_authoring_bytes) reject("authoring byte budget exceeded");
        PhaseStructuralReplacementAuthoring result;
        result.registry_id = value.at("registry_id").get<std::string>();
        result.alternative_id = value.at("alternative_id").get<std::string>();
        result.seed_object_ids = value.at("seed_object_ids").get<std::vector<std::string>>();
        result.identities = value.at("identities").get<PhaseStructuralReplacementIdentityMap>();
        result.complete_hosted = hosted;
        if (hosted) for (const auto& row : value.at("hosted_instance_identities")) {
            if (!row.is_object() || row.size() != 3 || !row.contains("catalog_id") || !row.at("catalog_id").is_string() ||
                !row.contains("instance_id") || !row.at("instance_id").is_string() ||
                !row.contains("proposed_instance_id") || !row.at("proposed_instance_id").is_string())
                reject("qualified hosted identity row must contain exactly three string fields");
            if (!result.hosted_instance_identities.emplace(
                std::pair{row.at("catalog_id").get<std::string>(), row.at("instance_id").get<std::string>()},
                row.at("proposed_instance_id").get<std::string>()).second)
                reject("duplicate qualified hosted source identity");
        }
        for (const auto& row : value.at("edits")) result.edits.push_back(decode_structural_object_edit_intent(row));
        const auto canonical = encode_phase_structural_replacement_authoring(result);
        if (canonical != value || canonical.dump() != value.dump()) reject("authoring differs from its canonical typed encoding");
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed authoring: ") + error.what()); }
}

PhaseStructuralReplacementEntities replay_phase_structural_replacement_authoring(
    const PhaseStructuralReplacementEntities& actual, const PhaseStructuralReplacementAuthoring& authoring) {
    try {
        const auto encoded = encode_phase_structural_replacement_authoring(authoring);
        (void)decode_phase_structural_replacement_authoring(encoded);
        if (authoring.demolition) return replay_demolition(actual, authoring);
        // All mathematical/profile/receipt intents read the same unchanged map.
        const auto physical = replay_structural_object_edit_entities(actual, authoring.edits);
        const auto request = classify(actual, physical, authoring.edits);
        if (!request || request->registry_id != authoring.registry_id || request->alternative_id != authoring.alternative_id ||
            Ids(request->seed_object_ids.begin(), request->seed_object_ids.end()) !=
                Ids(authoring.seed_object_ids.begin(), authoring.seed_object_ids.end()))
            reject("replacement seeds differ from actually changed saved baseline membership");
        const auto plan = inspect_phase_structural_replacement_plan(actual, authoring.seed_object_ids,
            authoring.registry_id, authoring.alternative_id, authoring.complete_hosted);
        if (!plan.ready()) reject("replacement has unresolved affected dependencies");
        const Ids seeds(plan.seed_object_ids.begin(), plan.seed_object_ids.end());
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (authoring.identities.size() != expected.size()) reject("requires complete exact entity/child mapping");
        const std::set<StructuralHostedInstanceKey> expected_instances(
            plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
        if (authoring.hosted_instance_identities.size() != expected_instances.size())
            reject("requires complete exact qualified hosted-instance mapping");
        if (plan.required_entity_ids.size() > maximum_entities - actual.size()) reject("final entity budget exceeded");
        auto occupied = occupied_strings(actual);
        EmbeddedAssemblyPresentationIds original_aliases;
        if (authoring.complete_hosted) {
            original_aliases = embedded_assembly_presentation_ids(actual);
            for (const auto& [key, alias] : original_aliases) { (void)key; occupied.reserve(alias); }
        }
        Ids fresh;
        for (const auto& [old_id, new_id] : authoring.identities) {
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (authoring.complete_hosted && std::any_of(original_aliases.begin(), original_aliases.end(),
                [&](const auto& entry) { return entry.second == old_id; }))
                reject("computed render aliases cannot be authored mapping keys");
            identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        for (const auto& [key, new_id] : authoring.hosted_instance_identities) {
            if (!expected_instances.contains(key)) reject("mapping contains an unrequested qualified hosted identity");
            identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second)
                reject("fresh qualified hosted identity collision: " + new_id);
        }
        // Also reserve authored opaque content: proposed tokens cannot silently
        // turn an intent's unrelated key or value into a live entity identity.
        Strings authored;
        for (const auto& edit : authoring.edits) authored.read(encode_structural_object_edit_intent(edit));
        for (const auto& id : fresh) if (authored.values.contains(id)) reject("fresh identity collides with authored content: " + id);
        std::vector<StructuralObjectEditIntent> baseline_edits, ordinary_edits;
        StructuralHostedIdentityMap host_ids, catalog_ids;
        for (const auto& edit : authoring.edits)
            (seeds.contains(edit.object_id) ? baseline_edits : ordinary_edits).push_back(edit);
        for (const auto& id : seeds) host_ids.emplace(id, authoring.identities.at(id));
        for (const auto& id : plan.required_entity_ids) if (!seeds.contains(id))
            catalog_ids.emplace(id, authoring.identities.at(id));
        // These independently replay disjoint edit partitions against the same
        // actual source. An ordinary hosted row may share a retained catalog
        // with baseline rows; only the latter enter the new private catalog.
        auto candidate = authoring.complete_hosted
            ? replay_structural_hosted_component_geometry(actual, ordinary_edits) : actual;
        StructuralHostedComponentCopyResult hosted_copy;
        if (authoring.complete_hosted)
            hosted_copy = copy_structural_hosted_components(actual, baseline_edits, host_ids,
                catalog_ids, authoring.hosted_instance_identities);
        Ids final_owners;
        for (const auto& edit : authoring.edits) {
            const auto& id = edit.object_id;
            if (seeds.contains(id)) {
                auto copy = physical.at(id); copy.id = authoring.identities.at(id);
                if (!candidate.emplace(copy.id, copy).second) reject("proposed owner identity already exists");
                final_owners.insert(copy.id);
            } else {
                candidate.at(id) = physical.at(id);
                if (!exact(actual.at(id), physical.at(id))) final_owners.insert(id);
            }
        }
        for (auto& catalog : hosted_copy.catalogs)
            if (!candidate.emplace(catalog.id, std::move(catalog)).second) reject("proposed hosted catalog identity already exists");
        const auto model = ModelPhases::from_json(actual.at(plan.registry_id).properties.at("model"));
        auto model_ids = model.entity_ids(); auto alternatives = model.alternatives();
        auto selected = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& row) { return row.id == plan.alternative_id; });
        if (selected == alternatives.end()) reject("actual active alternative is absent");
        for (const auto& id : plan.required_entity_ids) {
            model_ids.push_back(authoring.identities.at(id));
            selected->proposed_ids.push_back(authoring.identities.at(id));
        }
        for (const auto& id : plan.seed_object_ids) selected->demolished_ids.push_back(id);
        const auto final_model = ModelPhases::create(model_ids, model.baseline_ids(), alternatives, model.active_alternative());
        auto raw = actual.at(plan.registry_id).properties.at("model");
        for (const auto& id : plan.required_entity_ids) raw.at("entity_ids").push_back(authoring.identities.at(id));
        for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == plan.alternative_id) {
            for (const auto& id : plan.required_entity_ids)
                alternative.at("proposed_ids").push_back(authoring.identities.at(id));
            for (const auto& id : plan.seed_object_ids) alternative.at("demolished_ids").push_back(id);
        }
        if (ModelPhases::from_json(raw).to_json() != final_model.to_json()) reject("retained registry differs from typed additive update");
        candidate.at(plan.registry_id).properties.at("model") = std::move(raw);
        auto presentation_ids = authoring.identities;
        Ids presentation_owners = seeds;
        for (const auto& [original, proposed] : hosted_copy.original_to_proposed_presentation) {
            if (!presentation_ids.emplace(original, proposed).second)
                reject("computed render identity overlaps authored source mapping");
            presentation_owners.insert(original);
        }
        complete_presentation(candidate, actual, presentation_owners, presentation_ids);
        admit_structural(candidate, final_owners);
        (void)memberships(candidate);
        if (authoring.complete_hosted) {
            (void)occupied_strings(candidate);
            const auto final_aliases = embedded_assembly_presentation_ids(candidate);
            for (const auto& [key, alias] : original_aliases)
                if (final_aliases.at(key) != alias) reject("complete candidate changed a retained original render alias");
            Ids proposed_aliases;
            for (const auto& [key, proposed_instance] : authoring.hosted_instance_identities) {
                const auto& alias = final_aliases.at({catalog_ids.at(key.first), proposed_instance});
                if (alias != hosted_copy.original_to_proposed_presentation.at(original_aliases.at(key)))
                    reject("complete candidate changed a proposed render alias after presentation replay");
                if (occupied.values.contains(alias) || authored.values.contains(alias) || fresh.contains(alias) ||
                    !proposed_aliases.insert(alias).second)
                    reject("complete candidate render alias collision");
                const auto& old_rows = actual.at(key.first).properties.at("model").at("instances");
                const auto& retained_rows = candidate.at(key.first).properties.at("model").at("instances");
                const auto row_with_id = [&](const auto& rows) {
                    return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.at("id") == key.second; });
                };
                const auto old_row = row_with_id(old_rows), retained_row = row_with_id(retained_rows);
                if (old_row == old_rows.end() || retained_row == retained_rows.end() ||
                    *old_row != *retained_row || old_row->dump() != retained_row->dump())
                    reject("replacement changed a retained baseline hosted row");
            }
            const auto hosted_final = inspect_structural_hosted_components(candidate,
                {final_owners.begin(), final_owners.end()});
            if (!hosted_final.ready()) reject("complete candidate hosted/native admission failed");
            std::vector<std::string> proposed_hosts;
            for (const auto& [id, proposed] : host_ids) { (void)id; proposed_hosts.push_back(proposed); }
            const auto copied_final = inspect_structural_hosted_components(candidate, proposed_hosts);
            if (!copied_final.ready()) reject("complete candidate copied hosted admission failed");
            Ids expected_catalogs;
            std::set<StructuralHostedInstanceKey> proposed_instances;
            for (const auto& [id, proposed] : catalog_ids) { (void)id; expected_catalogs.insert(proposed); }
            for (const auto& [key, proposed] : authoring.hosted_instance_identities)
                proposed_instances.emplace(catalog_ids.at(key.first), proposed);
            if (Ids(copied_final.catalog_ids.begin(), copied_final.catalog_ids.end()) != expected_catalogs ||
                std::set<StructuralHostedInstanceKey>(copied_final.instance_ids.begin(), copied_final.instance_ids.end()) != proposed_instances)
                reject("complete candidate copied roster differs from actual-source authority");
        }
        for (const auto& id : seeds) if (!exact(candidate.at(id), actual.at(id))) reject("replacement changed a retained baseline owner");
        return candidate;
    } catch (const Json::exception& error) { reject(std::string("malformed typed replay: ") + error.what()); }
}

} // namespace sketch
