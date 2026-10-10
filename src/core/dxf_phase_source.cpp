#include "sketch/dxf_phase_source.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/dxf_annotation_source.hpp"
#include "sketch/dxf_constraint_source.hpp"
#include "sketch/dxf_sheet_view_source.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/measurement_linework.hpp"
#ifdef SKETCH_PHYSICAL_ROOMS
#include "sketch/physical_wall_room.hpp"
#endif

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
    require(budget.destination_asset_replay_bytes <= native_dxf_phase_destination_asset_replay_byte_limit &&
        budget.destination_asset_replay_work <= native_dxf_phase_destination_asset_replay_work_limit,
        "invalid destination asset replay ledger");
    const auto& a = budget.phase_assets;
    require(a.max_payload_bytes <= native_dxf_phase_asset_payload_limit &&
        a.max_work_bytes <= native_dxf_phase_asset_work_limit &&
        a.max_json_bytes <= native_dxf_phase_asset_json_byte_limit &&
        a.max_json_nodes <= native_dxf_phase_asset_json_node_limit &&
        a.consumed_work_bytes <= a.max_work_bytes && a.consumed_json_bytes <= a.max_json_bytes &&
        a.consumed_json_nodes <= a.max_json_nodes, "invalid phase asset ledger");
}
void asset_work(NativeDxfWallSourceWorkBudget& budget, std::uint64_t amount) {
    budget_limits(budget);
    auto& a = budget.phase_assets;
    require(amount <= a.max_work_bytes - a.consumed_work_bytes, "cumulative phase asset work limit");
    a.consumed_work_bytes += amount;
}
// A validator admits all descriptor metadata and the complete physical
// inventory before hashing. Every later payload/metadata copy is reserved here.
void asset_pass(const NativeDxfPhaseSourceAssets& assets, NativeDxfWallSourceWorkBudget& budget,
    std::size_t copies = 0, bool destination_inventory = false) {
    const auto first = budget.phase_assets.consumed_json_bytes;
    if (destination_inventory) {
        NativeDxfPhaseSourceAssetRefs refs;
        for (const auto& [id, asset] : assets) refs.emplace(id, &asset);
        validate_native_dxf_phase_destination_asset_refs(refs, &budget.phase_assets);
    } else validate_native_dxf_phase_source_assets(assets, &budget.phase_assets);
    std::uint64_t bytes = budget.phase_assets.consumed_json_bytes - first;
    for (const auto& [id, asset] : assets)
        bytes += asset.bytes.size() + id.size() + asset.id.size() + asset.media_type.size() + asset.sha256.size();
    require(!copies || bytes <= budget.phase_assets.max_work_bytes / copies, "phase asset copy work limit");
    asset_work(budget, bytes * copies);
}
void destination_asset_pass(const NativeDxfPhaseSourceAssets& assets, NativeDxfWallSourceWorkBudget& budget,
    std::size_t copies = 0) {
    asset_pass(assets, budget, copies, true);
}
void work(NativeDxfWallSourceWorkBudget& budget, std::size_t amount) {
    auto& b = budget.catalog_transfer;
    charge(b.consumed_validation_work, amount, b.max_validation_work, "cumulative graph work limit");
}
void product_work(NativeDxfWallSourceWorkBudget& budget, std::size_t a, std::size_t b) {
    require(!b || a <= native_dxf_phase_source_work_limit / b, "graph work product limit");
    work(budget, a * b);
}
void map_text(const std::string& text, NativeDxfWallSourceWorkBudget& budget) {
    // Admit raw local names without a JSON temporary or the document-owner
    // identity contract. Original missing witnesses may be whitespace or long.
    require(text.size() <= native_dxf_phase_source_byte_limit / 6, "raw map string limit");
    auto& b = budget.catalog_transfer;
    charge(b.consumed_json_bytes, text.size() * 6 + 3, b.max_json_bytes, "cumulative raw map byte limit");
    charge(b.consumed_json_nodes, 1, b.max_json_nodes, "cumulative raw map node limit");
    work(budget, text.size() + 1);
}
void map_lookup_work(const std::string& text, std::size_t count, NativeDxfWallSourceWorkBudget& budget) {
    std::size_t steps = 1;
    while (count > 1) { count = (count + 1) / 2; ++steps; }
    product_work(budget, text.size() + 1, steps * 4);
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
bool support_type(std::string_view type) {
    return native_dxf_annotation_source_type(type) || native_dxf_constraint_source_type(type) ||
        native_dxf_sheet_view_source_type(type);
}
bool source_type(std::string_view type) {
    return body_type(type) || context_type(type) || support_type(type) || type == "assembly_model" || type == "model_phases";
}
bool corner_graph_semantics(const Owners& owners) {
    return std::any_of(owners.begin(), owners.end(), [](const auto& row) {
        const auto& owner = row.second;
        const auto& properties = owner.properties;
        if (owner.type == "corner_window" || properties.contains("corner_window_id") || properties.contains("corner_leg"))
            return true;
        if (can_recognize_boundary_dimension_entity_type(owner.type) && properties.contains("dimension_version") &&
            properties.at("dimension_version") == 5) return true;
        return native_dxf_sheet_view_source_type(owner.type) && properties.contains("model") &&
            properties.at("model").is_object() && properties.at("model").contains("version") &&
            properties.at("model").at("version") == 9;
    });
}
const Entity& actual(const Owners& owners, const std::string& id, std::string_view type = {}) {
    const auto found = owners.find(id);
    require(found != owners.end(), "missing source owner " + id);
    const bool role_matches = type.empty() || found->second.type == type ||
        (type == "boundary" && can_recognize_boundary_entity_type(found->second.type));
    require(found->second.id == id && role_matches, "source owner role/identity differs " + id);
    require(source_type(found->second.type), "unsupported graph dependency " + id + " (" + found->second.type + ")");
    return found->second;
}

using References = std::map<std::string, std::string, std::less<>>;
Ids asset_references(const Entity& owner) {
    Ids result;
    for (const auto* slot : {"asset_id", "render_asset_id"}) {
        if (owner.properties.contains(slot)) result.insert(reference(owner.properties.at(slot)));
        const auto plural = std::string(slot) + "s";
        if (owner.properties.contains(plural)) {
            const auto& values = owner.properties.at(plural);
            require(values.is_array(), "typed asset reference collection is not an array");
            for (const auto& value : values) result.insert(reference(value));
        }
    }
    return result;
}
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
References dependencies(const Entity& owner, const Owners& owners, NativeDxfWallSourceWorkBudget& budget) {
    // Catalogs follow Document's exact canonical reference contract. In
    // particular, unrelated wall_join_id/roof_join_id metadata on a catalog
    // does not acquire ownership merely because physical bodies use it.
    auto result = owner.type == "assembly_model" ? References{} : direct_references(owner);
    const auto add = [&](const std::string& id, const char* type = "") {
        identity(id);
        const auto [found, inserted] = result.emplace(id, type);
        require(inserted || !*type || found->second.empty() || found->second == type, "dependency role conflict " + id);
        if (*type) found->second = type;
    };
    // Document does not interpret phase_id/phase_binding/phase_registry_id or
    // an arbitrary phase_membership extension as roster authority. Preserve
    // that source metadata; actual ModelPhases registries alone enroll owners.
    if (owner.type == "assembly_model") {
        const auto refs = complete_assembly_catalog_source_refs(owner, budget.catalog_transfer,
            AssemblyCatalogSourceReferencePolicy::document_authoring);
        for (const auto& id : refs.hosted_entity_ids) add(id);
        for (const auto& [id, role] : refs.document_owner_roles) {
            const auto& target = actual(owners, id);
            require(role.empty() || target.type == role, "catalog canonical dependency role differs " + id);
            add(id, role.c_str());
        }
        for (const auto& [slot, role] : std::array<std::pair<const char*, const char*>, 4>{{
            {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}}}) {
            if (owner.properties.contains(slot)) {
                const auto id = reference(owner.properties.at(slot));
                const auto& target = actual(owners, id);
                require(target.type == role, "catalog context dependency role differs " + id);
                add(id, role);
            }
        }
        return result; // Other catalog fields and nested metadata stay opaque.
    } else if (body_type(owner.type)) {
        const bool corner_source = owner.type == "corner_window" || (owner.type == "opening" &&
            (owner.properties.contains("corner_window_id") || owner.properties.contains("corner_leg")));
        if (corner_source) {
            // Legacy V5-V8 helpers deliberately refuse coordinated corners.
            // Retain a cut's actual host through this graph's typed contract.
            if (owner.type == "opening") add(reference(owner.properties.at("wall_id")), "wall");
            for (const auto& id : native_dxf_corner_window_source_dependencies(owner))
                add(id, owner.type == "opening" ? "corner_window" : "");
        } else {
            for (const auto& id : native_dxf_wall_source_dependency_ids(owner)) add(id);
        }
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
    if (owner.properties.contains("wall_measurement_source"))
        for (const auto& wall : owner.properties.at("wall_measurement_source").at("walls"))
            for (const auto& [slot, value] : wall.at("context").items()) {
                // Legacy phase_id is retained observation metadata; it is
                // never interpreted as an actual registry owner by Document.
                if (slot != "phase_id") add(reference(value));
            }
    if (owner.properties.contains("appraisal_facts")) {
        const auto& facts = owner.properties.at("appraisal_facts");
        if (facts.contains("ansi") && facts.at("ansi").contains("ceiling")) {
            const auto& ceiling = facts.at("ansi").at("ceiling");
            if (ceiling.contains("stair_from_floor_id") && ceiling.at("stair_from_floor_id") != "")
                add(reference(ceiling.at("stair_from_floor_id")), "floor");
        }
    }
    if (has_phase_qualified_roof_join_ownership(owner))
        add(reference(owner.extensions.at(std::string(roof_join_phase_ownership_extension_key)).at("registry_id")), "model_phases");
    if (native_dxf_constraint_source_type(owner.type))
        for (const auto& [id, role] : native_dxf_constraint_source_dependencies(owner, &budget)) add(id, role.c_str());
    if (native_dxf_sheet_view_source_type(owner.type))
        for (const auto& [id, role] : native_dxf_sheet_view_source_dependencies(owner, owners, &budget)) add(id, role.c_str());
    if (native_dxf_annotation_source_type(owner.type)) {
        // Dependency discovery reads typed IDs, not artwork or annotation
        // codecs. Actual SVG/codec consumers are admitted at their call sites.
        if (owner.type == "annotation_state") {
            const auto& state = owner.properties.at("state");
            for (const auto* slot : {"labels", "symbols", "overrides"}) {
                const auto& rows = state.at(slot);
                require(rows.is_array() && rows.size() <= 100'000, "annotation reference inventory limit");
                product_work(budget, rows.size(), 1024);
            }
        } else work(budget, 1024);
        for (const auto& [id, role] : native_dxf_annotation_source_dependencies(owner, owners, &budget)) add(id, role.c_str());
    }
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
void reserve_models(const Owners& owners, NativeDxfWallSourceWorkBudget& budget,
    bool admit_support_consumers = false) {
    product_work(budget, owners.size(), owners.size() + 16);
    std::size_t maximum_level_cost = 0, bound_floors = 0;
    for (const auto& [id, owner] : owners) {
        (void)id;
        if (admit_support_consumers && native_dxf_annotation_source_type(owner.type))
            // Fresh phase import decodes during entity-change, typed proof and
            // final state admission; explicit Site frames add one decoder.
            admit_native_dxf_annotation_source_work(owner, owners, budget,
                owner.type == "annotation_state" && owner.properties.contains("presentation_frame") ? 4 : 3);
        if (admit_support_consumers && native_dxf_constraint_source_type(owner.type))
            admit_native_dxf_constraint_source_work(owner, owners, budget);
        if (admit_support_consumers && native_dxf_sheet_view_source_type(owner.type))
            admit_native_dxf_sheet_view_source_work(owner, budget, 3);
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
// Site-frame resolution uses organization, not phase-model replay. Admit only
// the organizer's actual inputs before building its index; unrelated artwork,
// catalogs and phase rosters do not acquire a decoder or quadratic reservation.
void reserve_frame_organization(const Owners& owners, NativeDxfWallSourceWorkBudget& budget) {
    require(owners.size() <= native_dxf_phase_source_owner_limit, "frame owner inventory limit");
    // This admission builds one index and the downstream resolver builds one.
    // Typed ancestry has four container levels; map/index/string copies and
    // malformed-reference diagnostics remain bounded per actual owner.
    product_work(budget, owners.size(), 1024);
    Ids level_models;
    for (const auto& [id, owner] : owners) {
        const auto start_bytes = budget.catalog_transfer.consumed_json_bytes;
        identity(id); require(id == owner.id && !owner.type.empty() && owner.type.size() <= 255 &&
            owner.properties.is_object(), "invalid frame organization owner");
        raw(Json(id), budget); raw(Json(owner.type), budget);
        for (const auto* key : {"name", "property_id", "building_id", "floor_id", "layer_id", "wall_id"})
            if (owner.properties.contains(key)) raw(owner.properties.at(key), budget);
        product_work(budget, budget.catalog_transfer.consumed_json_bytes - start_bytes, 4);
        if (owner.type != "floor" || !owner.properties.contains("vertical_level_binding")) continue;
        const auto& binding = owner.properties.at("vertical_level_binding");
        raw(binding, budget);
        // The organizer reports malformed legacy bindings as issues. Do not
        // turn an unrelated issue into a new frame-admission refusal.
        if (!binding.is_object() || !binding.contains("graph_id") || !binding.at("graph_id").is_string()) continue;
        const auto graph = owners.find(binding.at("graph_id").get_ref<const std::string&>());
        if (graph == owners.end() || graph->second.type != "vertical_levels" ||
            !graph->second.properties.is_object() || !graph->second.properties.contains("model")) continue;
        const auto& model = graph->second.properties.at("model");
        if (level_models.insert(graph->first).second) raw(model, budget);
        if (!model.is_object() || !model.contains("levels") || !model.contains("links") ||
            !model.at("levels").is_array() || !model.at("links").is_array()) continue;
        const auto count = model.at("levels").size() + model.at("links").size() + 1;
        // Each bound floor really decodes this graph in each organization
        // index. Connected-height lookup can scan the graph for every link.
        product_work(budget, count, count * 2);
    }
}

class FrameWorkAdmission {
public:
    FrameWorkAdmission(const Owners& owners, NativeDxfWallSourceWorkBudget& budget)
        : owners_(owners), budget_(budget) {
        reserve_frame_organization(owners_, budget_);
        organization_ = organize_project(owners_);
    }
    Ids resolve(const std::string& id) {
        if (const auto found = cached_.find(id); found != cached_.end()) {
            product_work(budget_, found->second.size(), 256); return found->second;
        }
        const auto& owner = target(id);
        require(visiting_.size() < 64 && visiting_.insert(id).second, "frame host/join cycle or depth limit");
        Ids result; add(result, id);
        std::string host;
        if (owner.type == "opening") {
            require(owner.properties.contains("wall_id"), "opening frame requires a wall host");
            host = reference(owner.properties.at("wall_id")); (void)target(host, "wall");
        } else if (owner.type == "railing" && owner.properties.contains("host")) {
            const auto& value = owner.properties.at("host");
            require(value.is_object() && value.contains("stair_id"), "malformed frame railing host");
            host = reference(value.at("stair_id")); (void)target(host, "stair");
        }
        if (!host.empty()) merge(result, resolve(host));
        else if (owner.type == "wall_join" || owner.type == "roof_join" || owner.type == "corner_window") {
            const bool wall = owner.type != "roof_join";
            const auto& members = owner.properties.at(wall ? "wall_ids" : "roof_ids");
            require(members.is_array() && members.size() >= 2 && members.size() <= limits_.maximum_join_members,
                "frame join member inventory limit");
            require(owner.type != "corner_window" || members.size() == 2, "frame corner host inventory differs");
            Ids distinct;
            for (const auto& member : members) {
                const auto member_id = reference(member);
                require(distinct.insert(member_id).second, "duplicate frame join member");
                (void)target(member_id, wall ? "wall" : "roof"); merge(result, resolve(member_id));
            }
        } else if (owner.type == "assembly_instance" && owner.properties.contains("assembly_catalog_id")) {
            const auto catalog = reference(owner.properties.at("assembly_catalog_id"));
            (void)target(catalog, "assembly_model"); add(result, catalog);
        }
        own_context(owner, result);
        cache(result); visiting_.erase(id);
        product_work(budget_, result.size(), 512); // cached set and returned copy
        cached_.emplace(id, result); return result;
    }
    Ids annotation(const SiteAnnotationTarget& requested) {
        const auto& owner = target(requested.owner_entity_id, "annotation_state");
        require(owner.properties.contains("presentation_frame"), "annotation frame target is unframed");
        auto found = children_.find(owner.id);
        if (found == children_.end()) {
            // One actual codec-owned admission per owner, including SVG work;
            // structural closure below never decodes annotation artwork.
            admit_native_dxf_annotation_source_work(owner, owners_, budget_);
            const auto& state = owner.properties.at("state");
            const auto count = state.at("labels").size() + state.at("symbols").size();
            require(count <= limits_.maximum_entities, "frame annotation child inventory limit");
            charge(indexed_children_, count, limits_.maximum_cached_dependency_entries, "frame annotation child index limit");
            product_work(budget_, count, 512);
            std::map<std::string, std::string, std::less<>> children;
            for (const auto* key : {"labels", "symbols"}) for (const auto& child : state.at(key)) {
                const auto child_id = reference(child.at("id"));
                const auto& placement = child.at("placement");
                const auto layer = placement.contains("layer_id") ? placement.at("layer_id").get<std::string>() : std::string{};
                require(children.emplace(child_id, layer).second, "duplicate frame annotation child");
            }
            found = children_.emplace(owner.id, std::move(children)).first;
        }
        const auto child = found->second.find(requested.child_id);
        require(child != found->second.end() && !child->second.empty(), "framed annotation child requires its own layer");
        Ids result; add(result, owner.id);
        context(target(child->second, "layer"), result);
        cache(result); product_work(budget_, result.size(), 512); return result;
    }
    void finish(const Ids& dependencies, std::size_t annotation_identity_bytes = 0) {
        std::size_t receipt_bytes = 0, envelope_bytes = 1024 + annotation_identity_bytes * 6;
        for (const auto& id : dependencies) {
            charge(receipt_bytes, receipts_.at(id), limits_.maximum_dependency_bytes, "frame dependency receipt byte limit");
            charge(envelope_bytes, id.size() * 6 + 256, limits_.maximum_dependency_bytes, "frame dependency envelope byte limit");
        }
        // Per requested result: cached-set copy, dependency vector and JSON
        // envelope serialization/hash. Entity receipt hashing is shared below.
        product_work(budget_, dependencies.size(), 512);
        product_work(budget_, envelope_bytes, 16);
    }
private:
    const Owners& owners_;
    NativeDxfWallSourceWorkBudget& budget_;
    SiteFrameLimits limits_;
    ProjectOrganization organization_;
    std::map<std::string, Ids, std::less<>> cached_;
    std::map<std::string, std::size_t, std::less<>> receipts_;
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> children_;
    Ids visiting_;
    std::size_t cached_entries_{}, indexed_children_{};
    const Entity& target(const std::string& id, std::string_view type = {}) const {
        identity(id);
        const auto found = owners_.find(id);
        require(found != owners_.end() && (type.empty() || found->second.type == type), "frame reference role/owner differs " + id);
        return found->second;
    }
    void add(Ids& result, const std::string& id) {
        const auto& owner = target(id);
        work(budget_, 256);
        result.insert(id);
        require(result.size() <= limits_.maximum_dependencies, "frame dependency inventory limit");
        if (!receipts_.contains(id)) {
            const auto start = budget_.catalog_transfer.consumed_json_bytes;
            raw_entity(id, owner, budget_);
            const auto bytes = budget_.catalog_transfer.consumed_json_bytes - start + 256;
            require(bytes <= limits_.maximum_dependency_bytes, "frame entity receipt byte limit");
            product_work(budget_, bytes, 16);
            receipts_.emplace(id, bytes);
        }
    }
    void merge(Ids& result, const Ids& other) { for (const auto& id : other) add(result, id); }
    void cache(const Ids& result) {
        charge(cached_entries_, result.size(), limits_.maximum_cached_dependency_entries, "frame cached dependency entry limit");
    }
    void context(const Entity& container, Ids& result) {
        const auto& value = organization_.nodes.at(container.id).context;
        for (const auto* id : {&value.property_id, &value.building_id, &value.floor_id, &value.layer_id})
            if (!id->empty()) add(result, *id);
        if (!value.floor_id.empty()) {
            const auto& floor = target(value.floor_id, "floor");
            if (floor.properties.contains("vertical_level_binding")) {
                const auto& binding = floor.properties.at("vertical_level_binding");
                const auto graph = reference(binding.at("graph_id"));
                (void)target(graph, "vertical_levels"); add(result, graph);
            }
        }
    }
    void own_context(const Entity& owner, Ids& result) {
        if (owner.type == "property" || owner.type == "building" || owner.type == "floor" || owner.type == "layer") {
            context(owner, result); return;
        }
        for (const auto& [key, type] : std::array<std::pair<const char*, const char*>, 4>{{
            {"layer_id", "layer"}, {"floor_id", "floor"}, {"building_id", "building"}, {"property_id", "property"}}})
            if (owner.properties.contains(key)) context(target(reference(owner.properties.at(key)), type), result);
    }
};

// Inspect only schema-owned topology/replay slots. Opaque extensions and
// property metadata are already charged linearly by raw(), never quadratically.
void reserve_typed_replay(const Json& value, NativeDxfWallSourceWorkBudget& budget,
    bool root_geometry_array = false, std::size_t copies = 8) {
    std::size_t edges = 0, passes = 1, growth = 0;
    const auto count_edges = [&](const Json& rows) {
        require(rows.is_array() && rows.size() <= 16'384, "typed topology row limit");
        edges = std::max(edges, rows.size()); work(budget, rows.size());
    };
    const auto visit = [&](const auto& self, const Json& item, std::size_t depth) -> void {
        require(depth <= native_dxf_phase_source_depth_limit, "typed replay depth limit");
        work(budget, 1);
        if (!item.is_object()) return;
        const auto kind = item.find("kind");
        if (kind != item.end() && *kind == "insert_vertex")
            charge(growth, 1, 16'384, "typed replay topology growth limit");
        for (const auto* slot : {"segments", "boundary", "edges", "replacement_segments"}) {
            const auto rows = item.find(slot);
            if (rows != item.end() && rows->is_array()) count_edges(*rows);
        }
        for (const auto* slot : {"operations", "transforms", "edits"}) {
            const auto rows = item.find(slot); if (rows == item.end()) continue;
            require(rows->is_array(), "typed replay operation inventory shape");
            charge(passes, rows->size(), native_dxf_phase_source_node_limit, "typed replay operation limit");
            for (const auto& row : *rows) self(self, row, depth + 1);
        }
        for (const auto* slot : {"value", "edit", "replacement_authoring", "source_boundary_authoring",
            "source_boundary", "source_lineage", "outer"}) {
            const auto nested = item.find(slot);
            if (nested == item.end()) continue;
            if (nested->is_array()) for (const auto& row : *nested) self(self, row, depth + 1);
            else self(self, *nested, depth + 1);
        }
        const auto holes = item.find("holes");
        if (holes != item.end() && holes->is_array()) for (const auto& hole : *holes) {
            if (hole.is_array()) count_edges(hole); else self(self, hole, depth + 1);
        }
    };
    if (root_geometry_array && value.is_array()) count_edges(value); else visit(visit, value, 0);
    // Insert/edit operations can grow the topology beyond its captured origin.
    // Each typed operation adds at most one edge unless a retained replacement
    // array above already declares a larger shape.
    require(passes <= 16'384 && edges <= 16'384 - growth, "typed replay topology growth limit");
    const auto maximum_edges = edges + growth;
    require(!maximum_edges || maximum_edges <= native_dxf_phase_source_work_limit / maximum_edges, "typed replay topology work limit");
    product_work(budget, maximum_edges * maximum_edges, passes * copies);
}
void reserve_intrinsic_owner(const Entity& owner, NativeDxfWallSourceWorkBudget& budget) {
    if (can_recognize_boundary_entity_type(owner.type)) {
        std::size_t total_edges = 0;
        for (const auto* slot : {"segments", "boundary"}) if (owner.properties.contains(slot))
            if (owner.properties.at(slot).is_array()) {
                total_edges = std::max(total_edges, owner.properties.at(slot).size());
                reserve_typed_replay(owner.properties.at(slot), budget, true);
            }
        if (owner.properties.contains("holes")) {
            require(owner.properties.at("holes").is_array(), "intrinsic boundary hole inventory shape");
            for (const auto& hole : owner.properties.at("holes")) {
                require(hole.is_array(), "intrinsic boundary hole shape");
                charge(total_edges, hole.size(), 16'384, "intrinsic boundary aggregate edge limit");
                reserve_typed_replay(hole, budget, true);
            }
            product_work(budget, total_edges, total_edges * 16);
        }
        for (const auto* slot : {"boundary_geometry_derivation", "boundary_identity_history"})
            if (owner.extensions.contains(slot)) reserve_typed_replay(owner.extensions.at(slot), budget);
        if (owner.properties.contains("boundary_authoring")) reserve_typed_replay(owner.properties.at("boundary_authoring"), budget);
    }
    if (owner.type == "measurement_linework") reserve_typed_replay(owner.properties.at("model"), budget);
    if (owner.extensions.contains("physical_wall_room")) {
        const auto& descriptor = owner.extensions.at("physical_wall_room");
        reserve_typed_replay(descriptor, budget);
        const auto& holes = descriptor.at("holes");
        require(holes.is_array(), "intrinsic physical room hole inventory shape");
        std::size_t edges = owner.properties.contains("segments") ? owner.properties.at("segments").size() : 0;
        for (const auto& hole : holes) {
            require(hole.is_array(), "intrinsic physical room hole shape");
            charge(edges, hole.size(), 16'384, "intrinsic physical room aggregate edge limit");
        }
        product_work(budget, edges, edges * 16);
    }
    if (owner.properties.contains("wall_measurement_source")) {
        const auto& source = owner.properties.at("wall_measurement_source");
        const auto& walls = source.at("walls");
        require(walls.is_array() && walls.size() <= 2048, "intrinsic wall measurement source inventory limit");
        const auto translations = source.find("translations");
        const auto passes = translations == source.end() ? 1 : translations->size() + 2;
        require(translations == source.end() || (translations->is_array() && translations->size() <= 4096), "intrinsic wall translation replay limit");
        product_work(budget, walls.size() * walls.size(), passes * 16);
    }
    if (owner.type == "wall") for (const auto* slot : {"wall_split_archive", "wall_merge_archive"})
        if (owner.extensions.contains(slot)) reserve_typed_replay(owner.extensions.at(slot), budget);
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
    if (support_type(owner.type)) {
        if (owner.type == "annotation_state" && owner.properties.contains("layer_id")) {
            // Scoped annotation envelopes promise one actual drawing context.
            // Children may independently name other layers; that does not make
            // a contradictory owner envelope a valid unscoped annotation.
            const auto& layer = actual(owners, reference(owner.properties.at("layer_id")), "layer");
            const auto node = organization.nodes.find(layer.id);
            require(node != organization.nodes.end() && node->second.issues.empty() &&
                node->second.context.complete(), "annotation source layer hierarchy unresolved " + owner.id);
            const auto& context = node->second.context;
            for (const auto& [slot, expected] : {
                std::pair{"property_id", &context.property_id},
                std::pair{"building_id", &context.building_id},
                std::pair{"floor_id", &context.floor_id}})
                require(owner.properties.contains(slot) && reference(owner.properties.at(slot)) == *expected,
                    "annotation source hierarchy conflicts " + owner.id);
            if (owner.properties.contains("level_id"))
                require(reference(owner.properties.at("level_id")) == context.level_id,
                    "annotation source floor level conflicts " + owner.id);
            return context;
        }
        return organization.drawing_context(owner.id);
    }
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
    if (owner.type == "corner_window")
        return native_dxf_corner_window_source_context(owner, owners, organization);
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
        else if (support_type(owner.type)) graph.support_ids.push_back(id);
        else refuse("unsupported source role " + id);
    }
    graph.enrolled_hierarchy_ids.assign(phases.enrolled_hierarchy.begin(), phases.enrolled_hierarchy.end());
}
void semantic_graph(const NativeDxfPhaseSourceGraph& graph, NativeDxfWallSourceWorkBudget& budget) {
    Ids reached_assets;
    for (const auto& [id, owner] : graph.entities) {
        (void)id;
        for (const auto& asset : asset_references(owner)) {
            require(graph.assets.contains(asset), "missing reached source asset " + asset);
            reached_assets.insert(asset);
        }
    }
    require(reached_assets.size() == graph.assets.size(), "source asset inventory has unreachable extras");
    const auto phases = phase_inventory(graph.entities);
    NativeDxfPhaseSourceGraph expected; roles(expected, graph.entities, phases);
    require(graph.body_ids == expected.body_ids && graph.catalog_ids == expected.catalog_ids &&
        graph.registry_ids == expected.registry_ids && graph.context_ids == expected.context_ids && graph.support_ids == expected.support_ids &&
        graph.enrolled_hierarchy_ids == expected.enrolled_hierarchy_ids && graph.depicted_body_ids == expected.depicted_body_ids,
        "role/subset inventory differs from actual source graph");
    for (const auto& [id, owner] : graph.entities) { (void)id; reserve_intrinsic_owner(owner, budget); }
    // Admit every complete catalog before the first semantic catalog decode.
    // Dependency identities/roles are proved before architectural consumers
    // can inspect actual hosts. The raw source remains the representation.
    for (const auto& id : graph.catalog_ids) admit_complete_assembly_catalog_source(graph.entities.at(id), budget.catalog_transfer,
        AssemblyCatalogSourceReferencePolicy::document_authoring);
    // Sheet dependency gathering can resolve an actual corner leg. Reserve the
    // complete owner/host/cut and saved-state passes before support consumers.
    for (const auto& id : graph.body_ids)
        admit_native_dxf_corner_window_source_work(graph.entities.at(id), graph.entities, budget);
    std::map<std::string, References, std::less<>> reached;
    for (const auto& [id, owner] : graph.entities) {
        require(!(owner.properties.contains("corner_window_id") || owner.properties.contains("corner_leg")) ||
            owner.type == "opening", "corner cut qualifier on wrong role " + id);
        auto refs = dependencies(owner, graph.entities, budget);
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
            if (node != organization.nodes.end() && owner.type != "assembly_model" && !support_type(owner.type))
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
        if (owner.type == "corner_window") validate_native_dxf_corner_window_source(owner, graph.entities);
        if (native_dxf_annotation_source_type(owner.type)) {
            admit_native_dxf_annotation_source_work(owner, graph.entities, budget);
            validate_native_dxf_annotation_source(owner, graph.entities, &budget);
        }
        if (native_dxf_sheet_view_source_type(owner.type))
            validate_native_dxf_sheet_view_source(owner, graph.entities, &budget);
        if (owner.extensions.contains(std::string(roof_join_phase_ownership_extension_key)))
            require(owner.type == "roof_join", "roof join qualifier on wrong role " + id);
    }
    for (const auto& id : graph.context_ids) require(needed_contexts.contains(id), "orphan context owner " + id);
    validate_roof_join_ownership(graph.entities);
    if (std::any_of(graph.support_ids.begin(), graph.support_ids.end(), [&](const auto& id) {
        return native_dxf_constraint_source_type(graph.entities.at(id).type);
    })) validate_native_dxf_constraint_source_graph(graph.entities, &budget);
}
void raw_graph(const NativeDxfPhaseSourceGraph& graph, NativeDxfWallSourceWorkBudget& budget) {
    budget_limits(budget);
    require(!graph.entities.empty() && graph.entities.size() <= native_dxf_phase_source_owner_limit, "source owner inventory limit");
    for (const auto& [id, owner] : graph.entities) raw_entity(id, owner, budget);
    require(graph.assets.size() <= native_dxf_phase_asset_count_limit, "source asset inventory limit");
    if (!graph.assets.empty()) {
        // Descriptors and owner JSON share the existing graph lane. The asset
        // codec's separate metadata ledger does not double graph capacity.
        charge(budget.catalog_transfer.consumed_json_bytes, 80 + graph.assets.size() * 90,
            budget.catalog_transfer.max_json_bytes, "combined graph manifest byte limit");
        charge(budget.catalog_transfer.consumed_json_nodes, 4 + graph.assets.size() * 5,
            budget.catalog_transfer.max_json_nodes, "combined graph manifest node limit");
        for (const auto& [id, asset] : graph.assets) {
            map_text(id, budget); map_text(asset.media_type, budget);
            map_text(asset.sha256, budget); raw(asset.metadata, budget);
        }
    }
    asset_pass(graph.assets, budget);
    for (const auto* ids : {&graph.body_ids, &graph.catalog_ids, &graph.registry_ids, &graph.context_ids, &graph.support_ids,
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
Json unchecked_encode(const NativeDxfPhaseSourceGraph& graph, NativeDxfWallSourceWorkBudget& budget) {
    Json entities = Json::array();
    for (const auto& [id, owner] : graph.entities) entities.push_back({{"id", id}, {"type", owner.type},
        {"properties", owner.properties}, {"required", owner.required}, {"extensions", owner.extensions}});
    const bool sheet_companions = std::any_of(graph.support_ids.begin(), graph.support_ids.end(), [&](const auto& id) {
        return native_dxf_sheet_view_source_type(graph.entities.at(id).type);
    });
    const bool corners = corner_graph_semantics(graph.entities);
    Json value{{"version", corners ? 5 : !graph.assets.empty() ? 4 : graph.support_ids.empty() ? 1 : sheet_companions ? 3 : 2}, {"entities", std::move(entities)}, {"body_ids", graph.body_ids},
        {"catalog_ids", graph.catalog_ids}, {"registry_ids", graph.registry_ids}, {"context_ids", graph.context_ids},
        {"enrolled_hierarchy_ids", graph.enrolled_hierarchy_ids}, {"depicted_body_ids", graph.depicted_body_ids}};
    if (!graph.support_ids.empty() || !graph.assets.empty() || corners) value["support_ids"] = graph.support_ids;
    if (!graph.assets.empty()) value["asset_manifest"] = encode_native_dxf_phase_asset_manifest(graph.assets, &budget.phase_assets);
    return value;
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

// JSON's ordinary numeric equality coalesces integer/float and signed zero.
// Binding equivalence must retain the raw numeric representation as well.
bool exact_raw(const Json& left, const Json& right) {
    if (left.type() != right.type() || left.size() != right.size()) return false;
    if (left.is_object()) {
        for (const auto& [key, value] : left.items()) {
            const auto other = right.find(key);
            if (other == right.end() || !exact_raw(value, *other)) return false;
        }
        return true;
    }
    if (left.is_array()) {
        for (std::size_t i = 0; i < left.size(); ++i) if (!exact_raw(left[i], right[i])) return false;
        return true;
    }
    if (left.is_number_float()) {
        const auto a = left.get<double>(), b = right.get<double>();
        return a == b && std::signbit(a) == std::signbit(b);
    }
    return left == right;
}
bool exact_owner(const Entity& left, const Entity& right) {
    return left.id == right.id && left.type == right.type && left.required == right.required &&
        exact_raw(left.properties, right.properties) && exact_raw(left.extensions, right.extensions);
}
bool exact_asset(const Asset& left, const Asset& right) {
    return left.id == right.id && left.media_type == right.media_type && left.sha256 == right.sha256 &&
        left.bytes == right.bytes && exact_raw(left.metadata, right.metadata);
}
bool exact_graph(const NativeDxfPhaseSourceGraph& left, const NativeDxfPhaseSourceGraph& right,
    NativeDxfWallSourceWorkBudget& budget) {
    asset_pass(left.assets, budget, 1); asset_pass(right.assets, budget, 1);
    if (left.entities.size() != right.entities.size() || left.body_ids != right.body_ids ||
        left.catalog_ids != right.catalog_ids || left.registry_ids != right.registry_ids ||
        left.context_ids != right.context_ids || left.support_ids != right.support_ids || left.enrolled_hierarchy_ids != right.enrolled_hierarchy_ids ||
        left.depicted_body_ids != right.depicted_body_ids || left.assets.size() != right.assets.size()) return false;
    for (const auto& [id, owner] : left.entities) {
        const auto found = right.entities.find(id);
        if (found == right.entities.end() || !exact_owner(owner, found->second)) return false;
    }
    for (const auto& [id, asset] : left.assets) {
        const auto found = right.assets.find(id);
        if (found == right.assets.end() || !exact_asset(asset, found->second)) return false;
    }
    return true;
}
NativeDxfPhaseOwnerMap admit_maps(const NativeDxfPhaseSourceGraph& source,
    const NativeDxfPhaseDestinationMaps& maps, NativeDxfWallSourceWorkBudget& budget) {
    NativeDxfPhaseOwnerMap owners; Ids targets, asset_targets;
    for (const auto* map : {&maps.body_owner_ids, &maps.catalog_owner_ids, &maps.registry_owner_ids,
        &maps.reviewed_context_owner_ids, &maps.support_owner_ids, &maps.stair_child_ids, &maps.asset_ids}) {
        require(map->size() <= native_dxf_phase_source_node_limit, "destination map inventory limit");
        for (const auto& [original, destination] : *map) {
            map_text(original, budget); map_text(destination, budget);
        }
    }
    for (const auto* scoped : {&maps.sheet_view_ids, &maps.annotation_child_ids, &maps.sheet_witness_ids}) {
        require(scoped->size() <= source.entities.size(), "destination map scope limit");
        for (const auto& [id, map] : *scoped) {
            map_text(id, budget);
            require(map.size() <= native_dxf_phase_source_node_limit, "destination scoped map inventory limit");
            for (const auto& [original, destination] : map) {
                map_text(original, budget); map_text(destination, budget);
            }
        }
    }
    const auto admit = [&](const auto& map, const auto& expected) {
        require(map.size() == expected.size(), "destination map must exactly cover its namespace");
        product_work(budget, map.size(), 4 * 256);
        for (const auto& id : expected) {
            const auto found = map.find(id); require(found != map.end(), "missing destination map owner " + id);
            identity(found->first); identity(found->second);
            raw(Json(found->first), budget); raw(Json(found->second), budget);
            require(targets.insert(found->second).second, "document destination maps collide " + found->second);
            require(owners.emplace(found->first, found->second).second, "source owner occurs in multiple map namespaces");
        }
    };
    admit(maps.body_owner_ids, source.body_ids); admit(maps.catalog_owner_ids, source.catalog_ids);
    admit(maps.registry_owner_ids, source.registry_ids); admit(maps.reviewed_context_owner_ids, source.context_ids);
    admit(maps.support_owner_ids, source.support_ids);
    require(maps.asset_ids.size() == source.assets.size(), "asset map must exactly cover source assets");
    // Admit every supplied string, including an extra key in an exact-sized
    // malformed map, before any identity or ordered-map comparison.
    for (const auto& [original, destination] : maps.asset_ids) {
        map_text(original, budget); map_text(destination, budget);
        identity(original); identity(destination);
    }
    for (const auto& [id, asset] : source.assets) {
        (void)asset;
        const auto found = maps.asset_ids.find(id);
        require(found != maps.asset_ids.end(), "missing asset mapping " + id);
        require(!source.assets.contains(found->second) && !source.entities.contains(found->second),
            "asset target is not fresh " + found->second);
        require(targets.insert(found->second).second, "asset target collides with an allocated owner or asset " + found->second);
        asset_targets.insert(found->second);
    }
    if (!maps.sheet_view_ids.empty()) {
        Ids companion_owners;
        for (const auto& id : source.support_ids)
            if (native_dxf_sheet_view_source_type(source.entities.at(id).type)) companion_owners.insert(id);
        require(maps.sheet_view_ids.size() == companion_owners.size(), "view maps must exactly cover source companions");
        Ids view_targets;
        for (const auto& id : companion_owners) {
            const auto found = maps.sheet_view_ids.find(id);
            require(found != maps.sheet_view_ids.end(), "missing sheet/view companion map " + id);
            const auto views = native_dxf_sheet_view_source_view_identity_ids(source.entities.at(id), &budget);
            require(found->second.size() == views.size(), "companion view map must be exact");
            product_work(budget, views.size(), 4 * 256);
            for (const auto& view : views) {
                const auto mapped = found->second.find(view);
                require(mapped != found->second.end(), "missing companion view identity " + view);
                require(mapped->second.size() <= native_dxf_phase_source_byte_limit / 6, "mapped local view string limit");
                auto& ledger = budget.catalog_transfer;
                charge(ledger.consumed_json_bytes, mapped->second.size() * 6 + 3,
                    ledger.max_json_bytes, "cumulative mapped local view byte limit");
                charge(ledger.consumed_json_nodes, 1, ledger.max_json_nodes, "cumulative mapped local view node limit");
                work(budget, mapped->second.size() + 1);
                require(!mapped->second.empty() && !std::all_of(mapped->second.begin(), mapped->second.end(),
                    [](unsigned char c) { return std::isspace(c); }), "blank mapped local view identity");
                require(view_targets.insert(mapped->second).second, "mapped views collide across companions " + mapped->second);
            }
        }
    }
    if (!maps.annotation_child_ids.empty()) {
        Ids annotation_owners;
        for (const auto& id : source.support_ids)
            if (source.entities.at(id).type == "annotation_state") annotation_owners.insert(id);
        require(maps.annotation_child_ids.size() == annotation_owners.size(), "annotation child maps must exactly cover source states");
        Ids child_targets = targets;
        for (const auto& id : annotation_owners) {
            const auto found = maps.annotation_child_ids.find(id);
            require(found != maps.annotation_child_ids.end(), "missing annotation child map " + id);
            const auto children = native_dxf_annotation_child_identity_ids(source.entities.at(id));
            require(found->second.size() == children.size(), "annotation child map must be exact");
            product_work(budget, children.size(), 4 * 256);
            for (const auto& child : children) {
                const auto mapped = found->second.find(child);
                require(mapped != found->second.end(), "missing annotation child identity " + child);
                identity(mapped->second); raw(Json(mapped->second), budget);
                require(child_targets.insert(mapped->second).second, "annotation destination identity collision " + mapped->second);
            }
        }
    }
    Ids children;
    for (const auto& id : source.body_ids) for (const auto& child : native_dxf_architectural_child_identity_ids(source.entities.at(id)))
        require(children.insert(child).second, "ambiguous stair-local child map key " + child);
    require(maps.stair_child_ids.size() == children.size(), "stair child mapping must be exact and complete");
    Ids child_targets;
    product_work(budget, children.size(), 4 * 256);
    for (const auto& child : children) {
        const auto found = maps.stair_child_ids.find(child);
        require(found != maps.stair_child_ids.end(), "missing stair child mapping " + child);
        identity(found->first); identity(found->second); raw(Json(found->first), budget); raw(Json(found->second), budget);
        require(child_targets.insert(found->second).second, "stair child map collapses children");
        map_lookup_work(found->second, asset_targets.size(), budget);
        require(!asset_targets.contains(found->second), "asset target collides with allocated stair identity");
        // A child remains local even when it spells a document identity. No
        // string-based owner substitution is ever applied in that namespace.
    }
    for (const auto* scoped : {&maps.sheet_view_ids, &maps.annotation_child_ids})
        for (const auto& [id, scope] : *scoped) {
            map_text(id, budget);
            for (const auto& [original, destination] : scope) {
                map_text(original, budget); map_text(destination, budget);
                map_lookup_work(destination, asset_targets.size(), budget);
                require(!asset_targets.contains(destination), "asset target collides with allocated local identity");
            }
        }
    if (!maps.sheet_witness_ids.empty()) {
        // Admit ALL supplied raw strings before comparisons, including unknown
        // keys in malformed exact-sized scopes. No owner limit applies to the
        // inner original/mapped names and no unresolved name becomes an owner.
        auto& ledger = budget.catalog_transfer;
        require(maps.sheet_witness_ids.size() <= source.support_ids.size(), "extra witness companion scopes");
        charge(ledger.consumed_json_nodes, maps.sheet_witness_ids.size() + 1,
            ledger.max_json_nodes, "cumulative witness scope node limit");
        work(budget, maps.sheet_witness_ids.size());
        for (const auto& [id, scope] : maps.sheet_witness_ids) {
            map_text(id, budget); identity(id);
            require(scope.size() <= native_dxf_phase_source_node_limit, "witness map size limit");
            charge(ledger.consumed_json_nodes, scope.size() + 1,
                ledger.max_json_nodes, "cumulative witness map node limit");
            work(budget, scope.size());
            for (const auto& [original, destination] : scope) {
                map_text(original, budget); map_text(destination, budget);
                require(!destination.empty() && !std::all_of(destination.begin(), destination.end(),
                    [](unsigned char c) { return std::isspace(c); }), "blank mapped unresolved witness identity");
            }
        }
        // Refer to admitted strings instead of copying long local identities.
        // Other allocated selection/view/stair names must remain distinct even
        // when those local namespaces do not create actual document owners.
        std::set<std::string_view, std::less<>> allocated_names;
        const auto reserve_name = [&](const std::string& name) {
            map_text(name, budget);
            map_lookup_work(name, allocated_names.size(), budget);
            allocated_names.insert(name);
        };
        for (const auto& name : targets) reserve_name(name);
        for (const auto* scoped : {&maps.sheet_view_ids, &maps.annotation_child_ids})
            for (const auto& [id, scope] : *scoped) {
                (void)id;
                for (const auto& [original, destination] : scope) { (void)original; reserve_name(destination); }
            }
        for (const auto& [original, destination] : maps.stair_child_ids) { (void)original; reserve_name(destination); }
        std::size_t companion_count = 0;
        std::set<std::string_view, std::less<>> witness_targets;
        for (const auto& id : source.support_ids) {
            const auto& owner = source.entities.at(id);
            if (!native_dxf_sheet_view_source_type(owner.type)) continue;
            ++companion_count;
            map_lookup_work(id, maps.sheet_witness_ids.size(), budget);
            const auto scope = maps.sheet_witness_ids.find(id);
            require(scope != maps.sheet_witness_ids.end(), "missing sheet witness companion map " + id);
            const auto witnesses = native_dxf_sheet_view_source_unresolved_witness_ids(owner, source.entities, &budget);
            require(scope->second.size() == witnesses.size(), "companion witness map must be exact");
            for (const auto& original : witnesses) {
                map_lookup_work(original, scope->second.size(), budget);
                const auto mapped = scope->second.find(original);
                require(mapped != scope->second.end(), "missing companion unresolved witness identity");
                const auto& destination = mapped->second;
                map_lookup_work(destination, source.entities.size(), budget);
                require(!source.entities.contains(destination), "mapped unresolved witness names an actual source owner");
                map_lookup_work(destination, allocated_names.size(), budget);
                require(!allocated_names.contains(std::string_view(destination)),
                    "mapped unresolved witness collides with an allocated identity");
                map_lookup_work(destination, witness_targets.size(), budget);
                require(witness_targets.insert(destination).second, "mapped unresolved witnesses collide across companions");
            }
        }
        require(maps.sheet_witness_ids.size() == companion_count, "witness maps must exactly cover source companions");
    }
    return owners;
}
void patch_direct_refs(Entity& result, const Entity& original, const NativeDxfPhaseOwnerMap& owners) {
    for (const auto* slot : {"assembly_catalog_id", "property_id", "building_id", "floor_id", "layer_id",
        "boundary_id", "wall_id", "opening_id", "room_id", "slab_id", "roof_id", "stair_id", "sheet_id",
        "view_id", "constraint_id", "label_id", "column_id", "beam_id", "railing_id", "wall_join_id",
        "roof_join_id", "terrain_surface_id", "parent_id", "host_id", "target_id", "entity_id", "source_entity_id"}) {
        const auto singular = original.properties.find(slot);
        if (singular != original.properties.end()) result.properties.at(slot) = owners.at(reference(*singular));
        const auto plural = std::string(slot) + "s";
        const auto rows = original.properties.find(plural);
        if (native_dxf_constraint_source_type(original.type) && (plural == "wall_ids" || plural == "entity_ids"))
            continue; // Its typed codec already remapped and ordered this inventory.
        if (rows != original.properties.end()) for (std::size_t i = 0; i < rows->size(); ++i)
            result.properties.at(plural)[i] = owners.at(reference((*rows)[i]));
    }
    for (const auto* slot : {"refs", "references"}) if (original.properties.contains(slot))
        for (std::size_t i = 0; i < original.properties.at(slot).size(); ++i)
            result.properties.at(slot)[i] = owners.at(reference(original.properties.at(slot)[i]));
}
Entity remap_wall_measurement(const Entity& source, const NativeDxfPhaseDestinationMaps& maps) {
    if (!source.properties.contains("wall_measurement_source")) return source;
    (void)exterior_wall_measurement_source_ids(source);
    auto working = source;
    std::map<std::string, Json, std::less<>> phases;
    for (const auto& row : source.properties.at("wall_measurement_source").at("walls")) {
        const auto phase = row.at("context").find("phase_id");
        if (phase == row.at("context").end()) continue;
        phases.emplace(maps.body_owner_ids.at(reference(row.at("id"))), *phase);
    }
    for (auto& row : working.properties.at("wall_measurement_source").at("walls")) row.at("context").erase("phase_id");
    // Remove only the inert observation from the private helper input. It must
    // never participate in that helper's document-target injectivity proof.
    auto result = remap_exterior_wall_measurement_source_references(working, maps.body_owner_ids, maps.reviewed_context_owner_ids);
    for (auto& row : result.properties.at("wall_measurement_source").at("walls")) {
        const auto phase = phases.find(reference(row.at("id")));
        if (phase != phases.end()) row.at("context")["phase_id"] = phase->second;
    }
    (void)exterior_wall_measurement_source_ids(result);
    return result;
}
NativeDxfPhaseSourceGraph mapped_source(const NativeDxfPhaseSourceGraph& source,
    const NativeDxfPhaseDestinationMaps& maps, NativeDxfWallSourceWorkBudget& budget) {
    raw_graph(source, budget); semantic_graph(source, budget);
    const auto owners = admit_maps(source, maps, budget);
    NativeDxfPhaseSourceGraph result;
    for (const auto& [id, original] : source.entities) {
        // These active typed transport proofs require their own source tables.
        // Never retain stale pending/bound V2-V8 transfer state as live owners.
        for (const auto* slot : {"vertex_dxf_boundary", "vertex_dxf_wall_source_context_binding",
            "vertex_dxf_stair_floor_binding", "vertex_dxf_physical_source_graph", "vertex_dxf_measured_graph",
            "vertex_dxf_wall_source_hosted_openings", "vertex_dxf_resolved_context"})
            require(!original.extensions.contains(slot), "unsupported retained transport proof " + id + ": " + slot);
        Entity owner = can_recognize_boundary_entity_type(original.type) && owners.at(id) != id
            ? remap_boundary_owner_identity(original, owners.at(id)) : original;
        if (original.type == "assembly_model") owner = remap_complete_assembly_catalog_source_refs(
            original, maps.catalog_owner_ids, maps.body_owner_ids, maps.reviewed_context_owner_ids, budget.catalog_transfer,
            AssemblyCatalogSourceReferencePolicy::document_authoring, owners, maps.asset_ids);
        else if (original.type == "assembly_instance") owner = remap_native_dxf_independent_assembly_source(
            original, maps.body_owner_ids, maps.catalog_owner_ids);
        else owner.id = owners.at(id);
        if (native_dxf_constraint_source_type(original.type)) {
            owner = remap_native_dxf_constraint_source_dependencies(original, owners, {}, {}, &budget);
            owner.id = owners.at(id);
        }
        if (native_dxf_annotation_source_type(original.type)) {
            owner = original;
            remap_native_dxf_annotation_source_dependencies(owner, source.entities, owners,
                maps.reviewed_context_owner_ids, maps.annotation_child_ids, maps.sheet_view_ids, &budget);
            owner.id = owners.at(id);
        }
        if (native_dxf_sheet_view_source_type(original.type)) {
            owner = original;
            remap_native_dxf_sheet_view_source_dependencies(owner, source.entities, owners,
                maps.reviewed_context_owner_ids, maps.sheet_view_ids, &budget, maps.sheet_witness_ids);
            owner.id = owners.at(id);
        }
        if (original.type == "measurement_linework")
            owner.properties.at("model") = remap_measurement_linework_owner_identity(original.properties.at("model"), owner.id);
        if (body_type(original.type)) {
            if (original.type == "corner_window" || (original.type == "opening" &&
                (original.properties.contains("corner_window_id") || original.properties.contains("corner_leg"))))
                remap_native_dxf_corner_window_source_dependencies(owner, maps.body_owner_ids);
            owner = remap_architectural_material_source_refs(owner, maps.catalog_owner_ids);
            const Entity* host = nullptr;
            if (original.type == "railing" && original.properties.contains("host")) {
                host = &source.entities.at(reference(original.properties.at("host").at("stair_id")));
                remap_native_dxf_architectural_host_body_aliases(owner, *host, maps.body_owner_ids);
            }
            if (native_dxf_architectural_source_type(original.type)) {
                remap_native_dxf_architectural_source_dependencies(owner, maps.body_owner_ids);
                remap_native_dxf_architectural_source_context_dependencies(owner, maps.reviewed_context_owner_ids);
            }
            if (native_dxf_phase_auxiliary_source_type(original.type))
                remap_native_dxf_phase_auxiliary_source_dependencies(owner, maps.body_owner_ids);
            remap_native_dxf_architectural_child_identities(owner, maps.stair_child_ids, host);
            owner = remap_measurement_linework_source_references(owner, maps.body_owner_ids);
            owner = remap_wall_measurement(owner, maps);
            if (owner.extensions.contains("physical_wall_room")) {
#ifdef SKETCH_PHYSICAL_ROOMS
                owner = remap_physical_wall_room_source_references(owner, maps.body_owner_ids,
                    maps.reviewed_context_owner_ids, maps.registry_owner_ids);
#else
                refuse("physical room runtime unavailable");
#endif
            }
        }
        if (original.type != "assembly_model") patch_direct_refs(owner, original, owners);
        if (original.type != "assembly_model") for (const auto* slot : {"asset_id", "render_asset_id"}) {
            if (original.properties.contains(slot)) owner.properties.at(slot) = maps.asset_ids.at(reference(original.properties.at(slot)));
            const auto plural = std::string(slot) + "s";
            if (original.properties.contains(plural))
                for (std::size_t i = 0; i < original.properties.at(plural).size(); ++i)
                    owner.properties.at(plural)[i] = maps.asset_ids.at(reference(original.properties.at(plural)[i]));
        }
        const auto patch = [&](Json& value) { if (value != "") value = owners.at(reference(value)); };
        if (original.type != "assembly_model" && owner.properties.contains("deduction_ids"))
            for (auto& value : owner.properties.at("deduction_ids")) patch(value);
        if (original.type != "assembly_model" && owner.properties.contains("appraisal_facts")) {
            auto& facts = owner.properties.at("appraisal_facts");
            if (facts.contains("ansi") && facts.at("ansi").contains("ceiling")) {
                auto& ceiling = facts.at("ansi").at("ceiling");
                if (ceiling.contains("below_5ft_deduction_ids")) for (auto& value : ceiling.at("below_5ft_deduction_ids")) patch(value);
                for (const auto* slot : {"room_boundary_id", "stair_from_floor_id"}) if (ceiling.contains(slot)) patch(ceiling.at(slot));
            }
        }
        if (original.type == "floor" && owner.properties.contains("vertical_level_binding"))
            patch(owner.properties.at("vertical_level_binding").at("graph_id"));
        if (original.type == "model_phases") {
            NativeDxfPhaseOwnerMap roster;
            const auto phase_model = ModelPhases::from_json(original.properties.at("model"));
            for (const auto& member : phase_model.entity_ids())
                roster.emplace(member, owners.at(member));
            owner.properties.at("model") = remap_model_phase_owner_ids(original.properties.at("model"), roster);
        }
        if (has_phase_qualified_roof_join_ownership(original))
            owner.extensions.at(std::string(roof_join_phase_ownership_extension_key)).at("registry_id") =
                maps.registry_owner_ids.at(reference(original.extensions.at(std::string(roof_join_phase_ownership_extension_key)).at("registry_id")));
        const auto mapped_id = owner.id;
        require(result.entities.emplace(mapped_id, std::move(owner)).second, "mapped owner collision");
    }
    asset_pass(source.assets, budget, 1);
    for (const auto& [id, asset] : source.assets) {
        auto copy = asset;
        copy.id = maps.asset_ids.at(id);
        const auto mapped_id = copy.id;
        require(result.assets.emplace(mapped_id, std::move(copy)).second, "mapped asset collision");
    }
    const auto map_role = [&](const auto& ids) {
        std::vector<std::string> mapped; mapped.reserve(ids.size());
        for (const auto& id : ids) mapped.push_back(owners.at(id));
        std::sort(mapped.begin(), mapped.end()); return mapped;
    };
    result.body_ids = map_role(source.body_ids); result.catalog_ids = map_role(source.catalog_ids);
    result.registry_ids = map_role(source.registry_ids); result.context_ids = map_role(source.context_ids);
    result.support_ids = map_role(source.support_ids);
    result.enrolled_hierarchy_ids = map_role(source.enrolled_hierarchy_ids); result.depicted_body_ids = map_role(source.depicted_body_ids);
    for (const auto& id : source.support_ids)
        if (native_dxf_sheet_view_source_type(source.entities.at(id).type))
            validate_native_dxf_sheet_view_witness_binding(source.entities.at(id), source.entities, result.entities,
                &budget, maps.sheet_witness_ids);
    raw_graph(result, budget); semantic_graph(result, budget);
    return result;
}
void admit_assets(const std::map<std::string, Asset, std::less<>>& assets, NativeDxfWallSourceWorkBudget& budget) {
    // Each actual inventory retains native physical capacity independently.
    // Fork/preview payload copies and validation hashes share the binary lane.
    destination_asset_pass(assets, budget, 4);
}
// Same complete dynamic-field inventory used by mixed phase demolition's
// retained-history admission. Only raw fields are visited here; no command
// codec, digest, model decoder or historical geometry replay runs first.
struct HistoryAdmission {
    NativeDxfWallSourceWorkBudget& budget;
    void fixed(std::size_t n) {
        charge(budget.catalog_transfer.consumed_json_bytes, n, budget.catalog_transfer.max_json_bytes, "history proof byte limit");
        work(budget, n);
    }
    void text(const std::string& s) { raw(Json(s), budget); }
    void read(const std::string& s) { text(s); }
    void read(const Json& value) { raw(value, budget); }
    template<class T> void optional(const std::optional<T>& value) { if (value) { fixed(sizeof(T)); read(*value); } }
    template<class T> void sequence(const std::vector<T>& values) {
        require(values.size() <= native_dxf_phase_source_node_limit, "history proof row limit");
        product_work(budget, values.size(), 32);
        for (const auto& value : values) { fixed(sizeof(T)); read(value); }
    }
    void read(const Entity& value) {
        text(value.id); text(value.type); read(value.properties); read(value.extensions); reserve_intrinsic_owner(value, budget);
    }
    void read(const Asset& value) {
        map_text(value.id, budget);
        const NativeDxfPhaseSourceAssetRefs refs{{value.id, &value}};
        const auto first = budget.phase_assets.consumed_json_bytes;
        validate_native_dxf_phase_destination_asset_refs(refs, &budget.phase_assets);
        const auto bytes = budget.phase_assets.consumed_json_bytes - first + value.id.size() +
            value.media_type.size() + value.sha256.size() + value.bytes.size();
        require(bytes <= budget.phase_assets.max_work_bytes / 4, "history asset copy work limit");
        asset_work(budget, bytes * 4);
    }
    void read(const EntityChange& value) { text(value.entity_id); read(value.entity); }
    void read(const AssetChange& value) {
        text(value.asset_id);
        if (value.kind == AssetChangeKind::upsert) read(value.asset);
    }
    void read(const PhaseEntityImportProof& value) {
        fixed(256); text(value.message);
        sequence(value.registry_ids); sequence(value.entity_ids); sequence(value.asset_ids);
        sequence(value.reviewed_existing_hierarchy_ids);
    }
    void read(const Quantity& value) { text(value.original_expression); }
    void read(const AngleInput& value) { text(value.original_expression); text(value.normalized_expression); }
    void read(const ConstructionReceipt& value) {
        text(value.segment_id);
        if (value.chord_input) { read(value.chord_input->length); read(value.chord_input->heading); }
        optional(value.distance); optional(value.heading); optional(value.rise); optional(value.run); optional(value.turn);
        optional(value.angle); optional(value.height); optional(value.arc_length); optional(value.tangent); optional(value.sweep);
    }
    void read(const PhysicalWallRoomRepairIntent& value) {
        text(value.selected_wall_id); text(value.expected_descriptor_digest); read(value.reviewed_source_lineage);
    }
    void read(const BoundaryGeometryEdit& value) {
        text(value.boundary_id); text(value.target_id); text(value.new_vertex_id); text(value.new_segment_id); text(value.new_dimension_id);
        read(value.replacement_segments); read(value.replacement_authoring); read(value.replacement_properties);
        read(value.replacement_child_mapping); sequence(value.replacement_dimension_ids); sequence(value.replacement_removed_reference_ids);
        sequence(value.replacement_wall_source_ids); optional(value.arc_construction); optional(value.replacement_linework_sources);
        optional(value.physical_wall_room_repair); optional(value.wall_source_translation);
        reserve_typed_replay(value.replacement_segments, budget, true);
        reserve_typed_replay(value.replacement_authoring, budget);
        if (value.physical_wall_room_repair) reserve_typed_replay(value.physical_wall_room_repair->reviewed_source_lineage, budget);
    }
    void read(const BoundaryTranslation& value) { text(value.boundary_id); }
    void read(const BoundaryTransformation& value) { text(value.boundary_id); }
    void read(const RigidOwnerTransformation& value) { text(value.owner_id); }
    void read(const TranslateBoundaries& value) { text(value.message); sequence(value.translations); sequence(value.entity_changes); }
    void read(const TransformBoundaries& value) {
        text(value.message); sequence(value.transformations); sequence(value.entity_changes); sequence(value.source_transformations);
    }
    void read(const ConstraintWallGeometryEdit& value) {
        text(value.wall_id); optional(value.length_entry); optional(value.curve_construction); optional(value.wall_classification);
    }
    void read(const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& value) {
        text(value.stroke_id); optional(value.authored_edit); optional(value.authored_length); sequence(value.vertex_edits);
    }
    void read(const ApplyBoundaryConstraintChanges::DimensionPlacementMove& value) { text(value.dimension_id); }
    void read(const ExteriorCornerMoveIntent& value) { text(value.boundary_id); text(value.vertex_id); }
    void read(const ExteriorSegmentResizeIntent& value) { text(value.boundary_id); text(value.segment_id); read(value.exact_length); }
    void read(const ExteriorSegmentArcIntent& value) { text(value.boundary_id); text(value.segment_id); read(value.arc_construction); }
    void read(const WallSplitMeasuredOwnerIds& value) {
        text(value.boundary_id); text(value.vertex_id); text(value.segment_id); text(value.automatic_dimension_id);
    }
    void read(const WallSplitPhysicalRoomIds& value) { text(value.boundary_id); sequence(value.new_segment_ids); sequence(value.new_vertex_ids); }
    void read(const WallSplitIntent& value) {
        text(value.wall_id); text(value.second_wall_id); text(value.seam_constraint_id); sequence(value.measured_owners); sequence(value.physical_room_owners);
    }
    void read(const WallMergeIntent& value) { text(value.first_wall_id); text(value.second_wall_id); }
    void read(const JointAnnotationTranslationIntent& value) { text(value.owner_id); text(value.child_id); }
    void read(const JointReferenceTranslationIntent& value) { text(value.reference_id); }
    void read(const JointOwnerTranslationIntent& value) { text(value.owner_id); }
    void read(const JointTranslationIntent& value) {
        sequence(value.rigid_boundary_ids); sequence(value.rigid_stroke_ids); sequence(value.partial_wall_ids); sequence(value.dimension_ids);
        sequence(value.annotation_translations); sequence(value.reference_translations); sequence(value.owner_translations);
        sequence(value.dimension_translations); sequence(value.owner_transformations);
    }
    void read(const DistoMeasurementAttachment& value) {
        text(value.owner_id); const auto& row = value.record;
        text(row.reading_id); text(row.target_field); text(row.unit); text(row.captured_at); text(row.model);
        text(row.firmware); text(row.transport); text(row.provenance);
    }
    void read(const ApplyBoundaryConstraintChanges& value) {
        text(value.message); sequence(value.boundary_edits); sequence(value.wall_edits); sequence(value.entity_changes);
        sequence(value.physical_entity_changes); sequence(value.exterior_source_edits); sequence(value.supplemental_entity_changes);
        sequence(value.supplemental_asset_changes); sequence(value.measured_stroke_edits); sequence(value.dimension_placement_moves);
        sequence(value.selection_entity_changes); sequence(value.room_review_additional_intents);
        optional(value.exterior_corner_move); optional(value.exterior_segment_resize); optional(value.exterior_segment_arc);
        optional(value.wall_split); optional(value.wall_merge); optional(value.rigid_group_transform); optional(value.joint_translation);
        optional(value.disto_measurement); read(value.room_review_intent); read(value.room_review_geometry_proof);
        read(value.phase_room_review_intent); read(value.phase_constraint_authoring_intent); read(value.independent_drawing_removal_intent);
        // These understood proof envelopes can retain region topology and
        // ordered geometry operations. Their opaque metadata stays linear.
        reserve_typed_replay(value.room_review_intent, budget);
        reserve_typed_replay(value.room_review_geometry_proof, budget);
        reserve_typed_replay(value.phase_room_review_intent, budget);
        reserve_typed_replay(value.phase_constraint_authoring_intent, budget);
    }
};
void admit_destination_owners(const Owners& owners, NativeDxfWallSourceWorkBudget& budget,
    bool admit_support_consumers = true) {
    require(owners.size() <= native_dxf_phase_source_owner_limit, "actual destination owner limit");
    for (const auto& [id, owner] : owners) raw_entity(id, owner, budget);
    // Intrinsic topology/receipt admission is distinct from physical room
    // detection. Ordinary wall states and opaque metadata have linear cost.
    for (const auto& [id, owner] : owners) { (void)id; reserve_intrinsic_owner(owner, budget); }
    reserve_models(owners, budget, admit_support_consumers);
    for (const auto& [id, owner] : owners) {
        (void)id;
        if (owner.type == "assembly_model") admit_existing_assembly_catalog_work(owner, budget.catalog_transfer);
        if (body_type(owner.type)) {
            admit_native_dxf_architectural_source_work(owner, owners, budget);
            admit_native_dxf_phase_auxiliary_source_work(owner, owners, budget);
            admit_native_dxf_corner_window_source_work(owner, owners, budget);
        }
    }
}
void reserve_physical_room_checks(const Owners& owners, NativeDxfWallSourceWorkBudget& budget) {
    // All raw/model/intrinsic inputs must already have been admitted by the
    // current-state admission entry point. Only active physical owners can
    // reach DetectionCache; inactive room checks return before decoding.
    const auto phases = phase_inventory(owners);
    std::size_t walls = 0, rooms = 0, source_uses = 0, retained_edges = 0;
    for (const auto& [id, owner] : owners) {
        if (phases.inactive.contains(id)) continue;
        if (owner.type == "wall") ++walls;
        if (!owner.extensions.contains("physical_wall_room")) continue;
        ++rooms;
        const auto& descriptor = owner.extensions.at("physical_wall_room");
        const auto& sources = descriptor.at("source_lineage").at("physical_sources");
        require(sources.is_array(), "active physical source inventory shape");
        charge(source_uses, sources.size(), native_dxf_phase_source_node_limit, "active physical source-use limit");
        if (owner.properties.contains("segments"))
            charge(retained_edges, owner.properties.at("segments").size(), native_dxf_phase_source_node_limit, "active physical retained edge limit");
        for (const auto& hole : descriptor.at("holes"))
            charge(retained_edges, hole.size(), native_dxf_phase_source_node_limit, "active physical retained hole edge limit");
    }
    // Each room may name a separate context/plane. Reserve that worst-case
    // detection multiplicity, full actual-map scans and lineage matching.
    product_work(budget, walls * walls, rooms * 64);
    product_work(budget, owners.size(), rooms * 128);
    product_work(budget, walls, source_uses * 64);
    product_work(budget, retained_edges, retained_edges * 32);
}
NativeDxfPhaseDestinationBinding destination_binding(const NativeDxfPhaseSourceGraph& mapped,
    const DocumentSnapshot& destination, const std::vector<Entity>& reviewed_contexts,
    NativeDxfWallSourceWorkBudget& budget) {
    budget_limits(budget); require(destination.is_editable(), "actual destination is read-only");
    admit_destination_owners(destination.entities(), budget); destination_asset_pass(destination.assets(), budget);
    require(reviewed_contexts.size() <= mapped.context_ids.size(), "extra reviewed context owners");
    Ids context_targets(mapped.context_ids.begin(), mapped.context_ids.end());
    for (const auto& owner : reviewed_contexts) {
        raw_entity(owner.id, owner, budget);
        require(context_type(owner.type) && context_targets.contains(owner.id), "reviewed new owner is not a mapped context");
        require(!destination.entities().contains(owner.id), "reviewed new context overwrites an actual owner");
    }
    Owners combined = destination.entities();
    NativeDxfPhaseDestinationBinding result;
    asset_pass(mapped.assets, budget, 2); // mapped proof and staged payloads
    result.mapped_graph = mapped;
    // Child selection spans annotation states in the canvas. Retained local
    // names must not shadow actual destination components or appearance owners.
    Ids destination_annotation_names;
    for (const auto& [id, owner] : destination.entities()) {
        destination_annotation_names.insert(id);
        if (owner.type == "annotation_state")
            for (const auto& child : native_dxf_annotation_child_identity_ids(owner)) destination_annotation_names.insert(child);
    }
    for (const auto& [id, asset] : destination.assets()) {
        (void)asset;
        destination_annotation_names.insert(id);
    }
    for (const auto& [id, owner] : mapped.entities) {
        const bool existing_context = context_type(owner.type) && destination.entities().contains(id);
        if (!existing_context)
            require(destination_annotation_names.insert(id).second, "mapped owner collides with an existing annotation child " + id);
    }
    for (const auto& id : mapped.support_ids) if (mapped.entities.at(id).type == "annotation_state")
        for (const auto& child : native_dxf_annotation_child_identity_ids(mapped.entities.at(id)))
            require(destination_annotation_names.insert(child).second, "mapped annotation child collides in actual destination " + child);
    NativeDxfPhaseSourceAssetRefs combined_assets;
    for (const auto& [id, asset] : destination.assets()) combined_assets.emplace(id, &asset);
    for (const auto& [id, asset] : mapped.assets) {
        require(destination_annotation_names.insert(id).second, "fresh asset collides with actual destination or imported identity " + id);
        require(combined_assets.emplace(id, &asset).second, "fresh asset overwrites actual destination asset " + id);
    }
    // The aggregate actual destination inventory must fit before any staged
    // payload copy or private document replay; individual tables are insufficient.
    const auto first_asset_json = budget.phase_assets.consumed_json_bytes;
    validate_native_dxf_phase_destination_asset_refs(combined_assets, &budget.phase_assets);
    std::uint64_t combined_copy_bytes = budget.phase_assets.consumed_json_bytes - first_asset_json;
    for (const auto& [id, asset] : combined_assets)
        combined_copy_bytes += id.size() + asset->id.size() + asset->media_type.size() + asset->sha256.size() + asset->bytes.size();
    // Preview copies the next current state and its snapshot and compares the
    // retained raw result. Retained-history restoration is admitted separately.
    require(combined_copy_bytes <= budget.phase_assets.max_work_bytes / 3, "combined asset replay work limit");
    asset_work(budget, combined_copy_bytes * 3);
    for (const auto& [id, asset] : mapped.assets) { (void)id; result.staged_assets.push_back(asset); }
    if (std::any_of(mapped.support_ids.begin(), mapped.support_ids.end(), [&](const auto& id) {
            return native_dxf_sheet_view_source_type(mapped.entities.at(id).type);
        })) {
        Ids destination_view_names;
        for (const auto& [id, owner] : destination.entities()) {
            (void)id;
            if (native_dxf_sheet_view_source_type(owner.type))
                for (const auto& view : native_dxf_sheet_view_source_view_identity_ids(owner, &budget))
                    destination_view_names.insert(view);
            if (owner.type == "annotation_state")
                for (const auto& row : owner.properties.at("state").at("overrides"))
                    if (row.at("target_kind") == "output_view") {
                        const auto& target = row.at("target_id").get_ref<const std::string&>();
                        work(budget, target.size() + 1);
                        destination_view_names.insert(target);
                    }
        }
        for (const auto& id : mapped.support_ids)
            if (native_dxf_sheet_view_source_type(mapped.entities.at(id).type))
                for (const auto& view : native_dxf_sheet_view_source_view_identity_ids(mapped.entities.at(id), &budget))
                    require(!destination_view_names.contains(view), "imported view identity shadows an existing output view " + view);
    }
    for (const auto& owner : reviewed_contexts) {
        require(combined.emplace(owner.id, owner).second, "duplicate reviewed new context");
        result.staged_entities.push_back(owner);
    }
    for (const auto& id : mapped.context_ids) {
        const auto actual_context = combined.find(id);
        require(actual_context != combined.end() && exact_owner(mapped.entities.at(id), actual_context->second),
            "reviewed actual context differs from complete mapped source owner " + id);
    }
    for (const auto* ids : {&mapped.body_ids, &mapped.catalog_ids, &mapped.registry_ids, &mapped.support_ids}) for (const auto& id : *ids) {
        require(combined.emplace(id, mapped.entities.at(id)).second, "fresh authored target overwrites actual destination " + id);
        result.staged_entities.push_back(mapped.entities.at(id));
    }
    for (const auto& [id, owner] : destination.entities()) {
        (void)id;
        if (native_dxf_sheet_view_source_type(owner.type))
            validate_native_dxf_sheet_view_witness_binding(owner, destination.entities(), combined, &budget);
    }
    for (const auto& id : mapped.support_ids)
        if (native_dxf_sheet_view_source_type(mapped.entities.at(id).type))
            validate_native_dxf_sheet_view_witness_binding(mapped.entities.at(id), mapped.entities, combined, &budget);
    // Admit actual combined current state before its organizer, all-registry
    // inventory, global joins, and complete private Document semantic admission.
    admit_destination_owners(combined, budget);
    const auto phases = phase_inventory(combined);
    for (const auto& [id, owner] : combined) {
        (void)id;
        if (owner.type == "opening") hosted_phase(owner, reference(owner.properties.at("wall_id")), phases, budget);
        if (owner.type == "railing" && owner.properties.contains("host"))
            hosted_phase(owner, reference(owner.properties.at("host").at("stair_id")), phases, budget);
        if (native_dxf_phase_auxiliary_source_type(owner.type)) validate_native_dxf_phase_auxiliary_source(owner, combined);
        if (owner.type == "corner_window") validate_native_dxf_corner_window_source(owner, combined);
    }
    validate_roof_join_ownership(combined);
    if (std::any_of(mapped.support_ids.begin(), mapped.support_ids.end(), [&](const auto& id) {
        return native_dxf_constraint_source_type(mapped.entities.at(id).type);
    })) validate_native_dxf_constraint_source_graph(combined, &budget);
    for (const auto& id : mapped.support_ids)
        if (native_dxf_annotation_source_type(mapped.entities.at(id).type)) {
            admit_native_dxf_annotation_source_work(mapped.entities.at(id), combined, budget);
            validate_native_dxf_annotation_source(mapped.entities.at(id), combined, &budget);
        }
        else if (native_dxf_sheet_view_source_type(mapped.entities.at(id).type))
            validate_native_dxf_sheet_view_source(mapped.entities.at(id), combined, &budget);
    for (const auto& [id, owner] : mapped.entities)
        require(exact_owner(owner, combined.at(id)), "actual combined mapped owner differs " + id);
    // Preview the exact fresh additions against the actual retained destination.
    // Existing independent owner/asset namespaces stay existing; they must not
    // be reinterpreted as fresh additions to a fabricated empty document.
    admit_native_dxf_phase_destination_snapshot(destination, &budget);
    asset_pass(mapped.assets, budget, 6); // command/variant copies and native staged replay
    std::sort(result.staged_entities.begin(), result.staged_entities.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    ImportPhaseEntities command;
    command.expected_revision = destination.revision();
    command.registry_ids = mapped.registry_ids;
    for (const auto& id : mapped.enrolled_hierarchy_ids)
        if (destination.entities().contains(id)) command.reviewed_existing_hierarchy_ids.push_back(id);
    command.entity_changes.reserve(result.staged_entities.size());
    for (const auto& entity : result.staged_entities) command.entity_changes.push_back(EntityChange::upsert(entity));
    command.asset_changes.reserve(result.staged_assets.size());
    for (const auto& asset : result.staged_assets) command.asset_changes.push_back(AssetChange::upsert(asset));
    const auto admitted = Document::preview_command(destination, command);
    admit_native_dxf_phase_retained_asset_capacity(admitted, &budget);
    require(admitted.is_editable(), "combined destination has unsupported authoring state");
    require(admitted.assets().size() == combined_assets.size(), "private admission changed destination asset inventory");
    for (const auto& [id, asset] : combined_assets)
        require(exact_asset(*asset, admitted.assets().at(id)), "private admission changed actual destination asset " + id);
    require(admitted.entities().size() == combined.size(), "private admission changed destination owner inventory");
    for (const auto& [id, owner] : combined)
        require(exact_owner(owner, admitted.entities().at(id)), "private admission changed actual destination owner " + id);
#ifdef SKETCH_PHYSICAL_ROOMS
    // Retained inactive rooms are intrinsic lineage records, not claims about
    // the saved active design. Only actually depicted imported rooms require
    // fresh currentness against all existing/imported physical wall outsiders.
    if (std::any_of(mapped.depicted_body_ids.begin(), mapped.depicted_body_ids.end(), [&](const auto& id) {
            return mapped.entities.at(id).extensions.contains("physical_wall_room");
        })) {
        reserve_physical_room_checks(combined, budget);
        const auto checks = physical_wall_room_checks(admitted);
        for (const auto& id : mapped.depicted_body_ids) if (mapped.entities.at(id).extensions.contains("physical_wall_room")) {
            const auto found = checks.find(id);
            require(found != checks.end() && found->second.current, "mapped active physical room is not current in actual destination " + id);
        }
    }
#endif
    return result;
}
} // namespace

bool NativeDxfPhaseSourceGraph::operator==(const NativeDxfPhaseSourceGraph& other) const {
    NativeDxfWallSourceWorkBudget budget;
    // Equality is raw representation equality, including metadata numeric
    // types and signed zero; payload scans use a bounded standalone ledger.
    return exact_graph(*this, other, budget);
}

void validate_native_dxf_phase_source_graph(const NativeDxfPhaseSourceGraph& graph,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try { raw_graph(graph, budget); semantic_graph(graph, budget); }
    catch (const Json::exception& error) { refuse(std::string("malformed source graph: ") + error.what()); }
}
Json encode_native_dxf_phase_source_graph(const NativeDxfPhaseSourceGraph& graph,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    validate_native_dxf_phase_source_graph(graph, &budget);
    return unchecked_encode(graph, budget);
}
NativeDxfPhaseSourceGraph decode_native_dxf_phase_source_graph(const Json& value,
    NativeDxfWallSourceWorkBudget* work_budget, const NativeDxfPhaseSourceAssets& external_assets) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        budget_limits(budget); raw(value, budget);
        require(value.is_object() && value.at("version").is_number_integer() &&
            ((value.at("version") == 1 && value.size() == 8 && !value.contains("support_ids")) ||
             ((value.at("version") == 2 || value.at("version") == 3) && value.size() == 9 && value.contains("support_ids")) ||
             (value.at("version") == 4 && value.size() == 10 && value.contains("support_ids") && value.contains("asset_manifest")) ||
             (value.at("version") == 5 && value.contains("support_ids") &&
                ((!value.contains("asset_manifest") && value.size() == 9) || (value.contains("asset_manifest") && value.size() == 10)))),
            "unsupported graph schema/version");
        const bool corner_version = value.at("version") == 5;
        const bool assets_present = value.contains("asset_manifest");
        if (assets_present) {
            const auto manifest = decode_native_dxf_phase_asset_manifest(value.at("asset_manifest"), &budget.phase_assets);
            require(!manifest.empty(), "asset-bearing graph requires a nonempty asset manifest");
            bind_native_dxf_phase_asset_manifest(manifest, external_assets, &budget.phase_assets);
        } else require(external_assets.empty() && !value.contains("asset_manifest"), "legacy graph cannot bind external assets");
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
        require(corner_version == corner_graph_semantics(result.entities), "corner semantics require exactly graph version five");
        if (value.at("version") == 2 || value.at("version") == 3 || assets_present || corner_version) {
            result.support_ids = read_ids("support_ids");
            require(assets_present || corner_version || !result.support_ids.empty(), "support inventory cannot be empty");
        }
        const bool sheet_companions = std::any_of(result.entities.begin(), result.entities.end(), [](const auto& item) {
            return native_dxf_sheet_view_source_type(item.second.type);
        });
        require(assets_present || corner_version || (value.at("version") == 3) == sheet_companions, "sheet/view companions require graph version three");
        result.enrolled_hierarchy_ids = read_ids("enrolled_hierarchy_ids"); result.depicted_body_ids = read_ids("depicted_body_ids");
        asset_pass(external_assets, budget, 1);
        result.assets = external_assets;
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
            admit_complete_assembly_catalog_source(owner, budget.catalog_transfer,
                AssemblyCatalogSourceReferencePolicy::document_authoring);
        }
        // Read actual canonical catalog references without copying or hashing
        // assets from ambient unselected catalogs. Selected placement host
        // roles are authenticated by semantic_graph after complete closure.
        std::map<std::string, References, std::less<>> refs;
        for (const auto& id : all_catalogs) refs.emplace(id, dependencies(owners.at(id), owners, budget));
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
            if (dependencies_found == refs.end()) dependencies_found = refs.emplace(id, dependencies(owner, owners, budget)).first;
            for (const auto& [dependency, role] : dependencies_found->second) { (void)actual(owners, dependency, role); include(dependency); }
            if (owner.type != "model_phases" && owner.type != "vertical_levels")
                for (const auto& context : context_owners(owner, owners, organization)) include(context);
            // Capture all actual openings/rails/joins attached to reached hosts,
            // including inactive children and their other complete registries.
            for (const auto& [other_id, other] : owners) {
                work(budget, 1);
                if (selected.contains(other_id)) continue;
                if (support_type(other.type)) {
                    auto support_refs = refs.find(other_id);
                    if (support_refs == refs.end()) support_refs = refs.emplace(other_id, dependencies(other, owners, budget)).first;
                    if (support_refs->second.contains(id)) include(other_id);
                    continue;
                }
                if (other.type == "opening" && other.properties.contains("wall_id") && reference(other.properties.at("wall_id")) == id) include(other_id);
                else if (other.type == "corner_window") {
                    const auto cohort = native_dxf_corner_window_source_dependencies(other);
                    if (std::find(cohort.begin(), cohort.end(), id) != cohort.end()) include(other_id);
                }
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
        NativeDxfPhaseSourceAssetRefs reached_assets;
        for (const auto& id : selected) for (const auto& asset : asset_references(owners.at(id))) {
            const auto found = source.assets().find(asset);
            require(found != source.assets().end(), "missing actual reached asset " + asset);
            reached_assets.emplace(asset, &found->second);
        }
        require(reached_assets.size() <= native_dxf_phase_asset_count_limit, "reached asset inventory limit");
        if (!reached_assets.empty()) {
            charge(budget.catalog_transfer.consumed_json_bytes, 80 + reached_assets.size() * 90,
                budget.catalog_transfer.max_json_bytes, "captured combined manifest byte limit");
            charge(budget.catalog_transfer.consumed_json_nodes, 4 + reached_assets.size() * 5,
                budget.catalog_transfer.max_json_nodes, "captured combined manifest node limit");
            for (const auto& [id, asset] : reached_assets) {
                map_text(id, budget); map_text(asset->media_type, budget);
                map_text(asset->sha256, budget); raw(asset->metadata, budget);
            }
        }
        // Borrow actual snapshot payloads so complete subset admission precedes
        // both hashing and the first retained payload/metadata allocation.
        const auto first_asset_json = budget.phase_assets.consumed_json_bytes;
        validate_native_dxf_phase_source_asset_refs(reached_assets, &budget.phase_assets);
        std::uint64_t asset_copy_bytes = budget.phase_assets.consumed_json_bytes - first_asset_json;
        for (const auto& [id, asset] : reached_assets)
            asset_copy_bytes += id.size() + asset->id.size() + asset->media_type.size() + asset->sha256.size() + asset->bytes.size();
        asset_work(budget, asset_copy_bytes);
        for (const auto& [id, asset] : reached_assets) result.assets.emplace(id, *asset);
        for (const auto& id : selected) result.entities.emplace(id, owners.at(id));
        const auto captured_phases = phase_inventory(result.entities); roles(result, result.entities, captured_phases);
        reserve_models(result.entities, budget); semantic_graph(result, budget); return result;
    } catch (const Json::exception& error) { refuse(std::string("malformed captured source: ") + error.what()); }
}

void admit_native_dxf_phase_retained_asset_capacity(const DocumentSnapshot& destination,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    budget_limits(budget);
    const auto& history = destination.history();
    require(!history.empty() && history.size() <= 10'000 && destination.revision() < history.size(),
        "native retained asset history capacity limit");
    product_work(budget, history.size(), 1);
    std::size_t rows = 0, bytes = 0;
    std::map<std::string, const Asset*, std::less<>> payloads;
    for (const auto& record : history) {
        charge(rows, record.assets.size(), native_dxf_phase_destination_asset_count_limit,
            "Import would exceed the native retained asset row capacity.");
        product_work(budget, record.assets.size(), 1);
        for (const auto& [id, asset] : record.assets) {
            (void)id;
            require(asset.sha256.size() == 64 && asset.bytes.size() <= native_dxf_phase_asset_payload_limit,
                "invalid retained asset descriptor");
            if (payloads.emplace(asset.sha256, &asset).second)
                charge(bytes, asset.bytes.size(), static_cast<std::size_t>(native_dxf_phase_destination_asset_payload_limit),
                    "Import would exceed the native unique asset byte capacity.");
        }
    }
    // Admit the complete unique physical inventory before hashing. Declared
    // equal hashes do not establish equality of caller-supplied buffers.
    for (const auto& [hash, asset] : payloads) {
        asset_work(budget, asset->bytes.size());
        require(asset->bytes.verified_sha256() == hash, "retained asset hash differs from actual bytes");
    }
    for (const auto& record : history)
        for (const auto& [id, asset] : record.assets) {
            (void)id;
            const auto* first = payloads.at(asset.sha256);
            if (first->bytes.same_storage(asset.bytes)) continue;
            asset_work(budget, static_cast<std::uint64_t>(asset.bytes.size()) + first->bytes.size() + asset.bytes.size());
            require(asset.bytes.verified_sha256() == asset.sha256 && asset.bytes == first->bytes,
                "retained asset hash aliases different bytes");
        }
}

void admit_native_dxf_phase_destination_snapshot(const DocumentSnapshot& destination,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        budget_limits(budget);
        const auto& history = destination.history();
        require(!history.empty() && history.size() <= 4096 && destination.revision() < history.size(), "actual retained history limit");
        // Native v161 stores one verified content payload, while retaining every
        // revision's metadata row. Admit those separate inventories first.
        admit_native_dxf_phase_retained_asset_capacity(destination, &budget);
        HistoryAdmission proof{budget};
        const auto first_head_byte = budget.catalog_transfer.consumed_json_bytes;
        proof.text(destination.document_id()); proof.text(destination.read_only_reason());
        admit_destination_owners(destination.entities(), budget); admit_assets(destination.assets(), budget);
        require(destination.named_revisions().size() <= 4096, "actual named revision limit");
        for (const auto& [name, revision] : destination.named_revisions()) {
            proof.text(name); require(revision < history.size(), "named revision outside actual history");
        }
        const auto head_json_bytes = budget.catalog_transfer.consumed_json_bytes - first_head_byte;
        std::size_t prefix_json_bytes = 0, prefix_asset_bytes = 0;
        for (std::size_t i = 0; i < history.size(); ++i) {
            const auto& record = history[i];
            require(record.revision == i, "actual history must be contiguous");
            const auto first_record_byte = budget.catalog_transfer.consumed_json_bytes;
            proof.fixed(sizeof(RevisionRecord)); proof.text(record.action); proof.optional(record.name);
            admit_destination_owners(record.entities, budget); admit_assets(record.assets, budget);
            require(record.undo_stack.size() <= native_dxf_phase_source_node_limit &&
                record.redo_stack.size() <= native_dxf_phase_source_node_limit, "actual history navigation limit");
            product_work(budget, record.undo_stack.size() + record.redo_stack.size(), 32);
            proof.fixed((record.undo_stack.size() + record.redo_stack.size()) * sizeof(Revision));
            proof.optional(record.boundary_translation); proof.optional(record.boundary_transform); proof.optional(record.boundary_geometry_edit);
            proof.optional(record.boundary_constraint_changes); proof.optional(record.boundary_translations); proof.optional(record.boundary_transforms);
            proof.optional(record.phase_entity_import);
            charge(prefix_json_bytes, budget.catalog_transfer.consumed_json_bytes - first_record_byte,
                native_dxf_phase_source_byte_limit, "retained prefix JSON byte limit");
            for (const auto& [id, asset] : record.assets) {
                (void)id;
                // Prefix digests still serialize every logical row. Sharing
                // physical bytes must not discount their actual replay work.
                charge(prefix_asset_bytes, asset.bytes.size(), static_cast<std::size_t>(budget.phase_assets.max_work_bytes),
                    "retained prefix asset replay work limit");
            }
            if (record.boundary_constraint_changes) {
                const auto& command = *record.boundary_constraint_changes;
                if (command.room_review_completion || command.phase_room_review_completion ||
                    command.phase_constraint_authoring_completion || command.room_review_geometry_completion ||
                    !command.phase_constraint_authoring_intent.is_null()) {
                    // Restore's source-bound review reserializes/hashes the
                    // actual prefix for both authoring and snapshot digests.
                    // Two replay passes cover fork/preview; asset hex expansion
                    // is separately bounded rather than billed as foreign JSON.
                    product_work(budget, prefix_json_bytes + head_json_bytes, 16);
                    require(prefix_asset_bytes <= budget.phase_assets.max_work_bytes / 32,
                        "retained prefix asset hash replay capacity");
                    asset_work(budget, static_cast<std::uint64_t>(prefix_asset_bytes) * 32);
                    // Digest command codecs may replay every prior typed
                    // geometry proof as they serialize the prefix. Reserve its
                    // actual intrinsic family work, not opaque JSON pairs.
                    for (std::size_t prefix = 0; prefix <= i; ++prefix)
                        for (const auto& [id, owner] : history[prefix].entities) {
                            (void)id; reserve_intrinsic_owner(owner, budget);
                        }
                }
            }
            // Restore validates the same adjacent source/candidate models and
            // can replay source-bound typed commands several times. Reserve
            // those passes, plus lifetime identity scans over retained prefixes.
            product_work(budget, record.entities.size() + 1, (i + 1) * 64);
            reserve_models(record.entities, budget);
        }
        if (destination.saved_revision_optional()) require(*destination.saved_revision_optional() <= destination.revision() &&
            *destination.saved_revision_optional() < history.size(), "saved revision outside actual history");
    } catch (const Json::exception& error) { refuse(std::string("malformed destination admission: ") + error.what()); }
}
void admit_native_dxf_phase_document_entities(const Owners& owners, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try { budget_limits(budget); admit_destination_owners(owners, budget); }
    catch (const Json::exception& error) { refuse(std::string("malformed document admission: ") + error.what()); }
}
void admit_native_dxf_phase_scope_work(const Owners& owners, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        budget_limits(budget);
        require(owners.size() <= native_dxf_phase_source_owner_limit, "phase scope owner limit");
        for (const auto& [id, owner] : owners) raw_entity(id, owner, budget);
        // Saved registry/organization selection reads no annotation codecs.
        // Actual private Document consumers use complete admission instead.
        reserve_models(owners, budget);
    } catch (const Json::exception& error) { refuse(std::string("malformed phase scope admission: ") + error.what()); }
}
void admit_native_dxf_phase_annotation_frame_work(const Owners& owners,
    std::span<const SiteAnnotationTarget> targets, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        budget_limits(budget);
        require(owners.size() <= native_dxf_phase_source_owner_limit &&
            targets.size() <= native_dxf_phase_source_owner_limit, "annotation frame inventory limit");
        FrameWorkAdmission admission(owners, budget);
        std::set<SiteAnnotationTarget> unique;
        for (const auto& target : targets) {
            identity(target.owner_entity_id); identity(target.child_id);
            require(unique.insert(target).second, "duplicate annotation frame target");
            admission.finish(admission.annotation(target), target.owner_entity_id.size() + target.child_id.size());
        }
    } catch (const Json::exception& error) { refuse(std::string("malformed annotation frame admission: ") + error.what()); }
}
void admit_native_dxf_phase_owner_frame_work(const Owners& owners,
    std::span<const std::string> owner_ids, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        budget_limits(budget);
        require(owner_ids.size() <= native_dxf_phase_source_owner_limit, "owner frame target inventory limit");
        FrameWorkAdmission admission(owners, budget);
        Ids unique;
        for (const auto& id : owner_ids) {
            identity(id); require(unique.insert(id).second, "duplicate owner frame target");
            admission.finish(admission.resolve(id));
        }
    } catch (const Json::exception& error) { refuse(std::string("malformed owner frame admission: ") + error.what()); }
}
void admit_native_dxf_phase_physical_room_checks(const Owners& owners, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        // Checks consume geometry/phase/organization, never annotation artwork
        // or constraint codecs. A caller creating a private Document admits
        // those separate consumers explicitly before its create call.
        budget_limits(budget); admit_destination_owners(owners, budget, false);
        reserve_physical_room_checks(owners, budget);
    } catch (const Json::exception& error) { refuse(std::string("malformed physical room checks admission: ") + error.what()); }
}
NativeDxfPhaseSourceGraph remap_native_dxf_phase_source_graph(const NativeDxfPhaseSourceGraph& source,
    const NativeDxfPhaseDestinationMaps& maps, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try { return mapped_source(source, maps, budget); }
    catch (const Json::exception& error) { refuse(std::string("malformed source remapping: ") + error.what()); }
    catch (const std::out_of_range& error) { refuse(std::string("unmapped typed source reference: ") + error.what()); }
}
NativeDxfPhaseDestinationBinding bind_native_dxf_phase_source_destinations(const NativeDxfPhaseSourceGraph& source,
    const NativeDxfPhaseDestinationMaps& maps, const DocumentSnapshot& actual_destination,
    const std::vector<Entity>& reviewed_new_context_owners, NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        const auto mapped = mapped_source(source, maps, budget);
        return destination_binding(mapped, actual_destination, reviewed_new_context_owners, budget);
    } catch (const Json::exception& error) { refuse(std::string("malformed destination binding: ") + error.what()); }
    catch (const std::out_of_range& error) { refuse(std::string("unmapped typed binding reference: ") + error.what()); }
}
void validate_native_dxf_phase_destination_binding(const NativeDxfPhaseSourceGraph& source,
    const NativeDxfPhaseDestinationMaps& maps, const NativeDxfPhaseDestinationBinding& binding,
    const DocumentSnapshot& actual_destination, const std::vector<Entity>& reviewed_new_context_owners,
    NativeDxfWallSourceWorkBudget* work_budget) {
    NativeDxfWallSourceWorkBudget local; auto& budget = work_budget ? *work_budget : local;
    try {
        // Caller-owned result admission precedes copies or equality scans.
        raw_graph(binding.mapped_graph, budget);
        require(binding.staged_entities.size() <= native_dxf_phase_source_owner_limit, "staged destination owner limit");
        for (const auto& owner : binding.staged_entities) raw_entity(owner.id, owner, budget);
        NativeDxfPhaseSourceAssetRefs staged_assets;
        require(binding.staged_assets.size() <= native_dxf_phase_asset_count_limit, "staged asset inventory limit");
        for (const auto& asset : binding.staged_assets) {
            map_text(asset.id, budget);
            require(staged_assets.emplace(asset.id, &asset).second, "duplicate staged asset identity");
        }
        const auto first_asset_json = budget.phase_assets.consumed_json_bytes;
        validate_native_dxf_phase_source_asset_refs(staged_assets, &budget.phase_assets);
        std::uint64_t staged_scan_bytes = budget.phase_assets.consumed_json_bytes - first_asset_json;
        for (const auto& [id, asset] : staged_assets)
            staged_scan_bytes += id.size() + asset->media_type.size() + asset->sha256.size() + asset->bytes.size();
        asset_work(budget, staged_scan_bytes);
        const auto mapped = mapped_source(source, maps, budget);
        require(exact_graph(mapped, binding.mapped_graph, budget), "binding does not equal complete raw remapped source graph");
        const auto expected = destination_binding(mapped, actual_destination, reviewed_new_context_owners, budget);
        require(expected.staged_entities.size() == binding.staged_entities.size(), "binding staged-owner inventory differs");
        for (std::size_t i = 0; i < expected.staged_entities.size(); ++i)
            require(exact_owner(expected.staged_entities[i], binding.staged_entities[i]), "binding staged owner/order differs");
        require(expected.staged_assets.size() == binding.staged_assets.size(), "binding staged-asset inventory differs");
        asset_pass(mapped.assets, budget, 1);
        for (std::size_t i = 0; i < expected.staged_assets.size(); ++i)
            require(exact_asset(expected.staged_assets[i], binding.staged_assets[i]), "binding staged asset/order differs");
    } catch (const Json::exception& error) { refuse(std::string("malformed binding validation: ") + error.what()); }
    catch (const std::out_of_range& error) { refuse(std::string("unmapped typed validation reference: ") + error.what()); }
}
} // namespace sketch
