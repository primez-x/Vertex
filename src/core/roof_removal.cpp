#include "sketch/roof_removal.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_roof_demolition.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/phase_roof_transform.hpp"
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
    throw std::invalid_argument("Roof removal: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}

// Read-only reservation, including opaque values and object keys. This is
// deliberately never a reference rewrite codec.
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
Strings occupied_strings(const RoofRemovalEntities& source) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings result;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) result.values.insert(key);
    for (const auto& [id, entity] : source) {
        if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
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
void diagnostic(RoofRemovalPlan& plan, const std::string& id, const std::string& reason) {
    const RoofRemovalDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}
std::string overlay_owner(const Json& row) {
    if (row.contains("object_id") && !row.at("object_id").get<std::string>().empty())
        return row.at("object_id").get<std::string>();
    if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
        return row.at("dimension_binding").at("object_id").get<std::string>();
    return {};
}
void remove_ids(Json& rows, const Ids& removed) {
    auto retained = Json::array();
    for (const auto& row : rows) if (!removed.contains(row.get<std::string>())) retained.push_back(row);
    rows = std::move(retained);
}

void admit_assignment(const RoofRemovalEntities& source, const Json& assignment);

// Full resolved source/final roof admission. Retained geometry is never
// inferred from a supplied candidate or approximated by a bounding box.
std::map<std::string, TopoDS_Shape, std::less<>> admit_roofs_and_joins(
    const RoofRemovalEntities& source, const Ids& roofs, const Ids& joins) {
    std::map<std::string, TopoDS_Shape, std::less<>> shapes;
    for (const auto& id : roofs) {
        const auto& entity = source.at(id);
        if (entity.type != "roof") reject("affected source owner is not an actual roof: " + id);
        validate_roof_profile_source_entity(entity);
        if (entity.properties.contains("material_assignment")) admit_assignment(source, entity.properties.at("material_assignment"));
        shapes.emplace(id, make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, entity))));
    }
    for (const auto& id : joins) {
        const auto& entity = source.at(id);
        if (entity.type != "roof_join") reject("affected source owner is not an actual roof join: " + id);
        const auto join = parse_roof_join(entity.properties, id);
        if (join.material_assignment) admit_assignment(source, entity.properties.at("material_assignment"));
        std::vector<TopoDS_Shape> members;
        for (const auto& roof : join.roof_ids) {
            if (!shapes.contains(roof)) reject("join member is not an actual roof: " + roof);
            members.push_back(shapes.at(roof));
        }
        (void)make_roof_join(join, members);
    }
    return shapes;
}
void admit_assignment(const RoofRemovalEntities& source, const Json& assignment) {
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
    })) reject("source material assignment references a missing catalog material");
}

