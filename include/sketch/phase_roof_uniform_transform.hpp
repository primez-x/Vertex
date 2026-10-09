#pragma once

#include "sketch/architectural_document_adapter.hpp"

#include <string_view>

namespace sketch {

struct RoofUniformTransformIntent {
    std::string roof_id;
    ArchitecturalGroupTransform transform;
};

inline constexpr std::string_view roof_uniform_transform_derivations_key =
    "roof_uniform_transform_derivations";

// Closed v1: version, roof_id, transform; the six original group-transform
// fields are required. Scale is finite, positive and non-unit. Historical
// rigid and plan-resize operations retain their separate closed meanings.
[[nodiscard]] nlohmann::json encode_roof_uniform_transform_intent(const RoofUniformTransformIntent& intent);
[[nodiscard]] RoofUniformTransformIntent decode_roof_uniform_transform_intent(const nlohmann::json& value);
void validate_roof_uniform_transform_derivations(const Entity& source);
[[nodiscard]] nlohmann::json roof_uniform_transform_opaque_remainder(const Entity& source);
void validate_roof_uniform_transform_source_entity(const Entity& source);

// Only the actual map supplies the floor/level datum. Pose, native dimensions
// and cut scalars derive from the original operation; bindings, row order,
// pitch and opaque siblings stay exact. Affected entered receipts retire
// verbatim into historical evidence, never into computed Quantity values.
[[nodiscard]] Entity stage_roof_uniform_transform_entity(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const RoofUniformTransformIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_uniform_transform_entities(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const std::vector<RoofUniformTransformIntent>& intents);

// An arbitrary candidate cannot establish transform authority. The producer
// supplies the actual operation; complete replay includes receipt evidence.
[[nodiscard]] std::optional<RoofUniformTransformIntent> capture_roof_uniform_transform(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const Entity& candidate, const RoofUniformTransformIntent& actual_intent);

} // namespace sketch
