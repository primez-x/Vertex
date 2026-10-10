#pragma once

#include "sketch/geometry.hpp"

#include <array>
#include <cstddef>

namespace sketch::detail {

// Private retry for unresolved contacts. Inputs must be the original binary64
// endpoints, before any floating-point origin subtraction. A point is returned
// only after its complete rational enclosure fits within the metre tolerance.
enum class CertifiedArcContactKind { none, points, indeterminate };

struct CertifiedArcContact {
    CertifiedArcContactKind kind{CertifiedArcContactKind::indeterminate};
    std::array<Vec2, 2> points{};
    std::size_t point_count{};
};

[[nodiscard]] CertifiedArcContact certified_arc_contact(
    const Segment& left, const Segment& right, double tolerance);

// Topology requires strict clearance, even when supporting circles do not
// intersect. False certifies that every point of the original arcs is farther
// apart than tolerance; true includes both close contacts and unresolved math.
[[nodiscard]] bool certified_arc_clearance_unresolved_or_within(
    const Segment& left, const Segment& right, double tolerance);

} // namespace sketch::detail
