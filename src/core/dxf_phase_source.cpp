#include "sketch/dxf_phase_source.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/vertical_levels.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Owners = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
[[noreturn]] void refuse(const std::string& reason) {
    throw std::invalid_argument("Native DXF phase source: " + reason);
}
void require(bool condition, const std::string& reason) { if (!condition) refuse(reason); }
void identity(const std::string& id) {
    require(!id.empty() && id.size() <= 255 &&
        std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; }) &&
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }),
        "invalid document owner identity");
}
std::string reference(const Json& value) {
    require(value.is_string(), "typed owner reference must be a string");
    auto id = value.get<std::string>(); identity(id); return id;
}
void charge(std::size_t& consumed, std::size_t amount, std::size_t maximum, const char* message) {
    require(consumed <= maximum && amount <= maximum - consumed, message);
    consumed += amount;
}
void budget_limits(const NativeDxfWallSourceWorkBudget& budget) {
    const auto& b = budget.catalog_transfer;
    require(b.max_json_bytes <= native_dxf_phase_source_byte_limit &&
        b.max_json_nodes <= native_dxf_phase_source_node_limit &&
        b.max_validation_work <= native_dxf_phase_source_work_limit &&
        b.consumed_json_bytes <= b.max_json_bytes && b.consumed_json_nodes <= b.max_json_nodes &&
        b.consumed_validation_work <= b.max_validation_work, "invalid shared work ledger");
}
void work(NativeDxfWallSourceWorkBudget& budget, std::size_t amount) {
    auto& b = budget.catalog_transfer;
    charge(b.consumed_validation_work, amount, b.max_validation_work, "cumulative graph work limit");
}
void product_work(NativeDxfWallSourceWorkBudget& budget, std::size_t a, std::size_t b) {
    require(!b || a <= native_dxf_phase_source_work_limit / b, "graph work product limit");
    work(budget, a * b);
}
// Conservative JSON encoding bound, charged incrementally before copies,
// serialization, model codecs, sorting, or recursive graph consumers.
void raw(const Json& value, NativeDxfWallSourceWorkBudget& budget, std::size_t depth = 0) {
    require(depth <= native_dxf_phase_source_depth_limit, "raw JSON depth limit");
    auto& b = budget.catalog_transfer;
    charge(b.consumed_json_nodes, 1, b.max_json_nodes, "cumulative raw JSON node limit");
    work(budget, 1);
    const auto bytes = [&](std::size_t amount) {
        charge(b.consumed_json_bytes, amount, b.max_json_bytes, "cumulative raw JSON byte limit");
    };
    const auto string_bytes = [&](const std::string& text) {
        require(text.size() <= native_dxf_phase_source_byte_limit / 6, "raw JSON string limit");
        bytes(text.size() * 6 + 3);
    };
    if (value.is_string()) string_bytes(value.get_ref<const std::string&>());
    else if (value.is_object()) {
        bytes(2);
        for (const auto& [key, child] : value.items()) { string_bytes(key); bytes(1); raw(child, budget, depth + 1); }
    } else if (value.is_array()) {
        bytes(2);
        for (const auto& child : value) { bytes(1); raw(child, budget, depth + 1); }
    } else if (value.is_number()) {
        require(!value.is_number_float() || std::isfinite(value.get<double>()), "nonfinite source number");
        bytes(32);
    } else { require(value.is_boolean() || value.is_null(), "unsupported JSON value"); bytes(5); }
}
void raw_entity(const std::string& id, const Entity& entity, NativeDxfWallSourceWorkBudget& budget) {
    identity(id); require(id == entity.id, "map key differs from actual owner " + id);
    require(!entity.type.empty() && entity.type.size() <= 255, "source Entity type limit");
    require(entity.properties.is_object() && entity.extensions.is_object(), "invalid Entity envelope " + id);
    raw(Json(id), budget); raw(Json(entity.type), budget);
    raw(entity.properties, budget); raw(entity.extensions, budget); raw(Json(entity.required), budget);
}
void sorted_ids(const std::vector<std::string>& ids) {
    require(ids.size() <= native_dxf_phase_source_owner_limit, "role inventory limit");
    // Bound every string before ordered comparisons can inspect common prefixes.
    for (const auto& id : ids) identity(id);
    require(std::is_sorted(ids.begin(), ids.end()) && std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
        "role inventory must be sorted and unique");
}
bool context_type(std::string_view type) {
    return type == "property" || type == "building" || type == "floor" || type == "layer" || type == "vertical_levels";
}
bool body_type(std::string_view type) {
    return is_model_phase_entity_type(type) && type != "assembly_model" && type != "building" && type != "floor";
}
bool source_type(std::string_view type) { return body_type(type) || context_type(type) || type == "assembly_model" || type == "model_phases"; }
const Entity& actual(const Owners& owners, const std::string& id, std::string_view type = {}) {
    const auto found = owners.find(id);
    require(found != owners.end(), "missing source owner " + id);
    require(found->second.id == id && (type.empty() || found->second.type == type), "source owner role/identity differs " + id);
    require(source_type(found->second.type), "unsupported graph dependency " + id + " (" + found->second.type + ")");
    return found->second;
}

