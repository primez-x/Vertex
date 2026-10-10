#pragma once

#include "sketch/document.hpp"
#include "sketch/dxf_exchange.hpp"
#include "sketch/project_organization.hpp"

#include <cstddef>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Project mapping is deliberately separate from the bounded transport codec.
// It translates the native document graph to and from the representable DXF
// subset without opening files, mutating a Document, or inventing a hosted
// service. Callers can retain the original bytes whenever diagnostics are
// present and decide how/where to commit the returned entities.
struct DxfProjectDiagnostic {
    std::string source_id;
    std::string source_kind;
    std::string code;

    bool operator==(const DxfProjectDiagnostic&) const = default;
};

struct DxfProjectExportResult {
    DxfDrawing drawing;
    std::vector<DxfProjectDiagnostic> diagnostics;
};

using NativeDxfPhysicalSourceGraphs = std::map<std::string, nlohmann::json, std::less<>>;

struct DxfProjectImportResult {
    std::vector<Entity> entities;
    std::vector<DxfProjectDiagnostic> diagnostics;
    // A caller must retain the original input bytes when this is true if
    // unsupported transport/project records need to remain recoverable.
    bool source_retention_required{};
    // V7 shared source evidence. Never install these snapshots as entities;
    // keep the table through pending admission and reviewed destination binding.
    NativeDxfPhysicalSourceGraphs physical_source_graphs;

    bool complete() const noexcept { return diagnostics.empty(); }
};

// V5 carries complete exterior WALL-source components. V6 shares the same
// engine and adds measured strokes, retained area lineage and full applicable
// source-owner inventories. V6 wire contexts are captured from organize_project;
// pending version-2 context bindings retain direct and resolved observations.
// Destination organization activates only after complete reviewed graph binding.
// V7 adds retained physical rooms and actual hierarchy/level/phase source proof.
// Its small member references resolve through the operation's shared sidecar;
// completed transfer markers are removed before live publication. Raw input and
// opaque provenance remain evidence; the next export rebuilds from Document.
[[nodiscard]] nlohmann::json native_dxf_wall_source_dependency_graph(const Entity& entity);
[[nodiscard]] std::vector<std::string> native_dxf_wall_source_dependency_ids(const Entity& entity);
// A mapper processing components separately shares this ledger across the
// complete operation. Work remains charged if a later source proof fails.
// preflight_only performs bounded structural/work admission without replay or
// nonlinear source/containment checks; it still charges this shared ledger.
// V6 uses the complete provided measured inventory and captured contexts,
// including observation copies outside a component. Original source inventory
// must be supplied before remapping; destination separation cannot prove it.
// V7 uses a shared bounded graph table containing actual source snapshots and
// requires complete active same-context wall membership. The source proof and
// fresh owner references stay separate. Bound V7 groups require the actual
// destination map; resolved context tuples alone cannot prove physical currentness.
struct NativeDxfWallSourceWorkBudget {
    std::size_t segments{};
    std::size_t source_work{};
    // Applies V6/V7 topology admission to every component/fallback in an
    // operation that contains either source family. V5-only callers keep their
    // contract. Physical detection/replay shares source_work across components.
    bool measured_operation{};
};
void validate_native_dxf_wall_source_groups(const std::vector<Entity>& entities,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr, bool preflight_only = false,
    const NativeDxfPhysicalSourceGraphs* physical_source_graphs = nullptr,
    const std::map<std::string, Entity, std::less<>>* actual_destination_entities = nullptr);
void validate_native_dxf_wall_source_member(const Entity& entity);
// Owner identity is changed separately with the boundary owner codec where
// applicable. Only graph-owned references and membership change here.
void remap_native_dxf_wall_source_dependency_ids(Entity& entity,
    const std::map<std::string, std::string, std::less<>>& ids);
