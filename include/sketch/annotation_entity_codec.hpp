#pragma once

#include "sketch/annotation_catalog.hpp"
#include "sketch/document.hpp"

#include <string>

namespace sketch {

// Presentation annotations are persisted as one typed Document entity so
// labels and symbols remain portable, validated, and independent of a host
// renderer or subscription service.
inline constexpr const char* kAnnotationEntityType = "annotation_state";

[[nodiscard]] Entity make_annotation_entity(std::string id,
                                            const AnnotationState& state);
[[nodiscard]] AnnotationState decode_annotation_entity(const Entity& entity);
void validate_annotation_entity(const Entity& entity);
[[nodiscard]] ApplyEntityChanges make_symbol_migration_command(
    const DocumentSnapshot&, std::string_view entity_id, std::string_view instance_id,
    std::string pinned_svg = {});

}  // namespace sketch
