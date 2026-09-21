#pragma once

#include "sketch/document.hpp"
#include "sketch/output_fingerprint.hpp"

namespace sketch {

inline constexpr std::uint32_t kSheetOutputSceneVersion = 1;
inline constexpr std::uint32_t kSheetSetOutputSceneVersion = 1;

// Resolves one persisted sheet_view_model entity and selected sheet. The
// detached envelope retains the validated graph for cross-sheet references.
// This adapter owns the scene resource within inputs.views. Callers may provide
// additional resource-backed view inputs, such as effective visibility masks;
// the adapter appends and validates its own scene resource. All other
// fingerprint dependencies remain caller responsibilities.
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

// Resolves the complete drawing set from one persisted sheet_view_model entity.
// Its distinct sketch.sheet-set-output-scene envelope binds sheet_ids in the
// model's explicit sheet_order(), including every sheet exactly once. Storage
// array order is canonicalized; the explicit output order is never sorted.
// As for the selected-sheet adapter, caller view resources are retained and
// other fingerprint dependencies remain caller responsibilities.
[[nodiscard]] nlohmann::json make_sheet_set_output_scene(
    const DocumentSnapshot& snapshot, const std::string& entity_id,
    const OutputFingerprintInputs& inputs);

// Validates the entire detached set and its ordered membership/fingerprint
// binding before comparing with the current snapshot and dependencies.
// A valid old set is stale after order, content, or dependency changes.
// Invalid envelopes, memberships, bindings, or inputs return valid=false.
[[nodiscard]] OutputFingerprintCurrentness check_sheet_set_output_scene_current(
    const nlohmann::json& encoded, const DocumentSnapshot& snapshot,
    const OutputFingerprintInputs& inputs);

} // namespace sketch
