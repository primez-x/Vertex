#include "sketch/slab_clone.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_slab_profile_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/slab_geometry_edit.hpp"
#include "sketch/slab_layer_stack_edit.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_identities = 4096;
constexpr std::size_t maximum_slab_segments = 4096;
constexpr std::size_t maximum_aggregate_segments = 65536;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Slab clone: " + reason);
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
// Bounds precede codecs, serialization and native admission. Opaque keys and
// strings reserve fresh names; their presence never grants rewrite authority.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > 64 * 1024 * 1024 - bytes) reject("source JSON string budget exceeded");
        bytes += value.size(); values.insert(value);
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("source JSON node/nesting budget exceeded");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("source has a nonfinite JSON scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
};
Strings occupied_strings(const SlabCloneEntities& source) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings result;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) result.values.insert(key);
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes: " + id);
        result.text(id); result.text(entity.type);
        result.read(entity.properties); result.read(entity.extensions);
    }
    return result;
}
bool touches(const Json& value, const Ids& ids) {
    if (ids.empty()) return false;
    Strings strings; strings.read(value);
    return std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return strings.values.contains(id); });
}
void diagnostic(SlabClonePlan& plan, const std::string& id, const std::string& reason) {
    const SlabCloneDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}
void footprint_bounds(const Entity& entity, std::size_t& aggregate) {
    const auto boundary = field(entity.properties, "boundary"), holes = field(entity.properties, "holes");
    if (!boundary || !boundary->is_array() || boundary->empty() || !holes || !holes->is_array() || holes->size() > 1024)
        reject("slab has no bounded actual footprint: " + entity.id);
    std::size_t count = 0;
    const auto ring = [&](const Json& edges) {
        if (!edges.is_array() || edges.empty() || edges.size() > maximum_slab_segments - count)
            reject("slab footprint segment budget exceeded: " + entity.id);
        count += edges.size();
    };
    ring(*boundary);
    for (const auto& hole : *holes) ring(hole);
    if (count > maximum_aggregate_segments - aggregate) reject("aggregate slab footprint segment budget exceeded");
    aggregate += count;
    if (const auto layers = field(entity.properties, "layers"); layers && (!layers->is_array() || layers->size() > 1024))
        reject("slab layer inventory is not a bounded array: " + entity.id);
}
Slab actual_slab(const Entity& entity) {
    if (entity.type != "slab") reject("source owner is not a slab: " + entity.id);
    std::size_t segments = 0; footprint_bounds(entity, segments);
    Slab result; std::string error;
    if (!read_document_slab(entity, result, error)) reject("unsupported slab " + entity.id + ": " + error);
    return result;
}
void admit_slabs(const SlabCloneEntities& source, const Ids& owners) {
    std::size_t segments = 0;
    for (const auto& id : owners) footprint_bounds(source.at(id), segments);
    const auto organization = organize_project(source);
    const std::vector<std::string> ids(owners.begin(), owners.end());
    const auto resolved = resolve_vertical_placements(source, ids);
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    const auto material = [&](const std::string& catalog_id, const std::string& material_id) {
        identity(catalog_id); identity(material_id);
        const auto catalog = source.find(catalog_id);
        if (catalog == source.end() || catalog->second.type != "assembly_model")
            reject("slab material requires an actual assembly catalog: " + catalog_id);
        if (!catalogs.contains(catalog_id)) catalogs.emplace(catalog_id,
            AssemblyModel::from_json(catalog->second.properties.at("model")));
        const auto& materials = catalogs.at(catalog_id).materials();
        if (std::none_of(materials.begin(), materials.end(), [&](const auto& row) { return row.id == material_id; }))
            reject("slab material is absent from its actual catalog: " + material_id);
    };
    for (const auto& id : owners) {
        const auto& entity = source.at(id);
        validate_slab_profile_source_entity(entity);
        validate_slab_geometry_derivation(entity);
        if (entity.extensions.contains("slab_layer_stack_retirement"))
            validate_slab_layer_stack_retirement(entity.extensions.at("slab_layer_stack_retirement"));
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            reject("slab has unresolved actual drawing context: " + id);
        const auto slab = actual_slab(resolved.at(id));
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
bool affected_overlay(const Json& row, const Ids& owners) {
    return (row.contains("object_id") && owners.contains(row.at("object_id").get<std::string>())) ||
        (row.contains("dimension_binding") && !row.at("dimension_binding").is_null() &&
            owners.contains(row.at("dimension_binding").at("object_id").get<std::string>()));
}
void remap_field(Json& row, const char* key, const SlabCloneIdentityMap& identities) {
    if (!row.contains(key)) return;
    const auto found = identities.find(row.at(key).get<std::string>());
    if (found != identities.end()) row.at(key) = found->second;
}
// Only qualified slots are ignored in this scratch reference scan. In
// particular, receipt siblings remain opaque even inside understood archives.
Entity opaque_remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "slab") {
        (void)actual_slab(entity);
        if (entity.extensions.contains("slab_layer_stack_retirement")) {
            auto& archive = entity.extensions.at("slab_layer_stack_retirement");
            validate_slab_layer_stack_retirement(archive);
            for (auto& row : archive.at("receipts")) row.erase("layer_id");
        }
        const auto archive_key = std::string(slab_geometry_derivations_key);
        if (entity.extensions.contains(archive_key)) {
            validate_slab_geometry_derivation(entity);
            for (auto& row : entity.extensions.at(archive_key).at("operations")) row.at("operation").erase("slab_id");
        }
        if (p.contains("layers")) for (auto& layer : p.at("layers")) layer.erase("id");
    } else if (entity.type == "model_phases") {
        // The phase model is a closed codec. Envelope siblings remain opaque.
        (void)ModelPhases::from_json(p.at("model")); p.erase("model");
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
void child_declarations(const Json& value, std::map<std::string, std::size_t, std::less<>>& counts) {
    if (value.is_array()) for (const auto& row : value) child_declarations(row, counts);
    else if (value.is_object()) for (const auto& [key, child] : value.items()) {
        if (key == "id" && child.is_string()) ++counts[child.get<std::string>()];
        child_declarations(child, counts);
    }
}
void diagnose_hosted_assemblies(SlabClonePlan& plan, const SlabCloneEntities& source, const Ids& owners) {
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model") continue;
        const auto model = field(entity.properties, "model");
        const auto schema = model ? field(*model, "schema") : nullptr;
        const auto instances = model ? field(*model, "instances") : nullptr;
        if (!schema || !schema->is_string() ||
            (*schema != "sketch.assemblies.v1" && *schema != "sketch.assemblies.v2" &&
             *schema != "sketch.assemblies.v3" && *schema != "sketch.assemblies.v4") ||
            !instances || !instances->is_array()) continue;
        for (const auto& instance : *instances) {
            const auto placement = field(instance, "placement");
            const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
            if (host && host->is_string() && owners.contains(host->get_ref<const std::string&>()))
                diagnostic(plan, id, "slab-hosted assembly on " + host->get<std::string>() +
                    " requires a qualified additive instance-copy codec; original remains preserved");
        }
    }
}
SlabClonePlan derive(const SlabCloneEntities& source, const std::vector<std::string>& selected) {
    SlabClonePlan plan;
    try {
        (void)occupied_strings(source);
        if (selected.empty() || selected.size() > maximum_identities) reject("requires bounded nonempty explicit slab selection");
        for (const auto& id : selected) identity(id);
        plan.selected_slab_ids = selected;
        std::sort(plan.selected_slab_ids.begin(), plan.selected_slab_ids.end());
        if (std::adjacent_find(plan.selected_slab_ids.begin(), plan.selected_slab_ids.end()) != plan.selected_slab_ids.end())
            reject("selected slab owners must be unique");
        const auto scope = constraint_phase_scope(source);
        std::map<std::string, std::string, std::less<>> memberships;
        for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, registry.registry_id).second) reject("overlapping all-registry membership: " + id);
        const Ids owners(plan.selected_slab_ids.begin(), plan.selected_slab_ids.end());
        Ids children;
        std::size_t segments = 0;
        for (const auto& id : owners) {
            const auto found = source.find(id);
            if (found == source.end() || found->second.type != "slab") reject("target must be an actual slab owner: " + id);
            if (scope.inactive_owner_ids.contains(id)) reject("selected slab is inactive in saved scope: " + id);
            footprint_bounds(found->second, segments);
            const auto slab = actual_slab(found->second);
            for (const auto& layer : slab.layers) {
                identity(layer.id);
                if (!children.insert(layer.id).second) reject("selected slabs share a layer identity: " + layer.id);
                if (owners.size() + children.size() > maximum_identities) reject("clone entity/child identity budget exceeded");
            }
        }
        admit_slabs(source, owners);
        diagnose_hosted_assemblies(plan, source, owners);
        std::map<std::string, std::size_t, std::less<>> declarations;
        // Historical slab_id/layer_id slots do not declare current children.
        // Any opaque id field still participates in conservative alias checks.
        for (const auto& [id, entity] : source) {
            (void)id; child_declarations(entity.properties, declarations); child_declarations(entity.extensions, declarations);
        }
        for (const auto& [id, entity] : source) {
            if (scope.inactive_owner_ids.contains(id)) continue;
            try {
                if (entity.type == kSheetViewEntityType) {
                    const auto model = decode_sheet_view_entity(entity);
                    for (const auto& view : model.views()) {
                        for (const auto& owner : view.object_ids) if (owners.contains(owner)) ++plan.copied_view_object_reference_count;
                        if (view.presentation.appearance) for (const auto& row : view.presentation.appearance->objects)
                            if (owners.contains(row.object_id)) ++plan.copied_view_appearance_count;
                        for (const auto& row : view.overlays) if (owners.contains(row.object_id) ||
                            (row.dimension_binding && owners.contains(row.dimension_binding->object_id))) {
                            identity(row.id);
                            if (!children.insert(row.id).second) reject("copied overlay aliases another copied child: " + row.id);
                            ++plan.copied_view_overlay_count;
                            if (owners.size() + children.size() > maximum_identities) reject("clone entity/child identity budget exceeded");
                        }
                    }
                } else if (entity.type == kAnnotationEntityType) {
                    validate_annotation_entity(entity);
                    for (const auto& row : entity.properties.at("state").at("overrides"))
                        if (owners.contains(row.at("target_id").get<std::string>())) ++plan.copied_annotation_override_count;
                } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
                    const auto decoded = decode_boundary_dimension_entity(entity);
                    if (decoded.supported() && owners.contains(decoded.dimension->boundary_id))
                        diagnostic(plan, id, "source-bound dimension on native slab " + decoded.dimension->boundary_id +
                            " has no qualified boundary clone target codec; original remains preserved");
                }
            } catch (const std::exception& error) {
                if (touches(entity.properties, owners) || touches(entity.extensions, owners)) diagnostic(plan, id, error.what());
            }
        }
        for (const auto& child : children) if (source.contains(child) || declarations[child] != 1)
            diagnostic(plan, child, "copied child must have exactly one current declaration and no entity alias");
        Ids affected = owners; affected.insert(children.begin(), children.end());
        for (const auto& [id, entity] : source) {
            try {
                if (entity.type == kSheetViewEntityType) {
                    const auto model = decode_sheet_view_entity(entity);
                    const bool relevant = touches(entity.properties, affected) || touches(entity.extensions, affected);
                    const auto actual_owner = [&](const std::string& owner) {
                        if (relevant && !owner.empty() && !source.contains(owner)) reject("affected view has a dangling source-object reference: " + owner);
                        if (children.contains(owner)) reject("view reference targets a child rather than an actual owner: " + owner);
                    };
                    for (const auto& view : model.views()) {
                        for (const auto& owner : view.object_ids) actual_owner(owner);
                        if (view.presentation.appearance) for (const auto& row : view.presentation.appearance->objects) actual_owner(row.object_id);
                        for (const auto& row : view.overlays) {
                            actual_owner(row.object_id);
                            if (row.dimension_binding) actual_owner(row.dimension_binding->object_id);
                        }
                    }
                } else if (entity.type == kAnnotationEntityType) {
                    validate_annotation_entity(entity);
                    for (const auto& row : entity.properties.at("state").at("overrides"))
                        if (children.contains(row.at("target_id").get<std::string>())) reject("override targets a child without a qualified owner codec");
                }
                const auto remainder = opaque_remainder(entity);
                if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                    diagnostic(plan, id, "affected live reference has no qualified clone codec; original remains preserved");
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
            }
        }
        plan.required_entity_ids.assign(owners.begin(), owners.end());
        plan.required_child_ids.assign(children.begin(), children.end());
    } catch (const std::exception& error) { diagnostic(plan, {}, error.what()); }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
    });
    return plan;
}
void complete_presentation(SlabCloneEntities& candidate, const SlabCloneEntities& source,
    const Ids& owners, const SlabCloneIdentityMap& identities) {
    const auto scope = constraint_phase_scope(source);
    for (const auto& [id, original] : source) {
        if (scope.inactive_owner_ids.contains(id)) continue;
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            auto& changed = candidate.at(id);
            for (auto& view : changed.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    auto& rows = view.at("object_ids"); const auto retained = rows;
                    for (const auto& row : retained) if (owners.contains(row.get<std::string>())) rows.push_back(identities.at(row.get<std::string>()));
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
            auto& rows = candidate.at(id).properties.at("state").at("overrides"); const auto retained = rows;
            for (auto row : retained) if (owners.contains(row.at("target_id").get<std::string>())) {
                remap_field(row, "target_id", identities); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}
} // namespace

bool SlabClonePlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.blocking; });
}
SlabClonePlan inspect_slab_clone_plan(const SlabCloneEntities& source, const std::vector<std::string>& selected_slab_ids) {
    return derive(source, selected_slab_ids);
}
SlabCloneResult replay_slab_clone(const SlabCloneEntities& source, const SlabClonePlan& plan,
    const SlabCloneIdentityMap& identities) {
    try {
        const auto derived = derive(source, plan.selected_slab_ids);
        if (derived != plan) reject("supplied plan differs from actual source discovery");
        if (!plan.ready()) reject("clone has unresolved affected dependencies");
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (identities.size() != expected.size()) reject("requires exact complete entity/child mapping");
        if (plan.required_entity_ids.size() > maximum_entities - source.size()) reject("final entity budget exceeded");
        const auto occupied = occupied_strings(source);
        Ids fresh;
        for (const auto& [old_id, new_id] : identities) {
            identity(old_id); identity(new_id);
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        SlabCloneResult result{source, identities, {}};
        for (const auto& id : plan.required_entity_ids) {
            auto copy = source.at(id); copy.id = identities.at(id);
            if (copy.type != "slab") reject("unsupported copy owner reached replay");
            if (copy.properties.contains("layers")) for (auto& layer : copy.properties.at("layers"))
                layer.at("id") = identities.at(layer.at("id").get<std::string>());
            const auto copy_id = copy.id;
            if (!result.entities.emplace(copy_id, std::move(copy)).second) reject("copy insertion collided");
        }
        const Ids owners(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        complete_presentation(result.entities, source, owners, identities);
        (void)occupied_strings(result.entities);
        Ids copies;
        for (const auto& id : owners) copies.insert(identities.at(id));
        admit_slabs(result.entities, copies);
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed source-derived replay: ") + error.what()); }
}
} // namespace sketch
