#include "sketch/measurement_linework.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    return left.x == right.x && left.y == right.y;
}

void require_point(Vec2 point, std::string_view label) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        invalid(std::string(label) + " must be finite");
    }
}

void require_identifier(std::string_view value, std::string_view label) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '-' || character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(label) + " must contain 1..128 supported ASCII characters");
    }
}

void require_object(const Json& value, std::string_view label) {
    if (!value.is_object()) invalid(std::string(label) + " must be an object");
}

void require_keys(const Json& value, std::initializer_list<std::string_view> keys,
                  std::string_view label) {
    require_object(value, label);
    for (const auto key : keys) {
        if (!value.contains(std::string(key))) {
            invalid(std::string(label) + " is missing " + std::string(key));
        }
    }
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (std::find(keys.begin(), keys.end(), field.key()) == keys.end()) {
            invalid(std::string(label) + " has unknown field " + field.key());
        }
    }
}

std::uint64_t read_positive_version(const Json& value, std::string_view label) {
    if (value.is_number_unsigned()) {
        const auto version = value.get<std::uint64_t>();
        if (version > 0) return version;
    } else if (value.is_number_integer()) {
        const auto version = value.get<std::int64_t>();
        if (version > 0) return static_cast<std::uint64_t>(version);
    }
    invalid(std::string(label) + " must be a positive integer");
}

std::string read_identifier(const Json& value, std::string_view label) {
    if (!value.is_string()) invalid(std::string(label) + " must be a string");
    auto result = value.get<std::string>();
    require_identifier(result, label);
    return result;
}

Vec2 read_point(const Json& value, std::string_view label) {
    if (!value.is_array() || value.size() != 2 ||
        !value[0].is_number() || !value[1].is_number()) {
        invalid(std::string(label) + " must contain exactly two numeric coordinates");
    }
    const Vec2 result{value[0].get<double>(), value[1].get<double>()};
    require_point(result, label);
    return result;
}

void record_vertex(std::map<std::string, Vec2, std::less<>>& vertices,
                   const std::string& id, Vec2 point) {
    const auto [existing, inserted] = vertices.emplace(id, point);
    if (!inserted && !same_point(existing->second, point)) {
        invalid("measurement linework vertex identity has conflicting coordinates");
    }
}

