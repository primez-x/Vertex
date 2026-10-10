#pragma once

#include "sketch/document.hpp"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

inline constexpr std::uint64_t native_dxf_phase_asset_payload_limit = 256ULL * 1024 * 1024;
// Native ProjectStore/recovery destination capacity, distinct from a source
// transfer table. Individual payloads retain the 256 MiB native asset limit.
inline constexpr std::uint64_t native_dxf_phase_destination_asset_payload_limit = 512ULL * 1024 * 1024;
inline constexpr std::size_t native_dxf_phase_destination_asset_count_limit = 100'000;
inline constexpr std::uint64_t native_dxf_phase_asset_work_limit = 8ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t native_dxf_phase_asset_json_byte_limit = 16ULL * 1024 * 1024;
inline constexpr std::uint64_t native_dxf_phase_asset_json_node_limit = 262'144;
inline constexpr std::size_t native_dxf_phase_asset_count_limit =
    (native_dxf_phase_asset_json_node_limit - 4) / 6;
inline constexpr std::size_t native_dxf_phase_asset_chunk_byte_limit = 4'500;
inline constexpr std::size_t native_dxf_phase_asset_chunk_text_limit = 6'000;

// Payload capacity applies independently to each complete inventory/table.
// Work and JSON admission are cumulative; failed attempts remain charged.
// These lanes never debit the nonlinear source-geometry work budget.
struct NativeDxfPhaseAssetWorkBudget {
    std::uint64_t max_payload_bytes{native_dxf_phase_asset_payload_limit};
    std::uint64_t max_work_bytes{native_dxf_phase_asset_work_limit};
    std::uint64_t consumed_work_bytes{};
    std::uint64_t max_json_bytes{native_dxf_phase_asset_json_byte_limit};
    std::uint64_t max_json_nodes{native_dxf_phase_asset_json_node_limit};
    std::uint64_t consumed_json_bytes{};
    std::uint64_t consumed_json_nodes{};
};

using NativeDxfPhaseSourceAssets = std::map<std::string, Asset, std::less<>>;
using NativeDxfPhaseSourceAssetRefs = std::map<std::string, const Asset*, std::less<>>;
struct NativeDxfPhaseAssetDescriptor {
    std::string id;
    std::string media_type;
    std::uint64_t byte_count{};
    std::string sha256;
    nlohmann::json metadata = nlohmann::json::object();
};
using NativeDxfPhaseAssetManifest =
    std::map<std::string, NativeDxfPhaseAssetDescriptor, std::less<>>;

// Exact schema vertex.dxf.phase-assets.v1/version 1; sorted descriptor rows.
// Payload bytes are carried separately, never encoded into this JSON value.
[[nodiscard]] nlohmann::json encode_native_dxf_phase_asset_manifest(
    const NativeDxfPhaseSourceAssets& assets, NativeDxfPhaseAssetWorkBudget* budget = nullptr);
[[nodiscard]] NativeDxfPhaseAssetManifest decode_native_dxf_phase_asset_manifest(
    const nlohmann::json& value, NativeDxfPhaseAssetWorkBudget* budget = nullptr);
void validate_native_dxf_phase_source_assets(const NativeDxfPhaseSourceAssets& assets,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);
// Borrowed inventory admission before a caller copies retained payloads.
// Non-null values must match their keys; provenance remains caller-owned.
void validate_native_dxf_phase_source_asset_refs(const NativeDxfPhaseSourceAssetRefs& assets,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);
// Actual current/retained/combined destination inventory only: fixed 512 MiB
// verified unique-content aggregate and native 100,000-row ceiling. Equal
// declared hashes alone cannot alias different bytes. max_payload_bytes remains the
// lowerable per-asset limit (at most 256 MiB); shared JSON/work limits and all
// actual metadata/hash checks remain unchanged. No source carrier admission.
void validate_native_dxf_phase_destination_asset_refs(const NativeDxfPhaseSourceAssetRefs& assets,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);
void validate_native_dxf_phase_asset_manifest(const NativeDxfPhaseAssetManifest& manifest,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);
// Exact IDs, actual hashes, and raw metadata numeric types/signed zero.
// Does not copy payload bytes or grant document/publication authority.
void bind_native_dxf_phase_asset_manifest(const NativeDxfPhaseAssetManifest& manifest,
    const NativeDxfPhaseSourceAssets& assets, NativeDxfPhaseAssetWorkBudget* budget = nullptr);

// Canonical RFC 4648 Base64 with no whitespace; one bounded carrier chunk.
[[nodiscard]] std::string encode_native_dxf_phase_asset_chunk(std::span<const std::byte> bytes,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);
[[nodiscard]] std::vector<std::byte> decode_native_dxf_phase_asset_chunk(std::string_view text,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);

// PSAS0001, u32LE count, sorted (u32LE ID length, raw ID, u64LE length, bytes).
// The reader admits count/IDs/lengths against the supplied manifest before
// allocating bytes. It consumes exactly this table; outer framing checks EOF.
void write_native_dxf_phase_asset_table(std::ostream& output,
    const NativeDxfPhaseAssetManifest& manifest, const NativeDxfPhaseSourceAssets& assets,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);
[[nodiscard]] NativeDxfPhaseSourceAssets read_native_dxf_phase_asset_table(std::istream& input,
    const NativeDxfPhaseAssetManifest& manifest, NativeDxfPhaseAssetWorkBudget* budget = nullptr);

} // namespace sketch
