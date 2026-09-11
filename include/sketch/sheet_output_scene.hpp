#pragma once

#include "sketch/document.hpp"
#include "sketch/output_fingerprint.hpp"

namespace sketch {

inline constexpr std::uint32_t kSheetOutputSceneVersion = 1;

// Resolves one persisted sheet_view_model entity and selected sheet. The
// detached envelope retains the validated graph for cross-sheet references.
// This adapter owns inputs.views: callers must leave that group unspecified
// and empty. All other fingerprint dependencies remain caller responsibilities.
// Throws std::invalid_argument or OutputFingerprintError on invalid inputs.
[[nodiscard]] nlohmann::json make_sheet_output_scene(
    const DocumentSnapshot& snapshot, const std::string& entity_id,
    const std::string& sheet_id, const OutputFingerprintInputs& inputs);

// Validates envelope, graph, selection, fingerprint integrity and scene binding,
// then compares against the current snapshot and dependencies. A well-formed
// old scene is valid but not current. Invalid scenes/inputs return valid=false.
[[nodiscard]] OutputFingerprintCurrentness check_sheet_output_scene_current(
    const nlohmann::json& encoded, const DocumentSnapshot& snapshot,
    const OutputFingerprintInputs& inputs);

} // namespace sketch