using References = std::map<std::string, std::string, std::less<>>;
// Only typed document references are read. Nested arbitrary metadata and local
// identities never enter the global owner namespace.
References direct_references(const Entity& entity) {
    References result;
    const auto add = [&](const Json& value, std::string_view role) {
        const auto id = reference(value);
        const auto [found, inserted] = result.emplace(id, std::string(role));
        require(inserted || role.empty() || found->second.empty() || found->second == role,
            "conflicting typed reference roles on " + entity.id);
        if (!role.empty()) found->second = role;
    };
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 22> slots{{
        {"assembly_catalog_id", "assembly_model"}, {"property_id", "property"}, {"building_id", "building"},
        {"floor_id", "floor"}, {"layer_id", "layer"}, {"boundary_id", "boundary"}, {"wall_id", "wall"},
        {"opening_id", "opening"}, {"room_id", "room"}, {"slab_id", "slab"}, {"roof_id", "roof"},
        {"stair_id", "stair"}, {"sheet_id", "sheet"}, {"view_id", "view"}, {"constraint_id", "constraint"},
        {"label_id", "label"}, {"column_id", "column"}, {"beam_id", "beam"}, {"railing_id", "railing"},
        {"wall_join_id", "wall_join"}, {"roof_join_id", "roof_join"}, {"terrain_surface_id", "terrain_surface"}}};
    for (const auto& [slot, role] : slots) {
        if (entity.properties.contains(slot)) add(entity.properties.at(slot), role);
        const auto plural = std::string(slot) + "s";
        if (entity.properties.contains(plural)) {
            const auto& rows = entity.properties.at(plural); require(rows.is_array(), "typed reference collection is not an array");
            for (const auto& row : rows) add(row, role);
        }
    }
    for (const auto* slot : {"parent_id", "host_id", "target_id", "entity_id", "source_entity_id"}) {
        if (entity.properties.contains(slot)) add(entity.properties.at(slot), {});
        const auto plural = std::string(slot) + "s";
        if (entity.properties.contains(plural)) {
            const auto& rows = entity.properties.at(plural); require(rows.is_array(), "owner collection is not an array");
            for (const auto& row : rows) add(row, {});
        }
    }
    for (const auto* slot : {"refs", "references"}) if (entity.properties.contains(slot)) {
        const auto& rows = entity.properties.at(slot); require(rows.is_array(), "reference collection is not an array");
        for (const auto& row : rows) add(row, {});
    }
    return result;
}
References dependencies(const Entity& owner, NativeDxfWallSourceWorkBudget& budget) {
    auto result = direct_references(owner);
    const auto add = [&](const std::string& id, const char* type = "") {
        identity(id);
        const auto [found, inserted] = result.emplace(id, type);
        require(inserted || !*type || found->second.empty() || found->second == type, "dependency role conflict " + id);
        if (*type) found->second = type;
    };
    for (const auto* slot : {"asset_id", "asset_ids", "render_asset_id", "render_asset_ids"})
        require(!owner.properties.contains(slot), "unsupported typed dependency " + owner.id + ": " + slot);
    // Document does not interpret phase_id/phase_binding/phase_registry_id or
    // an arbitrary phase_membership extension as roster authority. Preserve
    // that source metadata; actual ModelPhases registries alone enroll owners.
    if (owner.type == "assembly_model") {
        const auto refs = complete_assembly_catalog_source_refs(owner, budget.catalog_transfer);
        for (const auto& id : refs.hosted_entity_ids) add(id);
        for (const auto& id : refs.context_owner_ids) add(id);
    } else if (body_type(owner.type)) {
        for (const auto& id : native_dxf_wall_source_dependency_ids(owner)) add(id);
        for (const auto& id : native_dxf_architectural_source_catalog_ids(owner)) add(id, "assembly_model");
        if (native_dxf_phase_auxiliary_source_type(owner.type))
            for (const auto& id : native_dxf_phase_auxiliary_source_dependencies(owner)) add(id);
        if (native_dxf_architectural_source_type(owner.type)) {
            const auto refs = native_dxf_architectural_source_dependencies(owner);
            if (refs.at("level_graph_id") != "") add(reference(refs.at("level_graph_id")), "vertical_levels");
        }
        if (owner.extensions.contains("physical_wall_room")) {
            const auto& saved = owner.extensions.at("physical_wall_room");
            const auto& holes = saved.at("holes"); require(holes.is_array(), "physical room hole inventory shape");
            for (const auto& hole : holes) {
                require(hole.is_array() && hole.size() <= 16'384, "physical room hole work limit");
                product_work(budget, hole.size(), hole.size() + 1);
            }
            const auto descriptor = decode_physical_wall_room_descriptor(owner);
            add(descriptor.selected_wall_id, "wall");
            const auto& lineage = descriptor.source_lineage;
            const auto contexts = [&](const Json& context, bool nullable) {
                for (const auto& [slot, role] : std::array<std::pair<const char*, const char*>, 4>{{
                    {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}}}) {
                    const auto& id = context.at(slot); if (!(nullable && id.is_null())) add(reference(id), role);
                }
            };
            contexts(lineage.at("context"), false);
            const auto& sources = lineage.at("physical_sources"); require(sources.is_array(), "physical room source inventory shape");
            for (const auto& source : sources) {
                work(budget, 1); add(reference(source.at("owner_id")), "wall"); contexts(source.at("source_context"), true);
            }
            const auto& phases = lineage.at("semantic_phases"); require(phases.is_array(), "physical room registry inventory shape");
            for (const auto& phase : phases) {
                work(budget, 1); add(reference(phase.at("id")), "model_phases");
                const auto& members = phase.at("owners"); require(members.is_array(), "physical room phase owner inventory shape");
                for (const auto& member : members) { work(budget, 1); add(reference(member.at("owner_id")), "wall"); }
            }
            const auto face = [&](const Json& value) {
                const auto& edges = value.at("edges"); require(edges.is_array(), "physical room source edge inventory shape");
                for (const auto& edge : edges) {
                    const auto& uses = edge.at("source_uses"); require(uses.is_array(), "physical room source use inventory shape");
                    for (const auto& use : uses) { work(budget, 1); add(reference(use.at("owner_id")), "wall"); }
                }
            };
            face(lineage.at("outer"));
            const auto& faces = lineage.at("holes"); require(faces.is_array(), "physical room source hole inventory shape");
            for (const auto& hole : faces) face(hole);
        }
    }
    if (owner.type == "floor" && owner.properties.contains("vertical_level_binding"))
        add(VerticalLevelBinding::from_json(owner.properties.at("vertical_level_binding")).graph_entity_id, "vertical_levels");
    return result;
}

struct Phases {
    std::map<std::string, ModelPhases, std::less<>> models;
    std::map<std::string, std::string, std::less<>> memberships;
    Ids inactive;
    Ids enrolled_hierarchy;
};
// Reserve whole-model work before the first decoder; this also bounds repeated
// level replay by organization and placement. No support model is pruned.
void reserve_models(const Owners& owners, NativeDxfWallSourceWorkBudget& budget) {
    product_work(budget, owners.size(), owners.size() + 16);
    std::size_t maximum_level_cost = 0, bound_floors = 0;
    for (const auto& [id, owner] : owners) {
        (void)id;
        if (owner.type == "model_phases") {
            const auto& model = owner.properties.at("model");
            require(model.is_object() && model.at("entity_ids").is_array() && model.at("baseline_ids").is_array() &&
                model.at("alternatives").is_array(), "raw phase model shape");
            std::size_t entries = model.at("entity_ids").size() + model.at("baseline_ids").size() + 1;
            require(entries <= native_dxf_phase_source_node_limit, "raw phase roster limit");
            for (const auto& alternative : model.at("alternatives")) {
                require(alternative.is_object() && alternative.at("demolished_ids").is_array() &&
                    alternative.at("proposed_ids").is_array(), "raw phase alternative shape");
                const auto count = alternative.at("demolished_ids").size() + alternative.at("proposed_ids").size() + 1;
                require(count <= native_dxf_phase_source_node_limit - entries, "raw phase membership limit"); entries += count;
            }
            // State construction for every alternative, ownership and joins.
            product_work(budget, entries + model.at("entity_ids").size() * (model.at("alternatives").size() + 1), 32);
        } else if (owner.type == "vertical_levels") {
            const auto& model = owner.properties.at("model");
            require(model.is_object() && model.at("levels").is_array() && model.at("links").is_array() &&
                model.at("levels").size() <= 4096 && model.at("links").size() <= 8192, "raw vertical graph shape/limit");
            const auto count = model.at("levels").size() + model.at("links").size() + 1;
            require(count <= native_dxf_phase_source_work_limit / count, "vertical graph work limit");
            const auto cost = count * count;
            work(budget, cost); maximum_level_cost = std::max(maximum_level_cost, cost);
        }
        if (owner.type == "floor" && owner.properties.contains("vertical_level_binding")) ++bound_floors;
    }
    product_work(budget, maximum_level_cost, bound_floors + 1);
}
Phases phase_inventory(const Owners& owners) {
    Phases result;
    for (const auto& [id, owner] : owners) if (owner.type == "model_phases") {
        const auto model = ModelPhases::from_json(owner.properties.at("model"));
        const auto state = model.active_state();
        for (const auto& member : model.entity_ids()) {
            const auto& actual_owner = actual(owners, member);
            require(is_model_phase_entity_type(actual_owner.type), "registry member has unsupported role " + member);
            const auto [existing, inserted] = result.memberships.emplace(member, id);
            require(inserted, "overlapping phase registries " + existing->second + " and " + id + " own " + member);
            if (actual_owner.type == "building" || actual_owner.type == "floor") result.enrolled_hierarchy.insert(member);
            const auto current = state.find(member);
            if (current == state.end() || current->second == ModelPhase::demolished) result.inactive.insert(member);
        }
        result.models.emplace(id, model);
    }
    return result;
}
std::optional<DrawingContext> source_context(const Entity& owner, const Owners& owners,
    const ProjectOrganization& organization) {
    if (owner.type == "assembly_model") {
        // Catalogs may bind a property, building or floor without a drawing
        // layer. Resolve their actual named ancestors instead of applying the
        // complete placeable-body contract to the catalog itself.
        std::optional<DrawingContext> result;
        for (const auto& [slot, role] : std::array<std::pair<const char*, const char*>, 4>{{
            {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}}}) {
            const auto value = owner.properties.find(slot);
            if (value == owner.properties.end()) continue;
            const auto& ancestor = actual(owners, reference(*value), role);
            const auto node = organization.nodes.find(ancestor.id);
            require(node != organization.nodes.end() && node->second.issues.empty() &&
                !node->second.context.property_id.empty(), "catalog source hierarchy unresolved " + ancestor.id);
            if (!result) result.emplace();
            const auto& context = node->second.context;
            for (const auto& [target, original] : {
                std::pair{&result->property_id, &context.property_id},
                std::pair{&result->building_id, &context.building_id},
                std::pair{&result->floor_id, &context.floor_id},
                std::pair{&result->layer_id, &context.layer_id},
                std::pair{&result->level_id, &context.level_id}}) {
                if (original->empty()) continue;
                require(target->empty() || *target == *original, "catalog source hierarchy conflicts " + owner.id);
                *target = *original;
            }
        }
        require(!result || result->layer_id.empty() || result->complete(), "catalog source layer hierarchy incomplete " + owner.id);
        return result;
    }
    if (native_dxf_phase_auxiliary_source_type(owner.type))
        return native_dxf_phase_auxiliary_source_context(owner, owners, organization);
    return native_dxf_architectural_source_context(owner, owners, organization);
}
std::vector<std::string> context_owners(const Entity& owner, const Owners& owners,
    const ProjectOrganization& organization) {
    const auto context = source_context(owner, owners, organization);
    if (!context) { require(!body_type(owner.type), "body hierarchy unresolved " + owner.id); return {}; }
    if (owner.type == "terrain_surface")
        require(!context->property_id.empty(), "terrain requires an actual source property " + owner.id);
    else
        require(!body_type(owner.type) || context->complete(), "body requires complete actual source hierarchy " + owner.id);
    std::vector<std::string> result;
    for (const auto& id : {context->property_id, context->building_id, context->floor_id, context->layer_id})
        if (!id.empty() && id != owner.id) result.push_back(id);
    if (!context->floor_id.empty()) {
        const auto& floor = actual(owners, context->floor_id, "floor");
        if (floor.properties.contains("vertical_level_binding")) result.push_back(
            VerticalLevelBinding::from_json(floor.properties.at("vertical_level_binding")).graph_entity_id);
    }
    return result;
}
void hosted_phase(const Entity& child, const std::string& host_id, const Phases& phases,
    NativeDxfWallSourceWorkBudget& budget) {
    const auto host = phases.memberships.find(host_id), member = phases.memberships.find(child.id);
    require((host == phases.memberships.end()) == (member == phases.memberships.end()),
        "hosted source phase enrollment differs " + child.id);
    if (host == phases.memberships.end()) return;
    require(host->second == member->second, "hosted source crosses registries " + child.id);
    const auto& model = phases.models.at(host->second);
    product_work(budget, model.entity_ids().size(), (model.alternatives().size() + 1) * 32);
    const auto check = [&](const std::optional<std::string>& alternative) {
        const auto states = model.state(alternative);
        const auto active = [&](const std::string& id) {
            const auto found = states.find(id); return found != states.end() && found->second != ModelPhase::demolished;
        };
        require(!active(child.id) || active(host_id), "hosted source has inactive actual host " + child.id);
    };
    check(std::nullopt);
    for (const auto& alternative : model.alternatives()) check(alternative.id);
}
void roles(NativeDxfPhaseSourceGraph& graph, const Owners& owners, const Phases& phases) {
    for (const auto& [id, owner] : owners) {
        if (body_type(owner.type)) {
            graph.body_ids.push_back(id);
            if (!phases.inactive.contains(id)) graph.depicted_body_ids.push_back(id);
        } else if (owner.type == "assembly_model") graph.catalog_ids.push_back(id);
        else if (owner.type == "model_phases") graph.registry_ids.push_back(id);
        else if (context_type(owner.type)) graph.context_ids.push_back(id);
        else refuse("unsupported source role " + id);
    }
    graph.enrolled_hierarchy_ids.assign(phases.enrolled_hierarchy.begin(), phases.enrolled_hierarchy.end());
}
void semantic_graph(const NativeDxfPhaseSourceGraph& graph, NativeDxfWallSourceWorkBudget& budget) {
    const auto phases = phase_inventory(graph.entities);
    NativeDxfPhaseSourceGraph expected; roles(expected, graph.entities, phases);
    require(graph.body_ids == expected.body_ids && graph.catalog_ids == expected.catalog_ids &&
        graph.registry_ids == expected.registry_ids && graph.context_ids == expected.context_ids &&
        graph.enrolled_hierarchy_ids == expected.enrolled_hierarchy_ids && graph.depicted_body_ids == expected.depicted_body_ids,
        "role/subset inventory differs from actual source graph");
    // Admit every complete catalog before the first semantic catalog decode.
    // Dependency identities/roles are proved before architectural consumers
    // can inspect actual hosts. The raw source remains the representation.
    for (const auto& id : graph.catalog_ids) admit_complete_assembly_catalog_source(graph.entities.at(id), budget.catalog_transfer);
    std::map<std::string, References, std::less<>> reached;
    for (const auto& [id, owner] : graph.entities) {
        auto refs = dependencies(owner, budget);
        for (const auto& [dependency, role] : refs) (void)actual(graph.entities, dependency, role);
        if (owner.type == "assembly_model") {
            const auto& instances = owner.properties.at("model").at("instances");
            work(budget, instances.size() + 1);
            for (const auto& instance : instances) if (instance.contains("placement")) {
                const auto& host = actual(graph.entities, reference(instance.at("placement").at("host_entity_id")));
                require(is_complete_assembly_catalog_host_type(host.type),
                    "embedded catalog placement has an invalid actual host role " + host.id);
            }
        }
        reached.emplace(id, std::move(refs));
    }
    for (const auto& id : graph.body_ids) {
        const auto& owner = graph.entities.at(id);
        for (const auto& material : architectural_material_source_refs(owner)) {
            const auto& catalog = actual(graph.entities, material.catalog_id, "assembly_model");
            const auto& rows = catalog.properties.at("model").at("materials");
            work(budget, rows.size() + 1);
            require(std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.at("id") == material.material_id; }),
                "material reference is not an actual catalog-local material " + id);
        }
        if (owner.type == "assembly_instance") {
            const auto instance = decode_document_assembly_instance(owner);
            const auto& catalog = actual(graph.entities, instance.assembly_catalog_id, "assembly_model");
            const auto& types = catalog.properties.at("model").at("types");
            work(budget, types.size() + 1);
            require(std::any_of(types.begin(), types.end(), [&](const auto& row) { return row.at("id") == instance.instance.type_id; }),
                "independent assembly references missing catalog-local type " + id);
        }
    }
    for (const auto& id : graph.body_ids) {
        const auto& owner = graph.entities.at(id);
        admit_native_dxf_architectural_source_work(owner, graph.entities, budget);
        admit_native_dxf_phase_auxiliary_source_work(owner, graph.entities, budget);
    }
    for (const auto& [id, owner] : graph.entities) if (owner.type == "vertical_levels") {
        (void)id; (void)VerticalLevelGraph::from_json(owner.properties.at("model"));
    }
    const auto organization = organize_project(graph.entities);
    Ids needed_contexts = phases.enrolled_hierarchy;
    for (const auto& [id, owner] : graph.entities) {
        for (const auto& [dependency, role] : reached.at(id)) {
            const auto& target = actual(graph.entities, dependency, role);
            if (context_type(target.type)) needed_contexts.insert(dependency);
        }
        if (owner.type != "model_phases" && owner.type != "vertical_levels") {
            const auto node = organization.nodes.find(id);
            if (node != organization.nodes.end() && owner.type != "assembly_model")
                require(node->second.issues.empty(), "contradictory source hierarchy " + id);
            for (const auto& dependency : context_owners(owner, graph.entities, organization)) {
                const auto& target = actual(graph.entities, dependency);
                require(context_type(target.type), "resolved context role differs " + dependency); needed_contexts.insert(dependency);
            }
        }
        if (owner.type == "opening") hosted_phase(owner, reference(owner.properties.at("wall_id")), phases, budget);
        if (owner.type == "railing" && owner.properties.contains("host")) {
            const auto host = reference(owner.properties.at("host").at("stair_id"));
            (void)actual(graph.entities, host, "stair"); hosted_phase(owner, host, phases, budget);
            std::map<std::string, std::string, std::less<>> children;
            for (const auto& child : native_dxf_architectural_child_identity_ids(graph.entities.at(host))) children.emplace(child, child);
            auto proof = owner;
            remap_native_dxf_architectural_child_identities(proof, children, &graph.entities.at(host));
            require(proof == owner, "local stair child witness changed source");
        }
        if (native_dxf_phase_auxiliary_source_type(owner.type)) validate_native_dxf_phase_auxiliary_source(owner, graph.entities);
        if (owner.extensions.contains(std::string(roof_join_phase_ownership_extension_key)))
            require(owner.type == "roof_join", "roof join qualifier on wrong role " + id);
    }
    for (const auto& id : graph.context_ids) require(needed_contexts.contains(id), "orphan context owner " + id);
    validate_roof_join_ownership(graph.entities);
}
void raw_graph(const NativeDxfPhaseSourceGraph& graph, NativeDxfWallSourceWorkBudget& budget) {
    budget_limits(budget);
    require(!graph.entities.empty() && graph.entities.size() <= native_dxf_phase_source_owner_limit, "source owner inventory limit");
    for (const auto& [id, owner] : graph.entities) raw_entity(id, owner, budget);
    for (const auto* ids : {&graph.body_ids, &graph.catalog_ids, &graph.registry_ids, &graph.context_ids,
        &graph.enrolled_hierarchy_ids, &graph.depicted_body_ids}) {
        // Bound caller-owned vectors before constructing any transport copy.
        require(ids->size() <= native_dxf_phase_source_owner_limit, "role inventory limit");
        // Identity, order and duplicate scans stay charged even when a late
        // malformed element or ordering error rejects a caller-owned graph.
        product_work(budget, ids->size(), 3 * 256);
        sorted_ids(*ids); raw(Json::array(), budget);
        for (const auto& id : *ids) {
            raw(Json(id), budget);
            charge(budget.catalog_transfer.consumed_json_bytes, 1,
                budget.catalog_transfer.max_json_bytes, "cumulative role JSON byte limit");
        }
    }
    reserve_models(graph.entities, budget);
}
Json unchecked_encode(const NativeDxfPhaseSourceGraph& graph) {
    Json entities = Json::array();
    for (const auto& [id, owner] : graph.entities) entities.push_back({{"id", id}, {"type", owner.type},
        {"properties", owner.properties}, {"required", owner.required}, {"extensions", owner.extensions}});
    return {{"version", 1}, {"entities", std::move(entities)}, {"body_ids", graph.body_ids},
        {"catalog_ids", graph.catalog_ids}, {"registry_ids", graph.registry_ids}, {"context_ids", graph.context_ids},
        {"enrolled_hierarchy_ids", graph.enrolled_hierarchy_ids}, {"depicted_body_ids", graph.depicted_body_ids}};
}
bool unsupported_incoming(const Entity& owner, const Ids& selected) {
    // Sharing a source drawing layer does not make an unrelated annotation an
    // authored dependency. Explicit owner attachments and constraint bindings
    // do; unknown entity types retain opaque properties without interpretation.
    if (!is_known_entity_type(owner.type)) return false;
    for (const auto& [id, role] : direct_references(owner))
        if (!context_type(role) && selected.contains(id)) return true;
    if (owner.type == "constraint" && owner.properties.contains("bindings")) {
        const auto& rows = owner.properties.at("bindings"); require(rows.is_array(), "unsupported constraint binding shape");
        for (const auto& row : rows) if (row.is_object() && row.contains("owner_id") && selected.contains(reference(row.at("owner_id")))) return true;
    }
    return false;
}
} // namespace

