#include "sketch/boundary_authoring_recovery.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <ostream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

void require_object(const Json& value, std::string_view label) {
    if (!value.is_object()) invalid(std::string(label) + " must be an object");
}

void require_array(const Json& value, std::string_view label) {
    if (!value.is_array()) invalid(std::string(label) + " must be an array");
}

void require_exact_keys(const Json& value, std::initializer_list<std::string_view> required,
                       std::initializer_list<std::string_view> allowed,
                       std::string_view label) {
    require_object(value, label);
    for (const auto key : required) {
        if (!value.contains(key)) invalid(std::string(label) + " is missing " + std::string(key));
    }
    for (const auto& item : value.items()) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
            invalid(std::string(label) + " contains unknown field " + item.key());
        }
    }
}

void require_required_keys(const Json& value, std::initializer_list<std::string_view> required,
                           std::string_view label) {
    require_object(value, label);
    for (const auto key : required) {
        if (!value.contains(key)) invalid(std::string(label) + " is missing " + std::string(key));
    }
}

void validate_limits(const BoundaryAuthoringRecoveryLimits& limits) {
    if (limits.max_json_depth > 64) {
        invalid("boundary authoring recovery JSON depth cannot exceed the portable ceiling of 64");
    }
    if (limits.max_encoded_bytes == 0 || limits.max_json_depth == 0 ||
        limits.max_json_values == 0 || limits.max_string_bytes == 0 || limits.max_actions == 0 ||
        limits.max_generated_ids_per_action == 0 || limits.max_total_generated_ids == 0 ||
        limits.max_chain_edges == 0 || limits.max_dimensions_per_chain == 0 ||
        limits.max_replay_work == 0 || limits.max_retained_history_bytes == 0 ||
        limits.max_operation_bytes == 0 || limits.max_materialization_bytes == 0 ||
        limits.max_cumulative_replay_copy_bytes == 0) {
        invalid("boundary authoring recovery limits must be positive");
    }
}

void add_budget(std::size_t& value, std::size_t amount,
                std::size_t maximum, std::string_view label) {
    if (amount > maximum - std::min(value, maximum)) {
        invalid(std::string(label) + " exceeds the recovery resource budget");
    }
    value += amount;
    if (value > maximum) invalid(std::string(label) + " exceeds the recovery resource budget");
}

class CountingJsonBuffer final : public std::streambuf {
public:
    explicit CountingJsonBuffer(std::size_t limit) : limit_(limit) {}
    std::size_t count{};
private:
    std::size_t limit_;
    std::streamsize xsputn(const char*, std::streamsize amount) override {
        if (amount < 0) invalid("negative JSON serialization length");
        add_budget(count, static_cast<std::size_t>(amount), limit_, "recovery JSON bytes");
        return amount;
    }
    int_type overflow(int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof()))
            add_budget(count, 1, limit_, "recovery JSON bytes");
        return traits_type::not_eof(value);
    }
};
std::size_t enforce_budget(const Json& value, const BoundaryAuthoringRecoveryLimits& limits) {
    validate_limits(limits);
    std::size_t value_count = 0;
    std::size_t string_bytes = 0;
    std::size_t minimum_bytes = 0;

    struct Frame { const Json* value; std::size_t depth; };
    std::vector<Frame> pending{{&value, 0}};
    while (!pending.empty()) {
        const auto frame = pending.back(); pending.pop_back();
        const auto& node = *frame.value;
        if (frame.depth > limits.max_json_depth) invalid("recovery JSON depth exceeds its budget");
        add_budget(value_count, 1, limits.max_json_values, "recovery JSON values");
        add_budget(minimum_bytes, 1, limits.max_encoded_bytes, "minimum JSON bytes");
        if (node.is_discarded() || node.is_binary() ||
            (node.is_number_float() && !std::isfinite(node.get<double>())))
            invalid("recovery JSON contains a nonportable value");
        if (node.is_string()) {
            add_budget(string_bytes, node.get_ref<const std::string&>().size(),
                       limits.max_string_bytes, "recovery JSON strings");
            add_budget(minimum_bytes, node.get_ref<const std::string&>().size(),
                       limits.max_encoded_bytes, "minimum JSON bytes");
        }
        if (node.is_structured()) {
            // Reject breadth before allocating the traversal stack.
            const auto remaining = limits.max_json_values - std::min(value_count, limits.max_json_values);
            if (pending.size() > remaining || node.size() > remaining - pending.size())
                invalid("recovery JSON values exceed their budget");
            const auto bytes_remaining = limits.max_encoded_bytes - minimum_bytes;
            if (pending.size() > bytes_remaining || node.size() > bytes_remaining - pending.size())
                invalid("recovery JSON breadth exceeds its byte budget");
            for (const auto& child : node.items()) {
                if (node.is_object()) {
                    add_budget(string_bytes, child.key().size(), limits.max_string_bytes, "recovery JSON strings");
                    add_budget(minimum_bytes, child.key().size(), limits.max_encoded_bytes, "minimum JSON bytes");
                }
                pending.push_back({&child.value(), frame.depth + 1});
            }
        }
    }
    CountingJsonBuffer buffer(limits.max_encoded_bytes);
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    try { output << value; }
    catch (const std::ios_base::failure&) { invalid("recovery JSON size exceeds its budget"); }
    return buffer.count;
}

std::string read_string(const Json& value, std::string_view label) {
    if (!value.is_string()) invalid(std::string(label) + " must be a string");
    return value.get<std::string>();
}

bool read_bool(const Json& value, std::string_view label) {
    if (!value.is_boolean()) invalid(std::string(label) + " must be boolean");
    return value.get<bool>();
}

