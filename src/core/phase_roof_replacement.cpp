#include "sketch/phase_roof_replacement.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_roof_demolition.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"
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
constexpr std::size_t maximum_replacements = 4096;
constexpr std::size_t maximum_authoring_bytes = 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase roof replacement: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
// Conservative read-only inspection. Unknown values and keys reserve identities;
// this is deliberately never used as a rewrite codec.
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
Strings occupied_strings(const PhaseRoofReplacementEntities& source) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings strings;
    for (const auto& [id, entity] : source) {
        if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes");
        strings.values.insert(id); strings.read(entity.properties); strings.read(entity.extensions);
    }
    return strings;
}
void diagnostic(PhaseRoofReplacementPlan& plan, const std::string& id, const std::string& reason) {
    const PhaseRoofReplacementDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}
std::string remap(const std::string& id, const PhaseRoofReplacementIdentityMap& identities) {
    const auto found = identities.find(id);
    return found == identities.end() ? id : found->second;
}
void remap_field(Json& value, const char* key, const PhaseRoofReplacementIdentityMap& identities) {
    if (value.contains(key)) value.at(key) = remap(value.at(key).get<std::string>(), identities);
}
bool affected_overlay(const Json& overlay, const Ids& owners) {
    if (overlay.contains("object_id") && owners.contains(overlay.at("object_id").get<std::string>())) return true;
    return overlay.contains("dimension_binding") && !overlay.at("dimension_binding").is_null() &&
        owners.contains(overlay.at("dimension_binding").at("object_id").get<std::string>());
}

// Strip only fields whose exact identity semantics are admitted by the codecs.
// Everything left is retained opaque and must not mention affected identities.
Entity opaque_remainder(Entity entity, bool include_hosted_instances = false) {
    auto& p = entity.properties;
    if (entity.type == "roof") {
        const bool known_schema = p.contains("version") && p.at("version").is_number_integer() &&
            (p.at("version") == 1 || p.at("version") == 2);
        const bool known_form = p.contains("form") && (p.at("form") == "sloped_roof_panel" ||
            p.at("form") == "gable_roof" || p.at("form") == "hip_roof");
        if (!known_schema || !known_form) return entity;
        if (entity.extensions.contains(std::string(roof_uniform_transform_derivations_key)))
            entity.extensions[std::string(roof_uniform_transform_derivations_key)] =
                roof_uniform_transform_opaque_remainder(entity);
        if (entity.extensions.contains(roof_rigid_transform_derivations_key)) {
            // Closed historical frames identify their original source; their
            // owner/cut IDs are provenance, not current replacement bindings.
            // Unknown receipt siblings retain their opaque reference checks.
            entity.extensions[std::string(roof_rigid_transform_derivations_key)] =
                roof_rigid_transform_opaque_remainder(entity);
        }
        if (entity.extensions.contains(roof_plan_resize_derivations_key))
            entity.extensions[std::string(roof_plan_resize_derivations_key)] = roof_plan_resize_opaque_remainder(entity);
        if (p.contains("roof_openings")) for (auto& opening : p.at("roof_openings")) opening.erase("id");
        if (entity.extensions.contains("roof_opening_input")) {
            auto& receipt = entity.extensions.at("roof_opening_input");
            if (!receipt.is_object() || !receipt.contains("version") || receipt.at("version") != 1 ||
                !receipt.contains("entries") || !receipt.at("entries").is_object())
                reject("unsupported roof opening input envelope");
            // Keep each opaque receipt value, omitting only its admitted child key.
            auto entries = Json::array();
            for (const auto& [id, value] : receipt.at("entries").items()) { (void)id; entries.push_back(value); }
            receipt.at("entries") = std::move(entries);
        }
    } else if (include_hosted_instances && entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model"));
        // Typed actual hosts only. Opaque property/extension siblings continue
        // to reserve references; this scratch value is never written back.
        for (auto& row : p.at("model").at("instances"))
            if (row.contains("placement")) row.at("placement").erase("host_entity_id");
    } else if (entity.type == "roof_join") p.erase("roof_ids");
    else if (entity.type == "model_phases") p.erase("model");
    else if (entity.type == kSheetViewEntityType) {
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

void admit_roofs_and_joins(const PhaseRoofReplacementEntities& source, const Ids& roofs, const Ids& joins) {
    std::map<std::string, TopoDS_Shape, std::less<>> shapes;
    const auto shape = [&](const std::string& id) -> const TopoDS_Shape& {
        if (!shapes.contains(id)) {
            const auto found = source.find(id);
            if (found == source.end() || found->second.type != "roof") reject("join source is not an actual roof: " + id);
            validate_roof_uniform_transform_source_entity(found->second);
            shapes.emplace(id, make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, found->second))));
        }
        return shapes.at(id);
    };
    for (const auto& id : roofs) (void)shape(id);
    for (const auto& id : joins) {
        const auto join = parse_roof_join(source.at(id).properties, id);
        std::vector<TopoDS_Shape> members;
        for (const auto& roof : join.roof_ids) members.push_back(shape(roof));
        (void)make_roof_join(join, members);
    }
}

// Additive presentation updates on the retained wire. Never serialize the typed
// value back over historical ordering, formatting or an unrelated raw record.
void complete_presentation(PhaseRoofReplacementEntities& candidate,
    const PhaseRoofReplacementEntities& source, const Ids& owners,
    const PhaseRoofReplacementIdentityMap& identities) {
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
                        object_ids.push_back(identities.at(object.get<std::string>()));
                }
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null()) {
                    auto& rows = presentation.at("appearance").at("objects");
                    const auto retained = rows;
                    for (auto row : retained) if (owners.contains(row.at("object_id").get<std::string>())) {
                        remap_field(row, "object_id", identities); rows.push_back(std::move(row));
                    }
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays");
                    const auto retained = rows;
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
            auto& rows = candidate.at(id).properties.at("state").at("overrides");
            const auto retained = rows;
            for (auto row : retained) if (owners.contains(row.at("target_id").get<std::string>())) {
                remap_field(row, "target_id", identities); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}

bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() &&
        a.extensions.dump() == b.extensions.dump();
}

PhaseRoofGeometryEditPartition partition_geometry(
    const PhaseRoofReplacementEntities& source, const PhaseRoofReplacementEntities& physical,
    const std::vector<RoofEditIntent>& edits) {
    const auto scope = constraint_phase_scope(source);
    PhaseRoofGeometryEditPartition result;
    for (const auto& edit : edits) {
        const auto& id = edit.roof_id;
        if (exact(source.at(id), physical.at(id))) continue;
        if (scope.inactive_owner_ids.contains(id)) reject("geometry target is inactive: " + id);
        const PhysicalWallPhaseState* membership = nullptr;
        for (const auto& registry : scope.registries)
            if (std::find(registry.registered_entity_ids.begin(), registry.registered_entity_ids.end(), id) != registry.registered_entity_ids.end()) {
                if (membership) reject("geometry target has overlapping registry membership");
                membership = &registry;
            }
        if (!membership) { result.ordinary_roof_edits.push_back(edit); continue; }
        const auto model = ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), id) != model.baseline_ids().end();
        if (!baseline || !model.active_alternative()) {
            // Without a saved active alternative a baseline has no replacement
            // destination, so it retains ordinary same-identity edit semantics.
            result.ordinary_roof_edits.push_back(edit);
            continue;
        }
        if (result.replacement && result.replacement->registry_id != membership->registry_id)
            reject("geometry edits span different shared-baseline registries");
        if (!result.replacement)
            result.replacement = PhaseRoofProfileReplacementRequest{membership->registry_id, *model.active_alternative(), {}};
        result.replacement->seed_roof_ids.push_back(id);
        result.baseline_roof_edits.push_back(edit);
    }
    if (result.replacement) std::sort(result.replacement->seed_roof_ids.begin(), result.replacement->seed_roof_ids.end());
    return result;
}

