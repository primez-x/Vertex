#pragma once

#include "sketch/boundary_entity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sketch {

enum class BoundaryDimensionFormat { supported_v1, supported_v2, unsupported_version };

struct BoundaryDimensionVersion {
    BoundaryDimensionFormat format{};
    std::optional<std::uint64_t> version;
    std::string diagnostic;
};

enum class BoundaryDimensionPlacement { manual, automatic };

// Placed dimensions share one persisted presentation contract while keeping
// their analytical target explicit. Segment lengths reference one stable
// edge; angles reference two stable edges and their common vertex; areas
// reference the complete closed boundary.
enum class BoundaryDimensionKind { segment_length, angle, area };

[[nodiscard]] std::string_view boundary_dimension_kind_name(BoundaryDimensionKind kind);

[[nodiscard]] std::string_view boundary_dimension_placement_name(
    BoundaryDimensionPlacement placement);

struct BoundaryDimensionResolution {
    Segment segment;
    double segment_length_metres{};
    BoundaryDimensionKind kind{BoundaryDimensionKind::segment_length};
    double angle_radians{};
    double area_square_metres{};

    [[nodiscard]] double segment_length() const noexcept { return segment_length_metres; }
    [[nodiscard]] double angle() const noexcept { return angle_radians; }
    [[nodiscard]] double area() const noexcept { return area_square_metres; }
};

struct BoundaryDimensionPresentation {
    double text_height_mm{2.5};
    std::string color{"#263241"};
    bool bold{};
    bool italic{};
    bool visible{true};
    double rotation_radians{};

    bool operator==(const BoundaryDimensionPresentation&) const = default;
};

struct BoundaryDimension {
    std::string id;
    std::string boundary_id;
    std::string segment_id;
    Vec2 text_position;
    BoundaryDimensionPlacement placement{BoundaryDimensionPlacement::manual};
    std::optional<std::uint32_t> automatic_placement_version;
    std::optional<BoundaryDimensionPresentation> presentation;
    BoundaryDimensionKind kind{BoundaryDimensionKind::segment_length};
    std::string vertex_id;
    std::string secondary_segment_id;

    bool operator==(const BoundaryDimension& other) const noexcept {
        return id == other.id && boundary_id == other.boundary_id &&
               segment_id == other.segment_id && text_position.x == other.text_position.x &&
               text_position.y == other.text_position.y && placement == other.placement &&
               automatic_placement_version == other.automatic_placement_version &&
               presentation == other.presentation && kind == other.kind &&
               vertex_id == other.vertex_id && secondary_segment_id == other.secondary_segment_id;
    }

    // Resolves the stable analytical target from canonical identified boundary
    // geometry. The returned value is a segment length, angle, or area based
    // on this dimension's semantic kind.
    [[nodiscard]] BoundaryDimensionResolution resolve(const Entity& boundary_entity) const;
};

// Unsupported future versions or dimension kinds remain opaque and retain
// their complete source entity. Malformed known v1/v2 data throws
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