void validate_native_dxf_phase_source_graph(const NativeDxfPhaseSourceGraph& graph,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try { raw_graph(graph, budget); semantic_graph(graph, budget); }
    catch (const Json::exception& error) { refuse(std::string("malformed source graph: ") + error.what()); }
}
Json encode_native_dxf_phase_source_graph(const NativeDxfPhaseSourceGraph& graph) {
    validate_native_dxf_phase_source_graph(graph);
    return unchecked_encode(graph);
}
NativeDxfPhaseSourceGraph decode_native_dxf_phase_source_graph(const Json& value,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        budget_limits(budget); raw(value, budget);
        require(value.is_object() && value.size() == 8 && value.at("version").is_number_integer() && value.at("version") == 1,
            "unsupported graph schema/version");
        const auto& rows = value.at("entities");
        require(rows.is_array() && !rows.empty() && rows.size() <= native_dxf_phase_source_owner_limit, "source table shape/limit");
        NativeDxfPhaseSourceGraph result; std::string previous;
        for (const auto& row : rows) {
            require(row.is_object() && row.size() == 5 && row.at("type").is_string() && row.at("properties").is_object() &&
                row.at("required").is_boolean() && row.at("extensions").is_object(), "raw Entity row schema");
            const auto id = reference(row.at("id")); require(id > previous, "source table must be sorted and unique"); previous = id;
            result.entities.emplace(id, Entity{id, row.at("type").get<std::string>(), row.at("properties"), row.at("required").get<bool>(), row.at("extensions")});
        }
        const auto read_ids = [&](const char* slot) {
            const auto& values = value.at(slot); require(values.is_array() && values.size() <= native_dxf_phase_source_owner_limit, "role array shape/limit");
            product_work(budget, values.size(), 3 * 256);
            std::vector<std::string> ids; ids.reserve(values.size());
            for (const auto& id : values) ids.push_back(reference(id)); sorted_ids(ids); return ids;
        };
        result.body_ids = read_ids("body_ids"); result.catalog_ids = read_ids("catalog_ids");
        result.registry_ids = read_ids("registry_ids"); result.context_ids = read_ids("context_ids");
        result.enrolled_hierarchy_ids = read_ids("enrolled_hierarchy_ids"); result.depicted_body_ids = read_ids("depicted_body_ids");
        reserve_models(result.entities, budget); semantic_graph(result, budget); return result;
    } catch (const Json::exception& error) { refuse(std::string("malformed graph codec: ") + error.what()); }
}