bool same_edits(const std::vector<RoofEditIntent>& a, const std::vector<RoofEditIntent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto left = encode_roof_edit_intent(a[i]);
        const auto right = encode_roof_edit_intent(b[i]);
        if (left != right || left.dump() != right.dump()) return false;
    }
    return true;
}
} // namespace

bool PhaseRoofReplacementPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) { return d.blocking; });
}

PhaseRoofReplacementPlan inspect_phase_roof_replacement_plan(
    const PhaseRoofReplacementEntities& source, const std::vector<std::string>& seeds,
    const std::string& registry_id, const std::string& alternative_id, bool phase_qualified_joins,
    bool include_hosted_instances) {
    try {
        identity(registry_id); identity(alternative_id); (void)occupied_strings(source);
        if (!source.contains(registry_id) || source.at(registry_id).type != "model_phases") reject("registry must be an actual model_phases entity");
        const auto model = ModelPhases::from_json(source.at(registry_id).properties.at("model"));
        if (model.active_alternative() != std::optional<std::string>{alternative_id}) reject("alternative must be the actual saved active selection");
        const auto scope = constraint_phase_scope(source);
        if (phase_qualified_joins) validate_roof_join_ownership(source);
        std::map<std::string, std::string, std::less<>> memberships;
        for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, registry.registry_id).second) reject("overlapping all-registry model membership: " + id);
        const Ids baseline(model.baseline_ids().begin(), model.baseline_ids().end());
        PhaseRoofReplacementPlan plan;
        plan.registry_id = registry_id; plan.alternative_id = alternative_id; plan.seed_roof_ids = seeds;
        plan.phase_qualified_joins = phase_qualified_joins;
        plan.include_hosted_instances = include_hosted_instances;
        if (include_hosted_instances && !phase_qualified_joins)
            reject("hosted replacement requires phase-qualified combined ownership");
        if (seeds.empty() || seeds.size() > maximum_replacements) reject("requires bounded nonempty explicit roof seeds");
        std::sort(plan.seed_roof_ids.begin(), plan.seed_roof_ids.end());
        if (std::adjacent_find(plan.seed_roof_ids.begin(), plan.seed_roof_ids.end()) != plan.seed_roof_ids.end()) reject("roof seeds must be unique");
        Ids roofs(plan.seed_roof_ids.begin(), plan.seed_roof_ids.end()), joins, children, retained_roofs;
        for (const auto& id : roofs) {
            identity(id);
            if (!source.contains(id) || source.at(id).type != "roof" || !baseline.contains(id) || scope.inactive_owner_ids.contains(id))
                reject("seed must be an active actual baseline roof in the selected registry: " + id);
        }
        std::map<std::string, RoofJoin, std::less<>> retained_joins;
        std::map<std::string, std::string, std::less<>> joined;
        // Legacy replacement retains its global uniqueness contract. Qualified
        // source ownership was proved across every variant above; its cohort
        // graph contains only actual active joins and active roof members.
        for (const auto& [id, entity] : source) if (entity.type == "roof_join") {
            const auto join = parse_roof_join(entity.properties, id);
            const bool active = !scope.inactive_owner_ids.contains(id);
            for (const auto& roof : join.roof_ids) {
                if (!source.contains(roof) || source.at(roof).type != "roof") reject("retained join references a missing/non-roof owner: " + roof);
                if (phase_qualified_joins && !active) continue;
                if (phase_qualified_joins && scope.inactive_owner_ids.contains(roof))
                    reject("active join references an inactive roof: " + roof);
                if (!joined.emplace(roof, id).second) reject("a retained roof occurs in more than one roof join: " + roof);
            }
            if (phase_qualified_joins && !active) continue;
            retained_joins.emplace(id, join);
        }
        bool expanded = true;
        while (expanded) {
            expanded = false;
            for (const auto& [id, join] : retained_joins) {
                if (scope.inactive_owner_ids.contains(id) || joins.contains(id) ||
                    std::none_of(join.roof_ids.begin(), join.roof_ids.end(), [&](const auto& roof) {
                        return roofs.contains(roof) || retained_roofs.contains(roof);
                    })) continue;
                joins.insert(id);
                if (phase_qualified_joins && source.at(id).extensions.contains(std::string(roof_join_phase_ownership_extension_key)) &&
                    !has_phase_qualified_roof_join_ownership(source.at(id)))
                    diagnostic(plan, id, "copied join has an unsupported ownership qualifier that must remain opaque");
                for (const auto& roof : join.roof_ids) {
                    if (!phase_qualified_joins || baseline.contains(roof)) {
                        expanded = roofs.insert(roof).second || expanded;
                        continue;
                    }
                    // A retained member keeps its actual role. Another active
                    // shared baseline needs a different registry transaction.
                    const auto membership = memberships.find(roof);
                    if (membership != memberships.end() && membership->second != registry_id) {
                        const auto foreign = ModelPhases::from_json(source.at(membership->second).properties.at("model"));
                        if (foreign.active_alternative() && std::find(foreign.baseline_ids().begin(),
                            foreign.baseline_ids().end(), roof) != foreign.baseline_ids().end())
                            diagnostic(plan, roof, "cross-registry active baseline join member requires an unsupported replacement scope");
                    }
                    expanded = retained_roofs.insert(roof).second || expanded;
                }
            }
            if (roofs.size() + joins.size() + retained_roofs.size() > maximum_replacements) reject("joined roof replacement budget exceeded");
        }
        Ids owners = roofs; owners.insert(joins.begin(), joins.end());
        for (const auto& id : owners) {
            const auto membership = memberships.find(id);
            if (membership == memberships.end() || membership->second != registry_id || !baseline.contains(id) || scope.inactive_owner_ids.contains(id))
                diagnostic(plan, id, "copied owner must be an active actual baseline member of the target registry");
        }
        Ids required_entities = owners, presentation_owners = owners;
        if (include_hosted_instances) {
            const auto hosted = inspect_roof_clone_plan(source,
                std::vector<std::string>(roofs.begin(), roofs.end()), true);
            for (const auto& item : hosted.diagnostics) diagnostic(plan, item.entity_id, item.reason);
            for (const auto& id : hosted.required_entity_ids)
                if (source.at(id).type == "assembly_model") required_entities.insert(id);
            plan.required_hosted_instance_ids = hosted.required_hosted_instance_ids;
            const auto aliases = embedded_assembly_presentation_ids(source);
            for (const auto& key : plan.required_hosted_instance_ids) presentation_owners.insert(aliases.at(key));
        }
        // Global child ownership includes unchanged designs and all view overlays.
        std::map<std::string, std::string, std::less<>> child_owners;
        Ids ambiguous_children;
        const auto reserve_child = [&](const std::string& child, const std::string& owner, bool required) {
            identity(child);
            if (source.contains(child) || !child_owners.emplace(child, owner).second) ambiguous_children.insert(child);
            if (required) children.insert(child);
        };
        for (const auto& [id, entity] : source) {
            if (entity.type == "roof") {
                Ids opening_ids;
                // Unaffected future roof records remain opaque. Only a known
                // schema declares this child roster; its scalar geometry is
                // admitted below when the roof belongs to this replacement.
                const auto& p = entity.properties;
                const bool known_schema = p.contains("version") && p.at("version").is_number_integer() &&
                    (p.at("version") == 1 || p.at("version") == 2);
                if (known_schema && p.contains("roof_openings")) {
                    const auto& roster = p.at("roof_openings");
                    if (!roster.is_array() || roster.size() > 256) reject("retained roof opening roster is unsupported");
                    for (const auto& opening : roster) {
                        const auto child = opening.at("id").get<std::string>();
                        reserve_child(child, id, roofs.contains(id)); opening_ids.insert(child);
                    }
                }
                if (roofs.contains(id) && entity.extensions.contains("roof_opening_input")) {
                    const auto& receipt = entity.extensions.at("roof_opening_input");
                    if (!receipt.is_object() || !receipt.contains("version") || receipt.at("version") != 1 ||
                        !receipt.contains("entries") || !receipt.at("entries").is_object())
                        diagnostic(plan, id, "roof opening input has an unsupported envelope");
                    else for (const auto& [child, value] : receipt.at("entries").items()) {
                        (void)value;
                        if (!opening_ids.contains(child)) diagnostic(plan, id, "roof opening input key has no actual owned opening: " + child);
                    }
                }
            } else if (entity.type == kSheetViewEntityType) {
                try {
                    const auto views = decode_sheet_view_entity(entity);
                    for (const auto& view : views.views()) for (const auto& overlay : view.overlays) {
                        const bool required = presentation_owners.contains(overlay.object_id) ||
                            (overlay.dimension_binding && presentation_owners.contains(overlay.dimension_binding->object_id));
                        reserve_child(overlay.id, id + ":" + view.id, required);
                    }
                } catch (const std::exception& error) {
                    if (touches(entity.properties, presentation_owners)) diagnostic(plan, id, "affected sheet/view record is unsupported: " + std::string(error.what()));
                }
            }
        }
        for (const auto& child : children) if (ambiguous_children.contains(child))
            diagnostic(plan, child, "copied child identity aliases another retained owner or actual entity");
        if (required_entities.size() + children.size() + plan.required_hosted_instance_ids.size() > maximum_replacements)
            reject("replacement entity/child/hosted budget exceeded");
        Ids affected = presentation_owners; affected.insert(children.begin(), children.end());
        Ids catalog_affected = owners; catalog_affected.insert(children.begin(), children.end());
        for (const auto& [id, entity] : source) {
            // Inactive model entities remain exact. Their typed references retain
            // the original owners; opaque active dependents cannot be inferred.
            if (scope.inactive_owner_ids.contains(id) && !owners.contains(id)) continue;
            try {
                const auto remainder = opaque_remainder(entity, include_hosted_instances);
                // Clone discovery separately qualifies local instance/type/
                // material namespaces when checking hosted presentation aliases.
                // A plain alias can equal a legal catalog-local definition ID.
                const auto& scanned = include_hosted_instances && entity.type == "assembly_model" ? catalog_affected : affected;
                if (touches(remainder.properties, scanned) || touches(remainder.extensions, scanned))
                    diagnostic(plan, id, "affected reference has no qualified replacement codec; original remains preserved");
            } catch (const std::exception& error) {
                if (owners.contains(id) || touches(entity.properties, affected) || touches(entity.extensions, affected))
                    diagnostic(plan, id, "affected typed record is unsupported: " + std::string(error.what()));
            }
        }
        try { admit_roofs_and_joins(source, roofs, joins); }
        catch (const std::exception& error) { diagnostic(plan, registry_id, "source roof/join admission failed: " + std::string(error.what())); }
        plan.required_entity_ids.assign(required_entities.begin(), required_entities.end());
        plan.required_child_ids.assign(children.begin(), children.end());
        plan.retained_join_roof_ids.assign(retained_roofs.begin(), retained_roofs.end());
        std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
            return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
        });
        return plan;
    } catch (const Json::exception& error) { reject(std::string("malformed typed source: ") + error.what()); }
}