double read_double(const Json& value, std::string_view label) {
    if (!value.is_number()) invalid(std::string(label) + " must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid(std::string(label) + " must be finite");
    return result;
}

std::uint64_t read_positive_uint(const Json& value, std::string_view label) {
    if (value.is_number_unsigned()) {
        const auto result = value.get<std::uint64_t>();
        if (result == 0) invalid(std::string(label) + " must be positive");
        return result;
    }
    if (!value.is_number_integer()) invalid(std::string(label) + " must be a positive integer");
    const auto result = value.get<std::int64_t>();
    if (result <= 0) invalid(std::string(label) + " must be positive");
    return static_cast<std::uint64_t>(result);
}

std::uint64_t read_nonnegative_uint(const Json& value, std::string_view label) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (!value.is_number_integer()) invalid(std::string(label) + " must be an integer");
    const auto result = value.get<std::int64_t>();
    if (result < 0) invalid(std::string(label) + " must be nonnegative");
    return static_cast<std::uint64_t>(result);
}

std::size_t read_index(const Json& value, std::string_view label) {
    const auto result = read_nonnegative_uint(value, label);
    if (result > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        invalid(std::string(label) + " exceeds the host size range");
    }
    return static_cast<std::size_t>(result);
}

std::uint32_t read_uint32(const Json& value, std::string_view label) {
    const auto result = read_positive_uint(value, label);
    if (result > std::numeric_limits<std::uint32_t>::max()) {
        invalid(std::string(label) + " exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(result);
}

void require_identifier(std::string_view value, std::string_view label) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(label) + " must contain 1..128 supported ASCII characters");
    }
}

Vec2 read_point(const Json& value, std::string_view label) {
    require_array(value, label);
    if (value.size() != 2) invalid(std::string(label) + " must contain exactly two coordinates");
    return {read_double(value[0], std::string(label) + " x"),
            read_double(value[1], std::string(label) + " y")};
}

Json write_point(Vec2 point, std::string_view label) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        invalid(std::string(label) + " must be finite");
    }
    return Json::array({point.x, point.y});
}

std::string mode_name(BoundaryAuthoringMode mode) {
    switch (mode) {
        case BoundaryAuthoringMode::draw_first:
            return "draw_first";
        case BoundaryAuthoringMode::define_first:
            return "define_first";
    }
    invalid("unsupported boundary authoring mode");
}

BoundaryAuthoringMode mode_from_name(std::string_view value) {
    if (value == "draw_first") return BoundaryAuthoringMode::draw_first;
    if (value == "define_first") return BoundaryAuthoringMode::define_first;
    invalid("unsupported boundary authoring mode");
}

Json write_options(const BoundaryAuthoringOptions& options) {
    return Json{{"default_boundary_type", options.default_boundary_type},
                {"boundary_id_prefix", options.boundary_id_prefix},
                {"vertex_id_prefix", options.vertex_id_prefix},
                {"segment_id_prefix", options.segment_id_prefix},
                {"dimension_id_prefix", options.dimension_id_prefix},
                {"automatic_dimension_placement", options.automatic_dimension_placement},
                {"automatic_placement_version", options.automatic_placement_version},
                {"geometry_tolerance_metres", options.geometry_tolerance_metres}};
}

BoundaryAuthoringOptions read_options(const Json& value) {
    require_exact_keys(value,
                       {"default_boundary_type", "boundary_id_prefix", "vertex_id_prefix",
                        "segment_id_prefix", "dimension_id_prefix",
                        "automatic_dimension_placement", "automatic_placement_version",
                        "geometry_tolerance_metres"},
                       {"default_boundary_type", "boundary_id_prefix", "vertex_id_prefix",
                        "segment_id_prefix", "dimension_id_prefix",
                        "automatic_dimension_placement", "automatic_placement_version",
                        "geometry_tolerance_metres"},
                       "boundary authoring options");
    BoundaryAuthoringOptions result;
    result.default_boundary_type =
        read_string(value.at("default_boundary_type"), "default boundary type");
    result.boundary_id_prefix = read_string(value.at("boundary_id_prefix"), "boundary id prefix");
    result.vertex_id_prefix = read_string(value.at("vertex_id_prefix"), "vertex id prefix");
    result.segment_id_prefix = read_string(value.at("segment_id_prefix"), "segment id prefix");
    result.dimension_id_prefix =
        read_string(value.at("dimension_id_prefix"), "dimension id prefix");
    result.automatic_dimension_placement =
        read_bool(value.at("automatic_dimension_placement"), "automatic dimension placement");
    result.automatic_placement_version =
        read_uint32(value.at("automatic_placement_version"), "automatic placement version");
    result.geometry_tolerance_metres =
        read_double(value.at("geometry_tolerance_metres"), "geometry tolerance");
    return result;
}

Json write_counters(const BoundaryAuthoringCounters& counters) {
    return Json{{"next_boundary_id", counters.next_boundary_id},
                {"next_vertex_id", counters.next_vertex_id},
                {"next_segment_id", counters.next_segment_id},
                {"next_dimension_id", counters.next_dimension_id}};
}

BoundaryAuthoringCounters read_counters(const Json& value) {
    require_exact_keys(value,
                       {"next_boundary_id", "next_vertex_id", "next_segment_id",
                        "next_dimension_id"},
                       {"next_boundary_id", "next_vertex_id", "next_segment_id",
                        "next_dimension_id"},
                       "boundary authoring counters");
    return {read_positive_uint(value.at("next_boundary_id"), "next boundary id counter"),
            read_positive_uint(value.at("next_vertex_id"), "next vertex id counter"),
            read_positive_uint(value.at("next_segment_id"), "next segment id counter"),
            read_positive_uint(value.at("next_dimension_id"), "next dimension id counter")};
}

// ConstructionReceipt JSON is owned by boundary_receipt.cpp. Recovery uses
// its public single-receipt adapter so the strict variant schema and exact
// quantity/angle normalization stay in one authority.

std::string dimension_placement_name(BoundaryDimensionPlacement placement) {
    switch (placement) {
        case BoundaryDimensionPlacement::manual:
            return "manual";
        case BoundaryDimensionPlacement::automatic:
            return "automatic";
    }
    invalid("unsupported dimension placement");
}

BoundaryDimensionPlacement dimension_placement_from_name(std::string_view value) {
    if (value == "manual") return BoundaryDimensionPlacement::manual;
    if (value == "automatic") return BoundaryDimensionPlacement::automatic;
    invalid("unsupported dimension placement");
}

