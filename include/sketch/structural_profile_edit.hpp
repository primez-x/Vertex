#pragma once

#include "sketch/quantity.hpp"
#include "sketch/structural_object_edit.hpp"

namespace sketch {

// Scalar authority only. Form, pose, section frame, vertical placement and
// metadata always come from the actual captured column or beam. All-null is a
// retained member; the enclosing selection policy controls unchanged owners.
struct StructuralProfileEditIntent {
    std::string object_id;
    std::optional<Quantity> width;
    std::optional<Quantity> depth;
    std::optional<Quantity> height;
    std::optional<Quantity> radius;
};

// Closed canonical v1: version, object_id, width, depth, height, radius.
// Nullable quantities use the exact shared constraint receipt codec.
[[nodiscard]] nlohmann::json encode_structural_profile_edit_intent(
    const StructuralProfileEditIntent& intent);
[[nodiscard]] StructuralProfileEditIntent decode_structural_profile_edit_intent(
    const nlohmann::json& value);
void validate_structural_profile_source_entity(const Entity& source);

// Derive one exact raw owner from its actual source. This carries scalar-only
// authority; callers own complete-map context, native and hosted admission.
[[nodiscard]] Entity prepare_structural_profile_edit_entity(
    const Entity& source, const StructuralProfileEditIntent& intent);

// Patches only supplied changed applicable scalars on each actual raw owner.
// Rectangular columns permit width/depth/height, circular columns radius/height,
// and straight beams width/depth. Dormant form-inapplicable source fields and
// receipts remain exact. Inapplicable input and future receipt overwrites refuse.
// Every retained or changed host and hosted component is admitted before/after;
// scalar edits preserve the complete raw hosted catalogs and original aliases.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_structural_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<StructuralProfileEditIntent>& intents);

} // namespace sketch
