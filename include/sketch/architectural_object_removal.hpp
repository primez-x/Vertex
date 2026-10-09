#pragma once

#include "sketch/document.hpp"

#include <utility>

namespace sketch {

// Pure actual-source removal for known stairs/railings, columns/beams and slabs.
// Derives attached rails, hosted catalog rows and known presentation dependents;
// refuses protected phase ownership and affected opaque/future references.
// Optional components are actual catalog/instance keys, never render aliases.
// Component-only selection is supported; ordinary wall/roof hosts are admitted
// through their existing codecs without acquiring physical deletion authority.
// The source is immutable. Document command admission retains source history.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_architectural_object_removal(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<std::string>& selected_object_ids,
    const std::vector<std::pair<std::string, std::string>>& explicit_components = {},
    bool allow_manufactured_opening_hosts = false);

} // namespace sketch