Json write_dimension(const BoundaryDimension& dimension) {
    require_identifier(dimension.id, "dimension id");
    require_identifier(dimension.boundary_id, "dimension boundary_id");
    require_identifier(dimension.segment_id, "dimension segment_id");
    const auto placement = dimension_placement_name(dimension.placement);
    Json result{{"id", dimension.id},
                {"boundary_id", dimension.boundary_id},
                {"segment_id", dimension.segment_id},
                {"text_position", write_point(dimension.text_position, "dimension text_position")},
                {"placement", placement}};
    if (dimension.placement == BoundaryDimensionPlacement::manual) {
        if (dimension.automatic_placement_version) {
            invalid("manual dimension contains an automatic placement version");
        }
    } else {
        if (!dimension.automatic_placement_version ||
            (*dimension.automatic_placement_version != 1 &&
             *dimension.automatic_placement_version != 2)) {
            invalid("automatic dimension placement version is unsupported");
        }
        result["automatic_placement_version"] = *dimension.automatic_placement_version;
    }
    return result;
}

BoundaryDimension read_dimension(const Json& value) {
    require_exact_keys(value,
                       {"id", "boundary_id", "segment_id", "text_position", "placement"},
                       {"id", "boundary_id", "segment_id", "text_position", "placement",
                        "automatic_placement_version"},
                       "boundary authoring dimension");
    const auto placement =
        dimension_placement_from_name(read_string(value.at("placement"), "dimension placement"));
    if (placement == BoundaryDimensionPlacement::manual && value.contains("automatic_placement_version")) {
        invalid("manual dimension contains an automatic placement version");
    }
    if (placement == BoundaryDimensionPlacement::automatic &&
        !value.contains("automatic_placement_version")) {
        invalid("automatic dimension is missing placement version");
    }
    std::optional<std::uint32_t> automatic_version;
    if (placement == BoundaryDimensionPlacement::automatic) {
        automatic_version = read_uint32(value.at("automatic_placement_version"),
                                         "automatic placement version");
        if (*automatic_version != 1 && *automatic_version != 2)
            invalid("automatic placement version is unsupported");
    }
    BoundaryDimension result{
        read_string(value.at("id"), "dimension id"),
        read_string(value.at("boundary_id"), "dimension boundary_id"),
        read_string(value.at("segment_id"), "dimension segment_id"),
        read_point(value.at("text_position"), "dimension text_position"), placement,
        automatic_version};
    require_identifier(result.id, "dimension id");
    require_identifier(result.boundary_id, "dimension boundary_id");
    require_identifier(result.segment_id, "dimension segment_id");
    return result;
}

Json write_chain(const BoundaryAuthoringChainRecord& chain) {
    require_identifier(chain.boundary_id, "chain boundary_id");
    require_identifier(chain.type, "chain type");
    if (!std::isfinite(chain.anchor.x) || !std::isfinite(chain.anchor.y)) {
        invalid("chain anchor must be finite");
    }
    if (!chain.classified && !chain.classification.empty()) {
        invalid("unclassified chain carries a classification");
    }
    Json edges = Json::array();
    for (const auto& edge : chain.edges) {
        require_identifier(edge.segment_id, "chain segment_id");
        require_identifier(edge.start_vertex_id, "chain start_vertex_id");
        require_identifier(edge.end_vertex_id, "chain end_vertex_id");
        if (edge.receipt.segment_id != edge.segment_id) {
            invalid("chain receipt and segment identities differ");
        }
        edges.push_back(Json{{"segment_id", edge.segment_id},
                             {"start_vertex_id", edge.start_vertex_id},
                             {"end_vertex_id", edge.end_vertex_id},
                             {"receipt", encode_construction_receipt(edge.receipt)}});
    }
    Json dimensions = Json::array();
    for (const auto& dimension : chain.dimensions) dimensions.push_back(write_dimension(dimension));
    return Json{{"anchor", write_point(chain.anchor, "chain anchor")},
                {"boundary_id", chain.boundary_id},
                {"type", chain.type},
                {"classification", chain.classification},
                {"edges", std::move(edges)},
                {"dimensions", std::move(dimensions)},
                {"classified", chain.classified}};
}

BoundaryAuthoringChainRecord read_chain(const Json& value,
                                        const BoundaryAuthoringRecoveryLimits& limits) {
    require_exact_keys(value,
                       {"anchor", "boundary_id", "type", "classification", "edges",
                        "dimensions", "classified"},
                       {"anchor", "boundary_id", "type", "classification", "edges",
                        "dimensions", "classified"},
                       "boundary authoring chain");
    BoundaryAuthoringChainRecord result;
    result.anchor = read_point(value.at("anchor"), "chain anchor");
    result.boundary_id = read_string(value.at("boundary_id"), "chain boundary_id");
    result.type = read_string(value.at("type"), "chain type");
    result.classification = read_string(value.at("classification"), "chain classification");
    result.classified = read_bool(value.at("classified"), "chain classified");
    require_identifier(result.boundary_id, "chain boundary_id");
    require_identifier(result.type, "chain type");
    if (!result.classified && !result.classification.empty()) {
        invalid("unclassified chain carries a classification");
    }
    const auto& encoded_edges = value.at("edges");
    require_array(encoded_edges, "chain edges");
    if (encoded_edges.size() > limits.max_chain_edges) {
        invalid("chain edge count exceeds its recovery budget");
    }
    std::set<std::string, std::less<>> segment_ids;
    for (const auto& encoded : encoded_edges) {
        require_exact_keys(encoded, {"segment_id", "start_vertex_id", "end_vertex_id", "receipt"},
                           {"segment_id", "start_vertex_id", "end_vertex_id", "receipt"},
                           "chain edge");
        ConstructionTopologyEdge edge;
        edge.segment_id = read_string(encoded.at("segment_id"), "chain segment_id");
        edge.start_vertex_id =
            read_string(encoded.at("start_vertex_id"), "chain start_vertex_id");
        edge.end_vertex_id = read_string(encoded.at("end_vertex_id"), "chain end_vertex_id");
        require_identifier(edge.segment_id, "chain segment_id");
        require_identifier(edge.start_vertex_id, "chain start_vertex_id");
        require_identifier(edge.end_vertex_id, "chain end_vertex_id");
        if (!segment_ids.insert(edge.segment_id).second) invalid("duplicate chain segment_id");
        edge.receipt = decode_construction_receipt(encoded.at("receipt"));
        if (edge.receipt.segment_id != edge.segment_id) {
            invalid("chain receipt and segment identities differ");
        }
        result.edges.push_back(std::move(edge));
    }
    const auto& encoded_dimensions = value.at("dimensions");
    require_array(encoded_dimensions, "chain dimensions");
    if (encoded_dimensions.size() > limits.max_dimensions_per_chain) {
        invalid("chain dimension count exceeds its recovery budget");
    }
    result.dimensions.reserve(encoded_dimensions.size());
    for (const auto& encoded : encoded_dimensions) {
        result.dimensions.push_back(read_dimension(encoded));
    }
    return result;
}

