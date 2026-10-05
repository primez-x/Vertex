#pragma once
#include "sketch/pinc_project_import.hpp"

namespace sketch {
// A separate, strict JSON protocol: VertexPincCandidate / pinc-project / v1.
// This carries typed detached source evidence, never Document entities, image
// decoding results, broker attestation or native identity authority.
[[nodiscard]] std::vector<std::byte> encode_pinc_import_candidate(const PincImportProject& project);
[[nodiscard]] PincImportProject decode_pinc_import_candidate(std::span<const std::byte> bytes);
} // namespace sketch