NativeDxfPhaseSourceGraph capture_native_dxf_phase_source_graph(const DocumentSnapshot& source,
    const std::vector<std::string>& seed_owner_ids, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        budget_limits(budget); const auto& owners = source.entities();
        require(!seed_owner_ids.empty() && seed_owner_ids.size() <= native_dxf_phase_source_owner_limit &&
            owners.size() <= native_dxf_phase_source_owner_limit, "capture owner/seed limit");
        // The inverse host/unsupported dependency scan and organizer consume the
        // actual snapshot. Admit it first, including every registry's raw model.
        const auto first_source_node = budget.catalog_transfer.consumed_json_nodes;
        for (const auto& [id, owner] : owners) raw_entity(id, owner, budget);
        const auto source_nodes = budget.catalog_transfer.consumed_json_nodes - first_source_node;
        reserve_models(owners, budget);
        const auto phases = phase_inventory(owners);
        const auto organization = organize_project(owners);
        std::vector<std::string> all_catalogs;
        for (const auto& [id, owner] : owners) if (owner.type == "assembly_model") {
            all_catalogs.push_back(id);
            admit_complete_assembly_catalog_source(owner, budget.catalog_transfer);
        }
        // Complete source capture authenticates actual embedded host roles and
        // every direct catalog context before inverse host closure can use it.
        // All catalogs are raw-admitted before the first catalog model decoder.
        const auto catalogs = capture_complete_assembly_catalog_sources(source, all_catalogs, budget.catalog_transfer);
        std::map<std::string, References, std::less<>> refs;
        for (const auto& [id, catalog] : catalogs) refs.emplace(id, dependencies(catalog, budget));
        Ids selected; std::vector<std::string> pending;
        const auto include = [&](const std::string& id) {
            (void)actual(owners, id);
            if (selected.insert(id).second) pending.push_back(id);
        };
        for (const auto& id : seed_owner_ids) { identity(id); require(!selected.contains(id), "duplicate capture seed"); include(id); }
        // Cache full catalogs once. References include embedded persisted hosts;
        // models retain unused materials/types/parts and all saved instances.
        for (std::size_t index = 0; index < pending.size(); ++index) {
            // Reverse join/attachment discovery inspects raw typed fields of
            // ambient owners too. Reserve that complete scan before helpers
            // traverse any envelopes; repeated failed attempts stay charged.
            product_work(budget, source_nodes, 8);
            const auto id = pending[index]; const auto& owner = actual(owners, id);
            const auto member = phases.memberships.find(id);
            if (member != phases.memberships.end()) include(member->second);
            if (owner.type == "model_phases") for (const auto& roster : phases.models.at(id).entity_ids()) include(roster);
            auto dependencies_found = refs.find(id);
            if (dependencies_found == refs.end()) dependencies_found = refs.emplace(id, dependencies(owner, budget)).first;
            for (const auto& [dependency, role] : dependencies_found->second) { (void)actual(owners, dependency, role); include(dependency); }
            if (owner.type != "model_phases" && owner.type != "vertical_levels")
                for (const auto& context : context_owners(owner, owners, organization)) include(context);
            // Capture all actual openings/rails/joins attached to reached hosts,
            // including inactive children and their other complete registries.
            for (const auto& [other_id, other] : owners) {
                work(budget, 1);
                if (selected.contains(other_id)) continue;
                if (other.type == "opening" && other.properties.contains("wall_id") && reference(other.properties.at("wall_id")) == id) include(other_id);
                else if (other.type == "railing" && other.properties.contains("host") && reference(other.properties.at("host").at("stair_id")) == id) include(other_id);
                else if (other.type == "wall_join" || other.type == "roof_join") {
                    const auto reached = other.type == "wall_join" ? native_dxf_phase_auxiliary_source_dependencies(other) :
                        native_dxf_wall_source_dependency_ids(other);
                    if (std::find(reached.begin(), reached.end(), id) != reached.end()) include(other_id);
                }
                else if (body_type(owner.type) && other.type == "assembly_model") {
                    // Only embedded placement.host_entity_id establishes this
                    // reverse relationship; a shared drawing context does not.
                    for (const auto& instance : other.properties.at("model").at("instances"))
                        if (instance.contains("placement") && reference(instance.at("placement").at("host_entity_id")) == id) {
                            include(other_id); break;
                        }
                }
                if (!source_type(other.type) && unsupported_incoming(other, selected))
                    refuse("unsupported incoming graph dependency " + other_id + " (" + other.type + ")");
            }
        }
        NativeDxfPhaseSourceGraph result;
        for (const auto& id : selected) result.entities.emplace(id, owners.at(id));
        const auto captured_phases = phase_inventory(result.entities); roles(result, result.entities, captured_phases);
        reserve_models(result.entities, budget); semantic_graph(result, budget); return result;
    } catch (const Json::exception& error) { refuse(std::string("malformed captured source: ") + error.what()); }
}
} // namespace sketch