// Contexts must come from the caller's actual staged destination hierarchy and
// cover every V5/V6/V7 member. For V6/V7, include active destination measured stroke
// outsiders and their actual contexts, with old DXF admission markers removed
// from private observation copies. Changed shared-layer inventory refuses.
// This changes the vector atomically on successful proof.
// Phase references are dropped; retained observations/report digests are inert.
// V7 additionally requires the shared original proof table and complete actual
// destination map, including existing/imported wall outsiders and real hierarchy,
// level graphs and phase registries. Source level IDs are floor-local and retained.
void bind_native_dxf_wall_source_destinations(std::vector<Entity>& entities,
    const std::map<std::string, DrawingContext, std::less<>>& actual_contexts,
    const std::map<std::string, Entity, std::less<>>* actual_destination_entities = nullptr,
    const NativeDxfPhysicalSourceGraphs* physical_source_graphs = nullptr);

// V2 carries one standalone boundary, not an appraisal/source dependency graph.
// Document's generic reference vocabulary does not cover these consumer-owned
// links. Both mapper and isolated-response admission must reject active links
// rather than accidentally resolving a source ID in the destination project.
[[nodiscard]] inline bool native_dxf_boundary_has_untransported_links(const Entity& entity) {
    const auto& properties = entity.properties;
    if (!properties.is_object() || properties.contains("parent_id") ||
        properties.contains("wall_measurement_source") ||
        entity.extensions.contains("measurement_linework_sources") ||
        entity.extensions.contains("measurement_linework_group"))
        return true;
    const auto nonempty_ids = [](const nlohmann::json& object, const char* key) {
        const auto value = object.find(key);
        return value != object.end() && (!value->is_array() || !value->empty());
    };
    if (nonempty_ids(properties, "deduction_ids")) return true;
    const auto facts = properties.find("appraisal_facts");
    if (facts == properties.end()) return false;
    if (!facts->is_object()) return true;
    const auto ansi = facts->find("ansi");
    if (ansi == facts->end()) return false;
    if (!ansi->is_object()) return true;
    const auto ceiling = ansi->find("ceiling");
    if (ceiling == ansi->end()) return false;
    if (!ceiling->is_object() || nonempty_ids(*ceiling, "below_5ft_deduction_ids")) return true;
    for (const auto* key : {"room_boundary_id", "stair_from_floor_id"}) {
        const auto value = ceiling->find(key);
        if (value != ceiling->end() && (!value->is_string() || !value->get_ref<const std::string&>().empty()))
            return true;
    }
    return false;
}

// Only these consumer-owned fields are boundary dependencies. In particular,
// stair_from_floor_id is a floor declaration, not a boundary owner identity.
// Malformed declarations refuse instead of becoming an empty graph.
[[nodiscard]] inline nlohmann::json native_dxf_boundary_dependency_graph(const Entity& entity) {
    using Json = nlohmann::json;
    Json graph{{"deduction_ids", Json::array()}, {"below_5ft_deduction_ids", Json::array()},
               {"room_boundary_id", ""}, {"stair_from_floor_id", ""}};
    const auto ids = [&](const Json& object, const char* key) {
        if (!object.contains(key)) return;
        const auto& value = object.at(key);
        if (!value.is_array()) throw std::invalid_argument("invalid appraisal dependency array");
        std::set<std::string> unique;
        for (const auto& id : value)
            if (!id.is_string() || id.get_ref<const std::string&>().empty() ||
                id.get_ref<const std::string&>().size() > 255 ||
                !unique.insert(id.get<std::string>()).second)
                throw std::invalid_argument("invalid appraisal dependency ID");
        graph[key] = value;
    };
    if (!entity.properties.is_object()) throw std::invalid_argument("invalid boundary properties");
    ids(entity.properties, "deduction_ids");
    const auto facts = entity.properties.find("appraisal_facts");
    if (facts == entity.properties.end()) return graph;
    if (!facts->is_object()) throw std::invalid_argument("invalid appraisal facts");
    const auto ansi = facts->find("ansi");
    if (ansi == facts->end()) return graph;
    if (!ansi->is_object()) throw std::invalid_argument("invalid ANSI facts");
    const auto ceiling = ansi->find("ceiling");
    if (ceiling == ansi->end()) return graph;
    if (!ceiling->is_object()) throw std::invalid_argument("invalid ceiling facts");
    ids(*ceiling, "below_5ft_deduction_ids");
    for (const auto* key : {"room_boundary_id", "stair_from_floor_id"}) {
        if (!ceiling->contains(key)) continue;
        const auto& value = ceiling->at(key);
        if (!value.is_string() || value.get_ref<const std::string&>().size() > 255)
            throw std::invalid_argument("invalid appraisal dependency ID");
        graph[key] = value;
    }
    return graph;
}

