#include "sketch/roof_clone.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_roof_transform.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/phase_roof_uniform_transform.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include <Standard_Failure.hxx>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_identities = 4096;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Roof clone: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}
// Reservation only: opaque strings, object keys and historical IDs all occupy
// the fresh namespace. This scanner never grants generic remapping authority.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("source JSON node/nesting budget exceeded");
        const auto reserve = [&](const std::string& text) {
            if (text.size() > 64 * 1024 * 1024 - bytes) reject("source JSON string budget exceeded");
            bytes += text.size(); values.insert(text);
        };
        if (value.is_string()) reserve(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            reserve(key); read(child, depth + 1);
        }
    }
};
Strings occupied_strings(const RoofCloneEntities& source, bool include_hosted_instances = false) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings result;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) result.values.insert(key);
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes");
        result.read(Json(id)); result.read(Json(entity.type));
        result.read(entity.properties); result.read(entity.extensions);
        if (include_hosted_instances && entity.type == "assembly_model") {
            const auto model = field(entity.properties, "model");
            const auto instances = model ? field(*model, "instances") : nullptr;
            if (instances && instances->is_array()) for (const auto& instance : *instances) {
                const auto local_id = field(instance, "id");
                if (local_id && local_id->is_string())
                    result.read(Json(id + ":instance:" + local_id->get_ref<const std::string&>()));
            }
        }
    }
    return result;
}
bool touches(const Json& value, const Ids& ids) {
    if (ids.empty()) return false;
    Strings strings; strings.read(value);
    return std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return strings.values.contains(id); });
}
void diagnostic(RoofClonePlan& plan, const std::string& id, const std::string& reason) {
    const RoofCloneDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}
bool affected_overlay(const Json& row, const Ids& owners) {
    return (row.contains("object_id") && owners.contains(row.at("object_id").get<std::string>())) ||
        (row.contains("dimension_binding") && !row.at("dimension_binding").is_null() &&
            owners.contains(row.at("dimension_binding").at("object_id").get<std::string>()));
}
void remap_field(Json& row, const char* key, const RoofCloneIdentityMap& identities) {
    if (!row.contains(key)) return;
    const auto found = identities.find(row.at(key).get<std::string>());
    if (found != identities.end()) row.at(key) = found->second;
}
void admit_assignment(const RoofCloneEntities& source, const Json& assignment) {
    if (!assignment.is_object() || !assignment.at("version").is_number_integer() ||
        assignment.at("version") != 1 || !assignment.at("catalog_id").is_string() ||
        !assignment.at("material_id").is_string()) reject("source material assignment is unsupported");
    const auto catalog_id = assignment.at("catalog_id").get<std::string>();
    const auto material_id = assignment.at("material_id").get<std::string>();
    identity(catalog_id); identity(material_id);
    const auto catalog = source.find(catalog_id);
    if (catalog == source.end() || catalog->second.type != "assembly_model")
        reject("source material assignment requires an actual assembly catalog");
    const auto model = AssemblyModel::from_json(catalog->second.properties.at("model"));
    if (std::none_of(model.materials().begin(), model.materials().end(), [&](const auto& material) {
        return material.id == material_id;
    })) reject("source material assignment references a missing material");
}