PhaseRoofReplacementResult replay_phase_roof_replacement(
    const PhaseRoofReplacementEntities& source, const PhaseRoofReplacementPlan& plan,
    const PhaseRoofReplacementIdentityMap& identities, const std::vector<RoofProfileEditIntent>& profiles,
    const std::vector<RoofOpeningEditIntent>& opening_edits, const std::vector<RoofEditIntent>& roof_edits,
    const std::vector<RoofEditIntent>& ordinary_roof_edits, bool phase_qualified_joins,
    const PhaseRoofReplacementHostedInstanceIdentityMap& hosted_instance_identities,
    bool include_hosted_instances) {
    try {
        if (!include_hosted_instances && !hosted_instance_identities.empty())
            reject("historical replay cannot declare hosted instance identities");
        const auto derived = inspect_phase_roof_replacement_plan(source, plan.seed_roof_ids, plan.registry_id,
            plan.alternative_id, phase_qualified_joins, include_hosted_instances);
        if (derived != plan) reject("supplied plan differs from actual source discovery");
        if (!derived.ready()) reject("replacement has unresolved affected dependencies");
        if (profiles.empty() && opening_edits.empty() && roof_edits.empty()) reject("replacement requires explicit roof edits");
        if ((!profiles.empty() && !opening_edits.empty()) ||
            (!roof_edits.empty() && (!profiles.empty() || !opening_edits.empty())))
            reject("replacement dialects cannot be mixed");
        const bool combined = !roof_edits.empty();
        if (phase_qualified_joins && !combined) reject("qualified joins require the exclusive combined replacement family");
        if (!ordinary_roof_edits.empty() && (!combined || !profiles.empty() || !opening_edits.empty()))
            reject("ordinary roof edits require the exclusive combined replacement family");
        const bool strict_children = combined || !opening_edits.empty();
        if (combined) {
            PhaseRoofReplacementAuthoring authoring;
            authoring.registry_id = plan.registry_id; authoring.alternative_id = plan.alternative_id;
            authoring.seed_roof_ids = plan.seed_roof_ids; authoring.identities = identities;
            authoring.roof_edits = roof_edits;
            authoring.ordinary_roof_edits = ordinary_roof_edits;
            authoring.phase_qualified_joins = phase_qualified_joins;
            authoring.include_hosted_instances = include_hosted_instances;
            authoring.hosted_instance_identities = hosted_instance_identities;
            (void)encode_phase_roof_replacement_authoring(authoring);
        }
        const Ids seeds(plan.seed_roof_ids.begin(), plan.seed_roof_ids.end());
        Ids edit_targets;
        for (const auto& profile : profiles)
            if (!seeds.contains(profile.roof_id) || !edit_targets.insert(profile.roof_id).second)
                reject("profile edits require unique explicit seed roofs");
        for (const auto& edit : opening_edits)
            if (!seeds.contains(edit.roof_id) || !edit_targets.insert(edit.roof_id).second)
                reject("opening edits require unique explicit seed roofs");
        for (const auto& edit : roof_edits)
            if (!seeds.contains(edit.roof_id) || !edit_targets.insert(edit.roof_id).second)
                reject("combined roof edits require unique explicit seed roofs");
        if (edit_targets!=seeds) reject("roof seeds must exactly match the authored edit targets");
        auto all_roof_edits = roof_edits;
        all_roof_edits.insert(all_roof_edits.end(), ordinary_roof_edits.begin(), ordinary_roof_edits.end());
        const auto physical = combined ? replay_roof_edit_entities(source, all_roof_edits)
            : opening_edits.empty() ? replay_roof_profile_entities(source, profiles)
            : replay_roof_opening_entities(source, opening_edits);
        Ids ordinary_targets, ordinary_joins;
        if (phase_qualified_joins || !ordinary_roof_edits.empty()) {
            const auto partition = partition_geometry(source, physical, all_roof_edits);
            if (!partition.replacement || partition.replacement->registry_id != plan.registry_id ||
                partition.replacement->alternative_id != plan.alternative_id ||
                Ids(partition.replacement->seed_roof_ids.begin(), partition.replacement->seed_roof_ids.end()) != seeds ||
                !same_edits(partition.baseline_roof_edits, roof_edits) ||
                !same_edits(partition.ordinary_roof_edits, ordinary_roof_edits))
                reject("mixed roof roles differ from actual changed saved membership");
            for (const auto& edit : ordinary_roof_edits) ordinary_targets.insert(edit.roof_id);
            const auto scope = constraint_phase_scope(source);
            for (const auto& [id, entity] : source) if (entity.type == "roof_join") {
                const auto join = parse_roof_join(entity.properties, id);
                if (!scope.inactive_owner_ids.contains(id) && std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
                    [&](const auto& member) { return ordinary_targets.contains(member); })) ordinary_joins.insert(id);
            }
            if (plan.required_entity_ids.size() > maximum_entities - source.size()) reject("final entity budget exceeded");
        }
        if (!strict_children && physical == source) {
            if (!identities.empty()) reject("source-equivalent roof profiles must not allocate identities");
            return {source, {}, {}};
        }
        for (const auto& id:edit_targets)
            if (source.at(id)==physical.at(id) && source.at(id).properties.dump()==physical.at(id).properties.dump() &&
                source.at(id).extensions.dump()==physical.at(id).extensions.dump())
                reject("An unchanged roof cannot acquire replacement authority as an extra seed");
        Ids expected(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        expected.insert(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (identities.size() != expected.size()) reject("requires complete exact entity/child mapping");
        auto occupied = occupied_strings(source);
        if (strict_children) {
            // Version one retains its historical reservation policy. The new
            // opening and combined dialects reserve the complete source envelope.
            for (const auto* key : {"id", "type", "properties", "required", "extensions"}) occupied.values.insert(key);
            for (const auto& [id, entity] : source) { (void)id; occupied.values.insert(entity.type); }
        }
        Ids fresh;
        for (const auto& [old_id, new_id] : identities) {
            identity(old_id); identity(new_id);
            if (!expected.contains(old_id)) reject("mapping contains an unrequested source identity: " + old_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second) reject("fresh identity collision: " + new_id);
        }
        const std::set<PhaseRoofReplacementHostedInstanceKey> expected_instances(
            plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
        if (hosted_instance_identities.size() != expected_instances.size())
            reject("requires exact qualified hosted instance mapping");
        for (const auto& [key, new_id] : hosted_instance_identities) {
            if (!expected_instances.contains(key)) reject("unrequested qualified hosted instance mapping");
            identity(new_id);
            if (occupied.values.contains(new_id) || !fresh.insert(new_id).second)
                reject("fresh hosted identity collision: " + new_id);
        }
        // These identities were authored explicitly against the complete actual
        // source. They are not mapping keys and are retained verbatim in copies.
        const auto authored_openings = combined ? roof_edit_opening_intents(all_roof_edits) : opening_edits;
        for (const auto& id : new_roof_opening_identity_ids(source, authored_openings)) {
            if (expected.contains(id) || identities.contains(id) || !fresh.insert(id).second)
                reject("authored opening identity collides with replacement mapping: " + id);
        }
        if (strict_children && fresh.size() > maximum_replacements)
            reject("replacement fresh identity budget exceeded");
        Ids roofs, joins;
        for (const auto& id : plan.required_entity_ids) {
            if (source.at(id).type == "roof") roofs.insert(id);
            else if (source.at(id).type == "roof_join") joins.insert(id);
        }
        admit_roofs_and_joins(physical, roofs, joins);
        PhaseRoofReplacementResult result{source, identities, {}, hosted_instance_identities};
        // Only admitted ordinary roof owners change in place. Baseline roofs and
        // original joins retain their exact envelopes; native join bodies are
        // independently derived from the final typed member geometry below.
        for (const auto& id : ordinary_targets) result.entities.at(id) = physical.at(id);
        if (include_hosted_instances && !ordinary_targets.empty()) {
            // Physical replay also moves baseline-hosted rows. Only admitted
            // ordinary hosts may update original catalogs in this transaction.
            for (const auto& [id, original] : source) {
                if (original.type != "assembly_model" || exact(original, physical.at(id))) continue;
                auto changed = physical.at(id);
                auto& rows = changed.properties.at("model").at("instances");
                const auto& retained = original.properties.at("model").at("instances");
                if (rows.size() != retained.size()) reject("hosted movement changed actual instance inventory");
                bool changed_ordinary = false;
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    if (rows.at(i).at("id") != retained.at(i).at("id")) reject("hosted movement reordered actual instance identities");
                    const auto& row = retained.at(i);
                    const bool ordinary = row.contains("placement") &&
                        ordinary_targets.contains(row.at("placement").at("host_entity_id").get<std::string>());
                    if (!ordinary) rows.at(i) = row;
                    else if (rows.at(i).dump() != row.dump()) changed_ordinary = true;
                }
                if (changed_ordinary) {
                    (void)AssemblyModel::from_json(changed.properties.at("model"));
                    result.entities.at(id) = std::move(changed);
                }
            }
        }
        const auto remap_child = [&](const std::string& child) {
            return strict_children ? remap(child, identities) : identities.at(child);
        };
        for (const auto& id : plan.required_entity_ids) {
            auto copy = physical.at(id);
            copy.id = identities.at(id);
            if (copy.type == "roof") {
                if (copy.properties.contains("roof_openings")) for (auto& opening : copy.properties.at("roof_openings"))
                    opening.at("id") = remap_child(opening.at("id").get<std::string>());
                if (copy.extensions.contains("roof_opening_input")) {
                    auto& entries = copy.extensions.at("roof_opening_input").at("entries");
                    auto remapped = Json::object();
                    for (const auto& [child, value] : entries.items()) remapped[remap_child(child)] = value;
                    entries = std::move(remapped);
                }
            } else if (copy.type == "roof_join") {
                const Ids retained(plan.retained_join_roof_ids.begin(), plan.retained_join_roof_ids.end());
                for (auto& roof : copy.properties.at("roof_ids")) {
                    const auto member = roof.get<std::string>();
                    if (identities.contains(member)) roof = identities.at(member);
                    else if (!phase_qualified_joins || !retained.contains(member))
                        reject("copied join member is neither a mapped baseline nor an actual retained member: " + member);
                }
                if (phase_qualified_joins)
                    copy.extensions[std::string(roof_join_phase_ownership_extension_key)] =
                        Json{{"version", 1}, {"registry_id", plan.registry_id}};
                (void)parse_roof_join(copy.properties, copy.id);
            } else if (include_hosted_instances && copy.type == "assembly_model") {
                // Actual physical replay supplies any coordinated placement
                // delta. Keep complete definitions and raw selected rows.
                const auto retained = copy.properties.at("model").at("instances");
                auto& rows = copy.properties.at("model").at("instances");
                rows = Json::array();
                for (auto row : retained) {
                    const PhaseRoofReplacementHostedInstanceKey key{id, row.at("id").get<std::string>()};
                    if (!expected_instances.contains(key)) continue;
                    if (!row.contains("placement") || !roofs.contains(row.at("placement").at("host_entity_id").get<std::string>()))
                        reject("proposed hosted instance lost its actual copied roof host");
                    row.at("id") = hosted_instance_identities.at(key);
                    remap_field(row.at("placement"), "host_entity_id", identities);
                    rows.push_back(std::move(row));
                }
                (void)AssemblyModel::from_json(copy.properties.at("model"));
            } else reject("unsupported copy owner reached replay");
            const auto copy_id = copy.id;
            if (!result.entities.emplace(copy_id, std::move(copy)).second) reject("copy insertion collides");
        }
        const auto model = ModelPhases::from_json(source.at(plan.registry_id).properties.at("model"));
        auto model_ids = model.entity_ids(); auto alternatives = model.alternatives();
        const auto target = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& a) { return a.id == plan.alternative_id; });
        if (target == alternatives.end()) reject("target alternative disappeared");
        for (const auto& id : plan.required_entity_ids) {
            model_ids.push_back(identities.at(id)); target->proposed_ids.push_back(identities.at(id));
            if (source.at(id).type != "assembly_model") target->demolished_ids.push_back(id);
        }
        const auto final_model = ModelPhases::create(model_ids, model.baseline_ids(), alternatives, model.active_alternative());
        auto raw = source.at(plan.registry_id).properties.at("model");
        for (const auto& id : plan.required_entity_ids) raw.at("entity_ids").push_back(identities.at(id));
        for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == plan.alternative_id)
            for (const auto& id : plan.required_entity_ids) {
                alternative.at("proposed_ids").push_back(identities.at(id));
                if (source.at(id).type != "assembly_model") alternative.at("demolished_ids").push_back(id);
            }
        if (ModelPhases::from_json(raw).to_json() != final_model.to_json()) reject("retained registry reconstruction differs from typed update");
        result.entities.at(plan.registry_id).properties.at("model") = std::move(raw);
        Ids owners(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        auto presentation_identities = identities;
        if (!expected_instances.empty()) {
            const auto original_aliases = embedded_assembly_presentation_ids(source);
            const auto final_aliases = embedded_assembly_presentation_ids(result.entities);
            for (const auto& [key, alias] : original_aliases)
                if (final_aliases.at(key) != alias) reject("replacement changed an original component presentation identity");
            for (const auto& [key, new_id] : hosted_instance_identities) {
                const auto& alias = final_aliases.at({identities.at(key.first), new_id});
                // A unique plain alias can equal its own explicitly reserved ID.
                if (occupied.values.contains(alias) || (fresh.contains(alias) && alias != new_id))
                    reject("proposed component presentation identity collision");
                if (!presentation_identities.emplace(original_aliases.at(key), alias).second)
                    reject("hosted presentation aliases overlap mapped owner identities");
                owners.insert(original_aliases.at(key));
            }
        }
        complete_presentation(result.entities, source, owners, presentation_identities);
        Ids copied_roofs, copied_joins;
        for (const auto& id : roofs) copied_roofs.insert(identities.at(id));
        for (const auto& id : joins) copied_joins.insert(identities.at(id));
        admit_roofs_and_joins(result.entities, copied_roofs, copied_joins);
        if (phase_qualified_joins) {
            // Original joins keep their exact historical envelopes. A join
            // demolished in this alternative is not a native final-body owner.
            const auto final_scope = constraint_phase_scope(result.entities);
            std::erase_if(ordinary_joins, [&](const auto& id) { return final_scope.inactive_owner_ids.contains(id); });
            validate_roof_join_ownership(result.entities);
        }
        if (!ordinary_targets.empty()) admit_roofs_and_joins(result.entities, ordinary_targets, ordinary_joins);
        (void)constraint_phase_scope(result.entities);
        if (include_hosted_instances) {
            const auto final_plan = inspect_roof_clone_plan(result.entities,
                std::vector<std::string>(copied_roofs.begin(), copied_roofs.end()), true);
            if (!final_plan.ready()) reject("proposed hosted geometry has unresolved actual dependencies");
            std::set<PhaseRoofReplacementHostedInstanceKey> proposed_instances;
            for (const auto& [key, new_id] : hosted_instance_identities)
                proposed_instances.emplace(identities.at(key.first), new_id);
            if (std::set<PhaseRoofReplacementHostedInstanceKey>(final_plan.required_hosted_instance_ids.begin(),
                final_plan.required_hosted_instance_ids.end()) != proposed_instances)
                reject("final proposed hosted roster differs from actual qualified source replay");
            for (const auto& id : plan.required_entity_ids) {
                if (source.at(id).type != "assembly_model" && !exact(result.entities.at(id), source.at(id)))
                    reject("replacement changed an original baseline roof or join");
                if (source.at(id).type == "assembly_model") {
                    const auto& retained = source.at(id).properties.at("model").at("instances");
                    const auto& actual = result.entities.at(id).properties.at("model").at("instances");
                    if (retained.size() != actual.size()) reject("replacement changed an original catalog roster");
                    bool has_ordinary = false;
                    for (std::size_t i = 0; i < retained.size(); ++i) {
                        const auto& row = retained.at(i);
                        const bool ordinary = row.contains("placement") &&
                            ordinary_targets.contains(row.at("placement").at("host_entity_id").get<std::string>());
                        has_ordinary = has_ordinary || ordinary;
                        if (!ordinary && row.dump() != actual.at(i).dump())
                            reject("replacement changed an original baseline/unselected hosted row");
                    }
                    if (!has_ordinary && !exact(result.entities.at(id), source.at(id)))
                        reject("replacement changed an original catalog without ordinary hosted authority");
                }
            }
        }
        result.fresh_identity_ids.assign(fresh.begin(), fresh.end());
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed typed replay: ") + error.what()); }
}