std::string action_kind_name(BoundaryAuthoringActionKind kind) {
    switch (kind) {
        case BoundaryAuthoringActionKind::set_classification:
            return "set_classification";
        case BoundaryAuthoringActionKind::classify_current_chain:
            return "classify_current_chain";
        case BoundaryAuthoringActionKind::classify_last_chain:
            return "classify_last_chain";
        case BoundaryAuthoringActionKind::anchor:
            return "anchor";
        case BoundaryAuthoringActionKind::pen_up:
            return "pen_up";
        case BoundaryAuthoringActionKind::pen_down:
            return "pen_down";
        case BoundaryAuthoringActionKind::line_heading:
            return "line_heading";
        case BoundaryAuthoringActionKind::line_rise_run:
            return "line_rise_run";
        case BoundaryAuthoringActionKind::line_relative_turn:
            return "line_relative_turn";
        case BoundaryAuthoringActionKind::line_closure:
            return "line_closure";
        case BoundaryAuthoringActionKind::line_to_point:
            return "line_to_point";
        case BoundaryAuthoringActionKind::arc_chord_angle:
            return "arc_chord_angle";
        case BoundaryAuthoringActionKind::arc_chord_height:
            return "arc_chord_height";
        case BoundaryAuthoringActionKind::arc_chord_length:
            return "arc_chord_length";
        case BoundaryAuthoringActionKind::arc_start_tangent:
            return "arc_start_tangent";
        case BoundaryAuthoringActionKind::manual_dimension:
            return "manual_dimension";
        case BoundaryAuthoringActionKind::automatic_dimension:
            return "automatic_dimension";
        case BoundaryAuthoringActionKind::close_chain:
            return "close_chain";
    }
    invalid("unsupported boundary authoring action kind");
}

BoundaryAuthoringActionKind action_kind_from_name(std::string_view value) {
    if (value == "set_classification") return BoundaryAuthoringActionKind::set_classification;
    if (value == "classify_current_chain") return BoundaryAuthoringActionKind::classify_current_chain;
    if (value == "classify_last_chain") return BoundaryAuthoringActionKind::classify_last_chain;
    if (value == "anchor") return BoundaryAuthoringActionKind::anchor;
    if (value == "pen_up") return BoundaryAuthoringActionKind::pen_up;
    if (value == "pen_down") return BoundaryAuthoringActionKind::pen_down;
    if (value == "line_heading") return BoundaryAuthoringActionKind::line_heading;
    if (value == "line_rise_run") return BoundaryAuthoringActionKind::line_rise_run;
    if (value == "line_relative_turn") return BoundaryAuthoringActionKind::line_relative_turn;
    if (value == "line_closure") return BoundaryAuthoringActionKind::line_closure;
    if (value == "line_to_point") return BoundaryAuthoringActionKind::line_to_point;
    if (value == "arc_chord_angle") return BoundaryAuthoringActionKind::arc_chord_angle;
    if (value == "arc_chord_height") return BoundaryAuthoringActionKind::arc_chord_height;
    if (value == "arc_chord_length") return BoundaryAuthoringActionKind::arc_chord_length;
    if (value == "arc_start_tangent") return BoundaryAuthoringActionKind::arc_start_tangent;
    if (value == "manual_dimension") return BoundaryAuthoringActionKind::manual_dimension;
    if (value == "automatic_dimension") return BoundaryAuthoringActionKind::automatic_dimension;
    if (value == "close_chain") return BoundaryAuthoringActionKind::close_chain;
    invalid("unsupported boundary authoring action kind");
}

void require_action_key_shape(const Json& value, BoundaryAuthoringActionKind kind) {
    switch (kind) {
        case BoundaryAuthoringActionKind::set_classification:
        case BoundaryAuthoringActionKind::classify_current_chain:
        case BoundaryAuthoringActionKind::classify_last_chain:
            require_exact_keys(value,
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "classification"},
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "classification"},
                               "boundary authoring action");
            return;
        case BoundaryAuthoringActionKind::anchor:
            require_exact_keys(value,
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "point"},
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "point"},
                               "boundary authoring action");
            return;
        case BoundaryAuthoringActionKind::pen_up:
        case BoundaryAuthoringActionKind::pen_down:
            require_exact_keys(value,
                               {"kind", "counters_before", "counters_after", "generated_ids"},
                               {"kind", "counters_before", "counters_after", "generated_ids"},
                               "boundary authoring action");
            return;
        case BoundaryAuthoringActionKind::line_heading:
        case BoundaryAuthoringActionKind::line_rise_run:
        case BoundaryAuthoringActionKind::line_relative_turn:
        case BoundaryAuthoringActionKind::line_closure:
        case BoundaryAuthoringActionKind::line_to_point:
        case BoundaryAuthoringActionKind::arc_chord_angle:
        case BoundaryAuthoringActionKind::arc_chord_height:
        case BoundaryAuthoringActionKind::arc_chord_length:
        case BoundaryAuthoringActionKind::arc_start_tangent:
            require_exact_keys(value,
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "receipt"},
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "receipt"},
                               "boundary authoring action");
            return;
        case BoundaryAuthoringActionKind::manual_dimension:
        case BoundaryAuthoringActionKind::automatic_dimension:
            require_exact_keys(value,
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "dimension"},
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "dimension"},
                               "boundary authoring action");
            return;
        case BoundaryAuthoringActionKind::close_chain:
            require_exact_keys(value,
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "chain"},
                               {"kind", "counters_before", "counters_after", "generated_ids",
                                "chain"},
                               "boundary authoring action");
            return;
    }
    invalid("unsupported boundary authoring action kind");
}