// Remove only codec-qualified reference fields for dependency inspection.
// Unknown affected data refuses instead of acquiring deletion/remapping authority.
Entity opaque_remainder(Entity entity, const Ids& retained_roofs) {
    auto& p = entity.properties;
    if (entity.type == "roof" && retained_roofs.contains(entity.id)) {
        validate_roof_profile_source_entity(entity);
        // These admitted receipts describe historical geometry, which remains
        // exact. Only their codec-qualified provenance identities are omitted.
        if (entity.extensions.contains(roof_rigid_transform_derivations_key))
            entity.extensions[std::string(roof_rigid_transform_derivations_key)] = roof_rigid_transform_opaque_remainder(entity);
        if (entity.extensions.contains(roof_plan_resize_derivations_key))
            entity.extensions[std::string(roof_plan_resize_derivations_key)] = roof_plan_resize_opaque_remainder(entity);
        Ids owned_children;
        if (p.contains("roof_openings")) for (auto& opening : p.at("roof_openings")) {
            owned_children.insert(opening.at("id").get<std::string>());
            opening.erase("id");
        }
        if (entity.extensions.contains("roof_opening_input")) {
            auto& receipt = entity.extensions.at("roof_opening_input");
            // Unchanged future envelopes retain opaque meaning. Source
            // validation admits known version one before these keys are stripped.
            if (receipt.is_object() && receipt.contains("version") && receipt.at("version") == 1) {
                auto entries = Json::array();
                for (const auto& [id, value] : receipt.at("entries").items()) {
                    if (owned_children.contains(id)) entries.push_back(value);
                    else entries.push_back(Json{{id, value}});
                }
                receipt.at("entries") = std::move(entries);
            }
        }
    } else if (entity.type == "roof_join") p.erase("roof_ids");
    else if (entity.type == "model_phases") p.erase("model");
    else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (decoded.supported() && retained_roofs.contains(decoded.dimension->boundary_id))
            p.at("target").erase("entity_id");
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

struct Derivation {
    RoofRemovalPlan plan;
    std::map<std::string, std::string, std::less<>> memberships;
    std::map<std::string, Json, std::less<>> singleton_assignments;
};
Derivation derive(const RoofRemovalEntities& source, const std::vector<std::string>& selected,
    bool preserve_phase_references) {
    Derivation result;
    auto& plan = result.plan;
    plan.selected_roof_ids = selected;
    try {
        (void)occupied_strings(source);
        if (selected.empty() || selected.size() > maximum_identities) reject("requires bounded nonempty explicit roof selection");
        std::sort(plan.selected_roof_ids.begin(), plan.selected_roof_ids.end());
        if (std::adjacent_find(plan.selected_roof_ids.begin(), plan.selected_roof_ids.end()) != plan.selected_roof_ids.end())
            reject("explicit selected roof owners must be unique");
        const auto scope = constraint_phase_scope(source);
        const auto qualified_cohorts = phase_qualified_roof_join_cohort_ids(source);
        for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids)
            if (!result.memberships.emplace(id, registry.registry_id).second) reject("overlapping all-registry membership: " + id);
        // Legacy removal cannot borrow shared-baseline demolition authority.
        if (!preserve_phase_references && roof_demolition_request(source, plan.selected_roof_ids))
            reject("shared-baseline roof selection requires typed phase demolition");
        const Ids seeds(plan.selected_roof_ids.begin(), plan.selected_roof_ids.end());
        Ids removed, affected = seeds, removed_children, retained_roofs, retained_children;
        const auto membership = [&](const std::string& id) {
            const auto found = result.memberships.find(id);
            return found == result.memberships.end() ? std::string{} : found->second;
        };
        const auto role = [&](const std::string& id) {
            const auto registry = membership(id);
            if (registry.empty()) return ModelPhase::existing;
            return ModelPhases::from_json(source.at(registry).properties.at("model")).active_state().at(id);
        };
        const auto mutable_owner = [&](const std::string& id) {
            if (scope.inactive_owner_ids.contains(id)) reject("affected roof owner is inactive: " + id);
            const auto registry = membership(id);
            if (registry.empty()) return;
            const auto model = ModelPhases::from_json(source.at(registry).properties.at("model"));
            if (model.active_alternative() && std::find(model.baseline_ids().begin(), model.baseline_ids().end(), id) != model.baseline_ids().end())
                reject("affected shared-baseline roof owner requires typed replacement: " + id);
        };
        for (const auto& id : seeds) {
            identity(id);
            if (!source.contains(id) || source.at(id).type != "roof") reject("selection must identify actual roof owners: " + id);
            if (scope.inactive_owner_ids.contains(id)) reject("affected roof owner is inactive: " + id);
        }
        std::map<std::string, RoofJoin, std::less<>> joins;
        std::map<std::string, std::string, std::less<>> joined;
        for (const auto& [id, entity] : source) if (entity.type == "roof_join") {
            const auto join = parse_roof_join(entity.properties, id);
            if (scope.inactive_owner_ids.contains(id) && qualified_cohorts.contains(id)) {
                for (const auto& roof : join.roof_ids) if (seeds.contains(roof)) {
                    if (!preserve_phase_references) {
                        diagnostic(plan, roof, "selected roof remains referenced by preserved inactive join " + id +
                            "; requires typed phase-preserving roof/join removal instead of physical deletion");
                        continue;
                    }
                    plan.phase_retention_required = true;
                    if (entity.extensions.contains(std::string(roof_join_phase_ownership_extension_key)) &&
                        !has_phase_qualified_roof_join_ownership(entity))
                        reject("preserved join has an unsupported ownership qualifier that cannot establish retention: " + id);
                    const auto registry = membership(id);
                    if (registry.empty()) reject("preserved qualified join has no unique actual registry: " + id);
                    const auto model = ModelPhases::from_json(source.at(registry).properties.at("model"));
                    if (!model.active_alternative())
                        reject("preserved qualified join retention requires a saved active alternative: " + id);
                    const auto roof_registry = membership(roof);
                    if (!roof_registry.empty() && roof_registry != registry)
                        reject("retained roof belongs to a foreign registry; baseline enrollment would change preserved alternatives: " + roof);
                    if (!roof_registry.empty() &&
                        std::find(model.baseline_ids().begin(), model.baseline_ids().end(), roof) == model.baseline_ids().end())
                        reject("retained proposed roof cannot be promoted to baseline without changing preserved alternatives: " + roof);
                    const auto [previous, inserted] = plan.retained_roof_registry_ids.emplace(roof, registry);
                    if (!inserted && previous->second != registry)
                        reject("preserved inactive join references require different retention registries: " + roof);
                    retained_roofs.insert(roof);
                }
                continue;
            }
            for (const auto& roof : join.roof_ids) {
                if (!source.contains(roof) || source.at(roof).type != "roof") reject("retained join has a dangling/non-roof member: " + roof);
                if (!joined.emplace(roof, id).second) reject("retained roof belongs to multiple joins: " + roof);
            }
            joins.emplace(id, join);
        }
        for (const auto& id : seeds) {
            if (retained_roofs.contains(id)) {
                if (source.at(id).properties.contains("roof_openings"))
                    for (const auto& opening : source.at(id).properties.at("roof_openings")) retained_children.insert(opening.at("id").get<std::string>());
                continue;
            }
            mutable_owner(id);
            removed.insert(id);
            if (source.at(id).properties.contains("roof_openings"))
                for (const auto& opening : source.at(id).properties.at("roof_openings")) removed_children.insert(opening.at("id").get<std::string>());
        }
        Ids admitted_roofs = seeds, admitted_joins;
        for (const auto& [id, join] : joins)
            if (std::any_of(join.roof_ids.begin(), join.roof_ids.end(), [&](const auto& roof) { return seeds.contains(roof); })) {
                admitted_joins.insert(id);
                admitted_roofs.insert(join.roof_ids.begin(), join.roof_ids.end());
            }
        const auto shapes = admit_roofs_and_joins(source, admitted_roofs, admitted_joins);
        for (const auto& [id, join] : joins) {
            if (std::none_of(join.roof_ids.begin(), join.roof_ids.end(), [&](const auto& roof) { return seeds.contains(roof); })) continue;
            mutable_owner(id); affected.insert(id);
            if (source.at(id).extensions.contains(std::string(roof_join_phase_ownership_extension_key)) &&
                !has_phase_qualified_roof_join_ownership(source.at(id)))
                reject("affected join has an unsupported ownership qualifier that must remain opaque: " + id);
            std::vector<std::string> survivors;
            std::vector<TopoDS_Shape> members;
            for (const auto& roof : join.roof_ids) {
                if (scope.inactive_owner_ids.contains(roof) ||
                    (!(preserve_phase_references && qualified_cohorts.contains(id)) &&
                        (membership(roof) != membership(id) || role(roof) != role(id))))
                    reject("affected join spans inactive, foreign or different-phase roof members: " + id);
                if (!seeds.contains(roof)) { survivors.push_back(roof); members.push_back(shapes.at(roof)); }
            }
            auto& components = plan.surviving_join_components[id];
            std::size_t retained_count = 0;
            for (const auto& indices : roof_shape_connected_components(members)) {
                auto& component = components.emplace_back();
                for (const auto index : indices) component.push_back(survivors.at(index));
                if (component.size() >= 2) ++retained_count;
                else if (join.material_assignment) {
                    const auto& roof = component.front(); mutable_owner(roof);
                    auto assignment = Json::object();
                    if (source.at(roof).properties.contains("material_assignment")) {
                        assignment = source.at(roof).properties.at("material_assignment");
                        admit_assignment(source, assignment);
                    }
                    const auto& effective = source.at(id).properties.at("material_assignment");
                    admit_assignment(source, effective);
                    for (const auto& [key, value] : effective.items()) assignment[key] = value;
                    admit_assignment(source, assignment);
                    result.singleton_assignments.emplace(roof, std::move(assignment));
                }
            }
            if (retained_count == 0) removed.insert(id);
            else if (retained_count > 1) plan.additional_identity_counts.emplace(id, retained_count - 1);
        }
        plan.removed_owner_ids.assign(removed.begin(), removed.end());
        // Every alternative outside the actual current membership remains exact.
        for (const auto& id : removed) if (!membership(id).empty()) {
            const auto model = ModelPhases::from_json(source.at(membership(id)).properties.at("model"));
            for (const auto& alternative : model.alternatives()) {
                if (model.active_alternative() == std::optional<std::string>{alternative.id}) continue;
                if (std::find(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), id) != alternative.demolished_ids.end() ||
                    std::find(alternative.proposed_ids.begin(), alternative.proposed_ids.end(), id) != alternative.proposed_ids.end())
                    diagnostic(plan, id, "removal would change a preserved alternative membership");
            }
        }
        Ids erased = removed;
        for (const auto& [id, entity] : source) if (can_recognize_boundary_dimension_entity_type(entity.type)) {
            try {
                const auto decoded = decode_boundary_dimension_entity(entity);
                if (decoded.supported() && removed.contains(decoded.dimension->boundary_id)) erased.insert(id);
            } catch (const std::exception& error) {
                if (touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
            }
        }
        plan.removed_entity_ids.assign(erased.begin(), erased.end());
        for (const auto& id : erased) if (source.at(id).required)
            diagnostic(plan, id, "required project entities cannot be deleted");
        affected.insert(erased.begin(), erased.end()); affected.insert(removed_children.begin(), removed_children.end());
        affected.insert(retained_children.begin(), retained_children.end());
        // Actual child uniqueness matters when a retained overlay is copied.
        Ids children, ambiguous;
        const auto reserve_child = [&](const std::string& id) {
            identity(id);
            if (source.contains(id) || !children.insert(id).second) ambiguous.insert(id);
        };
        const auto join_counts = plan.additional_identity_counts;
        for (const auto& [id, entity] : source) {
            if (entity.type == "roof" && entity.properties.contains("roof_openings"))
                for (const auto& child : entity.properties.at("roof_openings")) reserve_child(child.at("id").get<std::string>());
            if (entity.type == kAnnotationEntityType) {
                try {
                    validate_annotation_entity(entity);
                    for (const auto* key : {"labels", "symbols"})
                        for (const auto& child : entity.properties.at("state").at(key)) reserve_child(child.at("id").get<std::string>());
                } catch (const std::exception& error) {
                    if (touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
                }
            }
            if (entity.type != kSheetViewEntityType) continue;
            try {
                const auto model = decode_sheet_view_entity(entity);
                for (const auto& view : model.views()) for (const auto& overlay : view.overlays) reserve_child(overlay.id);
                for (const auto& view : entity.properties.at("model").at("views")) if (view.contains("overlays"))
                    for (const auto& row : view.at("overlays")) {
                        if (erased.contains(overlay_owner(row))) removed_children.insert(row.at("id").get<std::string>());
                        const auto count = join_counts.find(overlay_owner(row));
                        if (count == join_counts.end()) continue;
                        const auto child = row.at("id").get<std::string>();
                        if (!plan.additional_identity_counts.emplace(child, count->second).second)
                            diagnostic(plan, child, "additional overlay aliases another copied source identity");
                    }
            } catch (const std::exception& error) {
                if (touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
            }
        }
        affected.insert(removed_children.begin(), removed_children.end());
        // Child IDs have no admitted owner-removal semantics in these typed
        // fields. A reference to an erased owner's opening/overlay therefore
        // cannot hide behind the qualification of a view or override field.
        for (const auto& [id, entity] : source) if (!erased.contains(id)) {
            try {
                if (entity.type == kSheetViewEntityType) {
                    const auto model = decode_sheet_view_entity(entity);
                    const bool relevant = touches(entity.properties, affected) || touches(entity.extensions, affected);
                    const auto actual_owner = [&](const std::string& owner) {
                        if (relevant && !owner.empty() && !source.contains(owner))
                            diagnostic(plan, id, "affected view has a dangling source-object reference: " + owner);
                    };
                    for (const auto& view : model.views()) {
                        for (const auto& owner : view.object_ids) {
                            actual_owner(owner);
                            if (removed_children.contains(owner)) diagnostic(plan, id, "view object reference targets a removed child rather than an actual owner");
                        }
                        if (view.presentation.appearance) for (const auto& row : view.presentation.appearance->objects) {
                            actual_owner(row.object_id);
                            if (removed_children.contains(row.object_id)) diagnostic(plan, id, "appearance targets a removed child rather than an actual owner");
                        }
                        for (const auto& row : view.overlays) {
                            actual_owner(row.object_id);
                            if (row.dimension_binding) actual_owner(row.dimension_binding->object_id);
                            if (removed_children.contains(row.object_id) || (row.dimension_binding && removed_children.contains(row.dimension_binding->object_id)))
                                diagnostic(plan, id, "bound overlay targets a removed child rather than an actual owner");
                        }
                    }
                } else if (entity.type == kAnnotationEntityType) {
                    validate_annotation_entity(entity);
                    for (const auto& row : entity.properties.at("state").at("overrides"))
                        if (removed_children.contains(row.at("target_id").get<std::string>()))
                            diagnostic(plan, id, "annotation override targets a removed child without a qualified owner codec");
                }
            } catch (const std::exception& error) {
                if (touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
            }
        }
        for (const auto& [id, count] : plan.additional_identity_counts) {
            (void)count;
            if (ambiguous.contains(id)) diagnostic(plan, id, "copied identity aliases another retained child or entity");
        }
        for (const auto& id : ambiguous) if (affected.contains(id))
            diagnostic(plan, id, "affected identity aliases another retained child or entity");
        for (const auto& [id, entity] : source) {
            if (erased.contains(id)) continue;
            try {
                const auto remainder = opaque_remainder(entity, retained_roofs);
                if (touches(remainder.properties, affected) || touches(remainder.extensions, affected))
                    diagnostic(plan, id, "affected retained reference has no qualified removal codec");
            } catch (const std::exception& error) {
                if (touches(entity.properties, affected) || touches(entity.extensions, affected)) diagnostic(plan, id, error.what());
            }
        }
        auto total = seeds.size();
        for (const auto& [id, count] : plan.additional_identity_counts) {
            (void)id;
            if (count > maximum_identities - total) reject("explicit/fresh union identity budget exceeded");
            total += count;
        }
    } catch (const std::exception& error) { diagnostic(plan, {}, error.what()); }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
    });
    return result;
}

void complete_presentation(RoofRemovalEntities& candidate, const RoofRemovalEntities& source,
    const Ids& removed, const RoofRemovalAdditionalIdentities& copies,
    const RoofRemovalAdditionalIdentities& identities) {
    Ids affected = removed;
    for (const auto& [id, destinations] : copies) { (void)destinations; affected.insert(id); }
    for (const auto& [id, original] : source) {
        if (!candidate.contains(id) || (!touches(original.properties, affected) && !touches(original.extensions, affected))) continue;
        if (original.type == kSheetViewEntityType) {
            (void)decode_sheet_view_entity(original);
            auto& changed = candidate.at(id);
            for (auto& view : changed.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    auto& rows = view.at("object_ids");
                    const bool restricted = !rows.empty() || view.value("restrict_to_objects", false);
                    const auto retained = rows;
                    remove_ids(rows, removed);
                    for (const auto& row : retained) if (copies.contains(row.get<std::string>()))
                        for (const auto& copy : copies.at(row.get<std::string>())) rows.push_back(copy);
                    // Empty means all objects unless the restriction remains
                    // explicit. Older supported wires admit this field too.
                    if (restricted && rows.empty()) view["restrict_to_objects"] = true;
                }
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null()) {
                    auto& rows = presentation.at("appearance").at("objects");
                    auto result = Json::array();
                    auto additions = Json::array();
                    for (const auto& row : rows) {
                        const auto owner = row.at("object_id").get<std::string>();
                        if (removed.contains(owner)) continue;
                        result.push_back(row);
                        if (copies.contains(owner)) for (const auto& copy : copies.at(owner)) {
                            auto extra = row; extra.at("object_id") = copy; additions.push_back(std::move(extra));
                        }
                    }
                    for (const auto& row : additions) result.push_back(row);
                    rows = std::move(result);
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays");
                    auto result = Json::array();
                    auto additions = Json::array();
                    for (const auto& row : rows) {
                        const auto owner = overlay_owner(row);
                        if (removed.contains(owner)) continue;
                        result.push_back(row);
                        if (!copies.contains(owner)) continue;
                        const auto& destinations = copies.at(owner);
                        for (std::size_t index = 0; index < destinations.size(); ++index) {
                            auto extra = row;
                            extra.at("id") = identities.at(row.at("id").get<std::string>()).at(index);
                            if (extra.contains("object_id") && extra.at("object_id") == owner) extra.at("object_id") = destinations[index];
                            if (extra.contains("dimension_binding") && !extra.at("dimension_binding").is_null())
                                extra.at("dimension_binding").at("object_id") = destinations[index];
                            additions.push_back(std::move(extra));
                        }
                    }
                    for (const auto& row : additions) result.push_back(row);
                    rows = std::move(result);
                }
            }
            validate_sheet_view_entity(changed);
        } else if (original.type == kAnnotationEntityType) {
            validate_annotation_entity(original);
            auto& rows = candidate.at(id).properties.at("state").at("overrides");
            auto result = Json::array();
            auto additions = Json::array();
            for (const auto& row : rows) {
                const auto owner = row.at("target_id").get<std::string>();
                if (removed.contains(owner)) continue;
                result.push_back(row);
                if (copies.contains(owner)) for (const auto& copy : copies.at(owner)) {
                    auto extra = row; extra.at("target_id") = copy; additions.push_back(std::move(extra));
                }
            }
            for (const auto& row : additions) result.push_back(row);
            rows = std::move(result);
            validate_annotation_entity(candidate.at(id));
        }
    }
}