// Only codec-qualified live references and understood historical provenance
// are stripped. Opaque archived receipt siblings retain reference semantics.
Entity opaque_remainder(Entity entity, bool include_hosted_instances = false) {
    auto& p = entity.properties;
    if (entity.type == "roof") {
        (void)decode_roof_entity(entity);
        if (entity.extensions.contains(std::string(roof_uniform_transform_derivations_key)))
            entity.extensions.at(std::string(roof_uniform_transform_derivations_key)) =
                roof_uniform_transform_opaque_remainder(entity);
        if (entity.extensions.contains(std::string(roof_rigid_transform_derivations_key)))
            entity.extensions.at(std::string(roof_rigid_transform_derivations_key)) =
                roof_rigid_transform_opaque_remainder(entity);
        if (entity.extensions.contains(std::string(roof_plan_resize_derivations_key)))
            entity.extensions.at(std::string(roof_plan_resize_derivations_key)) =
                roof_plan_resize_opaque_remainder(entity);
        if (p.contains("roof_openings")) for (auto& cut : p.at("roof_openings")) cut.erase("id");
        if (entity.extensions.contains("roof_opening_input")) {
            auto& receipt = entity.extensions.at("roof_opening_input");
            if (!receipt.is_object() || !receipt.contains("version") || receipt.at("version") != 1 ||
                !receipt.contains("entries") || !receipt.at("entries").is_object())
                reject("unsupported roof opening input envelope");
            auto values = Json::array();
            for (const auto& [id, value] : receipt.at("entries").items()) { (void)id; values.push_back(value); }
            receipt.at("entries") = std::move(values);
        }
    } else if (include_hosted_instances && entity.type == "assembly_model") {
        // Closed typed admission releases only the actual live host slots.
        // Unknown envelope fields and definition/override strings stay opaque.
        (void)AssemblyModel::from_json(p.at("model"));
        for (auto& row : p.at("model").at("instances"))
            if (row.contains("placement")) row.at("placement").erase("host_entity_id");
    } else if (entity.type == "roof_join") {
        (void)parse_roof_join(p, entity.id); p.erase("roof_ids");
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model")); p.erase("model");
    } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto dimension = decode_boundary_dimension_entity(entity);
        if (dimension.supported()) p.at("target").erase("entity_id");
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("object_ids");
            auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                row.erase("id"); row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                    row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) row.erase("target_id");
    }
    return entity;
}
// Conservative current child declaration inventory also catches aliases in
// unknown retained objects. Only understood archived IDs are historical.
void child_declarations(const Json& value, std::map<std::string, std::size_t, std::less<>>& counts) {
    if (value.is_array()) for (const auto& row : value) child_declarations(row, counts);
    else if (value.is_object()) for (const auto& [key, child] : value.items()) {
        if (key == "id" && child.is_string()) ++counts[child.get<std::string>()];
        child_declarations(child, counts);
    }
}
Entity catalog_instance_reference_remainder(const Entity& entity, const Ids& selected,
    bool all_instance_declarations = false) {
    auto scratch = opaque_remainder(entity, true);
    auto& model = scratch.properties.at("model");
    // Catalog-local definition/material/profile/part namespaces are retained.
    // Only qualified slots may be removed from this conservative scratch scan.
    for (auto& material : model.at("materials")) material.erase("id");
    const auto clear_overrides = [](Json& row) { row.erase("material_overrides"); };
    for (auto& type : model.at("types")) {
        type.erase("id"); type.erase("materials");
        if (type.contains("profiles")) for (auto& profile : type.at("profiles")) {
            profile.erase("id"); profile.erase("material_slot");
        }
        if (type.contains("parts")) for (auto& part : type.at("parts")) {
            part.erase("id"); part.erase("type_id"); clear_overrides(part);
        }
    }
    for (auto& instance : model.at("instances")) {
        if (all_instance_declarations || selected.contains(instance.at("id").get<std::string>())) instance.erase("id");
        instance.erase("type_id"); clear_overrides(instance);
        if (instance.contains("nested_overrides")) for (auto& change : instance.at("nested_overrides")) {
            change.erase("part_path"); clear_overrides(change);
        }
    }
    return scratch;
}
void admit_catalog_opaque_instance_references(const Entity& entity,
    const std::vector<RoofCloneHostedInstanceKey>& selected) {
    Ids affected;
    for (const auto& key : selected) if (key.first == entity.id) affected.insert(key.second);
    if (affected.empty()) return;
    affected.insert(entity.id);
    const auto scratch = catalog_instance_reference_remainder(entity, affected);
    if (touches(scratch.properties, affected) || touches(scratch.extensions, affected))
        reject("affected catalog has an opaque catalog/instance reference without a qualified copy codec: " + entity.id);
}
// Follow the renderer's profile/legacy split, admitting the authored placement
// without converting its dialect or replacing its raw numeric representation.
void admit_hosted_instance(const AssemblyModel& model, const AssemblyInstance& instance,
    const RoofCloneEntities& source, AssemblyExpansionBudget& budget) try {
    if (!instance.placement) reject("hosted assembly requires an actual placement");
    const auto& placement = *instance.placement;
    if (!std::isfinite(placement.translation_z_m)) reject("hosted assembly Z placement must be finite");
    const auto host = source.find(placement.host_entity_id);
    if (host == source.end() || host->second.type != "roof") reject("hosted assembly requires an actual native roof");
    const auto expansion = model.expand(instance, budget);
    if (!expansion.profiles.empty()) {
        (void)make_assembly_geometry(expansion);
        return;
    }
    (void)transform_assembly_shape(make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, host->second))),
        {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
            placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
} catch (const Standard_Failure& error) {
    reject(std::string("hosted assembly native admission failed: ") + error.what());
}
Ids discover_hosted_assemblies(RoofClonePlan& plan, const RoofCloneEntities& source, const Ids& roofs,
    const Ids& inactive) {
    Ids catalogs;
    AssemblyExpansionBudget budget;
    std::size_t inventory = 0;
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model") continue;
        const auto model = field(entity.properties, "model");
        const auto instances = model ? field(*model, "instances") : nullptr;
        if (!instances || !instances->is_array()) continue;
        bool affected = false;
        for (const auto& instance : *instances) {
            const auto placement = field(instance, "placement");
            const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
            if (host && host->is_string() && roofs.contains(host->get_ref<const std::string&>())) affected = true;
        }
        if (!affected) continue;
        if (inactive.contains(id)) reject("affected hosted assembly catalog is inactive: " + id);
        for (const auto* key : {"materials", "types", "instances"}) {
            const auto rows = field(*model, key);
            if (!rows || !rows->is_array() || rows->size() > maximum_entities - inventory)
                reject("affected assembly catalog inventory budget exceeded: " + id);
            inventory += rows->size();
        }
        for (const auto& [key, type] : std::vector<std::pair<const char*, const char*>>{
            {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"},
            {"layer_id", "layer"}, {"wall_id", "wall"}}) {
            if (!entity.properties.contains(key)) continue;
            const auto target_id = entity.properties.at(key).get<std::string>(); identity(target_id);
            const auto target = source.find(target_id);
            if (target == source.end() || target->second.type != type)
                reject("affected catalog has unresolved actual context: " + id);
        }
        const auto parsed = AssemblyModel::from_json(*model);
        catalogs.insert(id);
        for (const auto& instance : parsed.instances()) {
            if (!instance.placement || !roofs.contains(instance.placement->host_entity_id)) continue;
            // Authored local IDs follow AssemblyModel rules, not fresh document
            // identity rules, and remain qualified by their actual catalog.
            plan.required_hosted_instance_ids.emplace_back(id, instance.id);
            if (roofs.size() + catalogs.size() + plan.required_hosted_instance_ids.size() > maximum_identities)
                reject("hosted clone identity budget exceeded");
            admit_hosted_instance(parsed, instance, source, budget);
        }
    }
    std::sort(plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
    return catalogs;
}
struct Derivation {
    RoofClonePlan plan;
    std::map<std::string, Json, std::less<>> independent_assignments;
};
Derivation derive(const RoofCloneEntities& source, const std::vector<std::string>& selected,
    bool include_hosted_instances) {
    Derivation result;
    auto& plan = result.plan; plan.selected_roof_ids = selected;
    plan.include_hosted_instances = include_hosted_instances;
    try {
        (void)occupied_strings(source, include_hosted_instances);
        if (selected.empty() || selected.size() > maximum_identities) reject("requires bounded nonempty explicit roof selection");
        std::sort(plan.selected_roof_ids.begin(), plan.selected_roof_ids.end());
        if (std::adjacent_find(plan.selected_roof_ids.begin(), plan.selected_roof_ids.end()) != plan.selected_roof_ids.end())
            reject("selected roof owners must be unique");
        const auto scope = constraint_phase_scope(source);
        const auto qualified_cohorts = phase_qualified_roof_join_cohort_ids(source);
        std::map<std::string, std::string, std::less<>> memberships;
        for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, registry.registry_id).second) reject("overlapping all-registry membership: " + id);
        const auto membership = [&](const std::string& id) {
            const auto found = memberships.find(id);
            return found == memberships.end() ? std::string{} : found->second;
        };
        const auto role = [&](const std::string& id) {
            const auto registry = membership(id);
            if (registry.empty()) return ModelPhase::existing;
            return ModelPhases::from_json(source.at(registry).properties.at("model")).active_state().at(id);
        };
        const Ids roofs(plan.selected_roof_ids.begin(), plan.selected_roof_ids.end());
        Ids owners = roofs, children;
        std::map<std::string, TopoDS_Shape, std::less<>> shapes;
        const auto admit_roof = [&](const std::string& id) {
            const auto found = source.find(id);
            if (found == source.end() || found->second.type != "roof") reject("target must be an actual roof: " + id);
            if (!shapes.contains(id)) {
                validate_roof_uniform_transform_source_entity(found->second);
                if (found->second.properties.contains("material_assignment"))
                    admit_assignment(source, found->second.properties.at("material_assignment"));
                const auto object = decode_roof_entity(resolve_vertical_placement(source, found->second));
                (void)make_roof_shape(object);
                shapes.emplace(id, make_roof_structure_shape(object));
            }
        };
        for (const auto& id : roofs) {
            if (scope.inactive_owner_ids.contains(id)) reject("selected roof is inactive in saved scope: " + id);
            admit_roof(id);
            const auto& roof = source.at(id);
            Ids roof_children;
            if (roof.properties.contains("roof_openings")) for (const auto& cut : roof.properties.at("roof_openings")) {
                const auto child = cut.at("id").get<std::string>(); identity(child);
                if (!children.insert(child).second) reject("selected roofs share an opening identity: " + child);
                roof_children.insert(child);
            }
            if (roof.extensions.contains("roof_opening_input")) {
                const auto& receipt = roof.extensions.at("roof_opening_input");
                if (!receipt.is_object() || !receipt.contains("version") || receipt.at("version") != 1 ||
                    !receipt.contains("entries") || !receipt.at("entries").is_object())
                    reject("selected roof has unsupported opening input envelope: " + id);
                for (const auto& [child, value] : receipt.at("entries").items()) {
                    (void)value;
                    if (!roof_children.contains(child)) reject("opening input key has no actual owned opening: " + child);
                }
            }
        }
        std::map<std::string, std::string, std::less<>> joined;
        for (const auto& [id, entity] : source) if (entity.type == "roof_join") {
            const auto join = parse_roof_join(entity.properties, id);
            if (scope.inactive_owner_ids.contains(id) && qualified_cohorts.contains(id)) continue;
            for (const auto& roof : join.roof_ids) {
                if (!source.contains(roof) || source.at(roof).type != "roof") reject("retained join has a dangling/non-roof member: " + roof);
                if (!joined.emplace(roof, id).second) reject("a roof belongs to multiple retained joins: " + roof);
            }
            if (std::none_of(join.roof_ids.begin(), join.roof_ids.end(), [&](const auto& roof) { return roofs.contains(roof); })) continue;
            if (scope.inactive_owner_ids.contains(id)) reject("selected roof belongs to an inactive join: " + id);
            if (entity.extensions.contains(std::string(roof_join_phase_ownership_extension_key)) &&
                !has_phase_qualified_roof_join_ownership(entity))
                reject("affected join has unsupported phase ownership metadata: " + id);
            const bool qualified = qualified_cohorts.contains(id);
            std::vector<TopoDS_Shape> members;
            for (const auto& roof : join.roof_ids) {
                // Validated active qualified joins may combine ordinary and
                // proposed members. Independent copies receive fresh owners.
                if (scope.inactive_owner_ids.contains(roof) ||
                    (!qualified && (membership(roof) != membership(id) || role(roof) != role(id))))
                    reject("affected join spans inactive, foreign or different-phase owners: " + id);
                admit_roof(roof); members.push_back(shapes.at(roof));
            }
            validate_roof_join_skylights(join, source);
            (void)make_roof_join(join, members);
            if (join.material_assignment) admit_assignment(source, entity.properties.at("material_assignment"));
            if (std::all_of(join.roof_ids.begin(), join.roof_ids.end(), [&](const auto& roof) { return roofs.contains(roof); })) {
                owners.insert(id);
            } else if (join.material_assignment) for (const auto& roof : join.roof_ids) if (roofs.contains(roof)) {
                auto assignment = source.at(roof).properties.value("material_assignment", Json::object());
                for (const auto& [key, value] : entity.properties.at("material_assignment").items()) assignment[key] = value;
                admit_assignment(source, assignment); result.independent_assignments.emplace(roof, std::move(assignment));
            }
        }
        for (const auto& [id, entity] : source) if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            try {
                const auto decoded = decode_boundary_dimension_entity(entity);
                if (decoded.supported() && owners.contains(decoded.dimension->boundary_id)) {
                    if (scope.inactive_owner_ids.contains(id)) continue;
                    validate_boundary_dimension_target(*decoded.dimension, source.at(decoded.dimension->boundary_id));
                    owners.insert(id);
                }
            } catch (const std::exception& error) {
                if (touches(entity.properties, owners) || touches(entity.extensions, owners)) diagnostic(plan, id, error.what());
            }
        }
        const auto catalogs = include_hosted_instances ?
            discover_hosted_assemblies(plan, source, roofs, scope.inactive_owner_ids) : Ids{};
        for (const auto& id : catalogs)
            admit_catalog_opaque_instance_references(source.at(id), plan.required_hosted_instance_ids);
        const auto embedded_presentations = include_hosted_instances ?
            embedded_assembly_presentation_ids(source) : EmbeddedAssemblyPresentationIds{};
        Ids all_embedded_aliases, selected_embedded_aliases, presentation_owners = owners;
        for (const auto& [key, alias] : embedded_presentations) {
            (void)key; all_embedded_aliases.insert(alias);
        }
        for (const auto& key : plan.required_hosted_instance_ids) {
            const auto& alias = embedded_presentations.at(key);
            selected_embedded_aliases.insert(alias); presentation_owners.insert(alias);
        }
        std::map<std::string, std::size_t, std::less<>> declarations;
        for (const auto& [id, entity] : source) {
            (void)id; child_declarations(entity.properties, declarations);
            auto extensions = entity.extensions;
            if (entity.type == "roof" && extensions.contains(std::string(roof_rigid_transform_derivations_key))) {
                try {
                    extensions.at(std::string(roof_rigid_transform_derivations_key)) =
                        roof_rigid_transform_opaque_remainder(entity);
                } catch (const std::exception&) {
                    // An unrelated unsupported archive remains opaque. A
                    // selected archive has already failed source admission.
                }
            }
            if (entity.type == "roof" && extensions.contains(std::string(roof_plan_resize_derivations_key))) {
                try {
                    extensions.at(std::string(roof_plan_resize_derivations_key)) =
                        roof_plan_resize_opaque_remainder(entity);
                } catch (const std::exception&) {
                    // Unsupported unrelated history remains opaque. Qualified
                    // historical frames never declare current cut identities.
                }
            }
            if (entity.type == "roof" && extensions.contains(std::string(roof_uniform_transform_derivations_key))) {
                try {
                    extensions.at(std::string(roof_uniform_transform_derivations_key)) =
                        roof_uniform_transform_opaque_remainder(entity);
                } catch (const std::exception&) {
                    // Unsupported unrelated history remains opaque. Only a
                    // validated uniform-transform archive can release its
                    // historical identity declarations.
                }
            }
            child_declarations(extensions, declarations);
        }
        for (const auto& [id, entity] : source) {
            if (scope.inactive_owner_ids.contains(id)) continue;
            try {
                if (entity.type == kSheetViewEntityType) {
                    const auto model = decode_sheet_view_entity(entity);
                    for (const auto& view : model.views()) {
                        for (const auto& owner : view.object_ids) if (presentation_owners.contains(owner)) ++plan.copied_view_object_reference_count;
                        if (view.presentation.appearance) for (const auto& row : view.presentation.appearance->objects)
                            if (presentation_owners.contains(row.object_id)) ++plan.copied_view_appearance_count;
                        for (const auto& row : view.overlays) if (presentation_owners.contains(row.object_id) ||
                            (row.dimension_binding && presentation_owners.contains(row.dimension_binding->object_id))) {
                            if (!children.insert(row.id).second) reject("copied overlay aliases an owned child: " + row.id);
                        }
                    }
                } else if (entity.type == kAnnotationEntityType) {
                    validate_annotation_entity(entity);
                    for (const auto& row : entity.properties.at("state").at("overrides"))
                        if (presentation_owners.contains(row.at("target_id").get<std::string>())) ++plan.copied_annotation_override_count;
                }
            } catch (const std::exception& error) {
                if (touches(entity.properties, presentation_owners) || touches(entity.extensions, presentation_owners)) diagnostic(plan, id, error.what());
            }
        }
        for (const auto& child : children) if (source.contains(child) || declarations[child] != 1)
            diagnostic(plan, child, "copied child must have exactly one current declaration and no entity alias");
        Ids affected = presentation_owners; affected.insert(children.begin(), children.end());
        Ids native_affected = owners; native_affected.insert(children.begin(), children.end());
        for (const auto& [id, entity] : source) {
            if (scope.inactive_owner_ids.contains(id)) continue;
            try {
                // A known owner-only reference to a cut/overlay is not qualified
                // merely because the surrounding presentation codec is known.
                if (entity.type == kSheetViewEntityType) {
                    const auto model = decode_sheet_view_entity(entity);
                    const bool relevant = touches(entity.properties, affected) || touches(entity.extensions, affected);
                    const auto actual_owner = [&](const std::string& owner) {
                        if (relevant && !owner.empty() && !source.contains(owner) && !all_embedded_aliases.contains(owner))
                            reject("affected view has a dangling source-object reference: " + owner);
                        if (children.contains(owner)) reject("view reference targets a child rather than an actual owner");
                    };
                    for (const auto& view : model.views()) {
                        for (const auto& owner : view.object_ids) actual_owner(owner);
                        if (view.presentation.appearance) for (const auto& row : view.presentation.appearance->objects)
                            actual_owner(row.object_id);
                        for (const auto& row : view.overlays) {
                            actual_owner(row.object_id);
                            if (row.dimension_binding) actual_owner(row.dimension_binding->object_id);
                        }
                    }
                } else if (entity.type == kAnnotationEntityType) {
                    validate_annotation_entity(entity);
                    for (const auto& row : entity.properties.at("state").at("overrides"))
                        if (children.contains(row.at("target_id").get<std::string>())) reject("override targets a child without a qualified owner codec");
                } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
                    const auto decoded = decode_boundary_dimension_entity(entity);
                    if (decoded.supported() && children.contains(decoded.dimension->boundary_id)) reject("dimension targets a child without a qualified owner codec");
                }
                const auto remainder = opaque_remainder(entity, include_hosted_instances);
                const bool catalog = include_hosted_instances && entity.type == "assembly_model";
                const auto& owner_targets = catalog ? native_affected : affected;
                if (catalog && !selected_embedded_aliases.empty()) {
                    // Catalog-local IDs may legally spell a generated alias.
                    // Admit those codec slots separately; opaque names, values
                    // and envelope siblings still retain reference semantics.
                    const auto alias_remainder = catalog_instance_reference_remainder(entity, {}, true);
                    if (touches(alias_remainder.properties, selected_embedded_aliases) ||
                        touches(alias_remainder.extensions, selected_embedded_aliases))
                        diagnostic(plan, id, "affected hosted presentation reference has no qualified clone codec");
                }
                if (touches(remainder.properties, owner_targets) || touches(remainder.extensions, owner_targets))
                    diagnostic(plan, id, "affected live reference has no qualified clone codec");
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
            }
        }
        if (owners.size() + catalogs.size() + children.size() + plan.required_hosted_instance_ids.size() > maximum_identities)
            reject(include_hosted_instances ? "clone entity/child/hosted identity budget exceeded" :
                "clone entity/child identity budget exceeded");
        plan.required_entity_ids.assign(owners.begin(), owners.end());
        plan.required_entity_ids.insert(plan.required_entity_ids.end(), catalogs.begin(), catalogs.end());
        std::sort(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        plan.required_child_ids.assign(children.begin(), children.end());
    } catch (const std::exception& error) { diagnostic(plan, {}, error.what()); }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
    });
    return result;
}

