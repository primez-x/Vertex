#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

enum class BoundaryEntityFormat { anonymous_legacy, identified_v1, unsupported_version };

struct BoundaryEntityVersion {
    BoundaryEntityFormat format{};
    std::optional<std::uint64_t> version;
    std::string diagnostic;
};

struct IdentifiedSegment {
    std::string segment_id;
    std::string start_vertex_id;
    std::string end_vertex_id;
    Segment segment;
    bool operator==(const IdentifiedSegment& other) const noexcept;
};

struct IdentifiedBoundary {
    std::string id;
    std::string type;
    std::vector<IdentifiedSegment> segments;
    bool operator==(const IdentifiedBoundary&) const = default;
};

struct LegacyBoundaryIdentityOptions {
    std::vector<std::string> segment_ids;
    std::vector<std::string> vertex_ids;
};

[[nodiscard]] bool can_recognize_boundary_entity_type(std::string_view type) noexcept;
[[nodiscard]] BoundaryEntityVersion inspect_boundary_entity_version(const Entity& entity);
[[nodiscard]] IdentifiedBoundary decode_identified_boundary_entity(const Entity& entity);
// Updating requires the same entity identity. Unknown JSON fields follow each
// stable segment ID. This codec does not validate external dependent references.
[[nodiscard]] Entity encode_identified_boundary_entity(
    const IdentifiedBoundary& boundary, const Entity* original = nullptr);
[[nodiscard]] Boundary boundary_geometry(const IdentifiedBoundary& boundary);
// Upgrade never adjusts coordinates. Tolerance-only joins require a separate
// explicit geometry repair before identities can be assigned.
[[nodiscard]] Entity upgrade_legacy_boundary_entity(
    const Entity& original, const LegacyBoundaryIdentityOptions& options = {});
[[nodiscard]] IdentifiedBoundary reverse_identified_boundary(const IdentifiedBoundary& boundary);
[[nodiscard]] Entity reverse_identified_boundary_entity(const Entity& original);

} // namespace sketch