void complete_registries(RoofRemovalEntities& candidate, const RoofRemovalEntities& source,
    const Derivation& derived, const Ids& removed, const RoofRemovalAdditionalIdentities& copies) {
    for (const auto& [id, entity] : source) if (entity.type == "model_phases") {
        auto raw = entity.properties.at("model");
        const auto model = ModelPhases::from_json(raw);
        Ids actual_removed;
        for (const auto& owner : removed) if (derived.memberships.contains(owner) && derived.memberships.at(owner) == id) actual_removed.insert(owner);
        bool changed = !actual_removed.empty();
        remove_ids(raw.at("entity_ids"), actual_removed);
        remove_ids(raw.at("baseline_ids"), actual_removed);
        for (auto& alternative : raw.at("alternatives")) if (model.active_alternative() && alternative.at("id") == *model.active_alternative()) {
            remove_ids(alternative.at("demolished_ids"), actual_removed);
            remove_ids(alternative.at("proposed_ids"), actual_removed);
        }
        for (const auto& [roof, registry] : derived.plan.retained_roof_registry_ids) {
            if (registry != id) continue;
            if (!model.active_alternative()) reject("retained roof registry has no saved active alternative");
            if (!derived.memberships.contains(roof)) {
                raw.at("entity_ids").push_back(roof);
                raw.at("baseline_ids").push_back(roof);
            }
            for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == *model.active_alternative())
                alternative.at("demolished_ids").push_back(roof);
            changed = true;
        }
        for (const auto& [owner, destinations] : copies) {
            if (!derived.memberships.contains(owner) || derived.memberships.at(owner) != id) continue;
            const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), owner) != model.baseline_ids().end();
            for (const auto& destination : destinations) {
                raw.at("entity_ids").push_back(destination);
                if (baseline) raw.at("baseline_ids").push_back(destination);
                else {
                    if (!model.active_alternative()) reject("additional proposed join has no actual active alternative");
                    for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == *model.active_alternative())
                        alternative.at("proposed_ids").push_back(destination);
                }
                changed = true;
            }
        }
        if (changed) {
            (void)ModelPhases::from_json(raw);
            candidate.at(id).properties.at("model") = std::move(raw);
        }
    }
}
} // namespace

