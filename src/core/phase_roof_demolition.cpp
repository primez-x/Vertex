#include "sketch/phase_roof_demolition.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/phase_roof_transform.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/phase_roof_uniform_transform.hpp"
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
constexpr std::size_t maximum_intent_bytes = 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase roof demolition: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}

// Read-only reservation of opaque strings and keys. Never a rewrite codec.
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
Strings occupied_strings(const RoofDemolitionEntities& source) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings strings;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) strings.values.insert(key);
    for (const auto& [id, entity] : source) {
        if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes");
        strings.values.insert(id); strings.values.insert(entity.type);
        strings.read(entity.properties); strings.read(entity.extensions);
    }
    return strings;
}
bool touches(const Json& value, const Ids& ids) {
    Strings strings; strings.read(value);
    return std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return strings.values.contains(id); });
}
std::string overlay_owner(const Json& overlay) {
    if (overlay.contains("object_id") && !overlay.at("object_id").get<std::string>().empty())
        return overlay.at("object_id").get<std::string>();
    if (overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null())
        return overlay.at("dimension_binding").at("object_id").get<std::string>();
    return {};
}

void admit_material_assignment(const RoofDemolitionEntities& source, const Json& assignment);

void admit_roofs_and_joins(const RoofDemolitionEntities& source, const Ids& roofs, const Ids& joins) {
    std::map<std::string, TopoDS_Shape, std::less<>> shapes;
    const auto shape = [&](const std::string& id) -> const TopoDS_Shape& {
        if (!shapes.contains(id)) {
            const auto found = source.find(id);
            if (found == source.end() || found->second.type != "roof") reject("join member is not an actual roof: " + id);
            // Known quantity, opening and historical uniform/rigid/resize receipts
            // bind the actual raw owner before context-derived native geometry.
            validate_roof_uniform_transform_source_entity(found->second);
            if (found->second.properties.contains("material_assignment"))
                admit_material_assignment(source, found->second.properties.at("material_assignment"));
            shapes.emplace(id, make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, found->second))));
        }
        return shapes.at(id);
    };
    for (const auto& id : roofs) (void)shape(id);
    for (const auto& id : joins) {
        const auto join = parse_roof_join(source.at(id).properties, id);
        if (join.material_assignment)
            admit_material_assignment(source, source.at(id).properties.at("material_assignment"));
        std::vector<TopoDS_Shape> members;
        for (const auto& roof : join.roof_ids) members.push_back(shape(roof));
        (void)make_roof_join(join, members);
    }
}

// The Document admits the same version-one catalog/material fields for roofs
// and joins. Roof assignments may retain opaque extra fields; the join codec
// independently admits its closed assignment before this source-derived copy.
void admit_material_assignment(const RoofDemolitionEntities& source, const Json& assignment) {
    if (!assignment.is_object() || !assignment.at("version").is_number_integer() ||
        assignment.at("version") != 1 || !assignment.at("catalog_id").is_string() ||
        !assignment.at("material_id").is_string())
        reject("singleton source material assignment is unsupported");
    const auto catalog_id = assignment.at("catalog_id").get<std::string>();
    const auto material_id = assignment.at("material_id").get<std::string>();
    identity(catalog_id);
    const auto catalog = source.find(catalog_id);
    if (catalog == source.end() || catalog->second.type != "assembly_model")
        reject("singleton material assignment requires an actual source assembly catalog");
    const auto model = AssemblyModel::from_json(catalog->second.properties.at("model"));
    if (std::none_of(model.materials().begin(), model.materials().end(), [&](const auto& material) {
        return material.id == material_id;
    })) reject("singleton material assignment references a missing actual catalog material");
}