Json write_action(const BoundaryAuthoringAction& action) {
    Json generated = Json::array();
    for (const auto& id : action.generated_ids) {
        require_identifier(id, "generated identity");
        generated.push_back(id);
    }
    Json result{{"kind", action_kind_name(action.kind)},
                {"counters_before", write_counters(action.counters_before)},
                {"counters_after", write_counters(action.counters_after)},
                {"generated_ids", std::move(generated)}};
    switch (action.kind) {
        case BoundaryAuthoringActionKind::set_classification:
        case BoundaryAuthoringActionKind::classify_current_chain:
        case BoundaryAuthoringActionKind::classify_last_chain:
            if (!action.classification) invalid("classification action is incomplete");
            result["classification"] = *action.classification;
            break;
        case BoundaryAuthoringActionKind::anchor:
            if (!action.point) invalid("anchor action is incomplete");
            result["point"] = write_point(*action.point, "anchor action point");
            break;
        case BoundaryAuthoringActionKind::pen_up:
        case BoundaryAuthoringActionKind::pen_down:
            break;
        case BoundaryAuthoringActionKind::line_heading:
        case BoundaryAuthoringActionKind::line_rise_run:
        case BoundaryAuthoringActionKind::line_relative_turn:
        case BoundaryAuthoringActionKind::line_closure:
        case BoundaryAuthoringActionKind::line_to_point:
        case BoundaryAuthoringActionKind::arc_chord_angle:
        case BoundaryAuthoringActionKind::arc_chord_height:
        case BoundaryAuthoringActionKind::arc_chord_length:
        case BoundaryAuthoringActionKind::arc_start_tangent:
            if (!action.receipt) invalid("edge action is incomplete");
            result["receipt"] = encode_construction_receipt(*action.receipt);
            break;
        case BoundaryAuthoringActionKind::manual_dimension:
        case BoundaryAuthoringActionKind::automatic_dimension:
            if (!action.dimension) invalid("dimension action is incomplete");
            result["dimension"] = write_dimension(*action.dimension);
            break;
        case BoundaryAuthoringActionKind::close_chain:
            if (!action.chain) invalid("close action is incomplete");
            result["chain"] = write_chain(*action.chain);
            break;
    }
    return result;
}

BoundaryAuthoringAction read_action(const Json& value,
                                    const BoundaryAuthoringRecoveryLimits& limits,
                                    std::size_t& total_generated_ids) {
    require_object(value, "boundary authoring action");
    if (!value.contains("kind")) invalid("boundary authoring action is missing kind");
    const auto kind = action_kind_from_name(
        read_string(value.at("kind"), "boundary authoring action kind"));
    require_action_key_shape(value, kind);
    BoundaryAuthoringAction result;
    result.kind = kind;
    result.counters_before = read_counters(value.at("counters_before"));
    result.counters_after = read_counters(value.at("counters_after"));
    const auto& encoded_ids = value.at("generated_ids");
    require_array(encoded_ids, "action generated_ids");
    if (encoded_ids.size() > limits.max_generated_ids_per_action) {
        invalid("action generated identity count exceeds its recovery budget");
    }
    if (encoded_ids.size() > limits.max_total_generated_ids -
                                 std::min(total_generated_ids, limits.max_total_generated_ids)) {
        invalid("total generated identity count exceeds its recovery budget");
    }
    total_generated_ids += encoded_ids.size();
    result.generated_ids.reserve(encoded_ids.size());
    for (const auto& encoded_id : encoded_ids) {
        auto id = read_string(encoded_id, "generated identity");
        require_identifier(id, "generated identity");
        result.generated_ids.push_back(std::move(id));
    }
    switch (kind) {
        case BoundaryAuthoringActionKind::set_classification:
        case BoundaryAuthoringActionKind::classify_current_chain:
        case BoundaryAuthoringActionKind::classify_last_chain:
            result.classification =
                read_string(value.at("classification"), "action classification");
            break;
        case BoundaryAuthoringActionKind::anchor:
            result.point = read_point(value.at("point"), "action point");
            break;
        case BoundaryAuthoringActionKind::pen_up:
        case BoundaryAuthoringActionKind::pen_down:
            break;
        case BoundaryAuthoringActionKind::line_heading:
        case BoundaryAuthoringActionKind::line_rise_run:
        case BoundaryAuthoringActionKind::line_relative_turn:
        case BoundaryAuthoringActionKind::line_closure:
        case BoundaryAuthoringActionKind::line_to_point:
        case BoundaryAuthoringActionKind::arc_chord_angle:
        case BoundaryAuthoringActionKind::arc_chord_height:
        case BoundaryAuthoringActionKind::arc_chord_length:
        case BoundaryAuthoringActionKind::arc_start_tangent:
            result.receipt = decode_construction_receipt(value.at("receipt"));
            break;
        case BoundaryAuthoringActionKind::manual_dimension:
        case BoundaryAuthoringActionKind::automatic_dimension:
            result.dimension = read_dimension(value.at("dimension"));
            break;
        case BoundaryAuthoringActionKind::close_chain:
            result.chain = read_chain(value.at("chain"), limits);
            break;
    }
    return result;
}