bool RoofRemovalPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.blocking; });
}

RoofRemovalPlan inspect_roof_removal_plan(const RoofRemovalEntities& source,
    const std::vector<std::string>& selected_roof_ids, bool preserve_phase_references) {
    return derive(source, selected_roof_ids, preserve_phase_references).plan;
}

RoofRemovalResult replay_roof_removal(const RoofRemovalEntities& source,
    const std::vector<std::string>& selected_roof_ids, const RoofRemovalAdditionalIdentities& additional_identities,
    bool preserve_phase_references) {
    try {
        const auto derived = derive(source, selected_roof_ids, preserve_phase_references);
        if (!derived.plan.ready()) {
            for (const auto& item : derived.plan.diagnostics) if (item.blocking) reject(item.entity_id + ": " + item.reason);
        }
        if (additional_identities.size() != derived.plan.additional_identity_counts.size()) reject("requires exact additional join/overlay identity keys");
        const auto occupied = occupied_strings(source);
        Ids fresh;
        std::size_t total = selected_roof_ids.size();
        for (const auto& [old_id, destinations] : additional_identities) {
            const auto expected = derived.plan.additional_identity_counts.find(old_id);
            if (expected == derived.plan.additional_identity_counts.end() || destinations.size() != expected->second)
                reject("additional identity slots differ from actual source components: " + old_id);
            if (destinations.size() > maximum_identities - total) reject("explicit/fresh union identity budget exceeded");
            total += destinations.size();
            for (const auto& id : destinations) {
                identity(id);
                if (occupied.values.contains(id) || !fresh.insert(id).second) reject("fresh identity collision: " + id);
            }
        }
        RoofRemovalResult result{source, {}};
        const Ids removed(derived.plan.removed_entity_ids.begin(), derived.plan.removed_entity_ids.end());
        for (const auto& id : removed) result.entities.erase(id);
        RoofRemovalAdditionalIdentities copies;
        for (const auto& [id, components] : derived.plan.surviving_join_components) {
            std::size_t index = 0;
            for (const auto& component : components) if (component.size() >= 2) {
                auto copy = source.at(id);
                copy.properties.at("roof_ids") = component;
                if (index == 0) result.entities.at(id) = std::move(copy);
                else {
                    copy.id = additional_identities.at(id).at(index - 1);
                    const auto destination = copy.id;
                    if (!result.entities.emplace(destination, std::move(copy)).second) reject("additional join insertion collided");
                    copies[id].push_back(destination);
                }
                ++index;
            }
        }
        for (const auto& [id, assignment] : derived.singleton_assignments)
            result.entities.at(id).properties["material_assignment"] = assignment;
        complete_registries(result.entities, source, derived, removed, copies);
        complete_presentation(result.entities, source, removed, copies, additional_identities);
        if (result.entities.size() > maximum_entities) reject("final entity budget exceeded");
        (void)occupied_strings(result.entities);
        validate_roof_join_ownership(result.entities);
        Ids admitted_roofs, admitted_joins;
        for (const auto& [roof, registry] : derived.plan.retained_roof_registry_ids) {
            if (result.entities.at(roof) != source.at(roof)) reject("retained roof envelope changed: " + roof);
            const auto model = ModelPhases::from_json(result.entities.at(registry).properties.at("model"));
            if (model.active_state().at(roof) != ModelPhase::demolished)
                reject("retained roof is not demolished in its actual saved active alternative: " + roof);
            admitted_roofs.insert(roof);
        }
        for (const auto& [id, components] : derived.plan.surviving_join_components) {
            for (const auto& component : components) admitted_roofs.insert(component.begin(), component.end());
            if (result.entities.contains(id)) admitted_joins.insert(id);
            if (const auto additional = copies.find(id); additional != copies.end())
                admitted_joins.insert(additional->second.begin(), additional->second.end());
        }
        (void)admit_roofs_and_joins(result.entities, admitted_roofs, admitted_joins);
        (void)constraint_phase_scope(result.entities);
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed source-derived replay: ") + error.what()); }
}
} // namespace sketch
