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
inline constexpr std::size_t native_dxf_phase_destination_asset_replay_byte_limit = 256 * 1024 * 1024;
inline constexpr std::size_t native_dxf_phase_destination_asset_replay_work_limit = 1024 * 1024 * 1024;

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
    const NativeDxfPhaseSourceGraph& graph, NativeDxfWallSourceWorkBudget* work_budget = nullptr);
[[nodiscard]] NativeDxfPhaseSourceGraph decode_native_dxf_phase_source_graph(
    const nlohmann::json& value, NativeDxfWallSourceWorkBudget* work_budget = nullptr);
void validate_native_dxf_phase_source_graph(const NativeDxfPhaseSourceGraph& graph,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);

using NativeDxfPhaseOwnerMap = std::map<std::string, std::string, std::less<>>;
// Exact, complete and injective document-owner maps. Stair children have their
// own namespace; local alternative/level/material/type/part/point IDs stay raw.
// Explicit stair children must have globally unambiguous original IDs because
// the architectural child helper consumes a flat map. Legacy flight/body
// aliases use body_owner_ids and never enter stair_child_ids.
struct NativeDxfPhaseDestinationMaps {
    NativeDxfPhaseOwnerMap body_owner_ids;
    NativeDxfPhaseOwnerMap catalog_owner_ids;
    NativeDxfPhaseOwnerMap registry_owner_ids;
    NativeDxfPhaseOwnerMap reviewed_context_owner_ids;
    NativeDxfPhaseOwnerMap stair_child_ids;
};
struct NativeDxfPhaseDestinationBinding {
    NativeDxfPhaseSourceGraph mapped_graph;
    // Fresh bodies/catalogs/registries and explicitly reviewed new contexts
    // only. Existing reviewed hierarchy is never copied into publication.
    std::vector<Entity> staged_entities;
};
// Complete raw current-state admission before private Document::create.
// Reserves ambient model/catalog/architectural/geometry consumer passes before
// those consumers can decode; this performs no native geometry qualification.
void admit_native_dxf_phase_document_entities(
    const std::map<std::string, Entity, std::less<>>& owners,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
// Reserve full current-design physical-room detection immediately before an
// explicit checks call. Inert/inactive descriptors and ordinary retained wall
// snapshots pay intrinsic admission only. Uses the 64 Mi shared typed-work
// ledger, never the legacy V5-V8 250,000 source-work lane.
void admit_native_dxf_phase_physical_room_checks(
    const std::map<std::string, Entity, std::less<>>& owners,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
// Admission before a caller invokes retained-history fork/preview. Charges all
// retained snapshots and assets, with conservative repeated replay bounds.
// Visits every current typed geometry-history proof's dynamic fields before
// replay, using the same inventory as mixed phase demolition admission.
void admit_native_dxf_phase_destination_snapshot(const DocumentSnapshot& actual_destination,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
// Pure mapped source evidence, with unchanged complete role/depiction semantics.
// This grants no destination owner creation or publication authority.
[[nodiscard]] NativeDxfPhaseSourceGraph remap_native_dxf_phase_source_graph(
    const NativeDxfPhaseSourceGraph& source, const NativeDxfPhaseDestinationMaps& maps,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
// Source proof remains separate. Every context target must exist in the actual
// destination or be explicitly supplied as a reviewed new context, and equal
// the source owner after typed ID substitution, including extensions, required
// bits and numeric representation. Caller review of genuinely new hierarchy
// is required; mapped source evidence itself cannot authorize creation. Fresh
// authored targets cannot overwrite any actual destination owner. Admission
// precedes private complete-document validation; this never mutates a Document.
[[nodiscard]] NativeDxfPhaseDestinationBinding bind_native_dxf_phase_source_destinations(
    const NativeDxfPhaseSourceGraph& source, const NativeDxfPhaseDestinationMaps& maps,
    const DocumentSnapshot& actual_destination,
    const std::vector<Entity>& reviewed_new_context_owners = {},
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
// Reconstructs the raw expected result, checks exact staged-owner/role/depiction
// parity, and revalidates the entire actual combined destination (all registries,
// hosts and join ownership). Suitable before one caller-owned atomic command;
// live full-snapshot publication authority remains the caller's responsibility.
void validate_native_dxf_phase_destination_binding(
    const NativeDxfPhaseSourceGraph& source, const NativeDxfPhaseDestinationMaps& maps,
    const NativeDxfPhaseDestinationBinding& binding, const DocumentSnapshot& actual_destination,
    const std::vector<Entity>& reviewed_new_context_owners = {},
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
} // namespace sketch