struct DemolitionDerivation {
    PhaseRoofReplacementPlan plan;
    // All components retain the original join's authored roof order.
    std::map<std::string, std::vector<std::vector<std::string>>, std::less<>> components;
    std::map<std::string, std::size_t, std::less<>> additional_counts;
    // Historical modes omit a singleton join and transfer its material binding.
    // Only admitted known binding fields change; original roof extras survive.
    std::map<std::string, Json, std::less<>> singleton_material_assignments;
};
DemolitionDerivation derive_demolition(const RoofDemolitionEntities& source,
    const RoofDemolitionRequest& request) {
    if (request.preserve_singleton_material && !request.phase_qualified_joins)
        reject("singleton material preservation requires phase-qualified joins");
    DemolitionDerivation result;
    result.plan = inspect_phase_roof_replacement_plan(source, request.seed_roof_ids,
        request.registry_id, request.alternative_id, request.phase_qualified_joins);
    if (!result.plan.ready()) {
        for (const auto& diagnostic : result.plan.diagnostics) if (diagnostic.blocking)
            reject("unresolved affected dependency " + diagnostic.entity_id + ": " + diagnostic.reason);
    }
    const Ids seeds(request.seed_roof_ids.begin(), request.seed_roof_ids.end());
    const Ids retained(result.plan.retained_join_roof_ids.begin(), result.plan.retained_join_roof_ids.end());
    Ids admitted_roofs = retained, admitted_joins;
    for (const auto& id : result.plan.required_entity_ids) {
        if (source.at(id).type == "roof") admitted_roofs.insert(id);
        else if (source.at(id).type == "roof_join") admitted_joins.insert(id);
    }
    if (request.phase_qualified_joins) validate_roof_join_ownership(source);
    admit_roofs_and_joins(source, admitted_roofs, admitted_joins);
    std::map<std::string, TopoDS_Shape, std::less<>> shapes;
    for (const auto& id : result.plan.required_entity_ids) {
        const auto& entity = source.at(id);
        if (entity.type != "roof_join") continue;
        const auto join = parse_roof_join(entity.properties, id);
        std::vector<std::string> survivors;
        std::vector<TopoDS_Shape> members;
        for (const auto& roof : join.roof_ids) if (!seeds.contains(roof)) {
            const auto& original = source.at(roof);
            if (!shapes.contains(roof)) {
                validate_roof_uniform_transform_source_entity(original);
                shapes.emplace(roof, make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, original))));
            }
            survivors.push_back(roof); members.push_back(shapes.at(roof));
        }
        auto& components = result.components[id];
        std::size_t copied_joins = 0;
        for (const auto& indices : roof_shape_connected_components(members)) {
            auto& component = components.emplace_back();
            for (const auto index : indices) component.push_back(survivors.at(index));
            if (component.size() >= 2 ||
                (join.material_assignment && (request.preserve_singleton_material || join.singleton_material_scope)))
                ++copied_joins;
            else if (join.material_assignment) {
                const auto& roof = source.at(component.front());
                const auto& effective = entity.properties.at("material_assignment");
                admit_material_assignment(source, effective);
                auto assignment = Json::object();
                if (roof.properties.contains("material_assignment")) {
                    assignment = roof.properties.at("material_assignment");
                    admit_material_assignment(source, assignment);
                }
                // Copy the exact source join binding rather than invent or
                // resolve a catalog/material, retaining compatible roof extras.
                for (const auto& [key, value] : effective.items()) assignment[key] = value;
                admit_material_assignment(source, assignment);
                if (retained.contains(component.front())) {
                    // This survivor belongs to its actual source role and is
                    // preserved byte-for-byte. A different effective material
                    // needs a richer representation than this typed family.
                    if (!roof.properties.contains("material_assignment") ||
                        roof.properties.at("material_assignment") != assignment ||
                        roof.properties.at("material_assignment").dump() != assignment.dump())
                        reject("retained singleton source join material has a representation conflict: " + component.front());
                    continue;
                }
                if (!result.singleton_material_assignments.emplace(component.front(), std::move(assignment)).second)
                    reject("singleton has more than one source join material override");
            }
        }
        if (copied_joins > 1) result.additional_counts.emplace(id, copied_joins - 1);
    }
    // The qualified view codec admits the sole owner/binding relationship. Each
    // actual bound overlay needs one extra child ID for each extra join copy.
    const auto join_counts = result.additional_counts;
    Ids split_joins;
    for (const auto& [join, count] : join_counts) { (void)count; split_joins.insert(join); }
    for (const auto& [id, entity] : source) if (entity.type == kSheetViewEntityType) {
        (void)id;
        if (!touches(entity.properties, split_joins)) continue;
        (void)decode_sheet_view_entity(entity);
        for (const auto& view : entity.properties.at("model").at("views"))
            if (view.contains("overlays")) for (const auto& overlay : view.at("overlays")) {
                const auto count = join_counts.find(overlay_owner(overlay));
                if (count != join_counts.end()) {
                    if (!result.additional_counts.emplace(overlay.at("id").get<std::string>(), count->second).second)
                        reject("additional overlay identity aliases another source owner");
                }
            }
    }
    auto total = result.plan.required_entity_ids.size() + result.plan.required_child_ids.size();
    for (const auto& [id, count] : result.additional_counts) {
        (void)id;
        if (count > maximum_identities - total) reject("complete demolition identity budget exceeded");
        total += count;
    }
    return result;
}