[[nodiscard]] inline std::vector<std::string> native_dxf_boundary_dependency_ids(const Entity& entity) {
    const auto graph = native_dxf_boundary_dependency_graph(entity);
    std::set<std::string> ids;
    for (const auto* key : {"deduction_ids", "below_5ft_deduction_ids"})
        for (const auto& id : graph.at(key)) ids.insert(id.get<std::string>());
    const auto room = graph.at("room_boundary_id").get<std::string>();
    if (!room.empty()) ids.insert(room);
    return {ids.begin(), ids.end()};
}

// V4 transports only the stairs consumer's self-floor declaration. A source
// floor stays raw evidence until a caller explicitly supplies a reviewed floor.
// No floor identity is ever resolved through the boundary owner ID map.
[[nodiscard]] inline std::string native_dxf_boundary_stair_floor_source(const Entity& entity) {
    using Json = nlohmann::json;
    const auto graph = native_dxf_boundary_dependency_graph(entity);
    const auto binding = entity.extensions.find("vertex_dxf_stair_floor_binding");
    const auto marker = entity.extensions.find("vertex_dxf_boundary");
    if (marker == entity.extensions.end() || !marker->is_object() || marker->value("version", 0) != 4) {
        if (binding != entity.extensions.end()) throw std::invalid_argument("stair floor binding requires V4");
        return {};
    }
    const auto valid_id = [](const Json& value) {
        return value.is_string() && !value.get_ref<const std::string&>().empty() &&
            value.get_ref<const std::string&>().size() <= 255;
    };
    if (marker->size() != 3 || !marker->at("version").is_number_integer() ||
        marker->value("depiction", std::string{}) != "BOUNDARY_PLAN_V1" ||
        !marker->contains("member_ids") || !marker->at("member_ids").is_array())
        throw std::invalid_argument("invalid V4 boundary marker");
    std::vector<std::string> members;
    for (const auto& id : marker->at("member_ids")) {
        if (!valid_id(id)) throw std::invalid_argument("invalid V4 boundary identity");
        members.push_back(id.get<std::string>());
    }
    if (members.empty() || !std::is_sorted(members.begin(), members.end()) ||
        std::adjacent_find(members.begin(), members.end()) != members.end() ||
        !std::binary_search(members.begin(), members.end(), entity.id))
        throw std::invalid_argument("invalid V4 boundary membership");
    const Json* facts = entity.properties.contains("appraisal_facts") ? &entity.properties.at("appraisal_facts") : nullptr;
    const Json* ceiling = facts && facts->contains("ansi") && facts->at("ansi").contains("ceiling")
        ? &facts->at("ansi").at("ceiling") : nullptr;
    const bool stairs = ceiling && ceiling->value("kind", std::string{}) == "stairs";
    const bool footprint = facts && facts->value("boundary_role", std::string{}) == "stair_footprint";
    const auto active = graph.at("stair_from_floor_id").get<std::string>();
    if (!stairs && !footprint && active.empty() && binding == entity.extensions.end()) return {};
    if (!stairs || !footprint || !ceiling->contains("stair_from_floor_id") ||
        (entity.type != "boundary" && entity.type != "measurement_boundary"))
        throw std::invalid_argument("stair floor binding requires stair footprint facts");
    const auto floor = entity.properties.find("floor_id");
    if (binding == entity.extensions.end()) {
        if (active.empty() || floor == entity.properties.end() || !valid_id(*floor) || *floor != active)
            throw std::invalid_argument("source stairs require their owning floor");
        return active;
    }
    if (!binding->is_object() || binding->size() != 3 || !binding->contains("version") ||
        !binding->at("version").is_number_integer() || binding->at("version") != 1 ||
        !binding->contains("source_floor_id") || !valid_id(binding->at("source_floor_id")) ||
        !binding->contains("destination_floor_id"))
        throw std::invalid_argument("invalid stair floor binding schema");
    const auto& destination = binding->at("destination_floor_id");
    if (destination.is_null()) {
        if (!active.empty() || floor != entity.properties.end())
            throw std::invalid_argument("pending stair floor binding must be detached");
    } else if (!valid_id(destination) || floor == entity.properties.end() || !valid_id(*floor) ||
               *floor != destination || active != destination.get<std::string>()) {
        throw std::invalid_argument("bound stairs require their reviewed owning floor");
    }
    return binding->at("source_floor_id").get<std::string>();
}

