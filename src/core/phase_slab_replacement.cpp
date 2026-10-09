#include "sketch/phase_slab_replacement.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/slab_clone.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"

#include <algorithm>
#include <numbers>
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
        // Presentation identities are synthesized from two qualified IDs and
        // therefore are not otherwise present in the raw string reservation.
        if (entity.type == "assembly_model" && entity.properties.contains("model")) {
            const auto& model = entity.properties.at("model");
            if (model.is_object() && model.contains("instances") && model.at("instances").is_array())
                for (const auto& instance : model.at("instances"))
                    if (instance.is_object() && instance.contains("id") && instance.at("id").is_string())
                        strings.read(Json(id + ":instance:" + instance.at("id").get<std::string>()));
        }
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

bool has_hosted_instances(const PhaseSlabReplacementEntities& source, const Ids& owners) {
    // occupied_strings bounds the complete source before this scratch scan.
    // This selects discovery only; raw slots never grant copy authority.
    for (const auto& [id, entity] : source) {
        (void)id;
        if (entity.type != "assembly_model" || !entity.properties.contains("model")) continue;
        const auto& model = entity.properties.at("model");
        if (!model.is_object() || !model.contains("instances") || !model.at("instances").is_array()) continue;
        for (const auto& row : model.at("instances")) {
            if (!row.is_object() || !row.contains("placement") || !row.at("placement").is_object()) continue;
            const auto& placement = row.at("placement");
            if (placement.contains("host_entity_id") && placement.at("host_entity_id").is_string() &&
                owners.contains(placement.at("host_entity_id").get_ref<const std::string&>())) return true;
        }
    }
    return false;
}

AssemblyTransform hosted_world_transform(const SlabGeometryEditIntent& intent) {
    AssemblyPoint3 pivot, offset;
    double angle{}, scale{1}, vertical_scale{1};
    bool horizontal{}, vertical{};
    if (intent.kind == SlabGeometryEditKind::transform_model) {
        const auto& t = *intent.model_transform;
        pivot = {t.pivot_m.x, t.pivot_m.y, t.pivot_m.z};
        offset = {t.offset_m.x, t.offset_m.y, t.offset_m.z};
        angle = t.rotation_radians; scale = t.uniform_scale;
        horizontal = t.flip_horizontal; vertical = t.flip_vertical;
    } else {
        const auto& t = *intent.transform;
        pivot = {t.pivot.x, t.pivot.y, 0}; offset = {t.offset.x, t.offset.y, 0};
        scale = intent.uniform_scale; vertical_scale = 1 / scale;
        angle = t.rotation_radians; horizontal = t.flip_horizontal; vertical = t.flip_vertical;
    }
    // Slab operations reflect after yaw. Assembly transforms reflect local Y
    // before yaw, so convert parity/order before evaluating the pivot shift.
    AssemblyTransform result{{}, std::remainder((horizontal ? std::numbers::pi : 0) +
        (horizontal != vertical ? -angle : angle), 2 * std::numbers::pi), scale, horizontal != vertical, vertical_scale};
    const auto mapped_pivot = transform_assembly_point(pivot, result);
    result.translation_m = {(pivot.x - mapped_pivot.x) + offset.x,
        (pivot.y - mapped_pivot.y) + offset.y, (pivot.z - mapped_pivot.z) + offset.z};
    return result;
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
    // Mathematical operation targets are retained historical provenance. The
    // validated archive's frames contain geometry only; opaque receipt/owner
    // siblings keep their ordinary scan authority and are never rewritten.
    const auto geometry_archive_key = std::string(slab_geometry_derivations_key);
    if (entity.type == "slab" && entity.extensions.contains(geometry_archive_key)) {
        validate_slab_geometry_derivation(entity);
        for (auto& row : entity.extensions.at(geometry_archive_key).at("operations"))
            row.at("operation").erase("slab_id");
    }
    auto& p = entity.properties;
    if (entity.type == "slab" && p.contains("layers")) {
        (void)actual_slab(entity);
        for (auto& layer : p.at("layers")) layer.erase("id");
    } else if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model"));
        // Admission scratch only: authored placements are retained verbatim.
        for (auto& row : p.at("model").at("instances"))
            if (row.contains("placement")) row.at("placement").erase("host_entity_id");
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

PhaseSlabGeometryEditPartition partition_geometry(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementEntities& physical,
    const std::vector<SlabGeometryEditIntent>& geometry) {
    const auto scope = constraint_phase_scope(source);
    PhaseSlabGeometryEditPartition result;
    for (const auto& intent : geometry) {
        const auto& id = intent.slab_id;
        if (exact(source.at(id), physical.at(id))) continue;
        if (scope.inactive_owner_ids.contains(id)) reject("geometry target is inactive: " + id);
        const PhysicalWallPhaseState* membership = nullptr;
        for (const auto& registry : scope.registries)
            if (std::find(registry.registered_entity_ids.begin(), registry.registered_entity_ids.end(), id) != registry.registered_entity_ids.end()) {
                if (membership) reject("geometry target has overlapping registry membership");
                membership = &registry;
            }
        if (!membership) { result.ordinary_geometry.push_back(intent); continue; }
        const auto model = ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), id) != model.baseline_ids().end();
        if (!baseline || !model.active_alternative()) {
            // A baseline with no saved active alternative has no proposed
            // destination and retains the ordinary geometry edit semantics.
            result.ordinary_geometry.push_back(intent);
            continue;
        }
        if (result.replacement && result.replacement->registry_id != membership->registry_id)
            reject("geometry edits span different shared-baseline registries");
        if (!result.replacement)
            result.replacement = PhaseSlabProfileReplacementRequest{membership->registry_id, *model.active_alternative(), {}};
        result.replacement->seed_slab_ids.push_back(id);
        result.baseline_geometry.push_back(intent);
    }
    if (result.replacement) std::sort(result.replacement->seed_slab_ids.begin(), result.replacement->seed_slab_ids.end());
    return result;
}