void preflight_action_fields(const BoundaryAuthoringAction& action,
                             const BoundaryAuthoringRecoveryLimits& limits) {
    bool classification = false, point = false, receipt = false, dimension = false, chain = false;
    switch (action.kind) {
    case BoundaryAuthoringActionKind::set_classification:
    case BoundaryAuthoringActionKind::classify_current_chain:
    case BoundaryAuthoringActionKind::classify_last_chain: classification = true; break;
    case BoundaryAuthoringActionKind::anchor: point = true; break;
    case BoundaryAuthoringActionKind::pen_up:
    case BoundaryAuthoringActionKind::pen_down: break;
    case BoundaryAuthoringActionKind::line_heading:
    case BoundaryAuthoringActionKind::line_rise_run:
    case BoundaryAuthoringActionKind::line_relative_turn:
    case BoundaryAuthoringActionKind::line_closure:
    case BoundaryAuthoringActionKind::line_to_point:
    case BoundaryAuthoringActionKind::arc_chord_angle:
    case BoundaryAuthoringActionKind::arc_chord_height:
    case BoundaryAuthoringActionKind::arc_chord_length:
    case BoundaryAuthoringActionKind::arc_start_tangent: receipt = true; break;
    case BoundaryAuthoringActionKind::manual_dimension:
    case BoundaryAuthoringActionKind::automatic_dimension: dimension = true; break;
    case BoundaryAuthoringActionKind::close_chain: chain = true; break;
    default: invalid("unknown action kind during resource preflight");
    }
    if (action.classification.has_value() != classification || action.point.has_value() != point ||
        action.receipt.has_value() != receipt || action.dimension.has_value() != dimension ||
        action.chain.has_value() != chain)
        invalid("action has incompatible optional payloads");
    std::size_t strings = 0;
    const auto text = [&](const std::string& value) {
        add_budget(strings, value.size(), std::min(limits.max_string_bytes, limits.max_encoded_bytes), "action raw strings");
    };
    const auto receipt_strings = [&](const ConstructionReceipt& r) {
        text(r.segment_id);
        for (const auto* q : {&r.distance, &r.rise, &r.run, &r.height, &r.arc_length})
            if (*q) text((*q)->original_expression);
        for (const auto* a : {&r.heading, &r.turn, &r.angle, &r.tangent, &r.sweep})
            if (*a) { text((*a)->original_expression); text((*a)->normalized_expression); }
    };
    const auto dimension_strings = [&](const BoundaryDimension& d) { text(d.id); text(d.boundary_id); text(d.segment_id); };
    if (action.generated_ids.size() > limits.max_generated_ids_per_action ||
        action.generated_ids.size() > limits.max_json_values || action.generated_ids.size() > limits.max_encoded_bytes / 3)
        invalid("action identity collection exceeds preallocation budget");
    for (const auto& id : action.generated_ids) text(id);
    if (action.classification) text(*action.classification);
    if (action.receipt) receipt_strings(*action.receipt);
    if (action.dimension) dimension_strings(*action.dimension);
    if (action.chain) {
        const auto& c = *action.chain;
        if (c.edges.size() > limits.max_chain_edges || c.dimensions.size() > limits.max_dimensions_per_chain ||
            c.edges.size() > limits.max_json_values || c.dimensions.size() > limits.max_json_values ||
            c.edges.size() > limits.max_encoded_bytes / 64 || c.dimensions.size() > limits.max_encoded_bytes / 64)
            invalid("action chain collection exceeds preallocation budget");
        text(c.boundary_id); text(c.type); text(c.classification);
        for (const auto& edge : c.edges) {
            text(edge.segment_id); text(edge.start_vertex_id); text(edge.end_vertex_id); receipt_strings(edge.receipt);
        }
        for (const auto& d : c.dimensions) dimension_strings(d);
    }
}
void validate_checkpoint_resource_shape(const BoundaryAuthoringCheckpoint& checkpoint,
                                        const BoundaryAuthoringRecoveryLimits& limits) {
    if (checkpoint.actions.size() > limits.max_actions) {
        invalid("action count exceeds its recovery budget");
    }
    if (checkpoint.history_position > checkpoint.actions.size()) {
        invalid("history position exceeds action count");
    }
    std::size_t total_generated_ids = 0;
    for (const auto& action : checkpoint.actions) {
        preflight_action_fields(action, limits);
        if (action.generated_ids.size() > limits.max_generated_ids_per_action) {
            invalid("action generated identity count exceeds its recovery budget");
        }
        if (action.generated_ids.size() > limits.max_total_generated_ids -
                                             std::min(total_generated_ids,
                                                      limits.max_total_generated_ids)) {
            invalid("total generated identity count exceeds its recovery budget");
        }
        total_generated_ids += action.generated_ids.size();
        if (action.chain) {
            if (action.chain->edges.size() > limits.max_chain_edges ||
                action.chain->dimensions.size() > limits.max_dimensions_per_chain) {
                invalid("chain resource count exceeds its recovery budget");
            }
        }

    }
}

}  // namespace

BoundaryAuthoringRecoveryVersion inspect_boundary_authoring_recovery(
    const Json& envelope, const BoundaryAuthoringRecoveryLimits& limits) {
    enforce_budget(envelope, limits);
    require_object(envelope, "boundary authoring recovery envelope");
    if (!envelope.contains("version")) {
        invalid("boundary authoring recovery envelope is missing version");
    }
    const auto version = read_positive_uint(envelope.at("version"),
                                            "boundary authoring recovery version");
    if (version == boundary_authoring_recovery_version) {
        return {BoundaryAuthoringRecoveryFormat::supported_v1, version, {}};
    }
    return {BoundaryAuthoringRecoveryFormat::unsupported_version, version,
            "unsupported boundary authoring recovery version"};
}

