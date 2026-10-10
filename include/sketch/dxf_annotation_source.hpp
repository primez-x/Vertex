#pragma once

#include "sketch/document.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {
struct NativeDxfWallSourceWorkBudget;

// Retained support owners for the complete V9 phase graph. Their raw source is
// independent of CAD presentation and grants no current analytical quantities.
[[nodiscard]] bool native_dxf_annotation_source_type(std::string_view type) noexcept;
// Empty role hints admit an actual document owner of any supported graph role.
// Child drawing layers have the exact "layer" role. Top-level context is
// handled by the graph's generic context pass; level and saved-view IDs are local.
// Object appearance resolves supported actual bodies before same-state children.
[[nodiscard]] std::map<std::string, std::string, std::less<>>
native_dxf_annotation_source_dependencies(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored);
using NativeDxfAnnotationChildMaps = std::map<std::string,
    std::map<std::string, std::string, std::less<>>, std::less<>>;
// Sorted unique label/symbol identities, qualified by this annotation owner.
// Definition/template/symbol IDs never appear here. Dimensions return no IDs.
[[nodiscard]] std::vector<std::string> native_dxf_annotation_child_identity_ids(const Entity& source);
// Patch typed references in a private raw copy, publishing it only on success.
// Entity.id and top-level contexts remain the graph mapper's responsibility.
// Pass the original source ID; it selects the optional owner-qualified child map.
// All other JSON, numeric dialects, local IDs and symbol snapshots stay raw.
void remap_native_dxf_annotation_source_dependencies(Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    const std::map<std::string, std::string, std::less<>>& owner_mapping,
    const std::map<std::string, std::string, std::less<>>& context_mapping,
    const NativeDxfAnnotationChildMaps& child_mapping = {});
// Raw/work admission precedes every annotation/dimension codec and retained
// target resolver. All attempts share catalog_transfer; failed work stays charged.
// Pinned SVG uses the actual bounded admission consumer with incremental billing
// and explicit reservations for later codec replays; artwork bytes remain raw.
void admit_native_dxf_annotation_source_work(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored,
    NativeDxfWallSourceWorkBudget& budget, std::size_t codec_replays = 1);
// Call after admission with the actual complete source map. Dimension targets
// are structurally validated, including retained stale physical-room targets;
// this does not qualify a current quantity or active physical inventory.
// Saved output-view overrides explicitly refuse until companion sheet/view
// transport is implemented; their local spellings are never remapped to owners.
void validate_native_dxf_annotation_source(const Entity& source,
    const std::map<std::string, Entity, std::less<>>& authored);
} // namespace sketch
