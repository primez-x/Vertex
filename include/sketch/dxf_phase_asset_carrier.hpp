#pragma once

#include "sketch/dxf_phase_asset_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

inline constexpr std::uint64_t native_dxf_phase_asset_transport_byte_limit = 512ULL * 1024 * 1024;
inline constexpr std::size_t native_dxf_phase_asset_carrier_chunk_bytes = 168;

// Each asset borrows one contiguous sequence of physical 999 data records;
// inventory storage requires no allocation per payload record. All views require the original
// input to remain alive and unchanged, including while decoding.
struct NativeDxfPhaseAssetCarrierRecord {
    std::string_view id;
    std::string_view sha256;
    std::uint64_t byte_count{};
    std::uint64_t chunk_count{};
    std::string_view data_records;
};
struct NativeDxfPhaseAssetCarrier {
    std::string ordinary_dxf;
    bool has_assets{};
    std::string_view full_input;
    std::vector<NativeDxfPhaseAssetCarrierRecord> records;
};

// Detection only, bounded to the transport ceiling. A true result confers no
// admission or validity; split/decode enforce framing and actual graph binding.
[[nodiscard]] bool native_dxf_phase_asset_carrier_present(std::string_view input) noexcept;
[[nodiscard]] NativeDxfPhaseAssetCarrier split_native_dxf_phase_asset_carrier(
    std::string_view input, NativeDxfPhaseAssetWorkBudget* budget = nullptr);
[[nodiscard]] NativeDxfPhaseSourceAssets decode_native_dxf_phase_asset_carrier(
    const NativeDxfPhaseAssetCarrier& carrier, const NativeDxfPhaseAssetManifest& manifest,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);
[[nodiscard]] std::string export_native_dxf_phase_asset_carrier(
    std::string_view ordinary_dxf, const NativeDxfPhaseSourceAssets& assets,
    NativeDxfPhaseAssetWorkBudget* budget = nullptr);

} // namespace sketch
