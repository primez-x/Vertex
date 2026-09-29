#pragma once

#include "sketch/document.hpp"

#include <nlohmann/json.hpp>
#include <string_view>

namespace sketch {

// Rebuilds an existing identified survey measurement boundary from entered
// calls. The current first vertex anchors the called north bearings. Derived
// coordinates and diagnostics in the report are ignored; exact input receipts
// are checked. The returned single upsert is revision-fenced and has passed a
// private Document preview, including dependent targets and constraints.
// Construction/derivation-backed boundaries are explicitly unsupported.
[[nodiscard]] ApplyEntityChanges survey_boundary_update_command(
    const DocumentSnapshot& source, std::string_view boundary_id,
    const nlohmann::json& report, bool adjust_final_endpoint);

} // namespace sketch