// Extend only admitted reference fields on the retained wire. In particular,
// overlay destinations for demolished/skipped owners remain declared but unused.
void complete_presentation(RoofDemolitionEntities& candidate,
    const RoofDemolitionEntities& source,
    const std::map<std::string, std::vector<std::string>, std::less<>>& copies,
    const RoofDemolitionIntent& intent) {
    Ids owners;
    for (const auto& [id, destinations] : copies) { (void)destinations; owners.insert(id); }
    for (const auto& [id, original] : source) {
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            (void)decode_sheet_view_entity(original);
            auto& changed = candidate.at(id);
            for (auto& view : changed.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    auto& object_ids = view.at("object_ids");
                    const auto retained = object_ids;
                    for (const auto& object : retained) if (owners.contains(object.get<std::string>()))
                        for (const auto& destination : copies.at(object.get<std::string>())) object_ids.push_back(destination);
                }
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null()) {
                    auto& rows = presentation.at("appearance").at("objects");
                    const auto retained = rows;
                    for (const auto& row : retained) if (owners.contains(row.at("object_id").get<std::string>()))
                        for (const auto& destination : copies.at(row.at("object_id").get<std::string>())) {
                            auto copy = row; copy.at("object_id") = destination; rows.push_back(std::move(copy));
                        }
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays");
                    const auto retained = rows;
                    for (const auto& row : retained) if (owners.contains(overlay_owner(row))) {
                        // The view codec requires object_id and binding to agree,
                        // so an admitted overlay cannot span copied/omitted owners.
                        const auto& destinations = copies.at(overlay_owner(row));
                        const auto child = row.at("id").get<std::string>();
                        for (std::size_t index = 0; index < destinations.size(); ++index) {
                            auto copy = row;
                            copy.at("id") = index == 0 ? intent.identities.at(child) : intent.additional_identities.at(child).at(index - 1);
                            if (copy.contains("object_id") && copy.at("object_id") == overlay_owner(row))
                                copy.at("object_id") = destinations[index];
                            if (copy.contains("dimension_binding") && !copy.at("dimension_binding").is_null())
                                copy.at("dimension_binding").at("object_id") = destinations[index];
                            rows.push_back(std::move(copy));
                        }
                    }
                }
            }
            validate_sheet_view_entity(changed);
        } else if (original.type == kAnnotationEntityType) {
            validate_annotation_entity(original);
            auto& rows = candidate.at(id).properties.at("state").at("overrides");
            const auto retained = rows;
            for (const auto& row : retained) if (owners.contains(row.at("target_id").get<std::string>()))
                for (const auto& destination : copies.at(row.at("target_id").get<std::string>())) {
                    auto copy = row; copy.at("target_id") = destination; rows.push_back(std::move(copy));
                }
            validate_annotation_entity(candidate.at(id));
        }
    }
}
} // namespace

