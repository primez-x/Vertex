#pragma once

#include "sketch/document.hpp"
#include "sketch/dxf_exchange.hpp"

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

struct DxfProjectImportResult {
    std::vector<Entity> entities;
    std::vector<DxfProjectDiagnostic> diagnostics;
    // A caller must retain the original input bytes when this is true if
    // unsupported transport/project records need to remain recoverable.
    bool source_retention_required{};

    bool complete() const noexcept { return diagnostics.empty(); }
};

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

[[nodiscard]] inline bool native_dxf_boundary_has_untransported_source_links(const Entity& entity) {
    if (!entity.properties.is_object() || entity.properties.contains("parent_id") ||
        entity.properties.contains("wall_measurement_source") ||
        entity.extensions.contains("physical_wall_room") ||
        entity.extensions.contains("measurement_linework_sources") ||
        entity.extensions.contains("measurement_linework_group")) return true;
    return native_dxf_boundary_dependency_graph(entity).at("stair_from_floor_id") != "";
}

// Identity changes never alter local topology, arbitrary JSON, or appraisal
// observation hashes. Those hashes remain source evidence and may become stale.
inline void remap_native_dxf_boundary_dependency_ids(Entity& entity,
    const std::map<std::string, std::string, std::less<>>& ids) {
    (void)native_dxf_boundary_dependency_graph(entity);
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
        // Floor authority is deliberately absent from a V3 boundary group.
        if (ceiling.contains("stair_from_floor_id") && ceiling.at("stair_from_floor_id") != "")
            throw std::invalid_argument("native floor source graph unavailable");
        if (ceiling.value("kind", std::string{}) == "sloped") {
            ceiling["room_boundary_id"] = "";
            ceiling["complete_room_observed"] = false;
        }
    }
    const auto marker = entity.extensions.find("vertex_dxf_boundary");
    if (marker != entity.extensions.end() && marker->is_object() && marker->value("version", 0) == 3) {
        std::vector<std::string> members;
        for (const auto& id : marker->at("member_ids")) members.push_back(ids.at(id.get<std::string>()));
        std::sort(members.begin(), members.end());
        (*marker)["member_ids"] = members;
    }
}

// Shared broker/desktop admission: markers use current (already fresh) owner
// IDs, with one identical sorted membership list on every connected member.
// This validates topology only; the mapper additionally proves each exact plan
// and complete editable Document before publishing any member.
inline void validate_native_dxf_boundary_groups(const std::vector<Entity>& entities) {
    std::map<std::string, const Entity*, std::less<>> owners;
    for (const auto& entity : entities)
        if (!owners.emplace(entity.id, &entity).second) throw std::invalid_argument("duplicate imported identity");
    std::set<std::string> checked;
    for (const auto& entity : entities) {
        const auto marker = entity.extensions.find("vertex_dxf_boundary");
        if (marker == entity.extensions.end() || !marker->is_object() || marker->value("version", 0) != 3) continue;
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
        for (const auto& id : members) {
            const auto owner = owners.find(id);
            if (owner == owners.end() || checked.contains(id)) throw std::invalid_argument("partial native boundary group");
            const auto& member = *owner->second;
            if ((member.type != "boundary" && member.type != "measurement_boundary") ||
                !member.extensions.contains("vertex_dxf_boundary") || member.extensions.at("vertex_dxf_boundary") != *marker ||
                native_dxf_boundary_has_untransported_source_links(member))
                throw std::invalid_argument("mismatched native boundary group");
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
// and explicit typed links. Ordinary outer/hole curves accompany bounded source
// properties, classifications and topology. Missing, inactive, cyclic or invalid
// groups fall back together. Live measurement and stair-floor graphs are not
// transported. Imported sloped observations require source reconfirmation.
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
// JSON and V2/V3 closed-boundary JSON with depiction BOUNDARY_PLAN_V1. V3 requires
// complete matching connected membership and typed appraisal dependency proof.
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
