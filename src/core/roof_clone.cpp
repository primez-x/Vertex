#include "sketch/roof_clone.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_roof_transform.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
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
Strings occupied_strings(const RoofCloneEntities& source) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings result;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) result.values.insert(key);
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes");
        result.read(Json(id)); result.read(Json(entity.type));
        result.read(entity.properties); result.read(entity.extensions);
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
Entity opaque_remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "roof") {
        (void)decode_roof_entity(entity);
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
struct Derivation {
    RoofClonePlan plan;
    std::map<std::string, Json, std::less<>> independent_assignments;
};
Derivation derive(const RoofCloneEntities& source, const std::vector<std::string>& selected) {
    Derivation result;
    auto& plan = result.plan; plan.selected_roof_ids = selected;
    try {
        (void)occupied_strings(source);
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
                validate_roof_rigid_transform_source_entity(found->second);
                if (found->second.properties.contains("material_assignment"))
                    admit_assignment(source, found->second.properties.at("material_assignment"));
                shapes.emplace(id, make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, found->second))));
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
            child_declarations(extensions, declarations);
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
                            if (!children.insert(row.id).second) reject("copied overlay aliases an owned child: " + row.id);
                        }
                    }
                } else if (entity.type == kAnnotationEntityType) {
                    validate_annotation_entity(entity);
                    for (const auto& row : entity.properties.at("state").at("overrides"))
                        if (owners.contains(row.at("target_id").get<std::string>())) ++plan.copied_annotation_override_count;
                }
            } catch (const std::exception& error) {
                if (touches(entity.properties, owners) || touches(entity.extensions, owners)) diagnostic(plan, id, error.what());
            }
        }
        for (const auto& child : children) if (source.contains(child) || declarations[child] != 1)
            diagnostic(plan, child, "copied child must have exactly one current declaration and no entity alias");
        Ids affected = owners; affected.insert(children.begin(), children.end());
        for (const auto& [id, entity] : source) {
            if (scope.inactive_owner_ids.contains(id)) continue;
            try {
                // A known owner-only reference to a cut/overlay is not qualified
                // merely because the surrounding presentation codec is known.
                if (entity.type == kSheetViewEntityType) {
                    const auto model = decode_sheet_view_entity(entity);
                    const bool relevant = touches(entity.properties, affected) || touches(entity.extensions, affected);
                    const auto actual_owner = [&](const std::string& owner) {
                        if (relevant && !owner.empty() && !source.contains(owner))
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
                const auto remainder = opaque_remainder(entity);
                if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                    diagnostic(plan, id, "affected live reference has no qualified clone codec");
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
            }
        }
        if (owners.size() + children.size() > maximum_identities) reject("clone entity/child identity budget exceeded");
        plan.required_entity_ids.assign(owners.begin(), owners.end());
        plan.required_child_ids.assign(children.begin(), children.end());
    } catch (const std::exception& error) { diagnostic(plan, {}, error.what()); }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
    });
    return result;
}

void complete_presentation(RoofCloneEntities& candidate, const RoofCloneEntities& source,
    const Ids& owners, const RoofCloneIdentityMap& identities) {
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

bool RoofClonePlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.blocking; });
}
RoofClonePlan inspect_roof_clone_plan(const RoofCloneEntities& source, const std::vector<std::string>& selected_roof_ids) {
    return derive(source, selected_roof_ids).plan;
}
RoofCloneResult replay_roof_clone(const RoofCloneEntities& source, const RoofClonePlan& plan,
    const RoofCloneIdentityMap& identities) {
    try {
        const auto derived = derive(source, plan.selected_roof_ids);
        if (derived.plan != plan) reject("supplied plan differs from actual source discovery");
        if (!plan.ready()) reject("clone has unresolved affected dependencies");
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (identities.size() != expected.size()) reject("requires exact complete entity/child mapping");
        const auto occupied = occupied_strings(source);
        Ids fresh;
        for (const auto& [old_id, new_id] : identities) {
            identity(old_id); identity(new_id);
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        RoofCloneResult result{source, identities, {}};
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
                validate_roof_rigid_transform_source_entity(copy);
            } else if (copy.type == "roof_join") {
                for (auto& roof : copy.properties.at("roof_ids")) roof = identities.at(roof.get<std::string>());
                // Independent copies own fresh members and do not inherit the
                // source join's registry authority. Unsupported source envelopes
                // refused during discovery and are never rewritten here.
                if (has_phase_qualified_roof_join_ownership(copy))
                    copy.extensions.erase(std::string(roof_join_phase_ownership_extension_key));
                (void)parse_roof_join(copy.properties, copy.id);
            } else if (can_recognize_boundary_dimension_entity_type(copy.type)) {
                copy.properties.at("target").at("entity_id") = identities.at(copy.properties.at("target").at("entity_id").get<std::string>());
                if (!decode_boundary_dimension_entity(copy).supported()) reject("copied dimension no longer supported");
            } else reject("unsupported copy owner reached replay");
            const auto copy_id = copy.id;
            if (!result.entities.emplace(copy_id, std::move(copy)).second) reject("copy insertion collided");
        }
        const Ids owners(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        complete_presentation(result.entities, source, owners, identities);
        if (result.entities.size() > maximum_entities) reject("final entity budget exceeded");
        (void)occupied_strings(result.entities);
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
            (void)make_roof_join(join, members);
        }
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed source-derived replay: ") + error.what()); }
}
} // namespace sketch
