#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <stdexcept>

namespace sketch {
struct Entity;

struct RetainedGeometryWorkLimits {
    std::size_t maximum_depth = 64;
    std::size_t maximum_nodes = 2'000'000;
};

class RetainedGeometryWorkError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

using RetainedGeometryReserveProduct = std::function<void(std::size_t, std::size_t)>;

// Admit the complete raw source before calling these helpers. Only schema-owned
// geometry/replay slots are inspected; opaque JSON never creates topology work.
// Every visit and edge inventory is reserved linearly before traversal continues.
// The callback reserves a*b in the caller's existing cumulative work ledger and
// must reject capacity/overflow before multiplying. Failed reservations stay
// charged according to that ledger. No codec, copy or geometry solve runs here.
// maximum_nodes bounds retained operation inventories; the caller's preceding
// raw admission owns the complete node budget. Geometry/growth/operation counts
// remain bounded by 16,384, and topology squares by 67,108,864 before the callback.
void reserve_retained_geometry_replay_work(const nlohmann::json& value,
    const RetainedGeometryReserveProduct& reserve_product,
    bool root_geometry_array = false, std::size_t copies = 8,
    const RetainedGeometryWorkLimits& limits = {});

// Actual intrinsic owner geometry, holes and retained boundary/measurement/
// physical-room/wall provenance use the same reservation semantics as replay.
void reserve_retained_geometry_owner_work(const Entity& owner,
    const RetainedGeometryReserveProduct& reserve_product,
    std::size_t copies = 8,
    const RetainedGeometryWorkLimits& limits = {});
} // namespace sketch