void apply_transforms(MeasurementLineworkReplay& result,
                      const std::vector<PlanarTransform>& transforms,
                      double tolerance_metres) {
    // Keep an anchor-relative reference independent of world offsets so
    // finite transformations cannot silently discard measured geometry.
    Boundary reference;
    std::vector<double> original_lengths;
    std::map<std::string, Vec2, std::less<>> vertices;
    for (const auto& edge : result.edges) {
        auto relative = edge.segment;
        relative.start = {relative.start.x - result.anchor.x, relative.start.y - result.anchor.y};
        relative.end = {relative.end.x - result.anchor.x, relative.end.y - result.anchor.y};
        reference.push_back(relative);
        original_lengths.push_back(segment_length(edge.segment));
        record_vertex(vertices, edge.start_vertex_id, edge.segment.start);
        record_vertex(vertices, edge.end_vertex_id, edge.segment.end);
    }
    const auto require_precision = [tolerance_metres](double error) {
        if (!std::isfinite(error) || error > tolerance_metres) {
            invalid("transformed measurement linework exceeds coordinate precision tolerance");
        }
    };
    Vec2 translation_error{};
    for (const auto& transform : transforms) {
        const PlanarTransform linear{{}, transform.rotation_radians,
            transform.flip_horizontal, transform.flip_vertical, {}};
        translation_error = transform_point(translation_error, linear);
        const auto previous_anchor = result.anchor;
        // Each stable vertex is evaluated once per operation, including
        // the anchor. All joints and explicit revisits reuse that value.
        for (auto& [id, point] : vertices) {
            (void)id;
            point = transform_point(point, transform);
        }
        result.anchor = vertices.at(result.edges.front().start_vertex_id);
        // Retain low-order affine terms independently of the rounded world
        // anchor. MSVC long double has no additional mantissa precision.
        // Relative edge lengths cannot detect a common displacement caused
        // by a large pivot followed by cancellation in the final offset.
        const auto along_x=transform_point({1,0},linear),along_y=transform_point({0,1},linear);
        const auto affine_error = [&](double a,double b,double pivot,double offset,double actual) {
            double high=0,low=0;
            const auto two_sum=[](double x,double y) {
                const double sum=x+y,virtual_y=sum-x;
                return std::pair{sum,(x-(sum-virtual_y))+(y-virtual_y)};
            };
            const auto add=[&](double term) {
                const auto first=two_sum(high,term);
                const auto tail=two_sum(low,first.second);
                const auto merged=two_sum(first.first,tail.first);
                const auto normalized=two_sum(merged.first,merged.second+tail.second);
                high=normalized.first;low=normalized.second;
                if (!std::isfinite(high) || !std::isfinite(low))
                    invalid("measurement linework affine precision reference overflowed");
            };
            const auto product=[&](double coordinate,double coefficient) {
                const auto value=coordinate*coefficient;
                const auto remainder=std::fma(coordinate,coefficient,-value);
                add(value);add(remainder);
            };
            product(previous_anchor.x,a);product(-transform.pivot.x,a);
            product(previous_anchor.y,b);product(-transform.pivot.y,b);
            add(pivot);add(offset);
            return (actual-high)-low;
        };
        translation_error.x += affine_error(along_x.x,along_y.x,transform.pivot.x,transform.offset.x,result.anchor.x);
        translation_error.y += affine_error(along_x.y,along_y.y,transform.pivot.y,transform.offset.y,result.anchor.y);
        require_precision(std::hypot(translation_error.x, translation_error.y));
        for (std::size_t index = 0; index < result.edges.size(); ++index) {
            auto& edge = result.edges[index];
            reference[index] = transform_segment(reference[index], linear);
            edge.segment = transform_segment(edge.segment, transform);
            edge.segment.start = vertices.at(edge.start_vertex_id);
            edge.segment.end = vertices.at(edge.end_vertex_id);
            const auto check_endpoint = [&](Vec2 world, Vec2 relative) {
                require_precision(std::hypot((world.x - result.anchor.x) - relative.x,
                                             (world.y - result.anchor.y) - relative.y));
            };
            check_endpoint(edge.segment.start, reference[index].start);
            check_endpoint(edge.segment.end, reference[index].end);
            const auto length = segment_length(edge.segment);
            const auto chord = std::hypot(edge.segment.end.x - edge.segment.start.x,
                                          edge.segment.end.y - edge.segment.start.y);
            if (!std::isfinite(length) || !(length > tolerance_metres) ||
                !std::isfinite(chord) || !(chord > tolerance_metres)) {
                invalid("transformed measurement linework segment is degenerate");
            }
            require_precision(std::abs(length - original_lengths[index]));
        }
    }
}

void validate_edit(const MeasurementLineworkEdit& operation, std::string_view stroke_id) {
    const auto& edit = operation.intent;
    if (edit.kind != BoundaryGeometryEditKind::move_vertex &&
        edit.kind != BoundaryGeometryEditKind::resize_segment) {
        invalid("measurement linework supports only vertex moves and segment lengths");
    }
    validate_boundary_geometry_edit(edit);
    if (edit.boundary_id != stroke_id)
        invalid("measurement linework edit stroke identity mismatch");
    if (edit.fixed_endpoint != BoundaryFixedEndpoint::start &&
        edit.fixed_endpoint != BoundaryFixedEndpoint::end)
        invalid("measurement linework edit fixed endpoint is unsupported");
    if (edit.replacement_properties != Json::object())
        invalid("measurement linework edit contains irrelevant replacement properties");
    if (operation.authored_length) {
        if (edit.kind != BoundaryGeometryEditKind::resize_segment)
            invalid("measurement linework vertex edit contains an authored length");
        const auto quantity = normalize_exact_quantity(*operation.authored_length, "measurement linework authored length");
        if (quantity.metres != edit.target_length_metres)
            invalid("measurement linework authored length disagrees with target length");
    }
}

