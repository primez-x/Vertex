#pragma once

#include "sketch/boundary_receipt.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

inline constexpr std::uint32_t measurement_linework_schema_version = 1;
inline constexpr std::uint32_t measurement_linework_replay_version = 1;

// One analytical measurement stroke, independent of physical walls and areas.
// Entity adapters own property/building/floor/layer context and place this
// model in properties.model. The anchor is the first edge's captured start;
// its stable vertex identity is the first edge's start_vertex_id. Geometry is
// always derived from receipts, never persisted as a parallel authority.
struct MeasurementLinework {
    std::uint32_t schema_version{measurement_linework_schema_version};
    std::uint32_t replay_version{measurement_linework_replay_version};
    std::string stroke_id;
    Vec2 anchor{};
    bool closed{};
    std::vector<ConstructionTopologyEdge> edges;
    nlohmann::json extensions = nlohmann::json::object();
};

struct MeasurementLineworkReplay {
    std::uint32_t replay_version{measurement_linework_replay_version};
    std::string stroke_id;
    Vec2 anchor{};
    bool closed{};
    std::vector<ReplayedConstructionEdge> edges;
    std::vector<ConstructionReceipt> receipts;
};

// Replays and validates atomically using replay_construction_receipt. Ordered
// joins require exact coordinates and identical vertex IDs. Segment IDs are
// unique and cannot collide with vertex IDs or the stroke ID; vertex IDs
// cannot alias the stroke ID. A vertex ID may recur only at
// the same exact coordinate. Explicit revisits, self-crossings and overlapping
// geometry are valid linework. A
// closed stroke must finish at its anchor and reuse its starting vertex ID;
// an open stroke must not claim that closing identity. Closure receipts are
// final edges of closed strokes only. No enclosed-area validation or totals
// are performed. Unsupported versions, invalid IDs, empty strokes, malformed
// typed inputs, degenerate geometry and conflicting joins throw
// std::invalid_argument. Tolerance controls individual receipt construction,
// never identity joins or closure.
[[nodiscard]] MeasurementLineworkReplay replay_measurement_linework(
    const MeasurementLinework& model,
    double tolerance_metres = default_geometry_tolerance_metres);

enum class MeasurementLineworkFormat {
    supported_v1,
    unsupported_version,
    unsupported_replay_version,
};

struct MeasurementLineworkVersion {
    MeasurementLineworkFormat format{};
    std::optional<std::uint64_t> version;
    std::optional<std::uint64_t> replay_version;
    std::string diagnostic;
};

struct MeasurementLineworkDecodeResult {
    std::optional<MeasurementLinework> model;
    std::optional<nlohmann::json> original_model;
    std::optional<std::uint64_t> version;
    std::optional<std::uint64_t> replay_version;
    std::string diagnostic;

    [[nodiscard]] bool supported() const noexcept { return model.has_value(); }
};

// Version one JSON keys: version, replay_version, stroke_id, anchor, closed,
// segments, extensions. Each segment has segment_id, start_vertex_id,
// end_vertex_id and the strict single-receipt codec's receipt. Only extensions
// is opaque; unknown typed keys fail closed. Inspection reads only the object
// and its positive integral version, plus a required positive integral
// replay_version for a known schema. Decode validates/replays known versions;
// unknown positive schema/replay versions return the exact opaque JSON for a
// caller to preserve without decoding. Encode validates and retains original
// expressions and extensions. Malformed known models throw invalid_argument.
[[nodiscard]] MeasurementLineworkVersion inspect_measurement_linework_model(
    const nlohmann::json& model);
[[nodiscard]] MeasurementLineworkDecodeResult decode_measurement_linework_model(
    const nlohmann::json& model);
[[nodiscard]] nlohmann::json encode_measurement_linework_model(
    const MeasurementLinework& model);

}  // namespace sketch