std::optional<RoofDemolitionRequest> roof_demolition_request(
    const RoofDemolitionEntities& source, const std::vector<std::string>& selected_roof_ids,
    bool phase_qualified_joins, bool preserve_singleton_material) {
    try {
        if (preserve_singleton_material && !phase_qualified_joins)
            reject("singleton material preservation requires phase-qualified joins");
        if (selected_roof_ids.empty() || selected_roof_ids.size() > maximum_identities)
            reject("requires bounded nonempty explicit roof selection");
        (void)occupied_strings(source);
        const auto scope = constraint_phase_scope(source);
        std::map<std::string, std::string, std::less<>> memberships;
        for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, registry.registry_id).second) reject("overlapping all-registry model membership: " + id);
        Ids selected;
        std::optional<RoofDemolitionRequest> request;
        std::size_t ordinary = 0;
        for (const auto& id : selected_roof_ids) {
            identity(id);
            if (!selected.insert(id).second) reject("roof selection must be unique");
            const auto owner = source.find(id);
            if (owner == source.end() || owner->second.type != "roof") reject("selection must identify actual roof owners: " + id);
            if (scope.inactive_owner_ids.contains(id)) reject("roof selection is inactive: " + id);
            const auto membership = memberships.find(id);
            if (membership == memberships.end()) { ++ordinary; continue; }
            const auto model = ModelPhases::from_json(source.at(membership->second).properties.at("model"));
            const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), id) != model.baseline_ids().end();
            if (!baseline || !model.active_alternative()) { ++ordinary; continue; }
            if (request && request->registry_id != membership->second) reject("selection spans shared-baseline registries");
            if (!request) request = RoofDemolitionRequest{membership->second, *model.active_alternative(), {}, phase_qualified_joins, preserve_singleton_material};
            request->seed_roof_ids.push_back(id);
        }
        if (request && ordinary) reject("selection mixes shared-baseline and ordinary/proposed roof owners");
        if (request) std::sort(request->seed_roof_ids.begin(), request->seed_roof_ids.end());
        return request;
    } catch (const Json::exception& error) { reject(std::string("malformed request source: ") + error.what()); }
}

Json encode_roof_demolition_intent(const RoofDemolitionIntent& intent) {
    if (intent.preserve_singleton_material && !intent.phase_qualified_joins)
        reject("singleton material preservation requires phase-qualified joins");
    identity(intent.registry_id); identity(intent.alternative_id);
    if (intent.seed_roof_ids.empty() || intent.seed_roof_ids.size() > maximum_identities ||
        intent.identities.empty() || intent.identities.size() > maximum_identities)
        reject("requires bounded nonempty explicit seeds and mapping");
    Ids seeds, fresh;
    for (const auto& id : intent.seed_roof_ids) {
        identity(id);
        if (!seeds.insert(id).second) reject("roof seeds must be unique");
    }
    for (const auto& [old_id, new_id] : intent.identities) {
        identity(old_id); identity(new_id);
        if (old_id == new_id || !fresh.insert(new_id).second) reject("mapping must be fresh and injective");
    }
    for (const auto& [old_id, new_ids] : intent.additional_identities) {
        identity(old_id);
        if (!intent.identities.contains(old_id) || new_ids.empty())
            reject("additional identities require a mapped source and nonempty slots");
        if (new_ids.size() > maximum_identities - fresh.size()) reject("complete identity budget exceeded");
        for (const auto& id : new_ids) {
            identity(id);
            if (old_id == id || !fresh.insert(id).second) reject("complete identity mapping must be fresh and injective");
        }
    }
    for (const auto& id : seeds) if (!intent.identities.contains(id)) reject("seed has no declared destination identity");
    Json result{{"version", intent.preserve_singleton_material ? 3 : intent.phase_qualified_joins ? 2 : 1}, {"registry_id", intent.registry_id}, {"alternative_id", intent.alternative_id},
        {"seed_roof_ids", intent.seed_roof_ids}, {"identities", intent.identities},
        {"additional_identities", intent.additional_identities}};
    if (intent.phase_qualified_joins) result["phase_qualified_joins"] = true;
    if (intent.preserve_singleton_material) result["preserve_singleton_material"] = true;
    if (result.dump().size() > maximum_intent_bytes) reject("intent byte budget exceeded");
    return result;
}

