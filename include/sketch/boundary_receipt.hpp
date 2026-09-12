#pragma once

#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Version one remains the default for directly assembled legacy records so
// their encoding stays byte-for-byte compatible. The construction adapter
// emits version two for current session output.
inline constexpr std::uint32_t boundary_receipt_schema_version_v1 = 1;
inline constexpr std::uint32_t boundary_receipt_schema_version_v2 = 2;
inline constexpr std::uint32_t boundary_receipt_schema_version =
    boundary_receipt_schema_version_v1;
inline constexpr std::uint32_t boundary_receipt_latest_schema_version =
    boundary_receipt_schema_version_v2;
inline constexpr std::uint32_t boundary_receipt_replay_version = 1;

// Angles retain the entered expression beside the value used by analytical
// geometry. The normalization helpers below require every retained
// expression to parse back to the exact stored double; no geometric
// tolerance is used for this consistency check.
struct AngleInput {
    double radians{};
    std::string original_expression;
    std::string normalized_expression;

    [[nodiscard]] static AngleInput from_radians(double radians);
    [[nodiscard]] static AngleInput parse(std::string_view expression);

    bool operator==(const AngleInput&) const = default;
};

[[nodiscard]] AngleInput angle_from_radians(double radians);
[[nodiscard]] AngleInput parse_angle(std::string_view expression);

enum class BoundaryConstructionKind {
    line_heading,
    line_rise_run,
    line_relative_turn,
    line_closure,
    line_to_point,
    arc_chord_angle,
    arc_chord_height,
    arc_chord_length,
    arc_start_tangent,
};

// This is the normalized, durable input receipt for one analytical edge.
// Captured start and chord endpoints are construction inputs, not a
// coordinate fallback inferred from a displayed Segment.
struct ConstructionReceipt {
    std::string segment_id;
    BoundaryConstructionKind kind{};
    Vec2 start{};
    std::optional<Vec2> chord_end;
    std::optional<Quantity> distance;
    std::optional<AngleInput> heading;
    std::optional<Quantity> rise;
    std::optional<Quantity> run;
    std::optional<AngleInput> turn;
    std::optional<AngleInput> angle;
    std::optional<Quantity> height;
    std::optional<Quantity> arc_length;
    std::optional<AngleInput> tangent;
    std::optional<AngleInput> sweep;
    std::optional<Vec2> closure_delta;
    bool clockwise{};

    bool operator==(const ConstructionReceipt& other) const noexcept;
};

// These functions encode and decode exactly one normalized construction
// receipt. They intentionally do not validate a chain or require a closing
// anchor; callers that replay a receipt must provide the explicit semantic
// ConstructionReplayContext to replay_construction_receipt().
// Encoding rejects values that would lose typed inputs or fail strict decoding.
[[nodiscard]] ConstructionReceipt decode_construction_receipt(
    const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_construction_receipt(
    const ConstructionReceipt& receipt);

// Exact normalization is shared by the authoring session, replay kernel and
// JSON codec. A malformed or internally inconsistent value throws
// std::invalid_argument atomically.
[[nodiscard]] Quantity normalize_exact_quantity(const Quantity& input,
                                                std::string_view label = "quantity");
[[nodiscard]] AngleInput normalize_exact_angle(const AngleInput& input,
                                               std::string_view label = "angle");

struct ConstructionReplayContext {
    Vec2 expected_start{};
    std::optional<Segment> previous_segment;
    std::optional<Vec2> closure_anchor;
    double tolerance_metres{default_geometry_tolerance_metres};
};

struct ReplayedConstructionReceipt {
    Segment segment;
    ConstructionReceipt receipt;

    bool operator==(const ReplayedConstructionReceipt& other) const noexcept;
};

// Reconstruct one edge solely from its receipt and the explicit semantic
// context required by relative turns and generated closure edges. The
// returned receipt is canonicalized while retaining its segment identity.
[[nodiscard]] ReplayedConstructionReceipt replay_construction_receipt(
    const ConstructionReceipt& receipt, const ConstructionReplayContext& context);

// A durable topology edge pairs exactly one receipt with its stable ordered
// endpoint identities. It intentionally carries no displayed geometry.
struct ConstructionTopologyEdge {
    std::string segment_id;
    std::string start_vertex_id;
    std::string end_vertex_id;
    ConstructionReceipt receipt;

    bool operator==(const ConstructionTopologyEdge&) const noexcept = default;
};

struct BoundaryConstructionRecord {
    std::uint32_t schema_version{boundary_receipt_schema_version};
    std::uint32_t replay_version{boundary_receipt_replay_version};
    Vec2 anchor{};
    std::string boundary_id;
    std::vector<ConstructionTopologyEdge> edges;
    nlohmann::json extensions = nlohmann::json::object();

    bool operator==(const BoundaryConstructionRecord&) const noexcept;
};

// Replay output is still Document independent: it has stable topology IDs
// and canonical analytical Segments, but no IdentifiedBoundary or Entity.
struct ReplayedConstructionEdge {
    std::string segment_id;
    std::string start_vertex_id;
    std::string end_vertex_id;
    Segment segment;

    bool operator==(const ReplayedConstructionEdge& other) const noexcept;
};

struct BoundaryConstructionReplayResult {
    std::uint32_t replay_version{boundary_receipt_replay_version};
    Vec2 anchor{};
    std::string boundary_id;
    std::vector<ReplayedConstructionEdge> edges;
    std::vector<ConstructionReceipt> receipts;

    bool operator==(const BoundaryConstructionReplayResult& other) const noexcept;
};

[[nodiscard]] BoundaryConstructionReplayResult replay_boundary_construction(
    const BoundaryConstructionRecord& record,
    double tolerance_metres = default_geometry_tolerance_metres);

// Copy a replayable record with translated captured points and optional typed
// identity replacements. Expressions, closure vectors and opaque extensions
// are preserved exactly. Both input and result must replay successfully;
// invalid offsets or identity replacements throw std::invalid_argument.
// Even finite offsets can reject when floating-point translation loses the
// exact closure-vector relationship required by replay.
[[nodiscard]] BoundaryConstructionRecord translated_boundary_construction(
    const BoundaryConstructionRecord& record, Vec2 offset,
    const std::map<std::string, std::string, std::less<>>& identity_map = {});

enum class BoundaryReceiptEnvelopeFormat {
    supported_v1,
    supported_v2,
    unsupported_version,
};

struct BoundaryReceiptEnvelopeVersion {
    BoundaryReceiptEnvelopeFormat format{};
    std::optional<std::uint64_t> version;
    std::string diagnostic;
};

struct BoundaryReceiptDecodeResult {
    std::optional<BoundaryConstructionRecord> record;
    std::optional<nlohmann::json> original_envelope;
    std::optional<std::uint64_t> version;
    std::string diagnostic;

    [[nodiscard]] bool supported() const noexcept { return record.has_value(); }
};

// inspect validates only the envelope shape needed to identify its positive
// schema version. decode strictly validates known v1/v2; an unknown positive
// schema or replay version is returned as opaque original JSON so a caller can
// preserve it.
[[nodiscard]] BoundaryReceiptEnvelopeVersion inspect_boundary_receipt_envelope(
    const nlohmann::json& envelope);
[[nodiscard]] BoundaryReceiptDecodeResult decode_boundary_receipt_envelope(
    const nlohmann::json& envelope);
[[nodiscard]] nlohmann::json encode_boundary_receipt_envelope(
    const BoundaryConstructionRecord& record);

}  // namespace sketch