void complete_presentation(RoofCloneEntities& candidate, const RoofCloneEntities& source,
    const Ids& owners, const RoofCloneIdentityMap& identities,
    const std::map<std::string, std::string, std::less<>>& hosted_aliases) {
    // Generated aliases are codec-qualified presentation targets, not document
    // identity slots. Their complete spellings may exceed identity()'s bound.
    const auto mapped_owner = [&](const std::string& id) -> const std::string& {
        const auto alias = hosted_aliases.find(id);
        return alias == hosted_aliases.end() ? identities.at(id) : alias->second;
    };
    const auto remap_owner = [&](Json& row, const char* key) {
        if (!row.contains(key)) return;
        const auto id = row.at(key).get<std::string>();
        if (owners.contains(id)) row.at(key) = mapped_owner(id);
    };
    const auto scope = constraint_phase_scope(source);
    for (const auto& [id, original] : source) {
        if (scope.inactive_owner_ids.contains(id)) continue;
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            auto& changed = candidate.at(id);
            for (auto& view : changed.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    auto& rows = view.at("object_ids"); const auto retained = rows;
                    for (const auto& row : retained) if (owners.contains(row.get<std::string>())) rows.push_back(mapped_owner(row.get<std::string>()));
                }
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null()) {
                    auto& rows = presentation.at("appearance").at("objects"); const auto retained = rows;
                    for (auto row : retained) if (owners.contains(row.at("object_id").get<std::string>())) {
                        remap_owner(row, "object_id"); rows.push_back(std::move(row));
                    }
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays"); const auto retained = rows;
                    for (auto row : retained) if (affected_overlay(row, owners)) {
                        row.at("id") = identities.at(row.at("id").get<std::string>());
                        remap_owner(row, "object_id");
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                            remap_owner(row.at("dimension_binding"), "object_id");
                        rows.push_back(std::move(row));
                    }
                }
            }
            validate_sheet_view_entity(changed);
        } else if (original.type == kAnnotationEntityType) {
            auto& rows = candidate.at(id).properties.at("state").at("overrides"); const auto retained = rows;
            for (auto row : retained) if (owners.contains(row.at("target_id").get<std::string>())) {
                remap_owner(row, "target_id"); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}
} // namespace