std::optional<PhaseRoofProfileReplacementRequest> phase_roof_profile_replacement_request(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofProfileEditIntent>& profiles) {
    // Typed replay also checks uniqueness, existence and profile admission.
    const auto physical = replay_roof_profile_entities(source, profiles);
    if (physical == source) return std::nullopt;
    const auto scope = constraint_phase_scope(source);
    std::optional<PhaseRoofProfileReplacementRequest> request;
    std::size_t ordinary = 0;
    for (const auto& profile : profiles) {
        if (scope.inactive_owner_ids.contains(profile.roof_id)) reject("profile target is inactive: " + profile.roof_id);
        const PhysicalWallPhaseState* membership = nullptr;
        for (const auto& registry : scope.registries)
            if (std::find(registry.registered_entity_ids.begin(), registry.registered_entity_ids.end(), profile.roof_id) != registry.registered_entity_ids.end()) {
                if (membership) reject("profile target has overlapping registry membership");
                membership = &registry;
            }
        if (!membership) { ++ordinary; continue; }
        const auto model = ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), profile.roof_id) != model.baseline_ids().end();
        if (!baseline || !model.active_alternative()) { ++ordinary; continue; }
        if (request && request->registry_id != membership->registry_id) reject("profiles span different shared-baseline registries");
        if (!request) request = PhaseRoofProfileReplacementRequest{membership->registry_id, *model.active_alternative(), {}};
        request->seed_roof_ids.push_back(profile.roof_id);
    }
    if (request && ordinary) reject("profiles mix shared-baseline and ordinary/proposed roof owners");
    if (request) std::sort(request->seed_roof_ids.begin(), request->seed_roof_ids.end());
    return request;
}