// Error-free addition exposes loss that MSVC's double-width long double
// cannot detect. Every derived coordinate must retain its intent to tolerance.
double checked_sum(double left, double right, double tolerance) {
    const double sum = left + right;
    const double virtual_right = sum - left;
    const double error = (left - (sum - virtual_right)) + (right - virtual_right);
    if (!std::isfinite(sum) || !std::isfinite(error) || std::abs(error) > tolerance)
        invalid("edited measurement linework exceeds coordinate precision tolerance");
    return sum;
}

double checked_product(double left, double right, double tolerance) {
    const auto product = left * right;
    const auto error = std::fma(left, right, -product);
    if (!std::isfinite(product) || !std::isfinite(error) || std::abs(error) > tolerance)
        invalid("edited measurement linework exceeds coordinate precision tolerance");
    return product;
}

bool apply_edit(MeasurementLineworkReplay& replay, const MeasurementLineworkEdit& operation,
                double tolerance) {
    validate_edit(operation, replay.stroke_id);
    const auto& edit = operation.intent;
    std::map<std::string, Vec2, std::less<>> vertices;
    std::vector<Segment> connected_reference;
    std::string connected_fixed_id;
    for (const auto& edge : replay.edges) {
        record_vertex(vertices, edge.start_vertex_id, edge.segment.start);
        record_vertex(vertices, edge.end_vertex_id, edge.segment.end);
    }
    if (edit.kind == BoundaryGeometryEditKind::move_vertex) {
        const auto found = vertices.find(edit.target_id);
        if (found == vertices.end()) invalid("measurement linework edit names unknown vertex");
        if (same_point(found->second, edit.target_position)) return false;
        found->second = edit.target_position;
    } else {
        const auto found = std::find_if(replay.edges.begin(), replay.edges.end(),
            [&](const auto& edge) { return edge.segment_id == edit.target_id; });
        if (found == replay.edges.end()) invalid("measurement linework edit names unknown segment");
        const auto length = segment_length(found->segment);
        if (length == edit.target_length_metres) return false;
        const bool fixed_start = edit.fixed_endpoint == BoundaryFixedEndpoint::start;
        const auto& fixed_id = fixed_start ? found->start_vertex_id : found->end_vertex_id;
        const auto& moving_id = fixed_start ? found->end_vertex_id : found->start_vertex_id;
        const auto fixed = vertices.at(fixed_id);
        const auto moving = vertices.at(moving_id);
        if (edit.move_connected) {
            connected_fixed_id = fixed_id;
            for (const auto& edge : replay.edges) connected_reference.push_back(edge.segment);
        }
        const auto scale = edit.target_length_metres / length;
        if (!std::isfinite(scale) || !(scale > 0))
            invalid("measurement linework resize scale is invalid");
        const auto scale_axis = [&](double anchor, double coordinate) {
            const auto difference = checked_sum(coordinate, -anchor, tolerance);
            return checked_sum(anchor, checked_product(difference, scale, tolerance), tolerance);
        };
        const Vec2 position{scale_axis(fixed.x, moving.x), scale_axis(fixed.y, moving.y)};
        const Vec2 delta{checked_sum(position.x, -moving.x, tolerance),
                         checked_sum(position.y, -moving.y, tolerance)};
        for (auto& [id, point] : vertices) {
            if (id == fixed_id) continue;
            if (id == moving_id) point = position;
            else if (edit.move_connected) {
                const Vec2 translated{checked_sum(point.x, delta.x, tolerance),
                                      checked_sum(point.y, delta.y, tolerance)};
                const auto rounding_error = [](double left, double right, double sum) {
                    const auto virtual_right = sum - left;
                    return (left - (sum - virtual_right)) + (right - virtual_right);
                };
                if (std::hypot(rounding_error(point.x, delta.x, translated.x),
                               rounding_error(point.y, delta.y, translated.y)) > tolerance)
                    invalid("measurement linework connected translation loses coordinate precision");
                point = translated;
            }
        }
    }
    // This is linework validation only: no enclosure, winding or area checks.
    for (std::size_t index = 0; index < replay.edges.size(); ++index) {
        auto& edge = replay.edges[index];
        edge.segment.start = vertices.at(edge.start_vertex_id);
        edge.segment.end = vertices.at(edge.end_vertex_id);
        require_point(edge.segment.start, "edited measurement linework start");
        require_point(edge.segment.end, "edited measurement linework end");
        const auto dx = checked_sum(edge.segment.end.x, -edge.segment.start.x, tolerance);
        const auto dy = checked_sum(edge.segment.end.y, -edge.segment.start.y, tolerance);
        const auto chord = std::hypot(dx, dy);
        const auto length = segment_length(edge.segment);
        if (!std::isfinite(chord) || !(chord > tolerance) ||
            !std::isfinite(length) || !(length > tolerance))
            invalid("edited measurement linework segment is degenerate");
        if (!connected_reference.empty() && edge.start_vertex_id != connected_fixed_id &&
            edge.end_vertex_id != connected_fixed_id) {
            const auto& reference = connected_reference[index];
            const auto original_dx = reference.end.x - reference.start.x;
            const auto original_dy = reference.end.y - reference.start.y;
            if (std::hypot(dx - original_dx, dy - original_dy) > tolerance ||
                std::abs(length - segment_length(reference)) > tolerance)
                invalid("measurement linework connected translation changes retained segment geometry");
        }
        if (edit.kind == BoundaryGeometryEditKind::resize_segment &&
            edge.segment_id == edit.target_id &&
            std::abs(length - edit.target_length_metres) > tolerance)
            invalid("measurement linework resize loses target length precision");
    }
    replay.anchor = vertices.at(replay.edges.front().start_vertex_id);
    return true;
}