// Call after assigning the actual reviewed destination floor/layer. Source
// observations, organizational evidence and report digests are left unchanged.
inline void bind_native_dxf_boundary_destination_floor(Entity& entity, const std::string& floor_id) {
    const auto binding = entity.extensions.find("vertex_dxf_stair_floor_binding");
    if (binding == entity.extensions.end()) {
        if (!native_dxf_boundary_stair_floor_source(entity).empty())
            throw std::invalid_argument("stair destination binding requires pending state");
        return;
    }
    if (floor_id.empty() || floor_id.size() > 255 || !entity.properties.contains("floor_id") ||
        entity.properties.at("floor_id") != floor_id)
        throw std::invalid_argument("reviewed stair destination floor differs");
    auto pending = entity;
    pending.properties.erase("floor_id");
    (void)native_dxf_boundary_stair_floor_source(pending);
    if (!binding->at("destination_floor_id").is_null())
        throw std::invalid_argument("stair destination binding requires pending state");
    entity.properties["appraisal_facts"]["ansi"]["ceiling"]["stair_from_floor_id"] = floor_id;
    (*binding)["destination_floor_id"] = floor_id;
}

[[nodiscard]] inline bool native_dxf_boundary_has_untransported_source_links(const Entity& entity) {
    if (!entity.properties.is_object() || entity.properties.contains("parent_id") ||
        entity.properties.contains("wall_measurement_source") ||
        entity.extensions.contains("physical_wall_room") ||
        entity.extensions.contains("measurement_linework_sources") ||
        entity.extensions.contains("measurement_linework_group")) return true;
    const auto stair_source = native_dxf_boundary_stair_floor_source(entity);
    return native_dxf_boundary_dependency_graph(entity).at("stair_from_floor_id") != "" && stair_source.empty();
}