bool same_geometry(const std::vector<SlabGeometryEditIntent>& a, const std::vector<SlabGeometryEditIntent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto left = encode_slab_geometry_edit_intent(a[i]);
        const auto right = encode_slab_geometry_edit_intent(b[i]);
        if (left != right || left.dump() != right.dump()) return false;
    }
    return true;
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
        Ids required_entities = owners;
        if (has_hosted_instances(source, owners)) {
            // Reuse the bounded actual-host/native admission producer, without
            // importing its additive presentation replay into replacement.
            const auto hosted = inspect_slab_clone_plan(source, plan.seed_slab_ids);
            for (const auto& item : hosted.diagnostics) diagnostic(plan, item.entity_id, item.reason);
            required_entities.insert(hosted.required_entity_ids.begin(), hosted.required_entity_ids.end());
            plan.required_hosted_instance_ids = hosted.required_hosted_instance_ids;
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
        if (required_entities.size() + children.size() + plan.required_hosted_instance_ids.size() > maximum_replacements)
            reject("replacement entity/child/hosted budget exceeded");
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
        plan.required_entity_ids.assign(required_entities.begin(), required_entities.end());
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
    const std::vector<SlabLayerStackEditIntent>& stacks, const std::vector<SlabGeometryEditIntent>& geometry,
    const std::vector<SlabGeometryEditIntent>& ordinary_geometry,
    const PhaseSlabReplacementHostedInstanceIdentityMap& hosted_instance_identities,
    bool coordinate_ordinary_hosted_geometry) {
    try {
        const bool profile_edit = !profiles.empty(), stack_edit = !stacks.empty(), geometry_edit = !geometry.empty();
        if ((profile_edit + stack_edit + geometry_edit) != 1)
            reject("requires exactly one nonempty profile, stack or geometry edit family");
        if (!ordinary_geometry.empty() && !geometry_edit)
            reject("ordinary geometry requires the exclusive baseline geometry family");
        if (coordinate_ordinary_hosted_geometry && (ordinary_geometry.empty() || !geometry_edit))
            reject("coordinated ordinary hosted movement requires both geometry families");
        const auto derived = inspect_phase_slab_replacement_plan(source, plan.seed_slab_ids, plan.registry_id, plan.alternative_id);
        if (derived != plan) reject("supplied plan differs from actual source discovery");
        if (!derived.ready()) reject("replacement has unresolved affected dependencies");
        const Ids seeds(plan.seed_slab_ids.begin(), plan.seed_slab_ids.end());
        Ids targets;
        const auto add_target = [&](const auto& intent) {
            if (!seeds.contains(intent.slab_id) || !targets.insert(intent.slab_id).second)
                reject(geometry_edit ? "geometry requires unique explicit seed slabs" :
                    stack_edit ? "stacks require unique explicit seed slabs" : "profiles require unique explicit seed slabs");
        };
        for (const auto& profile : profiles) add_target(profile);
        for (const auto& stack : stacks) add_target(stack);
        for (const auto& intent : geometry) add_target(intent);
        if (targets != seeds) reject(geometry_edit ? "slab seeds must exactly match authored geometry targets" :
            stack_edit ? "slab seeds must exactly match authored stack targets" :
            "slab seeds must exactly match authored profile targets");
        auto all_geometry = geometry;
        all_geometry.insert(all_geometry.end(), ordinary_geometry.begin(), ordinary_geometry.end());
        const auto physical = geometry_edit ? replay_slab_geometry_entities(source, all_geometry) :
            stack_edit ? replay_slab_layer_stack_entities(source, stacks) : replay_slab_profile_entities(source, profiles);
        Ids ordinary_targets;
        if (!ordinary_geometry.empty()) {
            const auto partition = partition_geometry(source, physical, all_geometry);
            if (!partition.replacement || partition.replacement->registry_id != plan.registry_id ||
                partition.replacement->alternative_id != plan.alternative_id ||
                Ids(partition.replacement->seed_slab_ids.begin(), partition.replacement->seed_slab_ids.end()) != seeds ||
                !same_geometry(partition.baseline_geometry, geometry) ||
                !same_geometry(partition.ordinary_geometry, ordinary_geometry))
                reject("mixed geometry roles differ from actual changed saved membership");
            for (const auto& intent : ordinary_geometry) ordinary_targets.insert(intent.slab_id);
        }
        for (const auto& id : targets) if (exact(source.at(id), physical.at(id)))
            reject("unchanged slab cannot acquire replacement authority as an extra seed");
        std::optional<PhaseSlabReplacementEntities> ordinary;
        if (coordinate_ordinary_hosted_geometry)
            ordinary=replay_slab_geometry_with_hosted_entities(source, ordinary_geometry);
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (identities.size() != expected.size()) reject("requires complete exact entity/child mapping");
        if (plan.required_entity_ids.size() > maximum_entities - source.size()) reject("final entity budget exceeded");
        const auto occupied = occupied_strings(source);
        Ids fresh;
        for (const auto& [old_id, new_id] : identities) {
            identity(old_id); identity(new_id);
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        const std::set<PhaseSlabReplacementHostedInstanceKey> expected_instances(
            plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
        if (hosted_instance_identities.size() != expected_instances.size()) reject("requires exact qualified hosted instance mapping");
        for (const auto& [key, new_id] : hosted_instance_identities) {
            if (!expected_instances.contains(key)) reject("unrequested qualified hosted instance mapping");
            identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh hosted identity collision: " + new_id);
        }
        // Material bindings deliberately retain the actual material catalog.
        // Catalog identity discovery grants no blanket reference remapping.
        Ids affected = seeds;
        affected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
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
            if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                reject("proposed stack metadata has an unqualified affected reference: " + stack.slab_id);
        }
        for (const auto& intent : all_geometry) {
            const auto remainder = opaque_remainder(physical.at(intent.slab_id));
            if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                reject("proposed geometry metadata has an unqualified affected reference: " + intent.slab_id);
        }
        admit_slabs(physical, seeds);
        PhaseSlabReplacementResult result{source, identities, {}, hosted_instance_identities};
        for (const auto& id : ordinary_targets) result.entities.at(id) = physical.at(id);
        if (ordinary) for (const auto& [id,entity]:*ordinary)
            if (entity.type=="assembly_model" && !exact(entity,source.at(id))) result.entities.at(id)=entity;
        for (const auto& id : seeds) {
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
        if (!expected_instances.empty()) {
            const auto hosted_plan = inspect_slab_clone_plan(source, plan.seed_slab_ids);
            PhaseSlabReplacementIdentityMap clone_ids;
            for (const auto& id : hosted_plan.required_entity_ids) clone_ids.emplace(id, identities.at(id));
            for (const auto& id : hosted_plan.required_child_ids) clone_ids.emplace(id, identities.at(id));
            const auto hosted_copies = replay_slab_clone(source, hosted_plan, clone_ids, hosted_instance_identities);
            AssemblyExpansionBudget hosted_source_budget;
            for (const auto& id : plan.required_entity_ids) {
                if (seeds.contains(id)) continue;
                auto copy = hosted_copies.entities.at(identities.at(id));
                std::map<std::string, AssemblyTransform, std::less<>> transforms;
                bool preserve_untouched_rows = false;
                const auto catalog = AssemblyModel::from_json(source.at(id).properties.at("model"));
                for (const auto& instance : catalog.instances()) {
                    if (!instance.placement || !expected_instances.contains({id, instance.id})) continue;
                    const auto intent = std::find_if(geometry.begin(), geometry.end(), [&](const auto& edit) {
                        return edit.slab_id == instance.placement->host_entity_id;
                    });
                    if (intent != geometry.end() && (intent->kind == SlabGeometryEditKind::transform_model ||
                        intent->kind == SlabGeometryEditKind::transform_plan)) {
                        auto delta = hosted_world_transform(*intent);
                        if (intent->coordinate_world_hosted_geometry) {
                            preserve_untouched_rows = true;
                            if (!catalog.expand(instance, hosted_source_budget).profiles.empty()) {
                                const auto& frame = resolve_site_presentation(source, instance.placement->host_entity_id).forward;
                                delta = conjugate_assembly_transform_through_rigid_frame(delta,
                                    {{frame.translation_m.x, frame.translation_m.y, frame.translation_m.z},
                                        frame.rotation_radians, 1.0, false});
                            }
                        }
                        transforms.emplace(instance.id, delta);
                    }
                }
                if (!transforms.empty()) {
                    const auto transformed = transform_hosted_assembly_model(source.at(id).properties.at("model"), transforms,
                        preserve_untouched_rows);
                    auto& copied_model = copy.properties.at("model");
                    // Keep the source envelope, including v6 vertical factors,
                    // or its admitted placement upgrade and required legacy
                    // nesting fields. Retain only qualified selected rows.
                    copied_model = transformed;
                    copied_model.at("instances") = Json::array();
                    for (const auto& row : transformed.at("instances")) {
                        const PhaseSlabReplacementHostedInstanceKey key{id, row.at("id").get<std::string>()};
                        if (!expected_instances.contains(key)) continue;
                        auto changed = row;
                        changed.at("id") = hosted_instance_identities.at(key);
                        remap_field(changed.at("placement"), "host_entity_id", identities);
                        copied_model.at("instances").push_back(std::move(changed));
                    }
                }
                const auto copy_id = copy.id;
                if (!result.entities.emplace(copy_id, std::move(copy)).second) reject("hosted catalog copy insertion collides");
            }
        }
        const auto model = ModelPhases::from_json(source.at(plan.registry_id).properties.at("model"));
        auto model_ids = model.entity_ids(); auto alternatives = model.alternatives();
        const auto target = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& a) { return a.id == plan.alternative_id; });
        if (target == alternatives.end()) reject("target alternative disappeared");
        for (const auto& id : plan.required_entity_ids) {
            model_ids.push_back(identities.at(id)); target->proposed_ids.push_back(identities.at(id));
            if (seeds.contains(id)) target->demolished_ids.push_back(id);
        }
        const auto final_model = ModelPhases::create(model_ids, model.baseline_ids(), alternatives, model.active_alternative());
        auto raw = source.at(plan.registry_id).properties.at("model");
        for (const auto& id : plan.required_entity_ids) raw.at("entity_ids").push_back(identities.at(id));
        for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == plan.alternative_id)
            for (const auto& id : plan.required_entity_ids) {
                alternative.at("proposed_ids").push_back(identities.at(id));
                if (seeds.contains(id)) alternative.at("demolished_ids").push_back(id);
            }
        if (ModelPhases::from_json(raw).to_json() != final_model.to_json())
            reject("retained registry reconstruction differs from typed update");
        result.entities.at(plan.registry_id).properties.at("model") = std::move(raw);
        complete_presentation(result.entities, source, seeds, identities);
        Ids copies;
        for (const auto& id : seeds) copies.insert(identities.at(id));
        copies.insert(ordinary_targets.begin(), ordinary_targets.end());
        admit_slabs(result.entities, copies);
        (void)constraint_phase_scope(result.entities);
        if (!expected_instances.empty()) {
            const auto original_presentations=embedded_assembly_presentation_ids(source);
            const auto final_presentations=embedded_assembly_presentation_ids(result.entities);
            for (const auto& [key,alias]:original_presentations)
                if (final_presentations.at(key)!=alias)
                    reject("replacement changed an original component's presentation identity");
            for (const auto& [key,new_id]:hosted_instance_identities) {
                const auto& alias=final_presentations.at({identities.at(key.first),new_id});
                if (occupied.values.contains(alias) || fresh.contains(alias))
                    reject("proposed component presentation identity collides with source or a fresh identity");
            }
            std::vector<std::string> proposed_owners;
            for (const auto& id : seeds) proposed_owners.push_back(identities.at(id));
            const auto final_plan = inspect_slab_clone_plan(result.entities, proposed_owners);
            if (!final_plan.ready()) reject("proposed hosted geometry has unresolved actual dependencies");
            std::set<PhaseSlabReplacementHostedInstanceKey> proposed_instances;
            for (const auto& [key, new_id] : hosted_instance_identities)
                proposed_instances.emplace(identities.at(key.first), new_id);
            if (std::set<PhaseSlabReplacementHostedInstanceKey>(final_plan.required_hosted_instance_ids.begin(),
                final_plan.required_hosted_instance_ids.end()) != proposed_instances)
                reject("final proposed hosted roster differs from actual qualified source replay");
            for (const auto& id : plan.required_entity_ids) {
                if (seeds.contains(id)) continue;
                const auto original = AssemblyModel::from_json(source.at(id).properties.at("model"));
                const auto proposed = AssemblyModel::from_json(result.entities.at(identities.at(id)).properties.at("model"));
                for (const auto& instance : original.instances()) {
                    const PhaseSlabReplacementHostedInstanceKey key{id, instance.id};
                    if (!expected_instances.contains(key)) continue;
                    const auto found = std::find_if(proposed.instances().begin(), proposed.instances().end(), [&](const auto& row) {
                        return row.id == hosted_instance_identities.at(key);
                    });
                    if (found == proposed.instances().end() || !instance.placement || !found->placement ||
                        found->placement->host_entity_id != identities.at(instance.placement->host_entity_id) ||
                        found->type_id != instance.type_id || found->property_overrides != instance.property_overrides ||
                        found->material_overrides != instance.material_overrides || found->quantity_overrides != instance.quantity_overrides ||
                        found->root_transform != instance.root_transform || found->nested_overrides != instance.nested_overrides)
                        reject("proposed hosted component differs from its exact actual source host/type/overrides");
                }
            }
            for (const auto& id : plan.required_entity_ids)
                if (!exact(result.entities.at(id), ordinary ? ordinary->at(id) : source.at(id)))
                    reject("replacement changed an original slab or source catalog");
        }
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

std::optional<PhaseSlabProfileReplacementRequest> phase_slab_geometry_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabGeometryEditIntent>& geometry) {
    const auto physical = replay_slab_geometry_entities(source, geometry);
    std::vector<std::string> targets;
    for (const auto& intent : geometry) targets.push_back(intent.slab_id);
    return replacement_request(source, physical, targets, "geometry edits", "geometry");
}

