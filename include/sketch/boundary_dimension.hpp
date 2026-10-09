#pragma once

#include "sketch/boundary_entity.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

enum class BoundaryDimensionFormat { supported_v1, supported_v2, supported_v3, unsupported_version, supported_v4 };

struct BoundaryDimensionVersion {
    BoundaryDimensionFormat format{};
    std::optional<std::uint64_t> version;
    std::string diagnostic;
};

enum class BoundaryDimensionPlacement { manual, automatic };

// Placed dimensions share one persisted presentation contract while keeping
// their analytical target explicit. Segment lengths reference one stable
// edge or an ordered physical edge chain; angles reference two stable edges and their common vertex; areas
// reference the complete closed boundary. Wall axis length references only a
// physical wall owner and measures its current straight or curved centreline.
// It does not represent an appraisal exterior-face measurement.
enum class BoundaryDimensionKind { segment_length, angle, area, wall_axis_length };

[[nodiscard]] std::string_view boundary_dimension_kind_name(BoundaryDimensionKind kind);

[[nodiscard]] std::string_view boundary_dimension_placement_name(
    BoundaryDimensionPlacement placement);

struct BoundaryDimensionAngleGeometry {
    Vec2 vertex;
    Vec2 first_direction;
    Vec2 second_direction;
};

struct BoundaryDimensionResolution {
    Segment segment;
    double segment_length_metres{};
    BoundaryDimensionKind kind{BoundaryDimensionKind::segment_length};
    double angle_radians{};
    double area_square_metres{};
    std::optional<BoundaryDimensionAngleGeometry> angle_geometry;

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
    // Stable owner identity; v4 wall_axis_length uses the actual physical wall
    // ID here and leaves all segment/vertex/chain fields empty.
    std::string boundary_id;
    std::string segment_id;
    Vec2 text_position;
    BoundaryDimensionPlacement placement{BoundaryDimensionPlacement::manual};
    std::optional<std::uint32_t> automatic_placement_version;
    std::optional<BoundaryDimensionPresentation> presentation;
    BoundaryDimensionKind kind{BoundaryDimensionKind::segment_length};
    std::string vertex_id;
    std::string secondary_segment_id;
    // Empty for a single edge. Otherwise 2..128 unique, contiguous forward
    // edges, with segment_id == front() for existing C++ callers. Persisted
    // v3 target.segment_ids is canonical and omits target.segment_id.
    std::vector<std::string> segment_chain_ids;

    bool operator==(const BoundaryDimension& other) const noexcept {
        return id == other.id && boundary_id == other.boundary_id &&
               segment_id == other.segment_id && text_position.x == other.text_position.x &&
               text_position.y == other.text_position.y && placement == other.placement &&
               automatic_placement_version == other.automatic_placement_version &&
               presentation == other.presentation && kind == other.kind &&
               vertex_id == other.vertex_id && secondary_segment_id == other.secondary_segment_id &&
               segment_chain_ids == other.segment_chain_ids;
    }

    // Resolves stable analytical targets from identified boundaries or replayed
    // measured strokes, or a physical wall axis for wall_axis_length. Area
    // dimensions require an identified boundary owner.
    [[nodiscard]] BoundaryDimensionResolution resolve(const Entity& boundary_entity) const;
    [[nodiscard]] BoundaryDimensionResolution resolve(
        const std::map<std::string, Entity, std::less<>>& entities) const;
    [[nodiscard]] BoundaryDimensionResolution resolve(const DocumentSnapshot& snapshot) const;
};

// Unsupported future versions or dimension kinds remain opaque and retain
// their complete source entity. Only v4/wall_axis_length adds new support;
// v4 prior kinds remain opaque. Malformed known v1/v2/v3 and typed v4 data throws
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
// Returns an endpoint/edge view of authoritative identified boundary geometry
// or supported measured-stroke replay, retaining both terminal and revisited
// stable vertex IDs. This view does not confer closed-area semantics on strokes.
// Unsupported or malformed owners throw std::invalid_argument.
[[nodiscard]] IdentifiedBoundary resolve_dimension_geometry_owner(const Entity& entity);
// Current supported physical wall centreline only. Reuses native wall decoding
// and validates authored geometry/provenance; never supplies boundary/area IDs.
// Malformed or unsupported owners throw std::invalid_argument.
[[nodiscard]] Segment resolve_dimension_wall_axis_owner(const Entity& entity);
// Structural retained-target admission only: verifies identified edges/vertices,
// chains and topology without certifying current physical-room source geometry
// or returning a quantity. Stale physical rooms may remain stored and editable.
void validate_boundary_dimension_target(
    const BoundaryDimension& dimension, const Entity& boundary_entity);
[[nodiscard]] BoundaryDimensionResolution resolve_boundary_dimension(
    const BoundaryDimension& dimension, const Entity& boundary_entity);
// Authoritative retained-map/snapshot resolution also qualifies source-bound
// physical rooms from current walls and returns net clear area including holes.
// Stale, malformed or unsupported sources throw without changing stored data.
[[nodiscard]] BoundaryDimensionResolution resolve_boundary_dimension(
    const BoundaryDimension& dimension,
    const std::map<std::string, Entity, std::less<>>& entities);
[[nodiscard]] BoundaryDimensionResolution resolve_boundary_dimension(
    const BoundaryDimension& dimension, const DocumentSnapshot& snapshot);

// Current authoring/display calculation from an actual captured authoritative
// map. Command callers must supply their actual captured source map, never a
// reconstructed or fabricated snapshot.
// A physical room may retain its original phase bookkeeping only when its
// admitted physical inventory, context/plane and exact clear geometry remain
// unchanged. Historical command replay retains the strict overloads above.
[[nodiscard]] BoundaryDimensionResolution resolve_current_boundary_dimension(
    const BoundaryDimension& dimension,
    const std::map<std::string, Entity, std::less<>>& entities);
// Snapshot callers supply their actual captured snapshot under the same
// current-value qualification and caller obligations as the overload above.
[[nodiscard]] BoundaryDimensionResolution resolve_current_boundary_dimension(
    const BoundaryDimension& dimension, const DocumentSnapshot& snapshot);

}  // namespace sketch