// Identity changes never alter local topology, arbitrary JSON, or appraisal
// observation hashes. Those hashes remain source evidence and may become stale.
inline void remap_native_dxf_boundary_dependency_ids(Entity& entity,
    const std::map<std::string, std::string, std::less<>>& ids) {
    const auto source_marker = entity.extensions.find("vertex_dxf_boundary");
    if (source_marker != entity.extensions.end() && source_marker->is_object() &&
        (source_marker->value("version", 0) == 5 || source_marker->value("version", 0) == 6 ||
         source_marker->value("version", 0) == 7)) {
        remap_native_dxf_wall_source_dependency_ids(entity, ids);
        return;
    }
    (void)native_dxf_boundary_dependency_graph(entity);
    const auto marker = entity.extensions.find("vertex_dxf_boundary");
    if (marker != entity.extensions.end() && marker->is_object() &&
        (marker->value("version", 0) == 3 || marker->value("version", 0) == 4)) {
        std::vector<std::string> members;
        for (const auto& id : marker->at("member_ids")) members.push_back(ids.at(id.get<std::string>()));
        std::sort(members.begin(), members.end());
        (*marker)["member_ids"] = members;
    }
    const auto remap = [&](nlohmann::json& object, const char* key, bool array) {
        if (!object.contains(key)) return;
        const auto replace = [&](nlohmann::json& value) {
            const auto id = value.get<std::string>();
            if (!id.empty()) value = ids.at(id);
        };
        if (array) for (auto& id : object[key]) replace(id);
        else replace(object[key]);
    };
    remap(entity.properties, "deduction_ids", true);
    if (entity.properties.contains("appraisal_facts") && entity.properties["appraisal_facts"].contains("ansi") &&
        entity.properties["appraisal_facts"]["ansi"].contains("ceiling")) {
        auto& ceiling = entity.properties["appraisal_facts"]["ansi"]["ceiling"];
        remap(ceiling, "below_5ft_deduction_ids", true);
        remap(ceiling, "room_boundary_id", false);
        // V4 floor authority is bound explicitly, never by this owner ID map.
        if (ceiling.contains("stair_from_floor_id") && ceiling.at("stair_from_floor_id") != "" &&
            native_dxf_boundary_stair_floor_source(entity).empty())
            throw std::invalid_argument("native floor source graph unavailable");
        if (ceiling.value("kind", std::string{}) == "sloped") {
            ceiling["room_boundary_id"] = "";
            ceiling["complete_room_observed"] = false;
        }
    }
}

