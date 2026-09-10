#pragma once

#include "sketch/boundary_entity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sketch {

enum class BoundaryDimensionFormat { supported_v1, unsupported_version };

struct BoundaryDimensionVersion {
    BoundaryDimensionFormat format{};
    std::optional<std::uint64_t> version;
    std::string diagnostic;
};

enum class BoundaryDimensionPlacement { manual, automatic };

[[nodiscard]] std::string_view boundary_dimension_placement_name(
    BoundaryDimensionPlacement placement);

struct BoundaryDimensionResolution {
    Segment segment;
    double segment_length_metres{};

    [[nodiscard]] double segment_length() const noexcept { return segment_length_metres; }
};

struct BoundaryDimension {
    std::string id;
    std::string boundary_id;
    std::string segment_id;
    Vec2 text_position;
    BoundaryDimensionPlacement placement{BoundaryDimensionPlacement::manual};
    std::optional<std::uint32_t> automatic_placement_version;

    bool operator==(const BoundaryDimension& other) const noexcept {
        return id == other.id && boundary_id == other.boundary_id &&
               segment_id == other.segment_id && text_position.x == other.text_position.x &&
               text_position.y == other.text_position.y && placement == other.placement &&
               automatic_placement_version == other.automatic_placement_version;
    }

    // Resolves the exact stable source segment and derives its current
    // analytical length from the canonical identified boundary geometry.
    [[nodiscard]] BoundaryDimensionResolution resolve(const Entity& boundary_entity) const;
};

// Unsupported future versions or dimension kinds remain opaque and retain
// their complete source entity. Malformed known v1 data throws
// std::invalid_argument rather than being partially decoded.
struct BoundaryDimensionDecodeResult {
    std::optional<BoundaryDimension> dimension;
    std::string unsupported_reason;
    std::optional<Entity> original_entity;
    std::uint64_t version{};
    std::string kind;

    [[nodiscard]] bool supported() const noexcept { return dimension.has_value(); }
};

[[nodiscard]] bool can_recognize_boundary_dimension_entity_type(
    std::string_view type) noexcept;
[[nodiscard]] BoundaryDimensionVersion inspect_boundary_dimension_version(
    const Entity& entity);
[[nodiscard]] BoundaryDimensionDecodeResult decode_boundary_dimension_entity(
    const Entity& entity);
[[nodiscard]] Entity encode_boundary_dimension_entity(
    const BoundaryDimension& dimension, const Entity* original = nullptr);
[[nodiscard]] BoundaryDimensionResolution resolve_boundary_dimension(
    const BoundaryDimension& dimension, const Entity& boundary_entity);

}  // namespace sketch