Json write_transform(const PlanarTransform& transform) {
    return Json{{"version", 1}, {"pivot", Json::array({transform.pivot.x, transform.pivot.y})},
        {"rotation_radians", transform.rotation_radians},
        {"flip_horizontal", transform.flip_horizontal}, {"flip_vertical", transform.flip_vertical},
        {"offset", Json::array({transform.offset.x, transform.offset.y})}};
}

PlanarTransform read_transform(const Json& value) {
    require_keys(value, {"version", "pivot", "rotation_radians", "flip_horizontal",
                         "flip_vertical", "offset"}, "measurement linework transform");
    if (read_positive_version(value.at("version"), "measurement linework transform version") != 1)
        invalid("unsupported measurement linework transform version");
    if (!value.at("rotation_radians").is_number())
        invalid("measurement linework transform rotation must be numeric");
    if (!value.at("flip_horizontal").is_boolean() || !value.at("flip_vertical").is_boolean())
        invalid("measurement linework transform flips must be boolean");
    const auto rotation = value.at("rotation_radians").get<double>();
    if (!std::isfinite(rotation)) invalid("measurement linework transform rotation must be finite");
    return {read_point(value.at("pivot"), "measurement linework transform pivot"), rotation,
        value.at("flip_horizontal").get<bool>(), value.at("flip_vertical").get<bool>(),
        read_point(value.at("offset"), "measurement linework transform offset")};
}

// Reuse the authoritative strict quantity codec, including exact rational,
// expression and unit consistency, without adding a second quantity dialect.
ConstructionReceipt quantity_codec_receipt(const Quantity& quantity) {
    ConstructionReceipt receipt;
    receipt.segment_id = "quantity";
    receipt.kind = BoundaryConstructionKind::line_heading;
    receipt.distance = quantity;
    receipt.heading = parse_angle("0 deg");
    return receipt;
}