// Shared broker/desktop admission: markers use current (already fresh) owner
// IDs, with one identical sorted membership list on every connected member.
// V3/V4 validate topology. V5 additionally proves its bounded complete raw
// wall-source graph through pending context evidence or reviewed destinations;
// the mapper proves exact plans and an editable Document before publication.
inline void validate_native_dxf_boundary_groups(const std::vector<Entity>& entities,
    const NativeDxfPhysicalSourceGraphs* physical_source_graphs = nullptr,
    const std::map<std::string, Entity, std::less<>>* actual_destination_entities = nullptr) {
    validate_native_dxf_wall_source_groups(entities, nullptr, false, physical_source_graphs, actual_destination_entities);
    std::map<std::string, const Entity*, std::less<>> owners;
    for (const auto& entity : entities)
        if (!owners.emplace(entity.id, &entity).second) throw std::invalid_argument("duplicate imported identity");
    std::set<std::string> checked;
    for (const auto& entity : entities) {
        const auto marker = entity.extensions.find("vertex_dxf_boundary");
        if (marker == entity.extensions.end() || !marker->is_object() ||
            (marker->value("version", 0) != 3 && marker->value("version", 0) != 4)) continue;
        if (checked.contains(entity.id)) continue;
        if (marker->size() != 3 || marker->value("depiction", std::string{}) != "BOUNDARY_PLAN_V1" ||
            !marker->contains("member_ids") || !marker->at("member_ids").is_array())
            throw std::invalid_argument("invalid native boundary group marker");
        std::vector<std::string> members;
        for (const auto& id : marker->at("member_ids")) {
            if (!id.is_string() || id.get_ref<const std::string&>().empty() || id.get_ref<const std::string&>().size() > 255)
                throw std::invalid_argument("invalid native boundary group identity");
            members.push_back(id.get<std::string>());
        }
        if (members.empty() || !std::is_sorted(members.begin(), members.end()) ||
            std::adjacent_find(members.begin(), members.end()) != members.end() ||
            !std::binary_search(members.begin(), members.end(), entity.id))
            throw std::invalid_argument("invalid native boundary group membership");
        std::map<std::string, std::set<std::string>> adjacency;
        std::map<std::string, std::size_t> incoming;
        std::map<std::string, std::vector<std::string>> deductions;
        bool has_stair_floor = false;
        for (const auto& id : members) {
            const auto owner = owners.find(id);
            if (owner == owners.end() || checked.contains(id)) throw std::invalid_argument("partial native boundary group");
            const auto& member = *owner->second;
            if ((member.type != "boundary" && member.type != "measurement_boundary") ||
                !member.extensions.contains("vertex_dxf_boundary") || member.extensions.at("vertex_dxf_boundary") != *marker ||
                native_dxf_boundary_has_untransported_source_links(member))
                throw std::invalid_argument("mismatched native boundary group");
            if (marker->at("version") == 4 && !native_dxf_boundary_stair_floor_source(member).empty())
                has_stair_floor = true;
            incoming.try_emplace(id, 0);
            const auto graph = native_dxf_boundary_dependency_graph(member);
            const auto room = graph.at("room_boundary_id").get<std::string>();
            if (!room.empty() && room != id) throw std::invalid_argument("ceiling room owner differs");
            for (const auto& child : graph.at("deduction_ids")) {
                const auto target = child.get<std::string>();
                deductions[id].push_back(target);
                ++incoming[target];
            }
            for (const auto& child : graph.at("below_5ft_deduction_ids"))
                if (std::find(graph.at("deduction_ids").begin(), graph.at("deduction_ids").end(), child) == graph.at("deduction_ids").end())
                    throw std::invalid_argument("ceiling exclusion is not a direct deduction");
                else {
                    const auto target = owners.find(child.get<std::string>());
                    if (target == owners.end() || !target->second->properties.contains("appraisal_facts") ||
                        target->second->properties.at("appraisal_facts").value("boundary_role", std::string{}) != "other_void" ||
                        !native_dxf_boundary_dependency_graph(*target->second).at("deduction_ids").empty())
                        throw std::invalid_argument("ceiling exclusion must be an other-void leaf");
                }
            for (const auto& target : native_dxf_boundary_dependency_ids(member)) {
                if (!std::binary_search(members.begin(), members.end(), target))
                    throw std::invalid_argument("untransported native boundary dependency");
                adjacency[id].insert(target); adjacency[target].insert(id);
            }
        }
        if (marker->at("version") == 4 && !has_stair_floor)
            throw std::invalid_argument("V4 boundary group requires a proved stair floor");
        std::vector<std::string> ready;
        for (const auto& [id, count] : incoming) if (count == 0) ready.push_back(id);
        std::size_t visited = 0;
        for (std::size_t i = 0; i < ready.size(); ++i) {
            ++visited;
            for (const auto& child : deductions[ready[i]]) if (--incoming.at(child) == 0) ready.push_back(child);
        }
        if (visited != members.size()) throw std::invalid_argument("cyclic native appraisal deductions");
        std::set<std::string> connected{members.front()};
        ready = {members.front()};
        for (std::size_t i = 0; i < ready.size(); ++i)
            for (const auto& id : adjacency[ready[i]]) if (connected.insert(id).second) ready.push_back(id);
        if (connected.size() != members.size()) throw std::invalid_argument("disconnected native boundary group");
        checked.insert(members.begin(), members.end());
    }
}

