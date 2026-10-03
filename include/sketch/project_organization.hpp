#pragma once

#include "sketch/document.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

struct DrawingContext {
    std::string property_id;
    std::string building_id;
    std::string floor_id;
    std::string layer_id;
    // Empty when the floor is intentionally unbound to a vertical-level graph.
    std::string level_id;

    [[nodiscard]] bool complete() const noexcept;
    bool operator==(const DrawingContext&) const = default;
};

struct OrganizationNode {
    std::string id;
    std::string type;
    std::string name;
    std::string parent_id;
    DrawingContext context;
    std::vector<std::string> children;
    // An unresolved or contradictory relationship remains visible at the root.
    // Diagnostics do not repair or rewrite the authoritative document.
    std::vector<std::string> issues;
};

struct ProjectOrganization {
    std::map<std::string, OrganizationNode, std::less<>> nodes;
    std::vector<std::string> roots;

    [[nodiscard]] std::optional<DrawingContext> drawing_context(
        std::string_view entity_id) const;
};

// Derived organization only. Existing world-coordinate geometry is untouched.
// Hosted openings inherit their wall's physical placement; an explicit valid
// layer on that same floor can override drawing organization/visibility.
// All other non-container
// objects resolve their explicit layer/floor/building/property references.
[[nodiscard]] ProjectOrganization organize_project(const DocumentSnapshot& snapshot);
// The same resolver for a retained revision's entity map. Derived indexing only;
// callers remain responsible for validating the enclosing document/history.
[[nodiscard]] ProjectOrganization organize_project(
    const std::map<std::string, Entity, std::less<>>& entities);

// Resolve an opt-in level-driven placement into a derived entity copy. The
// source entity remains unchanged. Entities without a vertical_placement
// property, or with mode "absolute", are returned byte-for-byte equivalent.
// Mode "level" requires a valid floor context bound to a vertical level graph
// and applies level elevation plus offset_m to supported Z coordinates.
[[nodiscard]] Entity resolve_vertical_placement(const DocumentSnapshot& snapshot,
                                                const Entity& entity);
[[nodiscard]] Entity resolve_vertical_placement(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& entity);

// Resolve only the requested owners, with the same validation and exact derived
// copies as the singular resolver. The organization is derived lazily once from
// this entity map and retained only for this call; no cross-snapshot cache is used.
[[nodiscard]] std::map<std::string, Entity, std::less<>> resolve_vertical_placements(
    const std::map<std::string, Entity, std::less<>>& entities,
    std::span<const std::string> entity_ids);

}  // namespace sketch