Json write_quantity(const Quantity& quantity) {
    return encode_construction_receipt(quantity_codec_receipt(quantity)).at("distance");
}

Quantity read_quantity(const Json& value) {
    auto receipt = encode_construction_receipt(quantity_codec_receipt(parse_quantity("1 m")));
    receipt["distance"] = value;
    return *decode_construction_receipt(receipt).distance;
}

void promote_to_v3(MeasurementLinework& model) {
    if (model.schema_version == measurement_linework_schema_version_v3) return;
    for (const auto& transform : model.transforms) model.operations.emplace_back(transform);
    model.transforms.clear();
    model.schema_version = measurement_linework_schema_version_v3;
    model.replay_version = measurement_linework_replay_version_v3;
}

}  // namespace

MeasurementLinework edited_measurement_linework(
    const MeasurementLinework& model, const BoundaryGeometryEdit& edit,
    std::optional<Quantity> authored_length) {
    auto replay = replay_measurement_linework(model);
    MeasurementLineworkEdit operation{edit, std::move(authored_length)};
    if (!apply_edit(replay, operation, default_geometry_tolerance_metres)) return model;
    auto result = model;
    promote_to_v3(result);
    result.operations.emplace_back(std::move(operation));
    (void)replay_measurement_linework(result);
    return result;
}

MeasurementLinework transformed_measurement_linework(
    const MeasurementLinework& model, const PlanarTransform& transform) {
    (void)replay_measurement_linework(model);
    require_point(transform.pivot, "measurement linework transform pivot");
    require_point(transform.offset, "measurement linework transform offset");
    if (!std::isfinite(transform.rotation_radians)) {
        invalid("measurement linework transform rotation must be finite");
    }
    if (transform.rotation_radians == 0 && !transform.flip_horizontal &&
        !transform.flip_vertical && transform.offset.x == 0 && transform.offset.y == 0) {
        return model;
    }
    auto result = model;
    if (result.schema_version == measurement_linework_schema_version_v3) {
        result.operations.emplace_back(transform);
    } else {
        result.schema_version = measurement_linework_schema_version_v2;
        result.replay_version = measurement_linework_replay_version_v2;
        result.transforms.push_back(transform);
    }
    (void)replay_measurement_linework(result);
    return result;
}

