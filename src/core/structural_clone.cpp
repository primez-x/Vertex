#include "sketch/structural_clone.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
constexpr std::size_t maximum_authoring_bytes = 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Structural independent clone: " + reason);
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
void source_instance_identity(const std::string& id) {
    if (id.empty() || id.size() > maximum_authoring_bytes ||
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }))
        reject("source hosted instance identity must be bounded and nonblank");
}
void source_saved_view_identity(const std::string& id) {
    if (id.empty() || id.size() > maximum_authoring_bytes ||
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }))
        reject("source saved-view identity must be bounded and nonblank");
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
// Read-only reservation also covers unknown future strings, keys and envelopes.
// Reservation never grants arbitrary JSON reference or rewrite authority.
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
Strings occupied_strings(const StructuralCloneEntities& actual) {
    if (actual.size() > maximum_entities) reject("source entity budget exceeded");
    Strings strings;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) strings.reserve(key);
    for (const auto& [id, entity] : actual) {
        if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes");
        strings.reserve(id); strings.reserve(entity.type);
        strings.read(entity.properties); strings.read(entity.extensions);
        if (entity.type != "assembly_model" || !entity.properties.contains("model")) continue;
        const auto& model = entity.properties.at("model");
        if (model.is_object() && model.contains("instances") && model.at("instances").is_array())
            for (const auto& row : model.at("instances"))
                if (row.is_object() && row.contains("id") && row.at("id").is_string())
                    strings.reserve(id + ":instance:" + row.at("id").get<std::string>());
    }
    return strings;
}
void diagnostic(StructuralClonePlan& plan, const std::string& id, const std::string& reason) {
    const StructuralCloneDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}