std::optional<PhaseRoofProfileReplacementRequest> phase_roof_opening_replacement_request(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofOpeningEditIntent>& opening_edits) {
    // The pure batch validates actual children, explicit new identities, source
    // quantities and physical fit before membership can confer clone authority.
    const auto physical = replay_roof_opening_entities(source, opening_edits);
    if (physical == source) return std::nullopt;
    const auto scope = constraint_phase_scope(source);
    std::optional<PhaseRoofProfileReplacementRequest> request;
    std::size_t ordinary = 0;
    for (const auto& edit : opening_edits) {
        if (scope.inactive_owner_ids.contains(edit.roof_id)) reject("opening target is inactive: " + edit.roof_id);
        const PhysicalWallPhaseState* membership = nullptr;
        for (const auto& registry : scope.registries)
            if (std::find(registry.registered_entity_ids.begin(), registry.registered_entity_ids.end(), edit.roof_id) != registry.registered_entity_ids.end()) {
                if (membership) reject("opening target has overlapping registry membership");
                membership = &registry;
            }
        if (!membership) { ++ordinary; continue; }
        const auto model = ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), edit.roof_id) != model.baseline_ids().end();
        if (!baseline || !model.active_alternative()) { ++ordinary; continue; }
        if (request && request->registry_id != membership->registry_id) reject("opening edits span different shared-baseline registries");
        if (!request) request = PhaseRoofProfileReplacementRequest{membership->registry_id, *model.active_alternative(), {}};
        request->seed_roof_ids.push_back(edit.roof_id);
    }
    if (request && ordinary) reject("opening edits mix shared-baseline and ordinary/proposed roof owners");
    if (request) {
        for (const auto& id : request->seed_roof_ids)
            if (source.at(id)==physical.at(id) && source.at(id).properties.dump()==physical.at(id).properties.dump() &&
                source.at(id).extensions.dump()==physical.at(id).extensions.dump())
                reject("An unchanged roof cannot acquire replacement authority as an extra seed");
        std::sort(request->seed_roof_ids.begin(), request->seed_roof_ids.end());
    }
    return request;
}