PhaseSlabGeometryEditPartition partition_phase_slab_geometry_edits(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabGeometryEditIntent>& geometry) {
    const auto physical = replay_slab_geometry_entities(source, geometry);
    return partition_geometry(source, physical, geometry);
}

nlohmann::json encode_phase_slab_replacement_authoring(const PhaseSlabReplacementAuthoring& authoring) {
    identity(authoring.registry_id); identity(authoring.alternative_id);
    const bool profile_edit = !authoring.slab_profiles.empty();
    const bool stack_edit = !authoring.slab_stacks.empty();
    const bool geometry_edit = !authoring.slab_geometry.empty();
    const bool mixed_geometry = !authoring.ordinary_geometry.empty();
    const bool hosted_edit = !authoring.hosted_instance_identities.empty();
    const bool coordinated = authoring.coordinate_ordinary_hosted_geometry;
    if (authoring.seed_slab_ids.empty() || authoring.seed_slab_ids.size() > maximum_replacements ||
        authoring.identities.empty() || authoring.identities.size() > maximum_replacements ||
        authoring.slab_profiles.size() > maximum_replacements || authoring.slab_stacks.size() > maximum_replacements ||
        authoring.slab_geometry.size() > maximum_replacements ||
        authoring.ordinary_geometry.size() > maximum_replacements ||
        authoring.slab_geometry.size() + authoring.ordinary_geometry.size() > maximum_replacements ||
        authoring.hosted_instance_identities.size() > maximum_replacements ||
        authoring.identities.size() + authoring.hosted_instance_identities.size() > maximum_replacements ||
        (mixed_geometry && !geometry_edit) || (coordinated && !mixed_geometry) ||
        (profile_edit + stack_edit + geometry_edit) != 1)
        reject("authoring requires bounded nonempty seeds, mapping and exactly one edit family");
    Ids seeds, targets, fresh;
    for (const auto& id : authoring.seed_slab_ids) { identity(id); if (!seeds.insert(id).second) reject("duplicate authoring seed"); }
    for (const auto& [old_id, new_id] : authoring.identities) {
        identity(old_id); identity(new_id);
        if (old_id == new_id || !fresh.insert(new_id).second) reject("authoring identities must be fresh and injective");
    }
    for (const auto& id : seeds) if (!authoring.identities.contains(id)) reject("authoring seed has no proposed identity");
    const auto* family = geometry_edit ? "slab_geometry" : stack_edit ? "slab_stacks" : "slab_profiles";
    Json result{{"version", coordinated ? 6 : hosted_edit ? 5 : mixed_geometry ? 4 : geometry_edit ? 3 : stack_edit ? 2 : 1}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
        {"seed_slab_ids", authoring.seed_slab_ids}, {"identities", authoring.identities}, {family, Json::array()}};
    if (mixed_geometry || hosted_edit || coordinated) result["ordinary_geometry"] = Json::array();
    if (hosted_edit || coordinated) {
        for (const auto* key : {"slab_profiles", "slab_stacks", "slab_geometry"}) result[key] = Json::array();
        result["hosted_instance_identities"] = Json::array();
    }
    if (coordinated) result["coordinate_ordinary_hosted_geometry"]=true;
    auto bytes = result.dump().size();
    if (bytes > maximum_authoring_bytes) reject("authoring byte budget exceeded");
    for (const auto& [key, new_id] : authoring.hosted_instance_identities) {
        identity(key.first); identity(new_id);
        if (key.second.empty() || key.second.size() > maximum_authoring_bytes ||
            !authoring.identities.contains(key.first) || seeds.contains(key.first) ||
            new_id == key.second || !fresh.insert(new_id).second)
            reject("authoring hosted identities require qualified mapped catalogs and injective fresh instance IDs");
        Json row{{"catalog_id", key.first}, {"instance_id", key.second}, {"proposed_instance_id", new_id}};
        const auto added = row.dump().size() + (result.at("hosted_instance_identities").empty() ? 0 : 1);
        if (added > maximum_authoring_bytes - bytes) reject("authoring byte budget exceeded");
        bytes += added; result.at("hosted_instance_identities").push_back(std::move(row));
    }
    auto& rows = result.at(family);
    const auto append = [&](const auto& intent, Json encoded) {
        if (!seeds.contains(intent.slab_id) || !targets.insert(intent.slab_id).second)
            reject(geometry_edit ? "authoring geometry requires unique seed targets" :
                stack_edit ? "authoring stacks require unique seed targets" : "authoring profiles require unique seed targets");
        const auto added = encoded.dump().size() + (rows.empty() ? 0 : 1);
        if (added > maximum_authoring_bytes - bytes) reject("authoring byte budget exceeded");
        bytes += added; rows.push_back(std::move(encoded));
    };
    for (const auto& profile : authoring.slab_profiles) append(profile, encode_slab_profile_edit_intent(profile));
    for (const auto& stack : authoring.slab_stacks) append(stack, encode_slab_layer_stack_edit_intent(stack));
    for (const auto& intent : authoring.slab_geometry) append(intent, encode_slab_geometry_edit_intent(intent));
    if (targets != seeds) reject(geometry_edit ? "authoring seeds must exactly match geometry targets" :
        stack_edit ? "authoring seeds must exactly match stack targets" :
        "authoring seeds must exactly match profile targets");
    if (mixed_geometry) {
        auto& ordinary_rows = result.at("ordinary_geometry");
        for (const auto& intent : authoring.ordinary_geometry) {
            if (!targets.insert(intent.slab_id).second || authoring.identities.contains(intent.slab_id))
                reject("authoring ordinary geometry requires disjoint unique unmapped targets");
            auto encoded = encode_slab_geometry_edit_intent(intent);
            const auto added = encoded.dump().size() + (ordinary_rows.empty() ? 0 : 1);
            if (added > maximum_authoring_bytes - bytes) reject("authoring byte budget exceeded");
            bytes += added; ordinary_rows.push_back(std::move(encoded));
        }
    }
    return result;
}

