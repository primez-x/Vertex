#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace sketch {

// Private core memo for successful full historical roof-archive validation.
// Callers must still validate current entities and native source results.
void validate_roof_derivation_cached(
    std::string_view dialect,
    const std::string& exact_archive_wire,
    const std::function<void()>& validate);

} // namespace sketch