std::optional<PhaseRoofProfileReplacementRequest> phase_roof_edit_replacement_request(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofEditIntent>& roof_edits) {
    // Complete atomic replay admits the actual source and final cohorts before
    // membership can confer replacement authority. No identities are allocated.
    const auto physical = replay_roof_edit_entities(source, roof_edits);
    const auto unchanged = [&](const std::string& id) {
        return source.at(id) == physical.at(id) &&
            source.at(id).properties.dump() == physical.at(id).properties.dump() &&
            source.at(id).extensions.dump() == physical.at(id).extensions.dump();
    };
    if (std::all_of(roof_edits.begin(), roof_edits.end(), [&](const auto& edit) { return unchanged(edit.roof_id); }))
        return std::nullopt;
    for (const auto& edit : roof_edits)
        if (unchanged(edit.roof_id)) reject("An unchanged roof cannot acquire replacement authority as an extra seed");
    const auto scope = constraint_phase_scope(source);
    std::optional<PhaseRoofProfileReplacementRequest> request;
    std::size_t ordinary = 0;
    for (const auto& edit : roof_edits) {
        if (scope.inactive_owner_ids.contains(edit.roof_id)) reject("roof edit target is inactive: " + edit.roof_id);
        const PhysicalWallPhaseState* membership = nullptr;
        for (const auto& registry : scope.registries)
            if (std::find(registry.registered_entity_ids.begin(), registry.registered_entity_ids.end(), edit.roof_id) != registry.registered_entity_ids.end()) {
                if (membership) reject("roof edit target has overlapping registry membership");
                membership = &registry;
            }
        if (!membership) { ++ordinary; continue; }
        const auto model = ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        const bool baseline = std::find(model.baseline_ids().begin(), model.baseline_ids().end(), edit.roof_id) != model.baseline_ids().end();
        if (!baseline || !model.active_alternative()) { ++ordinary; continue; }
        if (request && request->registry_id != membership->registry_id) reject("roof edits span different shared-baseline registries");
        if (!request) request = PhaseRoofProfileReplacementRequest{membership->registry_id, *model.active_alternative(), {}};
        request->seed_roof_ids.push_back(edit.roof_id);
    }
    if (request && ordinary) reject("roof edits mix shared-baseline and ordinary/proposed roof owners");
    if (request) std::sort(request->seed_roof_ids.begin(), request->seed_roof_ids.end());
    return request;
}

PhaseRoofGeometryEditPartition partition_phase_roof_geometry_edits(
    const PhaseRoofReplacementEntities& source, const std::vector<RoofEditIntent>& roof_edits) {
    // Membership never authorizes caller-supplied entity deltas. Both source and
    // complete result pass the existing typed native roof/cohort producer first.
    const auto physical = replay_roof_edit_entities(source, roof_edits);
    return partition_geometry(source, physical, roof_edits);
}

nlohmann::json encode_phase_roof_replacement_authoring(const PhaseRoofReplacementAuthoring& authoring) {
    identity(authoring.registry_id); identity(authoring.alternative_id);
    if (authoring.include_hosted_instances && (!authoring.phase_qualified_joins || authoring.demolition || authoring.roof_edits.empty()))
        reject("hosted replacement requires phase-qualified combined edits without demolition");
    if (!authoring.include_hosted_instances && !authoring.hosted_instance_identities.empty())
        reject("historical authoring cannot declare hosted instance identities");
    if (!authoring.include_hosted_instances &&
        (std::any_of(authoring.roof_edits.begin(), authoring.roof_edits.end(), [](const auto& edit) {
            return edit.coordinate_world_hosted_geometry;
        }) || std::any_of(authoring.ordinary_roof_edits.begin(), authoring.ordinary_roof_edits.end(), [](const auto& edit) {
            return edit.coordinate_world_hosted_geometry;
        }))) reject("coordinated hosted roof intents require version-nine replacement authority");
    if (authoring.demolition) {
        if (!authoring.roof_profiles.empty() || !authoring.roof_opening_edits.empty() || !authoring.roof_edits.empty() ||
            !authoring.ordinary_roof_edits.empty())
            reject("roof demolition cannot borrow body edit authority");
        auto result=encode_roof_demolition_intent({authoring.registry_id,authoring.alternative_id,
            authoring.seed_roof_ids,authoring.identities,authoring.demolition_additional_identities,authoring.phase_qualified_joins,
            authoring.preserve_singleton_material});
        result["demolition_additional_identities"] = std::move(result.at("additional_identities"));
        result.erase("additional_identities");
        result["version"]=authoring.preserve_singleton_material ? 8 : authoring.phase_qualified_joins ? 7 : 4;
        result["demolition"]=true;
        if (result.dump().size()>maximum_authoring_bytes) reject("roof demolition authoring byte budget exceeded");
        return result;
    }
    if (authoring.preserve_singleton_material)
        reject("singleton material preservation requires the demolition authoring family");
    if (!authoring.demolition_additional_identities.empty())
        reject("historical roof edit dialects cannot declare demolition identities");
    if (authoring.phase_qualified_joins && authoring.roof_edits.empty())
        reject("qualified join authoring requires nonempty combined roof edits");
    if (!authoring.ordinary_roof_edits.empty() && authoring.roof_edits.empty())
        reject("mixed roof authoring requires both nonempty combined edit lists");
    if (!authoring.roof_edits.empty()) {
        const bool mixed = !authoring.ordinary_roof_edits.empty();
        const bool ordinary_field = mixed || authoring.phase_qualified_joins;
        if (!authoring.roof_profiles.empty() || !authoring.roof_opening_edits.empty())
            reject("combined roof authoring cannot mix historical replacement dialects");
        if (authoring.seed_roof_ids.empty() || authoring.seed_roof_ids.size() > maximum_replacements ||
            authoring.identities.empty() || authoring.identities.size() > maximum_replacements ||
            authoring.roof_edits.size() > maximum_replacements)
            reject("combined authoring requires bounded nonempty seeds, mapping and edits");
        if (ordinary_field && (authoring.ordinary_roof_edits.size() > maximum_replacements ||
            authoring.roof_edits.size() + authoring.ordinary_roof_edits.size() > maximum_replacements))
            reject("mixed roof authoring target budget exceeded");
        Ids seeds, targets, fresh;
        for (const auto& id : authoring.seed_roof_ids) { identity(id); if (!seeds.insert(id).second) reject("duplicate authoring seed"); }
        for (const auto& [old_id, new_id] : authoring.identities) {
            identity(old_id); identity(new_id);
            if (old_id == new_id || !fresh.insert(new_id).second) reject("authoring identities must be fresh and injective");
        }
        for (const auto& id : seeds) if (!authoring.identities.contains(id)) reject("authoring seed has no proposed identity");
        Json result{{"version", authoring.include_hosted_instances ? 9 : authoring.phase_qualified_joins ? 6 : mixed ? 5 : 3}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
            {"seed_roof_ids", authoring.seed_roof_ids}, {"identities", authoring.identities},
            {"roof_profiles", Json::array()}, {"roof_opening_edits", Json::array()}, {"roof_edits", Json::array()}};
        if (ordinary_field) result["ordinary_roof_edits"] = Json::array();
        if (authoring.phase_qualified_joins) result["phase_qualified_joins"] = true;
        if (authoring.include_hosted_instances) {
            if (authoring.identities.size() + authoring.hosted_instance_identities.size() > maximum_replacements)
                reject("authoring entity/child/hosted identity budget exceeded");
            result["include_hosted_instances"] = true;
            result["hosted_instance_identities"] = Json::array();
            auto hosted_bytes = result.dump().size();
            if (hosted_bytes > maximum_authoring_bytes) reject("hosted authoring byte budget exceeded");
            for (const auto& [key, new_id] : authoring.hosted_instance_identities) {
                identity(key.first); identity(new_id);
                if (key.second.empty() || key.second.size() > maximum_authoring_bytes ||
                    !authoring.identities.contains(key.first) || seeds.contains(key.first) ||
                    new_id == key.second || !fresh.insert(new_id).second)
                    reject("hosted identities require mapped catalogs and injective fresh instance IDs");
                Json row{{"catalog_id", key.first}, {"instance_id", key.second}, {"proposed_instance_id", new_id}};
                const auto added_bytes = row.dump().size() + (result.at("hosted_instance_identities").empty() ? 0 : 1);
                if (added_bytes > maximum_authoring_bytes - hosted_bytes) reject("hosted authoring byte budget exceeded");
                hosted_bytes += added_bytes;
                result.at("hosted_instance_identities").push_back(std::move(row));
            }
        }
        auto bytes = result.dump().size();
        if (bytes > maximum_authoring_bytes) reject("combined authoring byte budget exceeded");
        auto& edits = result.at("roof_edits");
        for (const auto& edit : authoring.roof_edits) {
            if (!seeds.contains(edit.roof_id) || !targets.insert(edit.roof_id).second)
                reject("authoring combined edits require unique seed targets");
            auto encoded = encode_roof_edit_intent(edit);
            const auto added_bytes = encoded.dump().size() + (edits.empty() ? 0 : 1);
            if (added_bytes > maximum_authoring_bytes - bytes) reject("combined authoring byte budget exceeded");
            bytes += added_bytes;
            edits.push_back(std::move(encoded));
        }
        if (targets != seeds) reject("authoring seeds must exactly match combined edit targets");
        if (ordinary_field) {
            auto& ordinary_edits = result.at("ordinary_roof_edits");
            for (const auto& edit : authoring.ordinary_roof_edits) {
                if (!targets.insert(edit.roof_id).second || authoring.identities.contains(edit.roof_id) || fresh.contains(edit.roof_id))
                    reject("authoring ordinary roof edits require disjoint unique unmapped targets");
                auto encoded = encode_roof_edit_intent(edit);
                const auto added_bytes = encoded.dump().size() + (ordinary_edits.empty() ? 0 : 1);
                if (added_bytes > maximum_authoring_bytes - bytes) reject("mixed roof authoring byte budget exceeded");
                bytes += added_bytes;
                ordinary_edits.push_back(std::move(encoded));
            }
        }
        return result;
    }
    if (!authoring.roof_opening_edits.empty()) {
        if (!authoring.roof_profiles.empty()) reject("mixed roof profile and opening replacement is unsupported");
        if (authoring.seed_roof_ids.empty() || authoring.seed_roof_ids.size() > maximum_replacements ||
            authoring.identities.empty() || authoring.identities.size() > maximum_replacements ||
            authoring.roof_opening_edits.size() > maximum_replacements)
            reject("opening authoring requires bounded nonempty seeds, mapping and edits");
        Ids seeds, targets, fresh;
        for (const auto& id : authoring.seed_roof_ids) { identity(id); if (!seeds.insert(id).second) reject("duplicate authoring seed"); }
        for (const auto& [old_id, new_id] : authoring.identities) {
            identity(old_id); identity(new_id);
            if (old_id == new_id || !fresh.insert(new_id).second) reject("authoring identities must be fresh and injective");
        }
        for (const auto& id : seeds) if (!authoring.identities.contains(id)) reject("authoring seed has no proposed identity");
        Json result{{"version", 2}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
            {"seed_roof_ids", authoring.seed_roof_ids}, {"identities", authoring.identities},
            {"roof_profiles", Json::array()}, {"roof_opening_edits", Json::array()}};
        auto bytes = result.dump().size();
        if (bytes > maximum_authoring_bytes) reject("opening authoring byte budget exceeded");
        auto& edits = result.at("roof_opening_edits");
        for (const auto& edit : authoring.roof_opening_edits) {
            if (!seeds.contains(edit.roof_id) || !targets.insert(edit.roof_id).second)
                reject("authoring opening edits require unique seed targets");
            auto encoded = encode_roof_opening_edit_intent(edit);
            const auto added_bytes = encoded.dump().size() + (edits.empty() ? 0 : 1);
            if (added_bytes > maximum_authoring_bytes - bytes) reject("opening authoring byte budget exceeded");
            bytes += added_bytes;
            edits.push_back(std::move(encoded));
        }
        if (targets!=seeds) reject("authoring seeds must exactly match opening edit targets");
        return result;
    }
    if (authoring.seed_roof_ids.empty() || authoring.seed_roof_ids.size() > maximum_replacements ||
        authoring.identities.empty() || authoring.identities.size() > maximum_replacements ||
        authoring.roof_profiles.empty() || authoring.roof_profiles.size() > maximum_replacements)
        reject("authoring requires bounded nonempty seeds, mapping and profiles");
    Ids seeds, targets, fresh;
    for (const auto& id : authoring.seed_roof_ids) { identity(id); if (!seeds.insert(id).second) reject("duplicate authoring seed"); }
    for (const auto& [old_id, new_id] : authoring.identities) {
        identity(old_id); identity(new_id);
        if (old_id == new_id || !fresh.insert(new_id).second) reject("authoring identities must be fresh and injective");
    }
    auto profiles = Json::array();
    for (const auto& profile : authoring.roof_profiles) {
        if (!seeds.contains(profile.roof_id) || !targets.insert(profile.roof_id).second) reject("authoring profiles require unique seed targets");
        profiles.push_back(encode_roof_profile_edit_intent(profile));
    }
    if (targets!=seeds) reject("authoring seeds must exactly match profile targets");
    for (const auto& id : seeds) if (!authoring.identities.contains(id)) reject("authoring seed has no proposed identity");
    return {{"version", 1}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
        {"seed_roof_ids", authoring.seed_roof_ids}, {"identities", authoring.identities}, {"roof_profiles", profiles}};
}

