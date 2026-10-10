#pragma once

#include "sketch/document.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {
struct NativeDxfWallSourceWorkBudget;

// Retained coordinated documentation, independent of the active CAD picture.
[[nodiscard]] bool native_dxf_sheet_view_source_type(std::string_view type) noexcept;
using NativeDxfSheetViewSourceMaps = std::map<std::string,
    std::map<std::string, std::string, std::less<>>, std::less<>>;

// Complete actual global object, appearance, overlay and associative-host references.
// Empty role hints admit any actual document owner; bound dimension hosts are
// additionally checked against Document's canonical supported object types.
// Missing unbound overlay witnesses stay detached and are not required owners.
[[nodiscard]] std::map<std::string, std::string, std::less<>>
native_dxf_sheet_view_source_dependencies(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
// Sorted unique view IDs in this actual companion's local namespace. Sheets,
// viewports, overlays, revisions, callouts and schedules are never global IDs.
[[nodiscard]] std::vector<std::string> native_dxf_sheet_view_source_view_identity_ids(
    const Entity& source, NativeDxfWallSourceWorkBudget* work_budget = nullptr);

// Admission precedes every semantic decode/copy. Failed attempts stay charged
// to catalog_transfer. Reserve extra codec replays before a downstream consumer
// (such as private Document admission); this function does not decode a model.
void admit_native_dxf_sheet_view_source_work(const Entity& source,
    NativeDxfWallSourceWorkBudget& budget, std::size_t codec_replays = 1);
void validate_native_dxf_sheet_view_source(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);

// Preserve unresolved overlay witnesses as unresolved across publication.
// Check both imported and retained destination companions: adding an owner
// must not silently turn detached view coordinates into an object attachment.
void validate_native_dxf_sheet_view_witness_binding(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& original_owners,
    const std::map<std::string, Entity, std::less<>>& candidate_owners,
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);

// Patch a private raw copy and publish only after intrinsic revalidation.
// Entity.id remains the graph mapper's responsibility. The optional view map
// is exact, injective and scoped by the ORIGINAL source companion owner ID.
// Only model views[].id and sheets[].viewports[].view_id follow that map;
// callouts retain their sheet/viewport links. All other local IDs, raw numeric
// types, ordering, metadata and extensions remain untouched. The strict entity
// envelope has no canonical context slots; context_mapping is intentionally
// unused and is accepted for the graph's common dependency-mapper interface.
void remap_native_dxf_sheet_view_source_dependencies(Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    const std::map<std::string, std::string, std::less<>>& owner_mapping,
    const std::map<std::string, std::string, std::less<>>& context_mapping,
    const NativeDxfSheetViewSourceMaps& view_mapping = {},
    NativeDxfWallSourceWorkBudget* work_budget = nullptr);
} // namespace sketch