MeasurementLineworkReplay replay_measurement_linework(const MeasurementLinework& model,
                                                     double tolerance_metres) {
    if (!std::isfinite(tolerance_metres) || !(tolerance_metres > 0)) {
        invalid("measurement linework tolerance must be finite and positive");
    }
    if (model.schema_version == measurement_linework_schema_version_v3 &&
        model.replay_version == measurement_linework_replay_version_v3) {
        if (!model.transforms.empty())
            invalid("measurement linework version three forbids parallel transforms");
        auto local = model;
        local.schema_version = measurement_linework_schema_version_v1;
        local.replay_version = measurement_linework_replay_version_v1;
        local.operations.clear();
        auto result = replay_measurement_linework(local, tolerance_metres);
        result.replay_version = model.replay_version;
        for (std::size_t index = 0; index < model.operations.size();) {
            if (std::holds_alternative<PlanarTransform>(model.operations[index])) {
                std::vector<PlanarTransform> transforms;
                do {
                    transforms.push_back(std::get<PlanarTransform>(model.operations[index++]));
                } while (index < model.operations.size() &&
                    std::holds_alternative<PlanarTransform>(model.operations[index]));
                apply_transforms(result, transforms, tolerance_metres);
            } else {
                if (!apply_edit(result, std::get<MeasurementLineworkEdit>(model.operations[index++]),
                                tolerance_metres))
                    invalid("measurement linework derivation contains a redundant edit");
            }
        }
        return result;
    }
    if (!model.operations.empty())
        invalid("measurement linework operations require schema/replay version three");
    if (model.schema_version == measurement_linework_schema_version_v2 &&
        model.replay_version == measurement_linework_replay_version_v2) {
        auto local = model;
        local.schema_version = measurement_linework_schema_version_v1;
        local.replay_version = measurement_linework_replay_version_v1;
        local.transforms.clear();
        auto result = replay_measurement_linework(local, tolerance_metres);
        result.replay_version = model.replay_version;

        apply_transforms(result, model.transforms, tolerance_metres);
        return result;
    }
    if (!model.transforms.empty()) {
        invalid("measurement linework transforms require schema/replay version two");
    }
    if (model.schema_version != measurement_linework_schema_version) {
        invalid("unsupported measurement linework schema version");
    }
    if (model.replay_version != measurement_linework_replay_version) {
        invalid("unsupported measurement linework replay version");
    }
    require_identifier(model.stroke_id, "measurement linework stroke_id");
    require_point(model.anchor, "measurement linework anchor");
    require_object(model.extensions, "measurement linework extensions");
    if (model.edges.empty()) invalid("measurement linework has no segments");

    MeasurementLineworkReplay result;
    result.replay_version = model.replay_version;
    result.stroke_id = model.stroke_id;
    result.anchor = model.anchor;
    result.closed = model.closed;
    result.edges.reserve(model.edges.size());
    result.receipts.reserve(model.edges.size());
    std::set<std::string, std::less<>> segment_ids;
    std::map<std::string, Vec2, std::less<>> vertices;
    Vec2 expected_start = model.anchor;
    for (std::size_t index = 0; index < model.edges.size(); ++index) {
        const auto& edge = model.edges[index];
        require_identifier(edge.segment_id, "measurement linework segment_id");
        require_identifier(edge.start_vertex_id, "measurement linework start_vertex_id");
        require_identifier(edge.end_vertex_id, "measurement linework end_vertex_id");
        if (edge.segment_id == model.stroke_id || edge.start_vertex_id == model.stroke_id ||
            edge.end_vertex_id == model.stroke_id)
            invalid("measurement linework child identity collides with its stroke identity");
        if (!segment_ids.insert(edge.segment_id).second) {
            invalid("measurement linework contains duplicate segment identity");
        }
        if (edge.receipt.segment_id != edge.segment_id) {
            invalid("measurement linework receipt segment identity mismatch");
        }
        if (index > 0 && edge.start_vertex_id != model.edges[index - 1].end_vertex_id) {
            invalid("measurement linework vertex identities do not join");
        }
        const auto kind = edge.receipt.kind;
        if (kind == BoundaryConstructionKind::line_closure &&
            (!model.closed || index + 1 != model.edges.size())) {
            invalid("measurement linework closure receipt must be the final edge of a closed stroke");
        }
        const auto previous = kind == BoundaryConstructionKind::line_relative_turn && index > 0
            ? std::optional<Segment>{result.edges.back().segment} : std::nullopt;
        const auto closure_anchor = kind == BoundaryConstructionKind::line_closure
            ? std::optional<Vec2>{model.anchor} : std::nullopt;
        const auto rebuilt = replay_construction_receipt(
            edge.receipt, {expected_start, previous, closure_anchor, tolerance_metres});
        if (!(rebuilt.receipt == edge.receipt)) {
            invalid("measurement linework receipt is not canonically normalized");
        }
        record_vertex(vertices, edge.start_vertex_id, rebuilt.segment.start);
        record_vertex(vertices, edge.end_vertex_id, rebuilt.segment.end);
        result.edges.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id,
                                rebuilt.segment});
        result.receipts.push_back(rebuilt.receipt);
        expected_start = rebuilt.segment.end;
    }
    for (const auto& segment_id : segment_ids) {
        if (vertices.contains(segment_id)) {
            invalid("measurement linework segment and vertex identities collide");
        }
    }
    const bool closes_identity = model.edges.back().end_vertex_id == model.edges.front().start_vertex_id;
    if (model.closed) {
        if (!closes_identity || !same_point(expected_start, model.anchor)) {
            invalid("measurement linework is not exactly closed at its anchor identity");
        }
    } else if (closes_identity) {
        invalid("open measurement linework cannot finish with its anchor identity");
    }
    return result;
}