PhaseSlabReplacementAuthoring decode_phase_slab_replacement_authoring(const nlohmann::json& value) {
    try {
        if (!value.is_object() || !value.contains("version") ||
            !value.at("version").is_number_integer() ||
            (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 &&
                value.at("version") != 4 && value.at("version") != 5 && value.at("version") != 6) ||
            value.size() != (value.at("version") == 6 ? 11 : value.at("version") == 5 ? 10 : value.at("version") == 4 ? 7 : 6) ||
            !value.contains("registry_id") || !value.contains("alternative_id") || !value.contains("seed_slab_ids") ||
            !value.contains("identities") || !value.at("identities").is_object() || !value.at("seed_slab_ids").is_array())
            reject("authoring must contain exactly its supported versioned fields");
        const bool coordinated = value.at("version") == 6;
        if (coordinated && (!value.contains("coordinate_ordinary_hosted_geometry") ||
            !value.at("coordinate_ordinary_hosted_geometry").is_boolean() ||
            value.at("coordinate_ordinary_hosted_geometry")!=true ||
            !value.contains("ordinary_geometry") || !value.at("ordinary_geometry").is_array() ||
            value.at("ordinary_geometry").empty()))
            reject("v6 requires explicit coordinated ordinary hosted geometry");
        const bool hosted_edit = value.at("version") == 5 || coordinated;
        if (hosted_edit) {
            for (const auto* key : {"slab_profiles", "slab_stacks", "slab_geometry", "ordinary_geometry", "hosted_instance_identities"})
                if (!value.contains(key) || !value.at(key).is_array() || value.at(key).size() > maximum_replacements)
                    reject("hosted authoring requires all bounded typed arrays");
            if ((!coordinated && value.at("hosted_instance_identities").empty()) ||
                ((!value.at("slab_profiles").empty()) + (!value.at("slab_stacks").empty()) +
                    (!value.at("slab_geometry").empty())) != 1 ||
                (!value.at("ordinary_geometry").empty() && value.at("slab_geometry").empty()) ||
                value.at("slab_geometry").size() + value.at("ordinary_geometry").size() > maximum_replacements ||
                value.at("identities").size() + value.at("hosted_instance_identities").size() > maximum_replacements)
                reject("hosted authoring requires its qualified identities and exactly one exclusive primary family");
        }
        const bool stack_edit = value.at("version") == 2 || (hosted_edit && !value.at("slab_stacks").empty());
        const bool mixed_geometry = value.at("version") == 4 || (hosted_edit && !value.at("ordinary_geometry").empty());
        const bool geometry_edit = value.at("version") == 3 || mixed_geometry || (hosted_edit && !value.at("slab_geometry").empty());
        const auto* family = geometry_edit ? "slab_geometry" : stack_edit ? "slab_stacks" : "slab_profiles";
        if (!value.contains(family) || !value.at(family).is_array())
            reject("authoring edit family must exactly match its version");
        if (mixed_geometry && (!value.contains("ordinary_geometry") || !value.at("ordinary_geometry").is_array() ||
            value.at(family).empty() || value.at("ordinary_geometry").empty()))
            reject("v4 authoring requires both nonempty geometry lists");
        if (value.at("seed_slab_ids").size() > maximum_replacements || value.at("identities").size() > maximum_replacements ||
            value.at(family).size() > maximum_replacements ||
            (mixed_geometry && (value.at("ordinary_geometry").size() > maximum_replacements ||
                value.at(family).size() + value.at("ordinary_geometry").size() > maximum_replacements)))
            reject("authoring budget exceeded");
        Strings budget; budget.node_limit = maximum_authoring_bytes; budget.byte_limit = maximum_authoring_bytes;
        budget.read(value);
        if (value.dump().size() > maximum_authoring_bytes) reject("authoring byte budget exceeded");
        PhaseSlabReplacementAuthoring result;
        result.coordinate_ordinary_hosted_geometry=coordinated;
        result.registry_id = value.at("registry_id").get<std::string>();
        result.alternative_id = value.at("alternative_id").get<std::string>();
        result.seed_slab_ids = value.at("seed_slab_ids").get<std::vector<std::string>>();
        result.identities = value.at("identities").get<PhaseSlabReplacementIdentityMap>();
        if (hosted_edit) for (const auto& row : value.at("hosted_instance_identities")) {
            if (!row.is_object() || row.size() != 3 || !row.contains("catalog_id") || !row.contains("instance_id") ||
                !row.contains("proposed_instance_id") || !row.at("catalog_id").is_string() ||
                !row.at("instance_id").is_string() || !row.at("proposed_instance_id").is_string())
                reject("qualified hosted identity rows must have exactly three string fields");
            const PhaseSlabReplacementHostedInstanceKey key{row.at("catalog_id").get<std::string>(), row.at("instance_id").get<std::string>()};
            if (!result.hosted_instance_identities.emplace(key, row.at("proposed_instance_id").get<std::string>()).second)
                reject("duplicate qualified hosted identity row");
        }
        if (geometry_edit) for (const auto& intent : value.at(family))
            result.slab_geometry.push_back(decode_slab_geometry_edit_intent(intent));
        else if (stack_edit) for (const auto& stack : value.at(family))
            result.slab_stacks.push_back(decode_slab_layer_stack_edit_intent(stack));
        else for (const auto& profile : value.at(family))
            result.slab_profiles.push_back(decode_slab_profile_edit_intent(profile));
        if (mixed_geometry) for (const auto& intent : value.at("ordinary_geometry"))
            result.ordinary_geometry.push_back(decode_slab_geometry_edit_intent(intent));
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
    return replay_phase_slab_replacement(source, plan, authoring.identities, authoring.slab_profiles,
        authoring.slab_stacks, authoring.slab_geometry, authoring.ordinary_geometry,
        authoring.hosted_instance_identities, authoring.coordinate_ordinary_hosted_geometry).entities;
}

} // namespace sketch