bool RoofClonePlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.blocking; });
}
RoofClonePlan inspect_roof_clone_plan(const RoofCloneEntities& source, const std::vector<std::string>& selected_roof_ids,
    bool include_hosted_instances) {
    return derive(source, selected_roof_ids, include_hosted_instances).plan;
}
RoofCloneResult replay_roof_clone(const RoofCloneEntities& source, const RoofClonePlan& plan,
    const RoofCloneIdentityMap& identities, const RoofCloneHostedInstanceIdentityMap& hosted_instance_identities) {
    try {
        const auto derived = derive(source, plan.selected_roof_ids, plan.include_hosted_instances);
        if (derived.plan != plan) reject("supplied plan differs from actual source discovery");
        if (!plan.ready()) reject("clone has unresolved affected dependencies");
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (identities.size() != expected.size()) reject("requires exact complete entity/child mapping");
        if (plan.required_entity_ids.size() > maximum_entities - source.size()) reject("final entity budget exceeded");
        const auto occupied = occupied_strings(source, plan.include_hosted_instances);
        const auto original_presentations = plan.include_hosted_instances ?
            embedded_assembly_presentation_ids(source) : EmbeddedAssemblyPresentationIds{};
        Ids fresh;
        for (const auto& [old_id, new_id] : identities) {
            identity(old_id); identity(new_id);
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        const std::set<RoofCloneHostedInstanceKey> expected_instances(
            plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
        if (hosted_instance_identities.size() != expected_instances.size()) reject("requires exact qualified hosted instance mapping");
        for (const auto& [key, new_id] : hosted_instance_identities) {
            if (!expected_instances.contains(key)) reject("mapping contains an unrequested qualified hosted instance");
            identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh hosted identity collision: " + new_id);
        }
        RoofCloneResult result{source, identities, {}, hosted_instance_identities};
        const Ids roofs(plan.selected_roof_ids.begin(), plan.selected_roof_ids.end());
        Ids owners;
        for (const auto& id : plan.required_entity_ids) {
            auto copy = source.at(id); copy.id = identities.at(id);
            if (copy.type == "roof") {
                if (copy.properties.contains("roof_openings")) for (auto& cut : copy.properties.at("roof_openings"))
                    cut.at("id") = identities.at(cut.at("id").get<std::string>());
                if (copy.extensions.contains("roof_opening_input")) {
                    auto& entries = copy.extensions.at("roof_opening_input").at("entries"); auto mapped = Json::object();
                    for (const auto& [child, value] : entries.items()) mapped[identities.at(child)] = value;
                    entries = std::move(mapped);
                }
                if (derived.independent_assignments.contains(id)) copy.properties["material_assignment"] = derived.independent_assignments.at(id);
                if (plan.include_hosted_instances && copy.properties.contains("material_assignment"))
                    remap_field(copy.properties.at("material_assignment"), "catalog_id", identities);
                validate_roof_uniform_transform_source_entity(copy);
            } else if (copy.type == "roof_join") {
                for (auto& roof : copy.properties.at("roof_ids")) roof = identities.at(roof.get<std::string>());
                // Independent copies own fresh members and do not inherit the
                // source join's registry authority. Unsupported source envelopes
                // refused during discovery and are never rewritten here.
                if (has_phase_qualified_roof_join_ownership(copy))
                    copy.extensions.erase(std::string(roof_join_phase_ownership_extension_key));
                if (plan.include_hosted_instances && copy.properties.contains("material_assignment"))
                    remap_field(copy.properties.at("material_assignment"), "catalog_id", identities);
                (void)parse_roof_join(copy.properties, copy.id);
            } else if (can_recognize_boundary_dimension_entity_type(copy.type)) {
                copy.properties.at("target").at("entity_id") = identities.at(copy.properties.at("target").at("entity_id").get<std::string>());
                if (!decode_boundary_dimension_entity(copy).supported()) reject("copied dimension no longer supported");
            } else if (plan.include_hosted_instances && copy.type == "assembly_model") {
                // Raw definitions, material IDs, schema, envelope and numeric
                // representation survive. Only selected hosted rows are copied.
                const auto retained = copy.properties.at("model").at("instances");
                auto& rows = copy.properties.at("model").at("instances"); rows = Json::array();
                for (auto row : retained) {
                    if (!row.contains("placement")) continue;
                    const auto host = row.at("placement").at("host_entity_id").get<std::string>();
                    if (!roofs.contains(host)) continue;
                    const RoofCloneHostedInstanceKey key{id, row.at("id").get<std::string>()};
                    row.at("id") = hosted_instance_identities.at(key);
                    row.at("placement").at("host_entity_id") = identities.at(host);
                    rows.push_back(std::move(row));
                }
                (void)AssemblyModel::from_json(copy.properties.at("model"));
            } else reject("unsupported copy owner reached replay");
            if (copy.type != "assembly_model") owners.insert(id);
            const auto copy_id = copy.id;
            if (!result.entities.emplace(copy_id, std::move(copy)).second) reject("copy insertion collided");
        }
        std::map<std::string, std::string, std::less<>> hosted_aliases;
        if (plan.include_hosted_instances) {
            const auto copied_presentations = embedded_assembly_presentation_ids(result.entities);
            for (const auto& [key, new_id] : hosted_instance_identities) {
                const auto& original_alias = original_presentations.at(key);
                hosted_aliases.emplace(original_alias, copied_presentations.at({identities.at(key.first), new_id}));
                owners.insert(original_alias);
            }
        }
        complete_presentation(result.entities, source, owners, identities, hosted_aliases);
        if (result.entities.size() > maximum_entities) reject("final entity budget exceeded");
        (void)occupied_strings(result.entities, plan.include_hosted_instances);
        validate_roof_join_ownership(result.entities);
        std::map<std::string, TopoDS_Shape, std::less<>> copied_shapes;
        for (const auto& id : plan.required_entity_ids) {
            const auto& copy = result.entities.at(identities.at(id));
            if (copy.type != "roof_join") continue;
            const auto join = parse_roof_join(copy.properties, copy.id);
            std::vector<TopoDS_Shape> members;
            for (const auto& roof : join.roof_ids) {
                if (!copied_shapes.contains(roof))
                    copied_shapes.emplace(roof, make_roof_shape(decode_roof_entity(
                        resolve_vertical_placement(result.entities, result.entities.at(roof)))));
                members.push_back(copied_shapes.at(roof));
            }
            validate_roof_join_skylights(join, result.entities);
            (void)make_roof_join(join, members);
        }
        if (plan.include_hosted_instances) {
            const auto candidate_presentations = embedded_assembly_presentation_ids(result.entities);
            for (const auto& [key, alias] : original_presentations) {
                const auto found = candidate_presentations.find(key);
                if (found == candidate_presentations.end() || found->second != alias)
                    reject("copy would change an original component's presentation identity");
            }
            Ids copies;
            for (const auto& id : roofs) copies.insert(identities.at(id));
            // Admission independently rediscovers the new host roster rather
            // than trusting the supplied qualified identity mapping.
            RoofClonePlan copied_plan;
            const auto copied_catalogs = discover_hosted_assemblies(copied_plan, result.entities, copies, {});
            std::set<RoofCloneHostedInstanceKey> expected_copies;
            Ids expected_catalogs;
            for (const auto& [key, new_id] : hosted_instance_identities) {
                const auto catalog_id = identities.at(key.first);
                expected_catalogs.insert(catalog_id); expected_copies.emplace(catalog_id, new_id);
                if (occupied.values.contains(candidate_presentations.at({catalog_id, new_id})))
                    reject("copied component presentation identity is already reserved in the source");
            }
            const std::set<RoofCloneHostedInstanceKey> actual_copies(copied_plan.required_hosted_instance_ids.begin(),
                copied_plan.required_hosted_instance_ids.end());
            if (copied_catalogs != expected_catalogs || actual_copies != expected_copies)
                reject("copied hosted roster differs from actual qualified source replay");
            for (const auto& id : plan.required_entity_ids) {
                const auto& original = source.at(id);
                const auto& retained = result.entities.at(id);
                if (retained != original || retained.properties.dump() != original.properties.dump() ||
                    retained.extensions.dump() != original.extensions.dump())
                    reject("replay changed an original roof, dependency or assembly catalog");
                const auto& copy = result.entities.at(identities.at(id));
                if ((copy.type == "roof" || copy.type == "roof_join") && copy.properties.contains("material_assignment"))
                    admit_assignment(result.entities, copy.properties.at("material_assignment"));
            }
        }
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed source-derived replay: ") + error.what()); }
}
} // namespace sketch