// Maps the snapshot's saved active design into the supported DXF R2013 drawing
// model. Retained inactive owners, their hosted openings and bound annotations
// are omitted with fidelity diagnostics; wall cuts and native hosted IDs use
// the same active graph. The complete source snapshot remains unchanged. DXF
// does not preserve phase registries or alternatives. Without a phase registry,
// the existing whole-project mapping applies. The result remains in memory;
// use export_dxf_ascii separately when ready to serialize it to a local path.
// Current physical-room and wall-axis lengths resolve from the complete saved
// inventory. Single curved lengths use analytical ARC_DIMENSION; straight
// lengths use aligned DIMENSION. Bent/curved multi-edge totals retain their
// measured quantity as a named text callout with an explicit fidelity diagnostic.
// Angles retain their admitted vertex tangents as three-point angular DIMENSION;
// area quantities retain named callouts with their association loss diagnosed.
// Valid closed standalone boundaries use a V2 native block; complete connected
// appraisal boundary dependencies use V3 member blocks with identical membership
// and explicit typed links. V4 additionally proves stair footprints' self-floor
// declarations, retaining the source floor until explicit destination binding.
// V5 transports complete live exterior wall measurement components, including
// all active source-wall openings, with matching typed dependency declarations.
// Ordinary outer/hole curves accompany bounded source
// properties, classifications and topology. Missing, inactive, cyclic or invalid
// groups fall back together. V6 additionally carries native measured linework
// with LINEWORK_PLAN_V1 and complete measured-source graphs, including mixed
// wall/measured appraisal components. Physical-room/level/material/assembly
// graphs remain unavailable. Imported sloped observations require reconfirmation.
// Deduction containment uses the actual area engine in native-geometry builds;
// core-only builds retain ordinary geometry when that proof is unavailable.
// Organizational bindings detach on import; unavailable dependent source graphs
// cannot activate. Other boundaries and slabs retain ordinary analytical loops
// and diagnose unrepresented hole ownership, topology and classifications.
// Polyline vertices require exact authored joins. Distinct consecutive endpoints
// retain independent primitives with a connection-loss diagnostic; a merely
// tolerance-close final endpoint remains an open polyline without snapping.
[[nodiscard]] DxfProjectExportResult export_project_dxf(
    const DocumentSnapshot& document,
    const DxfExchangeLimits& limits = {});

// Parses a bounded DXF R2013 drawing and reconstructs editable native
// boundary/annotation entities and validated native boundary/wall/opening graphs.
// Foreign primitive candidates carry CAD classifications and separate hole loops.
// The existing VERTEX_ENTITY_V1 XDATA carrier admits unchanged V1 wall/opening
// JSON and V2/V3/V4 closed-boundary JSON with depiction BOUNDARY_PLAN_V1. V3/V4
// require complete matching connected membership and typed appraisal dependency
// proof. V4 stair declarations detach into an explicit pending floor binding;
// only the reviewed destination assignment may activate that declaration.
// V5 requires complete wall-source components and exact source/fresh plans;
// pending per-member direct context evidence stays detached until the shared
// destination binder receives actual reviewed hierarchy contexts. Phase registry
// bindings are withheld and diagnosed; numeric source lineage stays unchanged.
// V6 retains raw measured stroke models and typed source-use references, with
// captured resolved contexts and full shared-layer/isolated-cohort inventories.
// Exact source/fresh plans and current complete graphs precede activation;
// source local identities, intervals, reversal and observation hashes persist.
// Every member is remapped and validated together; original observation hashes
// remain unchanged and copied sloped ceiling anchors/confirmation are withheld.
// V2 requires
// full native document/geometry validation and exact regenerated outer/hole plan
// agreement before retaining native classifications and local topology IDs.
// Entity identities are fresh; organizational bindings remain source evidence.
// Every native carrier requires metre units and identity INSERT/base placement.
// Manufactured opening blocks additionally require
// depiction MANUFACTURED_PLAN_V1 and an architecture-enabled mapper that admits
// the host/assembly solids and regenerates their exact horizontal plan section.
// Core-only builds retain symbolic visual fallback with explicit diagnostics;
// they never activate manufactured native metadata. Legacy blocks without an
// assembly profile retain their original plan contract. The
// returned entities are unparented import candidates; a desktop adapter is
// responsible for assigning the active floor/layer and committing one atomic
// document command. Malformed transport input throws and returns no partial
// result. Unsupported records are reported in diagnostics.
// Inches, feet, millimetres, centimetres, metres and kilometres are normalized
// to native SI metres before mapping. Missing/unitless or other source units
// return no candidates, an explicit diagnostic and required source retention.
// Linear/arc/angular dimensions reconstruct their measured geometry and actual text,
// retaining their presentation metadata. Foreign owner association remains
// unbound and diagnosed; a dimension picture block supplies no native authority.
[[nodiscard]] DxfProjectImportResult import_project_dxf(
    std::string_view bytes,
    const DxfExchangeLimits& limits = {});

} // namespace sketch