MeasurementLineworkVersion inspect_measurement_linework_model(const Json& model) {
    require_object(model, "measurement linework model");
    if (!model.contains("version")) invalid("measurement linework model is missing version");
    const auto version = read_positive_version(model.at("version"), "measurement linework version");
    if (version != measurement_linework_schema_version_v1 &&
        version != measurement_linework_schema_version_v2 &&
        version != measurement_linework_schema_version_v3) {
        return {MeasurementLineworkFormat::unsupported_version, version, std::nullopt,
                "unsupported measurement linework schema version"};
    }
    if (!model.contains("replay_version")) {
        invalid("measurement linework model is missing replay_version");
    }
    const auto replay_version = read_positive_version(model.at("replay_version"),
                                                      "measurement linework replay_version");
    if ((version == measurement_linework_schema_version_v1 && replay_version != measurement_linework_replay_version_v1) ||
        (version == measurement_linework_schema_version_v2 && replay_version != measurement_linework_replay_version_v2) ||
        (version == measurement_linework_schema_version_v3 && replay_version != measurement_linework_replay_version_v3)) {
        return {MeasurementLineworkFormat::unsupported_replay_version, version, replay_version,
                "unsupported measurement linework replay version"};
    }
    return {version == measurement_linework_schema_version_v1 ? MeasurementLineworkFormat::supported_v1 :
            version == measurement_linework_schema_version_v2 ? MeasurementLineworkFormat::supported_v2 :
                                                               MeasurementLineworkFormat::supported_v3,
            version, replay_version, {}};
}

