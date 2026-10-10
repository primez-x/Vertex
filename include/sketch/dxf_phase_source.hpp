#pragma once

#include "sketch/document.hpp"

#include <map>
#include <string>
#include <vector>

namespace sketch {
struct NativeDxfWallSourceWorkBudget;

inline constexpr std::size_t native_dxf_phase_source_owner_limit = 16'384;
inline constexpr std::size_t native_dxf_phase_source_byte_limit = 16 * 1024 * 1024;
inline constexpr std::size_t native_dxf_phase_source_node_limit = 262'144;
inline constexpr std::size_t native_dxf_phase_source_work_limit = 67'108'864;
inline constexpr std::size_t native_dxf_phase_source_depth_limit = 68;

// Complete actual source authoring inventory, independent of current CAD
// depiction. Exactly one primary role owns every entity. Hierarchy enrollment
// is a subset of context_ids; depiction is the complete active body subset.
// Alternative/level/material/type/part/stair-child/terrain-point identities
// remain inside their raw owners, never in these document-owner inventories.
// This value grants no destination hierarchy or publication authority.
struct NativeDxfPhaseSourceGraph {
    std::map<std::string, Entity, std::less<>> entities;
    std::vector<std::string> body_ids;
    std::vector<std::string> catalog_ids;
    std::vector<std::string> registry_ids;
    std::vector<std::string> context_ids;
    std::vector<std::string> enrolled_hierarchy_ids;
    std::vector<std::string> depicted_body_ids;
    bool operator==(const NativeDxfPhaseSourceGraph&) const = default;
};

// Seeds name actual owners, not a depiction filter. Any touched registry adds
// its entire roster, including all inactive proposals and demolished baseline
// members. Complete raw catalogs, actual hosts and support hierarchy close
// recursively. Unsupported relationships refuse instead of pruning the roster.
[[nodiscard]] NativeDxfPhaseSourceGraph capture_native_dxf_phase_source_graph(
    const DocumentSnapshot& source, const std::vector<std::string>& seed_owner_ids,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);

// Version 1 has an exact sorted entities array of raw five-field Entity rows
// and six explicit sorted unique role/subset arrays. No canonical entity/model
// encoder is used. Admission precedes semantic decoders and the organizer;
// failed attempts remain charged to the shared catalog/architectural ledger.
[[nodiscard]] nlohmann::json encode_native_dxf_phase_source_graph(
    const NativeDxfPhaseSourceGraph& graph);
[[nodiscard]] NativeDxfPhaseSourceGraph decode_native_dxf_phase_source_graph(
    const nlohmann::json& value, NativeDxfWallSourceWorkBudget* work_budget = nullptr);
void validate_native_dxf_phase_source_graph(const NativeDxfPhaseSourceGraph& graph,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
} // namespace sketch
