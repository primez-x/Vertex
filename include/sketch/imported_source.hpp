#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Per call: current state only, no history traversal or snapshot copy. Refuse
// over-limit or malformed known owner records explicitly; never silently omit
// them. JSON limits apply cumulatively to inspected source descriptors, excluding
// diagnostic contents. Diagnostics are inspected only for their selected owner.
inline constexpr std::size_t imported_source_scan_limit = 100'000;
inline constexpr std::size_t imported_source_reference_limit = 100'000;
inline constexpr std::size_t imported_source_name_byte_limit = 16'384;
inline constexpr std::size_t imported_source_json_node_limit = 262'144;
inline constexpr std::uint64_t imported_source_json_byte_limit = 16ULL * 1024 * 1024;
inline constexpr std::size_t imported_source_json_depth_limit = 64;
inline constexpr std::size_t imported_source_stream_chunk_bytes = 64 * 1024;
inline constexpr std::size_t imported_source_diagnostic_record_limit = 100'000;
inline constexpr std::size_t imported_source_diagnostic_node_limit = 100'000;
inline constexpr std::uint64_t imported_source_diagnostic_string_byte_limit = 32ULL * 1024 * 1024;
inline constexpr std::size_t imported_source_diagnostic_page_limit = 1000;

enum class ImportedSourceFormat { dxf, ifc, pinc };

struct ImportedSourceInfo {
    std::string owner_id;
    ImportedSourceFormat format;
    // Original metadata only. Never use this string as a trusted destination.
    std::string source_name;
    std::uint64_t byte_count{};
    std::string sha256;
    std::size_t diagnostic_count{};
    bool reconstructed{};
};

// Lists actual DXF/IFC source owners and Pinc sheet owners, in owner-ID order.
// Receipt dependencies are references, not separate original files. Descriptors
// are structurally admitted here; actual full-file integrity is proved by export.
[[nodiscard]] std::vector<ImportedSourceInfo> list_imported_sources(const DocumentSnapshot& snapshot);

// Uses only this authoritative snapshot and its exact owner references. Actual
// payload/full reconstructed original hashes are verified before the first sink
// invocation. Sink/spans are synchronous and must not escape; throwing aborts.
// Core never opens a file, chooses a destination, or publishes partial output.
// The snapshot must stay alive and unchanged, including inside sink callbacks.
void stream_imported_source(const DocumentSnapshot& snapshot, std::string_view owner_id,
    const std::function<void(std::span<const std::byte>)>& sink);

// Validates the exact owner's whole persisted log against the document JSON
// limits and producer diagnostic limits, then copies one bounded page. Offset
// may equal the retained count (an empty page); larger offsets, zero limits and
// limits above the page ceiling are refused. ImportedSourceInfo::diagnostic_count
// is the retained total, independent of the returned page size.
[[nodiscard]] nlohmann::json imported_source_diagnostics(
    const DocumentSnapshot& snapshot, std::string_view owner_id,
    std::size_t offset = 0, std::size_t limit = imported_source_diagnostic_page_limit);

} // namespace sketch