BoundaryAuthoringRecoveryDecodeResult decode_boundary_authoring_recovery(
    const Json& envelope, const BoundaryAuthoringRecoveryLimits& limits) {
    const auto inspected = inspect_boundary_authoring_recovery(envelope, limits);
    if (inspected.format == BoundaryAuthoringRecoveryFormat::unsupported_version) {
        return {std::nullopt, envelope, inspected.version, std::nullopt, inspected.diagnostic};
    }
    // A supported envelope version can still carry a future replay dialect.
    // Inspect the positive replay discriminator before interpreting any
    // payload fields, so the complete bounded JSON value remains opaque.
    require_required_keys(envelope, {"version", "replay_version"},
                          "boundary authoring recovery envelope");
    const auto replay_version =
        read_positive_uint(envelope.at("replay_version"), "boundary authoring recovery replay version");
    if (replay_version != boundary_authoring_recovery_replay_version) {
        return {std::nullopt, envelope, inspected.version, replay_version,
                "unsupported boundary authoring recovery replay version"};
    }
    require_exact_keys(
        envelope,
        {"version", "replay_version", "mode", "options", "identity_namespace", "pointer",
         "actions", "history_position", "counters", "extensions"},
        {"version", "replay_version", "mode", "options", "identity_namespace", "pointer",
         "actions", "history_position", "counters", "extensions"},
        "boundary authoring recovery envelope");

    BoundaryAuthoringCheckpoint checkpoint;
    checkpoint.version = read_uint32(envelope.at("version"), "boundary authoring recovery version");
    checkpoint.replay_version =
        read_uint32(envelope.at("replay_version"), "boundary authoring recovery replay version");
    checkpoint.mode = mode_from_name(read_string(envelope.at("mode"), "boundary authoring mode"));
    checkpoint.options = read_options(envelope.at("options"));
    checkpoint.identity_namespace =
        read_string(envelope.at("identity_namespace"), "recovery identity namespace");
    require_identifier(checkpoint.identity_namespace, "recovery identity namespace");
    const auto& encoded_pointer = envelope.at("pointer");
    if (!encoded_pointer.is_null()) checkpoint.pointer = read_point(encoded_pointer, "recovery pointer");
    const auto& encoded_actions = envelope.at("actions");
    require_array(encoded_actions, "recovery actions");
    if (encoded_actions.size() > limits.max_actions) {
        invalid("action count exceeds its recovery budget");
    }
    checkpoint.actions.reserve(encoded_actions.size());
    std::size_t total_generated_ids = 0;
    for (const auto& encoded_action : encoded_actions) {
        checkpoint.actions.push_back(read_action(encoded_action, limits, total_generated_ids));
    }
    checkpoint.history_position = read_index(envelope.at("history_position"), "history position");
    checkpoint.counters = read_counters(envelope.at("counters"));
    checkpoint.extensions = envelope.at("extensions");
    if (!checkpoint.extensions.is_object()) {
        invalid("boundary authoring recovery extensions must be an object");
    }
    validate_checkpoint_resource_shape(checkpoint, limits);
    // The session is the semantic validator. It rejects decreasing or
    // colliding allocation fences, malformed variants and any output that
    // differs from the authoritative operations.
    (void)BoundaryAuthoringSession::from_recovery_checkpoint(checkpoint, limits);
    return {std::move(checkpoint), std::nullopt, inspected.version, replay_version, {}};
}

Json encode_boundary_authoring_recovery(
    const BoundaryAuthoringCheckpoint& checkpoint,
    const BoundaryAuthoringRecoveryLimits& limits) {
    validate_limits(limits);
    if (checkpoint.version != boundary_authoring_recovery_version ||
        checkpoint.replay_version != boundary_authoring_recovery_replay_version) {
        invalid("unsupported boundary authoring recovery version");
    }
    if (!checkpoint.extensions.is_object()) {
        invalid("boundary authoring recovery extensions must be an object");
    }
    enforce_budget(checkpoint.extensions, limits);
    validate_checkpoint_resource_shape(checkpoint, limits);
    (void)BoundaryAuthoringSession::from_recovery_checkpoint(checkpoint, limits);

    Json encoded_actions = Json::array();
    for (const auto& action : checkpoint.actions) encoded_actions.push_back(write_action(action));
    Json result{{"version", checkpoint.version},
                {"replay_version", checkpoint.replay_version},
                {"mode", mode_name(checkpoint.mode)},
                {"options", write_options(checkpoint.options)},
                {"identity_namespace", checkpoint.identity_namespace},
                {"pointer", checkpoint.pointer ? write_point(*checkpoint.pointer, "recovery pointer")
                                                 : Json(nullptr)},
                {"actions", std::move(encoded_actions)},
                {"history_position", checkpoint.history_position},
                {"counters", write_counters(checkpoint.counters)},
                {"extensions", checkpoint.extensions}};
    enforce_budget(result, limits);
    return result;
}

