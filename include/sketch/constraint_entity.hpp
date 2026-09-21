#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// These names are the stable v1 relation spellings in a persisted constraint
// entity. The codec deliberately does not expose solver point coordinates.
enum class ConstraintRelationKind {
    horizontal,
    vertical,
    coincident,
    fixed_length,
    parallel,
    perpendicular,
    fixed_anchor,
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

// Decodes a first-class type="constraint" entity. Malformed envelope or
// malformed known v1/v2 semantics throw std::invalid_argument. Version two
// adds stable boundary segment/vertex bindings and generic entity_ids owners.
// A structurally
// valid but unknown relation or version returns an unsupported result carrying
// the original entity.
[[nodiscard]] ConstraintEntityDecodeResult decode_constraint_entity(const Entity& entity);

// Encodes baseline-only relations as v1 and boundary relations as v2.
// When original is provided, its stable id/type,
// required flag, unrelated properties, extensions, and opaque future fields
// are retained while canonical v1 fields are replaced. The original must be a
// constraint entity with the same id.
[[nodiscard]] Entity encode_constraint_entity(
    const PersistentConstraint& constraint,
    const Entity* original = nullptr);

}  // namespace sketch
