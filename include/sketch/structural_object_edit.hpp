#pragma once

#include "sketch/architectural_document_adapter.hpp"

namespace sketch {

// Closed physical descriptors for v1 rectangular/circular columns and straight
// beams. Exactly one profile_fields or transform is supplied. A profile owns
// form and every codec geometry field; optional vertical_placement retains the
// actual source binding when omitted. Identity, context and metadata are never
// supplied by this intent. quantity_entries is a complete explicit input map,
// or null to preserve/invalidate source receipts without manufacturing input.
struct StructuralObjectEditIntent {
    std::string object_id;
    nlohmann::json profile_fields = nullptr;
    nlohmann::json quantity_entries = nullptr;
    std::optional<ArchitecturalGroupTransform> transform;
};

// Strict v1 fields: version, object_id, profile_fields, quantity_entries,
// transform. Transform retains the original six group operation fields.
[[nodiscard]] nlohmann::json encode_structural_object_edit_intent(
    const StructuralObjectEditIntent& intent);
[[nodiscard]] StructuralObjectEditIntent decode_structural_object_edit_intent(
    const nlohmann::json& value);

// Every operation reads the actual captured map. No temporary Document, fake
// snapshot, identity allocation or publication is used. Final resolved geometry
// is admitted only after all selected candidates have been composed.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_structural_object_edit_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<StructuralObjectEditIntent>& intents);

// Captures a profile only if typed replay preserves the source envelope and
// matches the entire candidate exactly, including opaque receipt siblings.
// Placement relationships still require actual-map replay before publication.
[[nodiscard]] std::optional<StructuralObjectEditIntent> capture_structural_object_edit(
    const Entity& source, const Entity& candidate);

// Typed raw descriptor and known entered receipt admission. Level relationships
// are deliberately resolved by actual-map replay rather than a synthetic map.
void validate_structural_object_source_entity(const Entity& source);

} // namespace sketch
