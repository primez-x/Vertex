#pragma once

#include "sketch/architectural_document_adapter.hpp"

namespace sketch {

// Closed v1 carries an actual owner and the complete captured operator. V2 also
// carries a complete entered-input map validated against the transformed actual
// profile; null preserves the original v1 source-derived receipt policy.
struct StairTransformIntent {
    std::string object_id;
    ArchitecturalGroupTransform transform;
    nlohmann::json quantity_entries = nullptr;
};

[[nodiscard]] nlohmann::json encode_stair_transform_intent(const StairTransformIntent& intent);
[[nodiscard]] StairTransformIntent decode_stair_transform_intent(const nlohmann::json& value);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_stair_transform_entities(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const std::vector<StairTransformIntent>& intents);

namespace stair_transform_detail {
// Internal pure producer. Complete source bounds precede all codec/layout work;
// public staging then uses typed profile and hosted catalog/native admission.
void source_bounds(const std::map<std::string, Entity, std::less<>>& actual_entities);
[[nodiscard]] std::map<std::string, Entity, std::less<>> stage_geometry(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    std::span<const ArchitecturalGroupTransformTarget> targets);
} // namespace stair_transform_detail
} // namespace sketch
