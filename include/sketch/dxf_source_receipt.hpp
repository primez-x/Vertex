#pragma once

#include "sketch/dxf_phase_asset_carrier.hpp"

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

inline constexpr std::size_t native_dxf_source_dependency_segment_limit = 4'096;
inline constexpr std::uint64_t native_dxf_source_ordinary_byte_limit = 16ULL * 1024 * 1024;
using NativeDxfSourceAssetMapping = std::map<std::string, std::string, std::less<>>;
using NativeDxfSourceDependencyRefs = std::span<const nlohmann::json* const>;

struct NativeDxfSourceReceipt {
    Asset ordinary_asset;
    Asset recipe_asset;
    // Merge these fields into the main dxf_source entity. asset_ids references
    // the two receipt Assets; source_receipt is a strict immutable envelope.
    nlohmann::json source_properties;
    // Publish every segment as another dxf_source entity in the same command.
    // Each has generic top-level asset_ids, never payloads or mapping rows.
    std::vector<nlohmann::json> dependency_properties;
};

// Asset-bearing canonical Vertex DXF only. Authenticate the actual original
// carrier against source_assets before accepting the exact one-to-one mapping
// to retained_assets. Both inventories are borrowed; payloads are never copied
// into the recipe. ordinary/recipe IDs must be fresh in the destination.
// Receipt metadata uses a strict binary v1 format (fixed-width unsigned numbers,
// lowercase SHA-256 hex and bounded raw IDs), capped at 256 MiB per Asset.
[[nodiscard]] NativeDxfSourceReceipt create_native_dxf_source_receipt(
    std::string_view original, const NativeDxfPhaseSourceAssets& source_assets,
    const NativeDxfSourceAssetMapping& original_to_fresh,
    const NativeDxfPhaseSourceAssetRefs& retained_assets,
    std::string ordinary_asset_id, std::string recipe_asset_id,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);

// Strict receipt, actual payload and streamed full original integrity, without
// allocating the Base64 carrier. source_properties may contain normal dxf_source
// fields, but source_receipt and each dependency segment have exact schemas.
// retained_assets must contain exactly the two receipt Assets plus the recipe's
// fresh payload dependencies. Borrow non-null segments in canonical ordinal
// order; all segment properties and Assets must remain alive and unchanged.
// Document/command validation must call this hook in addition to generic asset
// reference validation, and must preserve the immutable envelope/segments.
void validate_native_dxf_source_receipt(const nlohmann::json& source_properties,
    NativeDxfSourceDependencyRefs dependency_properties,
    const NativeDxfPhaseSourceAssetRefs& retained_assets,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);

// Reconstruct the immutable imported source, independently of all active model
// edits. Checks receipt/segment completeness and actual bytes first; canonical
// footer reconstruction must match the pinned full original length and SHA-256.
[[nodiscard]] std::string reconstruct_native_dxf_source_receipt(
    const nlohmann::json& source_properties,
    NativeDxfSourceDependencyRefs dependency_properties,
    const NativeDxfPhaseSourceAssetRefs& retained_assets,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);

} // namespace sketch
