#pragma once
#include "sketch/boundary_entity.hpp"

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Resolves stable segment/vertex geometry from an identified boundary or the
// authoritative replay of a supported measured stroke. This is an endpoint
// view only: it never grants a stroke closed-boundary or area semantics.
[[nodiscard]] IdentifiedBoundary resolve_constraint_segment_owner(const Entity& entity);

// Existing names are stable v1/v2 relation spellings; fixed_arc_length is v3/v4.
// The codec deliberately does not expose solver point coordinates.
enum class ConstraintRelationKind {
    horizontal,
    vertical,
    coincident,
    fixed_length,
    parallel,
    perpendicular,
    fixed_anchor,
    fixed_arc_length,
};

enum class WallEndpointRole { start, end };

struct WallEndpointBinding {
    std::string owner_id;
    WallEndpointRole role{WallEndpointRole::start};
    // Empty for a wall baseline; both IDs are required for an identified
    // boundary endpoint. The vertex ID is the shared solver identity.
    std::string segment_id;
    std::string vertex_id;

    bool operator==(const WallEndpointBinding&) const = default;
};

using ConstraintEndpointBinding = WallEndpointBinding;

struct PersistentConstraint {
    std::string id;
    ConstraintRelationKind relation{ConstraintRelationKind::horizontal};
    std::vector<WallEndpointBinding> bindings;
    std::optional<Quantity> length;
    std::optional<Vec2> anchor;
};

// The original entity is retained verbatim when semantics are not understood,
// so a later version can interpret its opaque payload without data loss.
struct ConstraintEntityDecodeResult {
    std::optional<PersistentConstraint> constraint;
    std::string unsupported_reason;
    std::optional<Entity> original_entity;
    std::uint64_t version{};
    std::string relation;

    [[nodiscard]] bool supported() const noexcept { return constraint.has_value(); }
};

[[nodiscard]] std::string_view constraint_relation_name(ConstraintRelationKind relation);
[[nodiscard]] std::string_view wall_endpoint_role_name(WallEndpointRole role);
// The same exact receipt codec is used by persisted relations and typed
// measured-edit command proofs. It reparses and verifies the entered quantity.
[[nodiscard]] nlohmann::json encode_constraint_quantity_receipt(const Quantity& quantity);
[[nodiscard]] Quantity decode_constraint_quantity_receipt(const nlohmann::json& value);

// Physical arc length is defined only by opposite endpoints of one genuine
// curved wall baseline or one stable identified boundary segment.
[[nodiscard]] Segment resolve_constraint_arc_segment(
    const PersistentConstraint& constraint,
    const std::map<std::string, Entity, std::less<>>& entities);
// v4 pairs describe an ordered, directed chain of genuine curved segments.
// Adjacent coordinates and same-owner stable vertex identities must agree.
[[nodiscard]] std::vector<Segment> resolve_constraint_arc_segments(
    const PersistentConstraint& constraint,
    const std::map<std::string, Entity, std::less<>>& entities);
[[nodiscard]] double resolve_constraint_arc_length(
    const PersistentConstraint& constraint,
    const std::map<std::string, Entity, std::less<>>& entities);
[[nodiscard]] double constraint_arc_length_coefficient(const Segment& segment);
[[nodiscard]] double constraint_arc_chord_target(
    const PersistentConstraint& constraint,
    const std::map<std::string, Entity, std::less<>>& entities);

// Decodes a first-class type="constraint" entity. Malformed envelope or
// malformed known semantics throw std::invalid_argument. Version two
// adds stable boundary segment/vertex bindings and generic entity_ids owners.
// A structurally
// valid but unknown relation or version returns an unsupported result carrying
// the original entity.
[[nodiscard]] ConstraintEntityDecodeResult decode_constraint_entity(const Entity& entity);

// Encodes baseline-only relations as v1, boundary relations as v2, and
// physical fixed_arc_length as v3 for one arc or v4 for a directed arc chain
// (generic entity_ids owners).
// When original is provided, its stable id/type,
// required flag, unrelated properties, extensions, and opaque future fields
// are retained while canonical v1 fields are replaced. The original must be a
// constraint entity with the same id.
[[nodiscard]] Entity encode_constraint_entity(
    const PersistentConstraint& constraint,
    const Entity* original = nullptr);

}  // namespace sketch