void admit_structural(const StructuralCloneEntities& actual, const Ids& owners) {
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
void remap_field(Json& value, const char* key, const StructuralCloneIdentityMap& identities) {
    if (!value.contains(key)) return;
    const auto found = identities.find(value.at(key).get<std::string>());
    if (found != identities.end()) value.at(key) = found->second;
}
// Only admitted known slots are removed in this scratch copy. Every unknown
// sibling remains visible to the affected-reference refusal check.
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
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                row.erase("id"); row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                    row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides"))
            if (row.at("target_kind") == "object") row.erase("target_id");
    }
    return entity;
}
Entity catalog_instance_remainder(Entity entity) {
    // Equal spellings in definition/material/part/profile namespaces are
    // admitted known identities, not references to a copied local instance.
    auto& model = entity.properties.at("model");
    for (auto& material : model.at("materials")) material.erase("id");
    const auto clear_materials = [](Json& row) { row.erase("material_overrides"); };
    for (auto& type : model.at("types")) {
        type.erase("id"); type.erase("materials");
        if (type.contains("profiles")) for (auto& profile : type.at("profiles")) {
            profile.erase("id"); profile.erase("material_slot");
        }
        if (type.contains("parts")) for (auto& part : type.at("parts")) {
            part.erase("id"); part.erase("type_id"); clear_materials(part);
        }
    }
    for (auto& instance : model.at("instances")) {
        instance.erase("type_id"); clear_materials(instance);
        if (instance.contains("nested_overrides")) for (auto& change : instance.at("nested_overrides")) {
            change.erase("part_path"); clear_materials(change);
        }
    }
    return entity;
}
StructuralClonePlan derive(const StructuralCloneEntities& actual, const std::vector<std::string>& selected) {
    StructuralClonePlan plan;
    plan.selected_object_ids = selected;
    std::sort(plan.selected_object_ids.begin(), plan.selected_object_ids.end());
    try {
        (void)occupied_strings(actual);
        if (selected.empty() || selected.size() > maximum_identities) reject("requires bounded nonempty structural selection");
        if (std::adjacent_find(plan.selected_object_ids.begin(), plan.selected_object_ids.end()) != plan.selected_object_ids.end())
            reject("selected actual structural owners must be unique");
        const Ids owners(plan.selected_object_ids.begin(), plan.selected_object_ids.end());
        const auto scope = constraint_phase_scope(actual);
        for (const auto& id : owners) {
            identity(id);
            const auto owner = actual.find(id);
            if (owner == actual.end() || (owner->second.type != "column" && owner->second.type != "beam"))
                reject("target must be an actual column/beam owner: " + id);
            if (scope.inactive_owner_ids.contains(id)) reject("selected structural owner is inactive: " + id);
        }
        admit_structural(actual, owners);
        const auto hosted = inspect_structural_hosted_components(actual, plan.selected_object_ids);
        for (const auto& item : hosted.diagnostics) diagnostic(plan, item.entity_id, item.reason);
        plan.required_hosted_instance_ids = hosted.instance_ids;
        const Ids catalogs(hosted.catalog_ids.begin(), hosted.catalog_ids.end());
        Ids presentation_owners = owners;
        for (const auto& [key, alias] : hosted.original_presentation_ids) {
            (void)key; presentation_owners.insert(alias);
        }
        Ids child_spellings;
        std::set<StructuralCloneOverlayKey> children;
        for (const auto& [id, entity] : actual) {
            if (entity.type != kSheetViewEntityType) continue;
            try {
                const auto model = decode_sheet_view_entity(entity);
                for (const auto& view : model.views()) for (const auto& overlay : view.overlays) {
                    source_child_identity(overlay.id);
                    if (presentation_owners.contains(overlay.object_id) ||
                        (overlay.dimension_binding && presentation_owners.contains(overlay.dimension_binding->object_id))) {
                        if (!children.emplace(id, view.id, overlay.id).second)
                            reject("copied overlay has duplicate qualified source identity");
                        child_spellings.insert(overlay.id);
                    }
                }
            } catch (const std::exception& error) {
                if (touches(entity.properties, presentation_owners) || touches(entity.extensions, presentation_owners))
                    diagnostic(plan, id, "affected view codec is unsupported: " + std::string(error.what()));
            }
        }
        Ids affected = presentation_owners;
        for (const auto& [id, entity] : actual) {
            try {
                const auto remainder = catalogs.contains(id)
                    ? structural_hosted_catalog_opaque_remainder(entity) : opaque_remainder(entity);
                auto physical_remainder = remainder;
                if (entity.type == kSheetViewEntityType)
                    for (auto& view : physical_remainder.properties.at("model").at("views")) view.erase("id");
                if (touches(physical_remainder.properties, affected) || touches(physical_remainder.extensions, affected))
                    diagnostic(plan, id, "affected reference has no qualified structural clone codec; original remains preserved");
                // Local instance names gain reference meaning only inside their
                // actual catalog, never across unrelated catalog namespaces.
                if (catalogs.contains(id)) {
                    Ids local_instances;
                    auto local = catalog_instance_remainder(remainder);
                    auto copied_rows = Json::array();
                    const auto& source_rows = entity.properties.at("model").at("instances");
                    for (std::size_t i = 0; i < source_rows.size(); ++i) {
                        const auto name = source_rows.at(i).at("id").get<std::string>();
                        local_instances.insert(name);
                        if (std::find(hosted.instance_ids.begin(), hosted.instance_ids.end(),
                                StructuralCloneHostedInstanceKey{id, name}) != hosted.instance_ids.end())
                            copied_rows.push_back(local.properties.at("model").at("instances").at(i));
                    }
                    // A private catalog omits unselected rows. Their opaque
                    // metadata is not copied; retained metadata must not point
                    // to any original local instance, including omitted ones.
                    local.properties.at("model").at("instances") = std::move(copied_rows);
                    if (touches(local.properties, local_instances) || touches(local.extensions, local_instances))
                        diagnostic(plan, id, "affected catalog-local instance reference has no qualified clone codec");
                }
                if (entity.type == kSheetViewEntityType) {
                    // Overlay-local names are checked only in their actual
                    // owning view, preserving repeated names in other views.
                    for (const auto& view : remainder.properties.at("model").at("views")) {
                        Ids local_children;
                        const auto view_id = view.at("id").get<std::string>();
                        for (const auto& key : children)
                            if (std::get<0>(key) == id && std::get<1>(key) == view_id)
                                local_children.insert(std::get<2>(key));
                        auto local = view; local.erase("id");
                        if (touches(local, local_children))
                            diagnostic(plan, id, "affected overlay-local reference has no qualified structural clone codec");
                    }
                    if (touches(remainder.extensions, child_spellings))
                        diagnostic(plan, id, "affected overlay reference in opaque view extensions has no qualified clone codec");
                } else if (touches(remainder.properties, child_spellings) || touches(remainder.extensions, child_spellings))
                    diagnostic(plan, id, "affected overlay reference outside its qualified view has no clone codec");
            } catch (const std::exception& error) {
                if (owners.contains(id) || catalogs.contains(id) || touches(entity.properties, affected) ||
                    touches(entity.extensions, affected) || touches(entity.properties, child_spellings) ||
                    touches(entity.extensions, child_spellings))
                    diagnostic(plan, id, "affected typed record is unsupported: " + std::string(error.what()));
            }
        }
        Ids required = owners; required.insert(catalogs.begin(), catalogs.end());
        plan.required_entity_ids.assign(required.begin(), required.end());
        plan.required_overlay_ids.assign(children.begin(), children.end());
        if (required.size() + children.size() + hosted.instance_ids.size() > maximum_identities)
            reject("clone entity/child/hosted identity budget exceeded");
    } catch (const std::exception& error) { diagnostic(plan, {}, error.what()); }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
    });
    return plan;
}
using WorldTransforms = std::map<std::string, AssemblyTransform, std::less<>>;
bool identity_transform(const AssemblyTransform& t) {
    return t.translation_m == AssemblyPoint3{} && t.rotation_radians == 0 &&
        t.scale == 1 && !t.mirrored_y && t.vertical_scale == 1;
}
void transform_overlay(Json& row, const CoordinatedView& view, const AssemblyTransform& transform) {
    if (identity_transform(transform)) return;
    // right = cross(up, -direction). The explicit point is on the actual
    // view plane; no host depth or inferred object coordinate is introduced.
    const auto& u = view.up; const auto& d = view.direction;
    const std::array<double, 3> right{u[2] * d[1] - u[1] * d[2],
        u[0] * d[2] - u[2] * d[0], u[1] * d[0] - u[0] * d[1]};
    const auto [c, s] = assembly_rotation_components(transform.rotation_radians);
    const auto parity = transform.mirrored_y ? -1.0 : 1.0;
    const auto x_coefficient = (transform.scale - 1.0) - transform.scale * (1.0 - c);
    const auto y_coefficient = parity == 1.0 ? x_coefficient :
        -(transform.scale + 1.0) + transform.scale * (1.0 - c);
    // Evaluate (L-I)*origin+t directly. Reconstructing a distant absolute
    // point and subtracting its origin can erase a small local translation.
    const AssemblyPoint3 origin_delta{
        x_coefficient * view.origin_m[0] - transform.scale * s * parity * view.origin_m[1] + transform.translation_m.x,
        transform.scale * s * view.origin_m[0] + y_coefficient * view.origin_m[1] + transform.translation_m.y,
        (transform.scale * transform.vertical_scale - 1.0) * view.origin_m[2] + transform.translation_m.z};
    auto linear = transform;
    linear.translation_m = {};
    const bool translation_only = transform.rotation_radians == 0.0 && transform.scale == 1.0 &&
        !transform.mirrored_y && transform.vertical_scale == 1.0;
    for (const auto* field : {"start_m", "end_m"}) {
        const auto& raw = row.at(field);
        const auto x = raw.at(0).get<double>(), y = raw.at(1).get<double>();
        const auto point = translation_only ? AssemblyPoint3{} : transform_assembly_point(
            {x * right[0] + y * u[0], x * right[1] + y * u[1], x * right[2] + y * u[2]}, linear);
        const std::array<double, 3> delta{point.x + origin_delta.x, point.y + origin_delta.y, point.z + origin_delta.z};
        double projected_x = translation_only ? x : 0.0;
        double projected_y = translation_only ? y : 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            projected_x += delta[i] * right[i]; projected_y += delta[i] * u[i];
        }
        if (!std::isfinite(projected_x) || !std::isfinite(projected_y)) reject("copied overlay projection is not finite");
        if (projected_x != x || projected_y != y) row.at(field) = Json::array({projected_x, projected_y});
    }
}
void complete_presentation(StructuralCloneEntities& candidate, const StructuralCloneEntities& actual,
    const Ids& owners, const StructuralCloneIdentityMap& identities,
    const StructuralCloneOverlayIdentityMap& overlay_identities, const WorldTransforms& transforms) {
    for (const auto& [id, original] : actual) {
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            const auto decoded = decode_sheet_view_entity(original);
            auto& changed = candidate.at(id);
            for (auto& view : changed.properties.at("model").at("views")) {
                const auto view_id = view.at("id").get<std::string>();
                const auto frame = std::find_if(decoded.views().begin(), decoded.views().end(), [&](const auto& v) { return v.id == view_id; });
                if (frame == decoded.views().end()) reject("actual saved view frame is absent");
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
                        row.at("id") = overlay_identities.at({id, view_id, row.at("id").get<std::string>()});
                        // Bound dimensions regenerate against the copied
                        // silhouette and retain their dimension-line offset.
                        if ((!row.contains("dimension_binding") || row.at("dimension_binding").is_null()) && row.contains("object_id")) {
                            const auto pose = transforms.find(row.at("object_id").get<std::string>());
                            if (pose != transforms.end()) transform_overlay(row, *frame, pose->second);
                        }
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
            for (auto row : retained) if (row.at("target_kind") == "object" && owners.contains(row.at("target_id").get<std::string>())) {
                remap_field(row, "target_id", identities); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}
} // namespace

bool StructuralClonePlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.blocking; });
}
StructuralClonePlan inspect_structural_clone_plan(const StructuralCloneEntities& actual,
    const std::vector<std::string>& selected_object_ids) {
    return derive(actual, selected_object_ids);
}
Json encode_structural_clone_authoring(const StructuralCloneAuthoring& authoring) {
    if (authoring.edits.empty() || authoring.edits.size() > maximum_identities ||
        authoring.identities.empty() || authoring.identities.size() > maximum_identities ||
        authoring.overlay_identities.size() > maximum_identities - authoring.identities.size() ||
        authoring.hosted_instance_identities.size() > maximum_identities - authoring.identities.size() - authoring.overlay_identities.size())
        reject("authoring requires bounded nonempty edits and exact identity inventories");
    Ids fresh, targets;
    for (const auto& [old_id, new_id] : authoring.identities) {
        identity(old_id); identity(new_id);
        if (old_id == new_id || !fresh.insert(new_id).second) reject("authoring identities must be fresh and injective");
    }
    Json overlay_rows = Json::array();
    for (const auto& [key, new_id] : authoring.overlay_identities) {
        identity(std::get<0>(key)); source_saved_view_identity(std::get<1>(key)); source_child_identity(std::get<2>(key)); identity(new_id);
        if (std::get<2>(key) == new_id || !fresh.insert(new_id).second)
            reject("overlay identities must be fresh and injective across all mappings");
        overlay_rows.push_back({{"view_entity_id", std::get<0>(key)}, {"saved_view_id", std::get<1>(key)},
            {"overlay_id", std::get<2>(key)}, {"proposed_overlay_id", new_id}});
    }
    Json hosted_rows = Json::array();
    std::size_t hosted_bytes{};
    for (const auto& [key, new_id] : authoring.hosted_instance_identities) {
        identity(key.first); source_instance_identity(key.second); identity(new_id);
        const auto size = key.first.size() + key.second.size() + new_id.size();
        if (size > maximum_authoring_bytes - hosted_bytes) reject("qualified hosted identity byte budget exceeded");
        hosted_bytes += size;
        if (key.second == new_id || !fresh.insert(new_id).second)
            reject("hosted identities must be fresh and injective across all mappings");
        hosted_rows.push_back({{"catalog_id", key.first}, {"instance_id", key.second}, {"proposed_instance_id", new_id}});
    }
    Json edits = Json::array();
    std::size_t bytes{};
    for (const auto& edit : authoring.edits) {
        if (!targets.insert(edit.object_id).second) reject("authoring requires unique actual structural edit targets");
        if (!authoring.identities.contains(edit.object_id)) reject("every actual edit target requires a copied owner identity");
        auto row = encode_structural_object_edit_intent(edit);
        const auto size = row.dump().size() + 1;
        if (size > maximum_authoring_bytes - bytes) reject("authoring byte budget exceeded");
        bytes += size; edits.push_back(std::move(row));
    }
    Json result{{"version", 1}, {"edits", std::move(edits)}, {"identities", authoring.identities},
        {"overlay_identities", std::move(overlay_rows)},
        {"hosted_instance_identities", std::move(hosted_rows)}};
    Strings bounds; bounds.node_limit = maximum_authoring_bytes; bounds.byte_limit = maximum_authoring_bytes;
    bounds.read(result);
    if (result.dump().size() > maximum_authoring_bytes) reject("authoring byte budget exceeded");
    return result;
}
StructuralCloneAuthoring decode_structural_clone_authoring(const Json& value) {
    try {
        if (!value.is_object() || value.size() != 5 || !value.contains("version") ||
            !value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.contains("edits") || !value.at("edits").is_array() ||
            !value.contains("identities") || !value.at("identities").is_object() ||
            !value.contains("overlay_identities") || !value.at("overlay_identities").is_array() ||
            !value.contains("hosted_instance_identities") || !value.at("hosted_instance_identities").is_array())
            reject("authoring must contain exactly its five v1 fields");
        if (value.at("edits").size() > maximum_identities || value.at("identities").size() > maximum_identities ||
            value.at("overlay_identities").size() > maximum_identities - value.at("identities").size() ||
            value.at("hosted_instance_identities").size() > maximum_identities - value.at("identities").size() - value.at("overlay_identities").size())
            reject("authoring item budget exceeded");
        Strings bounds; bounds.node_limit = maximum_authoring_bytes; bounds.byte_limit = maximum_authoring_bytes;
        bounds.read(value);
        if (value.dump().size() > maximum_authoring_bytes) reject("authoring byte budget exceeded");
        StructuralCloneAuthoring result;
        result.identities = value.at("identities").get<StructuralCloneIdentityMap>();
        for (const auto& row : value.at("edits")) result.edits.push_back(decode_structural_object_edit_intent(row));
        for (const auto& row : value.at("overlay_identities")) {
            if (!row.is_object() || row.size() != 4 || !row.contains("view_entity_id") || !row.at("view_entity_id").is_string() ||
                !row.contains("saved_view_id") || !row.at("saved_view_id").is_string() ||
                !row.contains("overlay_id") || !row.at("overlay_id").is_string() ||
                !row.contains("proposed_overlay_id") || !row.at("proposed_overlay_id").is_string())
                reject("qualified overlay identity row must contain exactly four string fields");
            if (!result.overlay_identities.emplace(StructuralCloneOverlayKey{
                row.at("view_entity_id").get<std::string>(), row.at("saved_view_id").get<std::string>(),
                row.at("overlay_id").get<std::string>()}, row.at("proposed_overlay_id").get<std::string>()).second)
                reject("duplicate qualified overlay source identity");
        }
        for (const auto& row : value.at("hosted_instance_identities")) {
            if (!row.is_object() || row.size() != 3 || !row.contains("catalog_id") || !row.at("catalog_id").is_string() ||
                !row.contains("instance_id") || !row.at("instance_id").is_string() ||
                !row.contains("proposed_instance_id") || !row.at("proposed_instance_id").is_string())
                reject("qualified hosted identity row must contain exactly three string fields");
            if (!result.hosted_instance_identities.emplace(
                std::pair{row.at("catalog_id").get<std::string>(), row.at("instance_id").get<std::string>()},
                row.at("proposed_instance_id").get<std::string>()).second)
                reject("duplicate qualified hosted source identity");
        }
        const auto canonical = encode_structural_clone_authoring(result);
        if (canonical != value || canonical.dump() != value.dump()) reject("authoring differs from canonical typed encoding");
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed authoring: ") + error.what()); }
}
StructuralCloneEntities replay_structural_clone_authoring(const StructuralCloneEntities& actual,
    const StructuralCloneAuthoring& authoring) {
    try {
        (void)decode_structural_clone_authoring(encode_structural_clone_authoring(authoring));
        std::vector<std::string> selected;
        for (const auto& edit : authoring.edits) selected.push_back(edit.object_id);
        const auto plan = derive(actual, selected);
        if (!plan.ready()) reject("clone has unresolved affected dependencies");
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        if (authoring.identities.size() != expected.size()) reject("requires complete exact physical/catalog mapping");
        const std::set<StructuralCloneOverlayKey> expected_overlays(plan.required_overlay_ids.begin(), plan.required_overlay_ids.end());
        if (authoring.overlay_identities.size() != expected_overlays.size()) reject("requires complete exact qualified overlay mapping");
        const std::set<StructuralCloneHostedInstanceKey> expected_instances(
            plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
        if (authoring.hosted_instance_identities.size() != expected_instances.size())
            reject("requires complete exact qualified hosted-instance mapping");
        if (plan.required_entity_ids.size() > maximum_entities - actual.size()) reject("final entity budget exceeded");
        auto occupied = occupied_strings(actual);
        const auto original_aliases = embedded_assembly_presentation_ids(actual);
        Ids aliases;
        for (const auto& [key, alias] : original_aliases) { (void)key; occupied.reserve(alias); aliases.insert(alias); }
        Ids fresh;
        const auto reserve = [&](const std::string& id) {
            identity(id);
            if (occupied.values.contains(id) || !fresh.insert(id).second) reject("fresh identity collision: " + id);
        };
        for (const auto& [old_id, new_id] : authoring.identities) {
            if (!expected.contains(old_id) || aliases.contains(old_id)) reject("unrequested or computed source identity mapping: " + old_id);
            reserve(new_id);
        }
        for (const auto& [key, new_id] : authoring.overlay_identities) {
            if (!expected_overlays.contains(key)) reject("unrequested qualified overlay identity mapping");
            reserve(new_id);
        }
        for (const auto& [key, new_id] : authoring.hosted_instance_identities) {
            if (!expected_instances.contains(key)) reject("unrequested qualified hosted identity mapping");
            reserve(new_id);
        }
        Strings authored;
        for (const auto& edit : authoring.edits) authored.read(encode_structural_object_edit_intent(edit));
        for (const auto& id : fresh) if (authored.values.contains(id)) reject("fresh identity collides with authored content: " + id);
        const Ids owners(selected.begin(), selected.end());
        StructuralHostedIdentityMap host_ids, catalog_ids;
        for (const auto& id : owners) host_ids.emplace(id, authoring.identities.at(id));
        for (const auto& id : plan.required_entity_ids) if (!owners.contains(id))
            catalog_ids.emplace(id, authoring.identities.at(id));
        const auto physical = replay_structural_object_edit_entities(actual, authoring.edits);
        auto hosted_copy = copy_structural_hosted_components(actual, authoring.edits, host_ids,
            catalog_ids, authoring.hosted_instance_identities, true);
        auto candidate = actual;
        Ids copied_owners;
        for (const auto& [id, proposed] : host_ids) {
            auto copy = physical.at(id); copy.id = proposed;
            if (!candidate.emplace(proposed, std::move(copy)).second) reject("copied physical owner insertion collided");
            copied_owners.insert(proposed);
        }
        for (auto& catalog : hosted_copy.catalogs) {
            const auto id = catalog.id;
            if (!candidate.emplace(id, std::move(catalog)).second) reject("copied hosted catalog insertion collided");
        }
        auto presentation_ids = authoring.identities;
        Ids presentation_owners = owners;
        for (const auto& [original, proposed] : hosted_copy.original_to_proposed_presentation) {
            if (!presentation_ids.emplace(original, proposed).second) reject("computed alias overlaps authored source mapping");
            presentation_owners.insert(original);
        }
        WorldTransforms transforms;
        for (const auto& edit : authoring.edits) if (edit.transform)
            transforms.emplace(edit.object_id, structural_world_transform(actual, edit.object_id, *edit.transform));
        // Hosted presentation uses the exact source catalog/profile consumer's
        // frame, which can differ from the physical owner's world frame.
        for (const auto& key : plan.required_hosted_instance_ids) {
            const auto& rows = actual.at(key.first).properties.at("model").at("instances");
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& value) { return value.at("id") == key.second; });
            if (row == rows.end()) reject("actual qualified hosted row is absent");
            const auto host = row->at("placement").at("host_entity_id").get<std::string>();
            const auto edit = std::find_if(authoring.edits.begin(), authoring.edits.end(), [&](const auto& value) { return value.object_id == host; });
            if (edit == authoring.edits.end()) reject("actual hosted owner has no typed copy edit");
            if (edit->transform) transforms.emplace(original_aliases.at(key),
                structural_hosted_transform(actual, key.first, key.second, *edit->transform));
        }
        complete_presentation(candidate, actual, presentation_owners, presentation_ids, authoring.overlay_identities, transforms);
        admit_structural(candidate, copied_owners);
        (void)occupied_strings(candidate);
        const auto final_aliases = embedded_assembly_presentation_ids(candidate);
        for (const auto& [key, alias] : original_aliases)
            if (final_aliases.at(key) != alias) reject("complete candidate changed an original presentation alias");
        Ids proposed_aliases;
        for (const auto& [key, new_id] : authoring.hosted_instance_identities) {
            const auto& alias = final_aliases.at({catalog_ids.at(key.first), new_id});
            if (alias != hosted_copy.original_to_proposed_presentation.at(original_aliases.at(key)))
                reject("complete candidate changed a copied alias after presentation replay");
            if (occupied.values.contains(alias) || authored.values.contains(alias) || fresh.contains(alias) ||
                !proposed_aliases.insert(alias).second) reject("complete candidate copied alias collision");
        }
        const auto final_hosted = inspect_structural_hosted_components(candidate, {copied_owners.begin(), copied_owners.end()});
        if (!final_hosted.ready()) reject("complete candidate copied hosted geometry/context admission failed");
        Ids expected_catalogs;
        std::set<StructuralCloneHostedInstanceKey> copied_instances;
        for (const auto& [id, proposed] : catalog_ids) { (void)id; expected_catalogs.insert(proposed); }
        for (const auto& [key, proposed] : authoring.hosted_instance_identities)
            copied_instances.emplace(catalog_ids.at(key.first), proposed);
        if (Ids(final_hosted.catalog_ids.begin(), final_hosted.catalog_ids.end()) != expected_catalogs ||
            std::set<StructuralCloneHostedInstanceKey>(final_hosted.instance_ids.begin(), final_hosted.instance_ids.end()) != copied_instances)
            reject("complete candidate copied roster differs from exact actual-source authority");
        for (const auto& [id, original] : actual) {
            if (original.type != kSheetViewEntityType && original.type != kAnnotationEntityType && !exact(candidate.at(id), original))
                reject("clone changed a retained physical, catalog, registry or unrelated owner");
        }
        return candidate;
    } catch (const Json::exception& error) { reject(std::string("malformed actual-source replay: ") + error.what()); }
}

} // namespace sketch
