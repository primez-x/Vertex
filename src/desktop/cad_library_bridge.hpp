#pragma once

#include <filesystem>
#include <cstdint>
#include <span>
#include <string_view>
#include <nlohmann/json.hpp>

namespace sketch::desktop {

// Worker-only, synchronous API. The caller verifies the bundled runtime before
// calling and supplies its absolute directory. Only the two adapter entry points
// are accepted; Python and native output are suppressed until shutdown finishes.
// Source-built IFC selection requires all three compiled controlled-runtime
// identities; the call has no environment or API switch for selecting that mode.
[[nodiscard]] nlohmann::json call_cad_library(
    const std::filesystem::path& runtime_root, const char* function,
    std::string_view bytes, std::span<const std::uint64_t> native_admitted_ids = {});

} // namespace sketch::desktop