PhaseRoofReplacementAuthoring decode_phase_roof_replacement_authoring(const nlohmann::json& value) {
    try {
        if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() &&
            (value.at("version") == 5 || value.at("version") == 6 || value.at("version") == 9)) {
            const bool hosted = value.at("version") == 9;
            const bool qualified = hosted || value.at("version") == 6;
            if (value.size() != (hosted ? 12 : qualified ? 10 : 9) || !value.contains("registry_id") || !value.contains("alternative_id") ||
                !value.contains("seed_roof_ids") || !value.contains("identities") || !value.contains("roof_profiles") ||
                !value.contains("roof_opening_edits") || !value.contains("roof_edits") || !value.contains("ordinary_roof_edits") ||
                (qualified && (!value.contains("phase_qualified_joins") || !value.at("phase_qualified_joins").is_boolean() ||
                    !value.at("phase_qualified_joins").get<bool>())) ||
                (hosted && (!value.contains("include_hosted_instances") || !value.at("include_hosted_instances").is_boolean() ||
                    !value.at("include_hosted_instances").get<bool>() || !value.contains("hosted_instance_identities") ||
                    !value.at("hosted_instance_identities").is_array())) ||
                !value.at("identities").is_object() || !value.at("seed_roof_ids").is_array() ||
                !value.at("roof_profiles").is_array() || !value.at("roof_profiles").empty() ||
                !value.at("roof_opening_edits").is_array() || !value.at("roof_opening_edits").empty() ||
                !value.at("roof_edits").is_array() || value.at("roof_edits").empty() ||
                !value.at("ordinary_roof_edits").is_array() || (!qualified && value.at("ordinary_roof_edits").empty()))
                reject("combined authoring must contain its exact version-five/six/nine fields and required edit lists");
            Strings budget; budget.node_limit = maximum_authoring_bytes; budget.byte_limit = maximum_authoring_bytes;
            budget.read(value);
            if (value.at("seed_roof_ids").size() > maximum_replacements || value.at("identities").size() > maximum_replacements ||
                value.at("roof_edits").size() > maximum_replacements || value.at("ordinary_roof_edits").size() > maximum_replacements ||
                value.at("roof_edits").size() + value.at("ordinary_roof_edits").size() > maximum_replacements ||
                (hosted && value.at("identities").size() + value.at("hosted_instance_identities").size() > maximum_replacements) ||
                value.dump().size() > maximum_authoring_bytes)
                reject("mixed authoring budget exceeded");
            PhaseRoofReplacementAuthoring result;
            result.phase_qualified_joins = qualified;
            result.include_hosted_instances = hosted;
            result.registry_id = value.at("registry_id").get<std::string>();
            result.alternative_id = value.at("alternative_id").get<std::string>();
            result.seed_roof_ids = value.at("seed_roof_ids").get<std::vector<std::string>>();
            result.identities = value.at("identities").get<PhaseRoofReplacementIdentityMap>();
            if (hosted) for (const auto& row : value.at("hosted_instance_identities")) {
                if (!row.is_object() || row.size() != 3 || !row.contains("catalog_id") || !row.contains("instance_id") ||
                    !row.contains("proposed_instance_id") || !row.at("catalog_id").is_string() ||
                    !row.at("instance_id").is_string() || !row.at("proposed_instance_id").is_string())
                    reject("qualified hosted identity rows require exactly three string fields");
                const PhaseRoofReplacementHostedInstanceKey key{row.at("catalog_id").get<std::string>(), row.at("instance_id").get<std::string>()};
                if (!result.hosted_instance_identities.emplace(key, row.at("proposed_instance_id").get<std::string>()).second)
                    reject("duplicate qualified hosted identity mapping");
            }
            for (const auto& edit : value.at("roof_edits")) result.roof_edits.push_back(decode_roof_edit_intent(edit));
            for (const auto& edit : value.at("ordinary_roof_edits")) result.ordinary_roof_edits.push_back(decode_roof_edit_intent(edit));
            const auto canonical = encode_phase_roof_replacement_authoring(result);
            if (canonical != value || canonical.dump() != value.dump()) reject("mixed authoring differs from its canonical typed encoding");
            return result;
        }
        if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() &&
            (value.at("version")==4 || value.at("version")==7 || value.at("version")==8)) {
            const bool singleton = value.at("version")==8;
            const bool qualified = singleton || value.at("version")==7;
            if (value.size()!=(singleton ? 9 : qualified ? 8 : 7) || !value.contains("demolition") || !value.at("demolition").is_boolean() ||
                !value.contains("demolition_additional_identities") || !value.at("demolition_additional_identities").is_object() ||
                !value.at("demolition").get<bool>() || value.dump().size()>maximum_authoring_bytes)
                reject("roof demolition must contain its exact version-four/seven/eight fields");
            auto wire=value;
            wire.erase("demolition");wire["version"]=singleton ? 3 : qualified ? 2 : 1;
            wire["additional_identities"] = std::move(wire.at("demolition_additional_identities"));
            wire.erase("demolition_additional_identities");
            const auto demolition=decode_roof_demolition_intent(wire);
            PhaseRoofReplacementAuthoring result;
            result.registry_id=demolition.registry_id;result.alternative_id=demolition.alternative_id;
            result.seed_roof_ids=demolition.seed_roof_ids;result.identities=demolition.identities;result.demolition=true;
            result.demolition_additional_identities=demolition.additional_identities;
            result.phase_qualified_joins=demolition.phase_qualified_joins;
            result.preserve_singleton_material=demolition.preserve_singleton_material;
            (void)encode_phase_roof_replacement_authoring(result);
            return result;
        }
        if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() && value.at("version") == 3) {
            if (value.size() != 8 || !value.contains("registry_id") || !value.contains("alternative_id") ||
                !value.contains("seed_roof_ids") || !value.contains("identities") || !value.contains("roof_profiles") ||
                !value.contains("roof_opening_edits") || !value.contains("roof_edits") || !value.at("identities").is_object() ||
                !value.at("seed_roof_ids").is_array() || !value.at("roof_profiles").is_array() || !value.at("roof_profiles").empty() ||
                !value.at("roof_opening_edits").is_array() || !value.at("roof_opening_edits").empty() ||
                !value.at("roof_edits").is_array() || value.at("roof_edits").empty())
                reject("combined authoring must contain exactly the eight version-three fields and only combined edits");
            if (value.at("seed_roof_ids").size() > maximum_replacements || value.at("identities").size() > maximum_replacements ||
                value.at("roof_edits").size() > maximum_replacements || value.dump().size() > maximum_authoring_bytes)
                reject("combined authoring budget exceeded");
            PhaseRoofReplacementAuthoring result;
            result.registry_id = value.at("registry_id").get<std::string>();
            result.alternative_id = value.at("alternative_id").get<std::string>();
            result.seed_roof_ids = value.at("seed_roof_ids").get<std::vector<std::string>>();
            result.identities = value.at("identities").get<PhaseRoofReplacementIdentityMap>();
            for (const auto& edit : value.at("roof_edits")) result.roof_edits.push_back(decode_roof_edit_intent(edit));
            (void)encode_phase_roof_replacement_authoring(result);
            return result;
        }
        if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() && value.at("version") == 2) {
            if (value.size() != 7 || !value.contains("registry_id") || !value.contains("alternative_id") ||
                !value.contains("seed_roof_ids") || !value.contains("identities") || !value.contains("roof_profiles") ||
                !value.contains("roof_opening_edits") || !value.at("identities").is_object() ||
                !value.at("seed_roof_ids").is_array() || !value.at("roof_profiles").is_array() ||
                !value.at("roof_profiles").empty() || !value.at("roof_opening_edits").is_array() ||
                value.at("roof_opening_edits").empty())
                reject("opening authoring must contain exactly the seven version-two fields and only opening edits");
            if (value.at("seed_roof_ids").size() > maximum_replacements || value.at("identities").size() > maximum_replacements ||
                value.at("roof_opening_edits").size() > maximum_replacements || value.dump().size() > maximum_authoring_bytes)
                reject("opening authoring budget exceeded");
            PhaseRoofReplacementAuthoring result;
            result.registry_id = value.at("registry_id").get<std::string>();
            result.alternative_id = value.at("alternative_id").get<std::string>();
            result.seed_roof_ids = value.at("seed_roof_ids").get<std::vector<std::string>>();
            result.identities = value.at("identities").get<PhaseRoofReplacementIdentityMap>();
            for (const auto& edit : value.at("roof_opening_edits")) result.roof_opening_edits.push_back(decode_roof_opening_edit_intent(edit));
            (void)encode_phase_roof_replacement_authoring(result);
            return result;
        }
        if (!value.is_object() || value.size() != 6 || !value.contains("version") ||
            !value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.contains("registry_id") || !value.contains("alternative_id") || !value.contains("seed_roof_ids") ||
            !value.contains("identities") || !value.contains("roof_profiles") || !value.at("identities").is_object() ||
            !value.at("seed_roof_ids").is_array() || !value.at("roof_profiles").is_array())
            reject("authoring must contain exactly the six version-one fields");
        if (value.at("seed_roof_ids").size() > maximum_replacements || value.at("identities").size() > maximum_replacements ||
            value.at("roof_profiles").size() > maximum_replacements) reject("authoring budget exceeded");
        PhaseRoofReplacementAuthoring result;
        result.registry_id = value.at("registry_id").get<std::string>();
        result.alternative_id = value.at("alternative_id").get<std::string>();
        result.seed_roof_ids = value.at("seed_roof_ids").get<std::vector<std::string>>();
        result.identities = value.at("identities").get<PhaseRoofReplacementIdentityMap>();
        for (const auto& profile : value.at("roof_profiles")) result.roof_profiles.push_back(decode_roof_profile_edit_intent(profile));
        (void)encode_phase_roof_replacement_authoring(result);
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed authoring: ") + error.what()); }
}

PhaseRoofReplacementEntities replay_phase_roof_replacement_authoring(
    const PhaseRoofReplacementEntities& source, const PhaseRoofReplacementAuthoring& authoring) {
    (void)encode_phase_roof_replacement_authoring(authoring);
    if (authoring.demolition)
        return replay_roof_demolition(source,{authoring.registry_id,authoring.alternative_id,
            authoring.seed_roof_ids,authoring.identities,authoring.demolition_additional_identities,authoring.phase_qualified_joins,
            authoring.preserve_singleton_material}).entities;
    const auto plan = inspect_phase_roof_replacement_plan(source, authoring.seed_roof_ids, authoring.registry_id,
        authoring.alternative_id, authoring.phase_qualified_joins, authoring.include_hosted_instances);
    return replay_phase_roof_replacement(source, plan, authoring.identities, authoring.roof_profiles,
        authoring.roof_opening_edits, authoring.roof_edits, authoring.ordinary_roof_edits,
        authoring.phase_qualified_joins, authoring.hosted_instance_identities, authoring.include_hosted_instances).entities;
}
} // namespace sketch