namespace detail {
void validate_authoring_recovery_json(const nlohmann::json& value,
                                      const BoundaryAuthoringResourcePolicy& policy) {
    enforce_budget(value, policy);
}
namespace {
std::size_t scaled_bytes(std::size_t count, std::size_t size) {
    std::size_t result;
    if (!boundary_authoring_recovery_checked_multiply(count, size, result))
        invalid("resource byte accounting overflow");
    return result;
}

BoundaryAuthoringResourceUsage measure_wire(const Json& value,
                                            const BoundaryAuthoringResourcePolicy& policy) {
    BoundaryAuthoringResourceUsage usage;
    usage.encoded_bytes = enforce_budget(value, policy);
    struct Item { const Json* value; std::size_t depth; };
    std::vector<Item> stack{{&value, 0}};
    while (!stack.empty()) {
        const auto item = stack.back(); stack.pop_back();
        ++usage.json_values;
        usage.json_depth = std::max(usage.json_depth, item.depth);
        if (item.value->is_string())
            usage.string_bytes += item.value->get_ref<const std::string&>().size();
        if (item.value->is_structured()) {
            for (const auto& child : item.value->items()) {
                if (item.value->is_object()) usage.string_bytes += child.key().size();
                stack.push_back({&child.value(), item.depth + 1});
            }
        }
    }
    return usage;
}
}

BoundaryAuthoringResourceUsage measure_authoring_recovery_json(
    const nlohmann::json& value, const BoundaryAuthoringResourcePolicy& policy) {
    return measure_wire(value, policy);
}

namespace {
BoundaryAuthoringResourceUsage measure_context_wire(
    const BoundaryAuthoringOptions& options, BoundaryAuthoringMode mode,
    std::string_view identity, const Json& extensions,
    const BoundaryAuthoringResourcePolicy& policy) {
    if (!extensions.is_object()) invalid("recovery extensions must be an object");
    std::size_t raw_strings = 0;
    for (const std::string_view value : {identity, std::string_view(options.default_boundary_type),
        std::string_view(options.boundary_id_prefix), std::string_view(options.vertex_id_prefix),
        std::string_view(options.segment_id_prefix), std::string_view(options.dimension_id_prefix)})
        add_budget(raw_strings, value.size(), std::min(policy.max_string_bytes, policy.max_encoded_bytes), "context raw strings");
    // Check the caller-owned tree before copying it into the context. This
    // iterative walk rejects excessive depth before JSON copy/dump recursion.
    enforce_budget(extensions, policy);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    Json context{{"version", 1}, {"replay_version", 1}, {"mode", mode_name(mode)},
                 {"options", write_options(options)}, {"identity_namespace", identity},
                 {"pointer", Json::array({-1.7976931348623157e308, -1.7976931348623157e308})},
                 {"actions", Json::array()}, {"history_position", maximum},
                 {"counters", write_counters({maximum, maximum, maximum, maximum})},
                 {"extensions", extensions}};
    auto usage = measure_wire(context, policy);
    // Float serialization is bounded by 32 bytes per finite double, including
    // sign/exponent. Reserve pointer changes and counter width permanently.
    add_budget(usage.encoded_bytes, 64, policy.max_encoded_bytes, "recovery context");
    return usage;
}

BoundaryAuthoringResourceUsage measure_action_wire(
    const BoundaryAuthoringAction& action, const BoundaryAuthoringResourcePolicy& policy) {
    preflight_action_fields(action, policy);
    auto usage = measure_wire(write_action(action), policy);
    usage.json_depth += 2; // envelope / actions / action
    add_budget(usage.encoded_bytes, 1, policy.max_encoded_bytes, "action delimiter");
    usage.action_count = 1;
    usage.generated_ids = action.generated_ids.size();
    if (usage.generated_ids > policy.max_generated_ids_per_action)
        invalid("action generated identity count exceeds its recovery budget");
    usage.replay_work = 1;
    if (action.chain) {
        if (action.chain->edges.size() > policy.max_chain_edges ||
            action.chain->dimensions.size() > policy.max_dimensions_per_chain)
            invalid("chain resource count exceeds its recovery budget");
        const auto edges = action.chain->edges.size();
        if (!boundary_authoring_recovery_checked_multiply(edges, edges, usage.replay_work) ||
            !boundary_authoring_recovery_checked_add(usage.replay_work, 1, usage.replay_work))
            invalid("closure work overflow");
    }
    return usage;
}
}

BoundaryAuthoringResourceUsage authoring_context_usage(
    const BoundaryAuthoringOptions& options, BoundaryAuthoringMode mode,
    std::string_view identity, const Json& extensions,
    const BoundaryAuthoringResourcePolicy& policy) {
    auto usage = measure_context_wire(options, mode, identity, extensions, policy);
    usage.retained_history_bytes = scaled_bytes(usage.encoded_bytes, 8);
    add_budget(usage.retained_history_bytes, sizeof(BoundaryAuthoringSession),
               policy.max_retained_history_bytes, "context retained bytes");
    usage.materialization_bytes = usage.retained_history_bytes;
    usage.operation_bytes = usage.retained_history_bytes;
    validate_authoring_usage(usage, policy);
    return usage;
}

BoundaryAuthoringResourceUsage authoring_action_usage(
    const BoundaryAuthoringAction& action, const BoundaryAuthoringResourcePolicy& policy) {
    auto usage = measure_action_wire(action, policy);
    // Fixed action storage plus generous dynamic allocation/DOM headroom.
    usage.materialization_bytes = scaled_bytes(usage.encoded_bytes, 8);
    add_budget(usage.materialization_bytes, sizeof(BoundaryAuthoringAction),
               policy.max_materialization_bytes, "action materialization");
    if (action.chain) {
        add_budget(usage.materialization_bytes,
                   scaled_bytes(action.chain->edges.size(), sizeof(ConstructionTopologyEdge)),
                   policy.max_materialization_bytes, "action materialization");
        add_budget(usage.materialization_bytes,
                   scaled_bytes(action.chain->dimensions.size(), sizeof(BoundaryDimension)),
                   policy.max_materialization_bytes, "action materialization");
    }
    usage.retained_history_bytes = usage.materialization_bytes;
    usage.operation_bytes = usage.materialization_bytes;
    usage.cumulative_replay_copy_bytes = usage.materialization_bytes;
    validate_authoring_usage(usage, policy);
    return usage;
}

BoundaryAuthoringResourceUsage measure_authoring_checkpoint_raw(
    const BoundaryAuthoringCheckpoint& checkpoint,
    const BoundaryAuthoringResourcePolicy& policy) {
    validate_limits(policy);
    validate_checkpoint_resource_shape(checkpoint, policy);
    auto usage = measure_context_wire(checkpoint.options, checkpoint.mode,
                                      checkpoint.identity_namespace, checkpoint.extensions, policy);
    usage.action_count = checkpoint.actions.size();
    validate_authoring_usage(usage, policy);
    for (const auto& action : checkpoint.actions) {
        const auto delta = measure_action_wire(action, policy);
        add_budget(usage.encoded_bytes, delta.encoded_bytes, policy.max_encoded_bytes, "wire bytes");
        add_budget(usage.json_values, delta.json_values, policy.max_json_values, "wire values");
        add_budget(usage.string_bytes, delta.string_bytes, policy.max_string_bytes, "wire strings");
        add_budget(usage.generated_ids, delta.generated_ids, policy.max_total_generated_ids, "generated IDs");
        add_budget(usage.replay_work, delta.replay_work, policy.max_replay_work, "replay work");
        usage.json_depth = std::max(usage.json_depth, delta.json_depth);
        validate_authoring_usage(usage, policy);
    }
    return usage;
}

void validate_authoring_checkpoint_raw(const BoundaryAuthoringCheckpoint& checkpoint,
                                      const BoundaryAuthoringResourcePolicy& policy) {
    (void)measure_authoring_checkpoint_raw(checkpoint, policy);
}
} // namespace detail

}  // namespace sketch
