#pragma once

#include "sketch/document.hpp"

#include <optional>

namespace sketch {

// An ordinary physical profile, never a candidate entity payload. The closed
// profile includes version/form and the family's complete known field list.
// Optional fields use explicit null tombstones (including absent placement,
// connection, topology and host). Nested records contain only codec fields;
// surviving child metadata is taken from the actual same typed child ID.
// quantity_entries is a complete entered-input map, or null to retain/remap
// unaffected source receipts and remove receipts for changed owned dimensions.
struct StairObjectEditIntent {
    std::string object_id;
    nlohmann::json profile_fields = nlohmann::json::object();
    nlohmann::json quantity_entries = nullptr;
};

[[nodiscard]] nlohmann::json encode_stair_object_edit_intent(const StairObjectEditIntent& intent);
[[nodiscard]] StairObjectEditIntent decode_stair_object_edit_intent(const nlohmann::json& value);

// Actual authored maps only. Admits complete attachment state, actual connected
// levels, surviving typed child roles and affected native geometry. The enclosing
// Document must additionally reserve introduced identities against all history.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_stair_object_edit_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<StairObjectEditIntent>& intents);

// Refuses candidate changes to identity, context, appearance, material and opaque
// metadata. Known unchanged numeric encodings retain their original raw source
// representation. Actual-map relationship/native admission remains replay's job.
[[nodiscard]] std::optional<StairObjectEditIntent> capture_stair_object_edit(
    const Entity& original, const Entity& edited);

} // namespace sketch
