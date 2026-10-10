#pragma once

#include "sketch/document.hpp"

#include <string_view>

namespace sketch {

struct ArchitecturalSelectionRemovalIntent;

using DrawingSelectionRemovalEntities = std::map<std::string, Entity, std::less<>>;

enum class DrawingSelectionAnnotationKind { label, symbol };

struct DrawingSelectionAnnotationTarget {
    std::string owner_id;
    DrawingSelectionAnnotationKind kind{};
    std::string child_id;
    bool operator==(const DrawingSelectionAnnotationTarget&) const = default;
};

// Closed v1 authority: actual independent drawing owners and actual local
// annotation rows. Both lists must be strictly sorted (annotation order is
// owner_id, wire kind, child_id). Render aliases and container promotion are
// never removal authority. Policy is captured explicitly by the caller.
struct DrawingSelectionRemovalIntent {
    std::vector<std::string> owner_ids;
    std::vector<DrawingSelectionAnnotationTarget> annotations;
    bool operator==(const DrawingSelectionRemovalIntent&) const = default;
};

[[nodiscard]] nlohmann::json encode_drawing_selection_removal_intent(
    const DrawingSelectionRemovalIntent& intent);
[[nodiscard]] DrawingSelectionRemovalIntent decode_drawing_selection_removal_intent(
    const nlohmann::json& value);
[[nodiscard]] std::string drawing_selection_removal_intent_bytes(
    const DrawingSelectionRemovalIntent& intent);
[[nodiscard]] DrawingSelectionRemovalIntent decode_drawing_selection_removal_intent_bytes(
    std::string_view bytes);

// Analytical, bounded actual-source replay; no native geometry construction.
// Returns the complete candidate, retaining raw surviving rows and dialects.
// Malformed/unsupported affected references and protected phase owners refuse.
// Supported authored measured strokes retain ordinary-removal semantics even
// with required=true (reader support); other required roots are protected.
// Required annotation carriers remain present when their selected rows retire.
[[nodiscard]] DrawingSelectionRemovalEntities replay_drawing_selection_removal(
    const DrawingSelectionRemovalEntities& actual,
    const DrawingSelectionRemovalIntent& intent, bool active_phase_constraints);

// The enclosing Document independently replays the exact prior room-review
// proof before supplying admitted_review_stage. This function does not confer
// provenance on that stage. Selection authority always comes from actual;
// selected owners/rows cannot borrow unrelated staged edits. Identical known
// reference retirements collapse, while other admitted review changes survive.
// The caller still owns final complete Document preview/admission.
[[nodiscard]] DrawingSelectionRemovalEntities replay_drawing_selection_removal_after_review(
    const DrawingSelectionRemovalEntities& actual,
    const DrawingSelectionRemovalEntities& admitted_review_stage,
    const DrawingSelectionRemovalIntent& intent, bool active_phase_constraints);

// Reconstructs the architectural stage from actual selection authority itself.
// Only that stage's exact annotation override consequences may compose with
// independent selected rows. The generic reviewed-stage contract is unchanged.
// The snapshot-owning caller reserves fresh identities across retained history.
[[nodiscard]] DrawingSelectionRemovalEntities replay_drawing_selection_removal_with_architectural(
    const DrawingSelectionRemovalEntities& actual,
    const DrawingSelectionRemovalIntent& drawing,
    const ArchitecturalSelectionRemovalIntent& architectural,
    bool allow_manufactured_opening_hosts, bool active_phase_constraints);

} // namespace sketch
