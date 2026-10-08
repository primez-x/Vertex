#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

struct RoofProfileEditIntent {
    std::string roof_id;
    // Horizontal run for a sloped panel; ridge length for gable/hip roofs.
    std::optional<Quantity> length;
    std::optional<Quantity> span;
    std::optional<Quantity> rise;
    std::optional<Quantity> overhang;
    std::optional<Quantity> thickness;
};

// Strict version 1: exactly version, roof_id, length, span, rise, overhang,
// thickness. Absent dimensions are explicit null; empty intents are refused.
[[nodiscard]] nlohmann::json encode_roof_profile_edit_intent(const RoofProfileEditIntent& intent);
[[nodiscard]] RoofProfileEditIntent decode_roof_profile_edit_intent(const nlohmann::json& value);

// Admit the actual canonical fields and known quantity/opening receipt bindings.
// Unknown/future receipts are retained opaque and cannot be rewritten as input.
// Geometry is admitted through the shared native roof builders, never persisted.
void validate_roof_profile_source_entity(const Entity& source);

// Same-form scalar edit; pitch is derived from resulting rise/run or half-span.
// Pose/context/material/opening roster and opaque owner data remain exact.
// Equal dimensions return the exact source without synthesizing pitch/receipts.
[[nodiscard]] Entity replay_roof_profile_entity(const Entity& source, const RoofProfileEditIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofProfileEditIntent>& intents);

// Changed fields require exact entered candidate receipts. Numeric-only
// candidates are refused. Equivalent scalar re-entry retains the original
// receipt after checking its source-derived core and every opaque sibling.
// The complete candidate must match independent typed replay.
// Unsupported changes throw; nullopt means exact/equivalent source no-op only.
[[nodiscard]] std::optional<RoofProfileEditIntent> capture_roof_profile_edit(
    const Entity& original, const Entity& candidate);
} // namespace sketch
