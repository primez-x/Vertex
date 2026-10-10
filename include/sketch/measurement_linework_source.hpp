#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sketch {
struct MeasurementLineworkSourceCheck {
    bool current{};
    std::string diagnostic;
    // A unique same-topology face, ordered to match the retained area's edges.
    // Its stable identities and metadata are owned by the document adapter.
    std::optional<Boundary> proposed_boundary;
    nlohmann::json proposed_lineage;
    // Group refreshes replace boundary, outer lineage and canonical member
    // record together. Indices identify the distinct current graph faces.
    nlohmann::json proposed_group;
    std::vector<std::size_t> group_face_indices;
};
// Validate/query the optional extensions.measurement_linework_copy_scope
// semantic flag. Absent returns false; present requires measurement_linework
// and exactly {"version":1} with an integer version. Malformed/unknown markers
// throw invalid_argument. The marker does not contain a source cohort identity.
[[nodiscard]] bool measurement_linework_copy_isolated(const Entity& entity);
// Sorted unique source owners from retained outer and group-member lineage.
// No lineage returns empty. Present lineage requires a supported measurement
// boundary and the existing strict outer/member schemas and budgets.
[[nodiscard]] std::vector<std::string> measurement_linework_source_ids(const Entity& entity);
// Remap only typed retained source-use references, preserving the entity owner,
// local boundary IDs, receipts, use order and raw interval/reversal JSON.
// Every referenced owner needs a nonempty mapping; distinct owners cannot share
// a target. An empty segment map preserves source-local segment IDs. Otherwise
// every original {owner,segment} pair needs a nonempty mapping and distinct
// source segments within one owner cannot share a target. Extra map entries are
// ignored. Validation occurs before and after rewriting a private copy; callers
// must separately validate the complete remapped source graph and geometry.
[[nodiscard]] Entity remap_measurement_linework_source_references(const Entity& entity,
    const std::map<std::string,std::string,std::less<>>& owner_ids,
    const std::map<std::pair<std::string,std::string>,std::string>& segment_ids = {});
// Unmarked lineage uses the historical shared drawing-layer graph, excluding
// marked copy owners. If any retained outer/group-member source owner is marked,
// rebuild from exactly all referenced owners and ALL their canonical edges.
// Cohorts come from validated retained lineage, never arbitrary scope IDs.
// Graph/source/topology and group matching budgets still apply; the additional
// cohort cache/work is bounded per call: 16 resident graphs, 256 rebuilds,
// 65536 canonical source segments and 16000000 possible segment pairs. Cache
// misses/evictions and failed builds consume work; exhaustion refuses a check.
// With no markers, graph construction/replay follows the historical path.
// Ordinary boundaries without measured lineage are omitted. No inputs change.
[[nodiscard]] std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>
measurement_linework_source_checks(const std::map<std::string,Entity,std::less<>>& entities,
    const std::set<std::string,std::less<>>* semantic_visible=nullptr);
// Reconstruct only previously current, unambiguous, unauthored consumers
// from the final source geometry. Preserves area IDs, facts and topology.
// Active semantic phases apply; presentation visibility does not.
[[nodiscard]] std::map<std::string,Entity,std::less<>>
complete_measurement_linework_sources(
    const std::map<std::string,Entity,std::less<>>& before,
    const std::map<std::string,Entity,std::less<>>& candidate);
// Uses all admitted saved registries; inactive consumers are skipped before decoding.
[[nodiscard]] std::map<std::string,Entity,std::less<>>
complete_measurement_linework_sources_active_phase(
    const std::map<std::string,Entity,std::less<>>& before,
    const std::map<std::string,Entity,std::less<>>& candidate);
[[nodiscard]] bool measurement_linework_sources_visible(const Entity& area,
    const std::set<std::string,std::less<>>* semantic_visible);
[[nodiscard]] inline bool measurement_linework_source_current(
    const std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>& checks,const Entity& area) {
    if(!area.extensions.contains("measurement_linework_sources") &&
       !(area.type=="measurement_boundary" && area.extensions.contains("measurement_linework_group"))) return true;
    const auto found=checks.find(area.id);
    return found!=checks.end() && found->second.current;
}
} // namespace sketch