MeasurementLineworkDecodeResult decode_measurement_linework_model(const Json& encoded) {
    const auto inspected = inspect_measurement_linework_model(encoded);
    if (inspected.format != MeasurementLineworkFormat::supported_v1 &&
        inspected.format != MeasurementLineworkFormat::supported_v2 &&
        inspected.format != MeasurementLineworkFormat::supported_v3) {
        return {std::nullopt, encoded, inspected.version, inspected.replay_version,
                inspected.diagnostic};
    }
    if (inspected.format == MeasurementLineworkFormat::supported_v3) {
        require_keys(encoded, {"version", "replay_version", "stroke_id", "anchor", "closed",
                               "segments", "extensions", "operations"}, "measurement linework model");
    } else if (inspected.format == MeasurementLineworkFormat::supported_v2) {
        require_keys(encoded, {"version", "replay_version", "stroke_id", "anchor", "closed",
                               "segments", "extensions", "transforms"}, "measurement linework model");
    } else {
        require_keys(encoded, {"version", "replay_version", "stroke_id", "anchor", "closed",
                               "segments", "extensions"}, "measurement linework model");
    }
    MeasurementLinework model;
    model.schema_version = static_cast<std::uint32_t>(*inspected.version);
    model.replay_version = static_cast<std::uint32_t>(*inspected.replay_version);
    model.stroke_id = read_identifier(encoded.at("stroke_id"), "measurement linework stroke_id");
    model.anchor = read_point(encoded.at("anchor"), "measurement linework anchor");
    if (!encoded.at("closed").is_boolean()) invalid("measurement linework closed must be boolean");
    model.closed = encoded.at("closed").get<bool>();
    require_object(encoded.at("extensions"), "measurement linework extensions");
    model.extensions = encoded.at("extensions");
    if (inspected.format == MeasurementLineworkFormat::supported_v2) {
        const auto& transforms = encoded.at("transforms");
        if (!transforms.is_array()) invalid("measurement linework transforms must be an array");
        for (const auto& value : transforms) model.transforms.push_back(read_transform(value));
    }
    if (inspected.format == MeasurementLineworkFormat::supported_v3) {
        const auto& operations = encoded.at("operations");
        if (!operations.is_array()) invalid("measurement linework operations must be an array");
        for (const auto& value : operations) {
            require_object(value, "measurement linework operation");
            if (!value.contains("type") || !value.at("type").is_string())
                invalid("measurement linework operation type must be a string");
            const auto type = value.at("type").get<std::string>();
            if (type == "transform") {
                require_keys(value, {"type", "transform"}, "measurement linework transform operation");
                model.operations.emplace_back(read_transform(value.at("transform")));
            } else if (type == "edit") {
                require_keys(value, {"type", "edit", "authored_length"}, "measurement linework edit operation");
                MeasurementLineworkEdit edit{decode_boundary_geometry_edit(value.at("edit")), std::nullopt};
                if (!value.at("authored_length").is_null())
                    edit.authored_length = read_quantity(value.at("authored_length"));
                validate_edit(edit, model.stroke_id);
                model.operations.emplace_back(std::move(edit));
            } else {
                invalid("measurement linework operation type is unsupported");
            }
        }
    }
    const auto& segments = encoded.at("segments");
    if (!segments.is_array()) invalid("measurement linework segments must be an array");
    model.edges.reserve(segments.size());
    for (const auto& segment : segments) {
        require_keys(segment, {"segment_id", "start_vertex_id", "end_vertex_id", "receipt"},
                     "measurement linework segment");
        model.edges.push_back({
            read_identifier(segment.at("segment_id"), "measurement linework segment_id"),
            read_identifier(segment.at("start_vertex_id"), "measurement linework start_vertex_id"),
            read_identifier(segment.at("end_vertex_id"), "measurement linework end_vertex_id"),
            decode_construction_receipt(segment.at("receipt"))});
    }
    (void)replay_measurement_linework(model);
    return {std::move(model), std::nullopt, inspected.version, inspected.replay_version, {}};
}

Json encode_measurement_linework_model(const MeasurementLinework& model) {
    const auto replay = replay_measurement_linework(model);
    Json segments = Json::array();
    for (std::size_t index = 0; index < replay.edges.size(); ++index) {
        const auto& edge = replay.edges[index];
        segments.push_back(Json{{"segment_id", edge.segment_id},
                                {"start_vertex_id", edge.start_vertex_id},
                                {"end_vertex_id", edge.end_vertex_id},
                                {"receipt", encode_construction_receipt(replay.receipts[index])}});
    }
    Json result{{"version", model.schema_version}, {"replay_version", model.replay_version},
                {"stroke_id", model.stroke_id}, {"anchor", Json::array({model.anchor.x, model.anchor.y})},
                {"closed", model.closed}, {"segments", std::move(segments)},
                {"extensions", model.extensions}};
    if (model.schema_version == measurement_linework_schema_version_v2) {
        result["transforms"] = Json::array();
        for (const auto& transform : model.transforms) {
            result["transforms"].push_back(write_transform(transform));
        }
    }
    if (model.schema_version == measurement_linework_schema_version_v3) {
        result["operations"] = Json::array();
        for (const auto& operation : model.operations) {
            if (const auto* transform = std::get_if<PlanarTransform>(&operation)) {
                result["operations"].push_back(Json{{"type", "transform"},
                                                    {"transform", write_transform(*transform)}});
            } else {
                const auto& edit = std::get<MeasurementLineworkEdit>(operation);
                result["operations"].push_back(Json{{"type", "edit"},
                    {"edit", encode_boundary_geometry_edit(edit.intent)},
                    {"authored_length", edit.authored_length ? write_quantity(*edit.authored_length) : Json(nullptr)}});
            }
        }
    }
    return result;
}

}  // namespace sketch