RoofDemolitionIntent decode_roof_demolition_intent(const Json& value) {
    try {
        const bool singleton = value.is_object() && value.contains("version") &&
            value.at("version").is_number_integer() && value.at("version") == 3;
        const bool qualified = singleton || (value.is_object() && value.contains("version") &&
            value.at("version").is_number_integer() && value.at("version") == 2);
        if (!value.is_object() || value.size() != (singleton ? 8 : qualified ? 7 : 6) || !value.contains("version") ||
            !value.at("version").is_number_integer() || (!qualified && value.at("version") != 1) ||
            (qualified && (!value.contains("phase_qualified_joins") || !value.at("phase_qualified_joins").is_boolean() ||
                !value.at("phase_qualified_joins").get<bool>())) ||
            (singleton && (!value.contains("preserve_singleton_material") || !value.at("preserve_singleton_material").is_boolean() ||
                !value.at("preserve_singleton_material").get<bool>())) ||
            !value.contains("registry_id") || !value.contains("alternative_id") ||
            !value.contains("seed_roof_ids") || !value.at("seed_roof_ids").is_array() ||
            !value.contains("identities") || !value.at("identities").is_object() ||
            !value.contains("additional_identities") || !value.at("additional_identities").is_object())
            reject("intent must contain its exact version-one/two/three fields");
        if (value.at("seed_roof_ids").size() > maximum_identities ||
            value.at("identities").size() > maximum_identities || value.dump().size() > maximum_intent_bytes)
            reject("intent budget exceeded");
        RoofDemolitionIntent result;
        result.phase_qualified_joins = qualified;
        result.preserve_singleton_material = singleton;
        result.registry_id = value.at("registry_id").get<std::string>();
        result.alternative_id = value.at("alternative_id").get<std::string>();
        result.seed_roof_ids = value.at("seed_roof_ids").get<std::vector<std::string>>();
        result.identities = value.at("identities").get<RoofDemolitionIdentityMap>();
        result.additional_identities = value.at("additional_identities").get<decltype(result.additional_identities)>();
        (void)encode_roof_demolition_intent(result);
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed intent: ") + error.what()); }
}

std::map<std::string, std::size_t, std::less<>> roof_demolition_additional_identity_counts(
    const RoofDemolitionEntities& source, const RoofDemolitionRequest& request) {
    try {
        return derive_demolition(source, request).additional_counts;
    } catch (const Json::exception& error) { reject(std::string("malformed count source: ") + error.what()); }
}

RoofDemolitionResult replay_roof_demolition(const RoofDemolitionEntities& source, const RoofDemolitionIntent& intent) {
    try {
        (void)encode_roof_demolition_intent(intent);
        const auto derived = derive_demolition(source, {intent.registry_id, intent.alternative_id,
            intent.seed_roof_ids, intent.phase_qualified_joins, intent.preserve_singleton_material});
        const auto& plan = derived.plan;
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (intent.identities.size() != expected.size()) reject("requires complete exact entity/child mapping");
        const auto occupied = occupied_strings(source);
        Ids fresh;
        for (const auto& [old_id, new_id] : intent.identities) {
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        if (intent.additional_identities.size() != derived.additional_counts.size())
            reject("requires exact actual additional join/overlay identity keys");
        for (const auto& [old_id, new_ids] : intent.additional_identities) {
            const auto count = derived.additional_counts.find(old_id);
            if (count == derived.additional_counts.end() || count->second != new_ids.size())
                reject("additional identity count differs from actual survivor components: " + old_id);
            for (const auto& new_id : new_ids)
                if (occupied.values.contains(new_id) || !fresh.insert(new_id).second)
                    reject("additional fresh identity collision: " + new_id);
        }
        const Ids seeds(intent.seed_roof_ids.begin(), intent.seed_roof_ids.end());
        const Ids retained(plan.retained_join_roof_ids.begin(), plan.retained_join_roof_ids.end());
        // Inspection already admitted the complete actual resolved source cohort.
        // Recheck owner kinds here; final resolved copies are admitted below.
        Ids copied_roofs, copied_joins;
        std::map<std::string, std::vector<std::string>, std::less<>> copies;
        RoofDemolitionResult result{source, intent.identities, {}, {}, plan.required_entity_ids};
        for (const auto& id : plan.required_entity_ids) {
            auto copy = source.at(id);
            if (copy.type == "roof") {
                if (seeds.contains(id)) continue;
                if (const auto assignment = derived.singleton_material_assignments.find(id);
                    assignment != derived.singleton_material_assignments.end())
                    copy.properties["material_assignment"] = assignment->second;
                if (copy.properties.contains("roof_openings")) for (auto& opening : copy.properties.at("roof_openings"))
                    opening.at("id") = intent.identities.at(opening.at("id").get<std::string>());
                if (copy.extensions.contains("roof_opening_input")) {
                    auto& entries = copy.extensions.at("roof_opening_input").at("entries");
                    auto remapped = Json::object();
                    for (const auto& [child, value] : entries.items()) remapped[intent.identities.at(child)] = value;
                    entries = std::move(remapped);
                }
                copied_roofs.insert(intent.identities.at(id));
            } else if (copy.type == "roof_join") {
                const auto source_join = parse_roof_join(copy.properties, id);
                std::size_t index = 0;
                for (const auto& component : derived.components.at(id)) if (component.size() >= 2 ||
                    (source_join.material_assignment && (intent.preserve_singleton_material || source_join.singleton_material_scope))) {
                    auto join_copy = copy;
                    auto members = Json::array();
                    for (const auto& roof : component) {
                        if (intent.identities.contains(roof)) members.push_back(intent.identities.at(roof));
                        else if (intent.phase_qualified_joins && retained.contains(roof)) members.push_back(roof);
                        else reject("survivor is neither a mapped baseline nor an actual retained roof: " + roof);
                    }
                    join_copy.properties.at("roof_ids") = std::move(members);
                    if (component.size() == 1) join_copy.properties.at("version") = 3;
                    if (intent.phase_qualified_joins)
                        join_copy.extensions[std::string(roof_join_phase_ownership_extension_key)] =
                            Json{{"version", 1}, {"registry_id", plan.registry_id}};
                    join_copy.id = index == 0 ? intent.identities.at(id) : intent.additional_identities.at(id).at(index - 1);
                    const auto copy_id = join_copy.id;
                    if (!result.entities.emplace(copy_id, std::move(join_copy)).second) reject("join copy insertion collides");
                    copied_joins.insert(copy_id); copies[id].push_back(copy_id);
                    result.copied_owner_ids.push_back(copy_id);
                    ++index;
                }
                continue;
            } else reject("unsupported cohort owner reached replay");
            copy.id = intent.identities.at(id);
            const auto copy_id = copy.id;
            if (!result.entities.emplace(copy_id, std::move(copy)).second) reject("copy insertion collides");
            copies[id].push_back(copy_id); result.copied_owner_ids.push_back(copy_id);
        }
        const auto model = ModelPhases::from_json(source.at(plan.registry_id).properties.at("model"));
        auto model_ids = model.entity_ids(); auto alternatives = model.alternatives();
        const auto target = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& a) { return a.id == plan.alternative_id; });
        if (target == alternatives.end()) reject("target alternative disappeared");
        for (const auto& id : result.copied_owner_ids) {
            model_ids.push_back(id); target->proposed_ids.push_back(id);
        }
        for (const auto& id : plan.required_entity_ids) target->demolished_ids.push_back(id);
        const auto final_model = ModelPhases::create(model_ids, model.baseline_ids(), alternatives, model.active_alternative());
        auto raw = source.at(plan.registry_id).properties.at("model");
        for (const auto& id : result.copied_owner_ids) raw.at("entity_ids").push_back(id);
        for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == plan.alternative_id) {
            for (const auto& id : result.copied_owner_ids) alternative.at("proposed_ids").push_back(id);
            for (const auto& id : plan.required_entity_ids) alternative.at("demolished_ids").push_back(id);
        }
        if (ModelPhases::from_json(raw).to_json() != final_model.to_json()) reject("retained registry reconstruction differs from typed update");
        result.entities.at(plan.registry_id).properties.at("model") = std::move(raw);
        complete_presentation(result.entities, source, copies, intent);
        // The complete candidate registry proves exclusion for shared retained
        // members. Native/context/material admission uses this same owner map.
        if (intent.phase_qualified_joins) validate_roof_join_ownership(result.entities);
        copied_roofs.insert(retained.begin(), retained.end());
        admit_roofs_and_joins(result.entities, copied_roofs, copied_joins);
        (void)constraint_phase_scope(result.entities);
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed typed replay: ") + error.what()); }
}
} // namespace sketch
