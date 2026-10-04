#pragma once

#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_edit.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <variant>

namespace sketch {

inline constexpr std::uint32_t measurement_linework_schema_version_v1 = 1;
inline constexpr std::uint32_t measurement_linework_schema_version_v2 = 2;
inline constexpr std::uint32_t measurement_linework_schema_version_v3 = 3;
inline constexpr std::uint32_t measurement_linework_schema_version_v4 = 4;
inline constexpr std::uint32_t measurement_linework_schema_version_v5 = 5;
inline constexpr std::uint32_t measurement_linework_schema_version = measurement_linework_schema_version_v1;
inline constexpr std::uint32_t measurement_linework_latest_schema_version = measurement_linework_schema_version_v5;
inline constexpr std::uint32_t measurement_linework_replay_version_v1 = 1;
inline constexpr std::uint32_t measurement_linework_replay_version_v2 = 2;
inline constexpr std::uint32_t measurement_linework_replay_version_v3 = 3;
inline constexpr std::uint32_t measurement_linework_replay_version_v4 = 4;
inline constexpr std::uint32_t measurement_linework_replay_version_v5 = 5;
inline constexpr std::uint32_t measurement_linework_replay_version = measurement_linework_replay_version_v1;
inline constexpr std::uint32_t measurement_linework_latest_replay_version = measurement_linework_replay_version_v5;

struct MeasurementLineworkEdit {
    BoundaryGeometryEdit intent;
    std::optional<Quantity> authored_length;
};
struct MeasurementLineworkVertexBatch {
    std::vector<BoundaryGeometryEdit> edits;
};
using MeasurementLineworkOperation = std::variant<PlanarTransform, MeasurementLineworkEdit,
    MeasurementLineworkVertexBatch>;

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
    // Schema/replay two: ordered world operations; all receipts stay local.
    std::vector<PlanarTransform> transforms;
    // Schema/replay three/four/five: ordered derivations in the world frame at that
    // operation. Receipts remain the original immutable authoring evidence.
    // The historical transforms member must be empty in this dialect.
    std::vector<MeasurementLineworkOperation> operations;
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

// Explicitly opts into typed chord receipts while retaining local input and
// ordered world operations. Also accepts an empty live authoring candidate;
// persistence still requires a nonempty replayable model.
[[nodiscard]] MeasurementLinework promoted_measurement_linework_for_typed_chord(const MeasurementLinework& model);

// Preserve the local anchor, exact receipts, identities and extensions while
// appending a rigid world-space operation. Both input and copy must replay;
// invalid or precision-losing operations throw without modifying the input.
[[nodiscard]] MeasurementLinework transformed_measurement_linework(
    const MeasurementLinework& model, const PlanarTransform& transform);

// Supports move_vertex and resize_segment only. Every occurrence of a stable
// vertex moves together; open, crossing and retraced strokes remain valid.
// Resize retains chord direction and signed sweep. move_connected translates
// all vertices except the fixed endpoint. Optional exact input is valid only
// for resize and must equal its target_length_metres. No-ops retain the source
// dialect; invalid/precision-losing edits throw without source mutation.
[[nodiscard]] MeasurementLinework edited_measurement_linework(
    const MeasurementLinework& model, const BoundaryGeometryEdit& edit,
    std::optional<Quantity> authored_length = std::nullopt);

// Simultaneous plain move_vertex intents for unique existing stable vertices
// of this stroke. All occurrences move together; only the final geometry is
// admitted. Real changes append one schema/replay-five operation preserving
// receipts and metadata; empty/all-no-op batches retain the exact source.
// Invalid targets, duplicate targets, final degeneracy or precision loss throw
// without changing the source.
[[nodiscard]] MeasurementLinework edited_measurement_linework_vertices(
    const MeasurementLinework& model, const std::vector<BoundaryGeometryEdit>& edits);

enum class MeasurementLineworkFormat {
    supported_v1,
    supported_v2,
    supported_v3,
    supported_v4,
    supported_v5,
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
// is opaque; unknown typed keys fail closed. Schema/replay two additionally
// requires transforms: an ordered array of objects with version: 1, pivot,
// rotation_radians, flip_horizontal, flip_vertical and offset. V1 forbids that
// field, and neither dialect accepts uniform scaling. Inspection reads only the object
// and its positive integral version, plus a required positive integral
// replay_version for a known schema. Decode validates/replays known pairs
// (1,1)..(5,5); unknown positive schema/replay pairs return exact opaque JSON for a
// caller to preserve without decoding. Encode validates and retains original
// expressions and extensions. V3 forbids transforms and requires operations:
// {type: "transform", transform: <v1 transform>} or {type: "edit", edit:
// <strict v1 move_vertex/resize_segment boundary intent>, authored_length:
// <strict exact quantity or null>}. Every edit must name this stroke and an
// existing child, contain only relevant fields, and change its geometry.
// V4 admits typed chord receipts. V5 additionally admits
// {type: "vertex_batch", edits: [<strict plain move_vertex intent>, ...]}.
// Batches require unique existing targets and a nonredundant final change;
// earlier dialects reject this operation. Malformed known models throw invalid_argument.
[[nodiscard]] MeasurementLineworkVersion inspect_measurement_linework_model(
    const nlohmann::json& model);
[[nodiscard]] MeasurementLineworkDecodeResult decode_measurement_linework_model(
    const nlohmann::json& model);
[[nodiscard]] nlohmann::json encode_measurement_linework_model(
    const MeasurementLinework& model);

}  // namespace sketch
