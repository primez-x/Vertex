#pragma once

#include "sketch/boundary_entity.hpp"
#include "sketch/assembly_dimension.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

enum class BoundaryDimensionFormat { supported_v1, supported_v2, supported_v3, unsupported_version, supported_v4, supported_v5, supported_v6, supported_v7 };

[[nodiscard]] constexpr bool is_assembly_boundary_dimension_format(BoundaryDimensionFormat format) noexcept {
    return format == BoundaryDimensionFormat::supported_v6 || format == BoundaryDimensionFormat::supported_v7;
}

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
// Corner-window leg length references its manufactured owner and measures one
// actual shared host endpoint-to-outer-jamb span from both retained hosts/cuts.
// Assembly extents reference an actual qualified root/path and measure its
// physical width, depth or height. They do not invent plan boundary edges.
// Assembly profiles reference an exact own-profile ring edge or vertex pair,
// retaining analytical edge length or the physical vertex chord.
enum class BoundaryDimensionKind { segment_length, angle, area, wall_axis_length, corner_window_leg_length, assembly_extent, assembly_profile };

[[nodiscard]] constexpr bool is_assembly_boundary_dimension_kind(BoundaryDimensionKind kind) noexcept {
    return kind == BoundaryDimensionKind::assembly_extent || kind == BoundaryDimensionKind::assembly_profile;
}

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
    // Present only for assembly_extent/assembly_profile. segment is empty: use
    // these physical 3D endpoints/frame and precision-preserving offsets for
    // projection, including height; segment_length_metres is the actual span.
    std::optional<AssemblyDimensionResolution> assembly_geometry;

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
    // ID here and leaves all segment/vertex/chain fields empty. v5 corner-window
    // leg length uses the actual corner_window owner ID with corner_leg.
    // v6 assembly_extent/v7 assembly_profile use the actual independent document_entity_id, or
    // the actual catalog_id for an embedded root. It must match assembly_target
    // and is a reference carrier, never a fabricated boundary/presentation ID.
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
    // Only corner_window_leg_length uses this zero-based leg selector. No
    // synthetic boundary edge or owned cut ID is part of its target.
    std::optional<std::uint32_t> corner_leg;
    // Only assembly_extent/assembly_profile use this closed, qualified identity.
    std::optional<AssemblyDimensionTarget> assembly_target;

    bool operator==(const BoundaryDimension& other) const noexcept {
        return id == other.id && boundary_id == other.boundary_id &&
               segment_id == other.segment_id && text_position.x == other.text_position.x &&
               text_position.y == other.text_position.y && placement == other.placement &&
               automatic_placement_version == other.automatic_placement_version &&
               presentation == other.presentation && kind == other.kind &&
               vertex_id == other.vertex_id && secondary_segment_id == other.secondary_segment_id &&
               segment_chain_ids == other.segment_chain_ids && corner_leg == other.corner_leg &&
               assembly_target == other.assembly_target;
    }

    // Resolves stable analytical targets from identified boundaries or replayed
    // measured strokes, or a physical wall axis for wall_axis_length. Area
    // dimensions require an identified boundary owner. Corner-window legs
    // and assembly dimensions require the complete authoritative map or snapshot
    // overload. Entity-only resolution cannot admit complete assembly catalogs.
    [[nodiscard]] BoundaryDimensionResolution resolve(const Entity& boundary_entity) const;
    [[nodiscard]] BoundaryDimensionResolution resolve(
        const std::map<std::string, Entity, std::less<>>& entities) const;
    [[nodiscard]] BoundaryDimensionResolution resolve(const DocumentSnapshot& snapshot) const;
};

// Unsupported future versions or dimension kinds remain opaque and retain
// their complete source entity. v4 adds only wall_axis_length and v5 adds only
// corner_window_leg_length; other kinds at those versions remain opaque.
// v6 adds only assembly_extent with closed target {entity_id, assembly}, where
// assembly uses the closed AssemblyDimensionTarget codec.
// v7 adds only assembly_profile with the same closed carrier and requires the
// distinct v2 assembly_profile_dimension_target. V6 remains extent-only.
// Malformed known v1/v2/v3 and typed v4/v5/v6/v7 data throws
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
// Current supported physical wall centreline only. Reuses shared wall decoding
// and validates authored geometry/provenance; never supplies boundary/area IDs.
// Malformed or unsupported owners throw std::invalid_argument.
[[nodiscard]] Segment resolve_dimension_wall_axis_owner(const Entity& entity);
// Complete retained-map corner admission, including inactive alternatives.
// Returns the actual host endpoint-to-outer-jamb leg in local plan coordinates.
// Entity-only resolution cannot certify a corner's two hosts and owned cuts.
[[nodiscard]] Segment resolve_dimension_corner_window_leg_owner(
    const BoundaryDimension& dimension,
    const std::map<std::string, Entity, std::less<>>& entities);
// Structural retained-target admission only: verifies identified edges/vertices,
// chains and topology without certifying current physical-room source geometry
// or returning a quantity. For corner windows this checks only owner structure
// and the leg selector. Stale physical rooms may remain stored and editable.
// Assembly dimension admission requires the actual map; this Entity-only overload
// refuses rather than treating a catalog or isolated root as complete evidence.
void validate_boundary_dimension_target(
    const BoundaryDimension& dimension, const Entity& boundary_entity);
[[nodiscard]] BoundaryDimensionResolution resolve_boundary_dimension(
    const BoundaryDimension& dimension, const Entity& boundary_entity);
// Authoritative retained-map/snapshot resolution also qualifies source-bound
// physical rooms from current walls and returns net clear area including holes.
// Corner-window legs qualify the complete retained cohort, including valid
// inactive alternatives, and use actual host/cut geometry in local coordinates.
// Assembly dimensions admit complete actual catalogs and roots and expose physical
// 3D geometry; retained inactive roots may resolve in these strict overloads.
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
// A corner-window leg requires its owner, both hosts and both cuts to be active
// and undemolished in the saved phase selection.
// Assembly dimensions additionally require the exact qualified root and actual
// placement host (including an opening's wall) active in the saved selection.
[[nodiscard]] BoundaryDimensionResolution resolve_current_boundary_dimension(
    const BoundaryDimension& dimension,
    const std::map<std::string, Entity, std::less<>>& entities);
// Snapshot callers supply their actual captured snapshot under the same
// current-value qualification and caller obligations as the overload above.
[[nodiscard]] BoundaryDimensionResolution resolve_current_boundary_dimension(
    const BoundaryDimension& dimension, const DocumentSnapshot& snapshot);

// Mutable-map preparation callers resolve all component annotations together.
// 1..1000 assembly extent/profile dimensions, in input order. Complete actual-map
// admission, geometry expansion, phase scope and unique host qualification
// are shared once; new/retargeted proposals receive no snapshot/publication
// authority. Other dimension kinds must use their existing owning resolver.
[[nodiscard]] std::vector<BoundaryDimensionResolution> resolve_current_assembly_boundary_dimensions(
    std::span<const BoundaryDimension> dimensions,
    const std::map<std::string, Entity, std::less<>>& entities);

}  // namespace sketch
