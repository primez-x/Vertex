#include "sketch/assistance_engine.hpp"

#include "sketch/quantity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <numbers>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_raster_dimension = 8192;
constexpr std::size_t maximum_edge_trace_pixels = 16 * 1024 * 1024;
constexpr std::size_t maximum_dimension_proposals = 64;
constexpr double minimum_metres_per_pixel = 1e-7;
constexpr double maximum_metres_per_pixel = 1e3;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

std::string trim_copy(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool valid_identifier(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.' || character == ':';
    });
}

void finite_positive(double value, const char* message) {
    if (!std::isfinite(value) || !(value > 0.0)) invalid(message);
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct TraceComponent {
    struct Support {
        double score{};
        std::size_t index{};
        bool present{};
    };
    std::size_t pixel_count{};
    std::size_t min_x{};
    std::size_t min_y{};
    std::size_t max_x{};
    std::size_t max_y{};
    std::array<Support, 16> support{};
};

double cross_product(Vec2 origin, Vec2 first, Vec2 second) {
    return (first.x - origin.x) * (second.y - origin.y) -
           (first.y - origin.y) * (second.x - origin.x);
}

std::vector<Vec2> convex_hull(std::vector<Vec2> points) {
    std::sort(points.begin(), points.end(), [](Vec2 first, Vec2 second) {
        if (first.x != second.x) return first.x < second.x;
        return first.y < second.y;
    });
    points.erase(std::unique(points.begin(), points.end(), [](Vec2 first, Vec2 second) {
        return first.x == second.x && first.y == second.y;
    }), points.end());
    if (points.size() <= 2) return {};

    std::vector<Vec2> hull;
    hull.reserve(points.size() * 2);
    for (const auto point : points) {
        while (hull.size() >= 2 &&
               cross_product(hull[hull.size() - 2], hull.back(), point) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(point);
    }
    const auto lower_size = hull.size();
    for (auto iterator = points.rbegin(); iterator != points.rend(); ++iterator) {
        while (hull.size() > lower_size &&
               cross_product(hull[hull.size() - 2], hull.back(), *iterator) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(*iterator);
    }
    if (!hull.empty()) hull.pop_back();
    return hull.size() >= 3 ? hull : std::vector<Vec2>{};
}

std::vector<TraceComponent> trace_components(const AssistanceRaster& raster, int threshold) {
    const auto pixel_count = raster.width * raster.height;
    std::vector<std::uint8_t> visited(pixel_count, 0);
    std::vector<std::uint32_t> queue;
    std::vector<TraceComponent> result;
    const std::array<std::pair<int, int>, 8> neighbors{{
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};
    const std::array<double, 16> direction_angles = [] {
        std::array<double, 16> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            values[index] = 2.0 * std::numbers::pi * static_cast<double>(index) /
                            static_cast<double>(values.size());
        }
        return values;
    }();

    const auto dark = [&](std::size_t index) {
        return static_cast<int>(raster.luminance[index]) <= threshold;
    };
    for (std::size_t y = 0; y < raster.height; ++y) {
        for (std::size_t x = 0; x < raster.width; ++x) {
            const auto first_index = y * raster.width + x;
            if (visited[first_index] != 0 || !dark(first_index)) continue;
            TraceComponent component;
            component.min_x = component.max_x = x;
            component.min_y = component.max_y = y;
            queue.clear();
            queue.push_back(static_cast<std::uint32_t>(first_index));
            visited[first_index] = 1;
            for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
                const auto index = static_cast<std::size_t>(queue[cursor]);
                const auto current_x = index % raster.width;
                const auto current_y = index / raster.width;
                ++component.pixel_count;
                component.min_x = std::min(component.min_x, current_x);
                component.min_y = std::min(component.min_y, current_y);
                component.max_x = std::max(component.max_x, current_x);
                component.max_y = std::max(component.max_y, current_y);
                for (std::size_t direction = 0; direction < direction_angles.size(); ++direction) {
                    const auto angle = direction_angles[direction];
                    const auto cosine = std::cos(angle);
                    const auto sine = std::sin(angle);
                    const auto score = static_cast<double>(current_x) * cosine +
                                       static_cast<double>(current_y) * sine;
                    auto& support = component.support[direction];
                    if (!support.present || score > support.score) {
                        support = {score, index, true};
                    } else if (score == support.score) {
                        // Keep a stable endpoint when a support direction runs
                        // along a straight raster edge. The stored Y value is
                        // the pixel index, so no floating-point tie is involved.
                        if (index > support.index) {
                            support = {score, index, true};
                        }
                    }
                }
                for (const auto [delta_x, delta_y] : neighbors) {
                    const auto next_x = static_cast<std::ptrdiff_t>(current_x) + delta_x;
                    const auto next_y = static_cast<std::ptrdiff_t>(current_y) + delta_y;
                    if (next_x < 0 || next_y < 0 ||
                        next_x >= static_cast<std::ptrdiff_t>(raster.width) ||
                        next_y >= static_cast<std::ptrdiff_t>(raster.height)) {
                        continue;
                    }
                    const auto next_index = static_cast<std::size_t>(next_y) * raster.width +
                                            static_cast<std::size_t>(next_x);
                    if (visited[next_index] == 0 && dark(next_index)) {
                        visited[next_index] = 1;
                        queue.push_back(static_cast<std::uint32_t>(next_index));
                    }
                }
            }
            result.push_back(std::move(component));
        }
    }
    std::sort(result.begin(), result.end(), [](const TraceComponent& first,
                                               const TraceComponent& second) {
        return std::tie(first.min_y, first.min_x, first.max_y, first.max_x) <
               std::tie(second.min_y, second.min_x, second.max_y, second.max_x);
    });
    return result;
}

std::string stable_id(std::string_view prefix, std::string_view material) {
    return std::string(prefix) + "-" + hex_u64(fnv1a(material));
}

std::vector<AssistanceResource> resources() {
    return {
        {"assistance-engine-v1", "assets/assistance/deterministic-engine-v1.json",
         "Vertex deterministic offline assistance engine v1", "private-source-notice", true},
        {"Property-Studio-LICENSE", "LICENSE",
         "Vertex source ownership notice", "private-source-notice", true},
    };
}

AssistanceSource source_for(std::string reference_id, std::string original_text,
                            double x, double y, double width, double height,
                            double confidence) {
    AssistanceSource source{std::move(reference_id), std::move(original_text),
                            x, y, width, height, confidence};
    return source;
}

AssistanceProposal proposal(std::string id, AssistanceKind kind, AssistanceSource source,
                            AssistanceCommandPreview preview) {
    AssistanceProposal result{std::move(id), kind, "property-studio-assisted-v1",
                              resources(), std::move(source), std::move(preview)};
    validate_assistance_proposal(result);
    return result;
}

Json point_json(Vec2 point) {
    return Json::array({point.x, point.y});
}

Json rectangle_points(double width, double height, Vec2 origin = {}) {
    return Json::array({Json::array({origin.x, origin.y}),
                        Json::array({origin.x + width, origin.y}),
                        Json::array({origin.x + width, origin.y + height}),
                        Json::array({origin.x, origin.y + height})});
}

Json transformed_rectangle_points(double width, double height, Vec2 origin,
                                  double rotation_radians) {
    const auto cosine = std::cos(rotation_radians);
    const auto sine = std::sin(rotation_radians);
    const auto transform = [&](double x, double y) {
        return Vec2{origin.x + cosine * x - sine * y,
                    origin.y + sine * x + cosine * y};
    };
    return Json::array({point_json(transform(0.0, 0.0)),
                        point_json(transform(width, 0.0)),
                        point_json(transform(width, height)),
                        point_json(transform(0.0, height))});
}

void validate_options(const AssistanceEngineOptions& options) {
    if (!std::isfinite(options.metres_per_pixel) ||
        options.metres_per_pixel < minimum_metres_per_pixel ||
        options.metres_per_pixel > maximum_metres_per_pixel) {
        invalid("assistance metres-per-pixel calibration is outside the supported range");
    }
    if (!std::isfinite(options.origin_metres.x) || !std::isfinite(options.origin_metres.y) ||
        !std::isfinite(options.rotation_radians) || !std::isfinite(options.image_scale) ||
        !(options.image_scale > 0.0) || options.image_scale > 1e4) {
        invalid("assistance reference transform is invalid");
    }
}

std::string normalized_quantity_expression(std::string expression) {
    expression = trim_copy(expression);
    auto lowered = lowercase(expression);
    auto replace_suffix = [&](std::string_view suffix, std::string_view replacement) {
        if (lowered.ends_with(suffix)) {
            expression.resize(expression.size() - suffix.size());
            expression = trim_copy(expression) + " " + std::string(replacement);
            lowered = lowercase(expression);
            return true;
        }
        return false;
    };
    (void)(replace_suffix("inches", "in") || replace_suffix("inch", "in") ||
           replace_suffix("feet", "ft") || replace_suffix("foot", "ft"));
    return expression;
}

struct DimensionMatch {
    std::size_t offset{};
    std::string text;
};

std::vector<DimensionMatch> find_dimension_tokens(std::string_view text) {
    // This deliberately accepts only explicit units. Unqualified numbers in
    // a plan note are too ambiguous to become measurement suggestions.
    static const std::regex expression(
        R"((?:[0-9]+(?:\.[0-9]+)?|\.[0-9]+)(?:\s+(?:[0-9]+(?:\/[0-9]+)?))?\s*(?:mm|cm|ft|feet|foot|in|inch|inches|m|['"]))",
        std::regex_constants::icase);
    std::vector<DimensionMatch> result;
    const std::string input(text);
    for (auto iterator = std::sregex_iterator(input.begin(), input.end(), expression);
         iterator != std::sregex_iterator{}; ++iterator) {
        const auto& match = *iterator;
        result.push_back({static_cast<std::size_t>(match.position()), match.str()});
        if (result.size() == maximum_dimension_proposals) break;
    }
    return result;
}

double parse_coordinate(std::string_view value) {
    const auto trimmed = trim_copy(value);
    if (trimmed.empty()) invalid("natural-language coordinate is empty");
    std::size_t consumed = 0;
    double result = 0.0;
    try {
        result = std::stod(trimmed, &consumed);
    } catch (const std::exception&) {
        invalid("natural-language coordinate is not numeric");
    }
    if (consumed != trimmed.size() || !std::isfinite(result)) {
        invalid("natural-language coordinate is not finite");
    }
    return result;
}

}  // namespace

void validate_assistance_raster(const AssistanceRaster& raster) {
    if (!valid_identifier(raster.reference_id)) {
        invalid("assistance raster reference ID is empty or invalid");
    }
    if (raster.source_text.size() > 16384 || raster.source_text.find('\0') != std::string::npos) {
        invalid("assistance raster source text is oversized or contains a NUL");
    }
    if (raster.text_runs.size() > 512) invalid("assistance raster has too many text selections");
    std::size_t previous_end = 0;
    for (const auto& run : raster.text_runs) {
        const auto boundary = [&](std::size_t at) {
            return at == raster.source_text.size() ||
                (static_cast<unsigned char>(raster.source_text[at]) & 0xc0) != 0x80;
        };
        if (run.offset < previous_end || run.offset > raster.source_text.size() || !run.length ||
            run.length > raster.source_text.size() - run.offset ||
            !boundary(run.offset) || !boundary(run.offset + run.length) ||
            !std::isfinite(run.x) || !std::isfinite(run.y) || !std::isfinite(run.width) ||
            !std::isfinite(run.height) || run.x < 0 || run.y < 0 || run.width <= 0 ||
            run.height <= 0 || run.x + run.width > 1 || run.y + run.height > 1)
            invalid("assistance raster text selection is invalid");
        previous_end = run.offset + run.length;
    }
    if (raster.width == 0 || raster.height == 0 || raster.width > maximum_raster_dimension ||
        raster.height > maximum_raster_dimension) {
        invalid("assistance raster dimensions are outside the supported range");
    }
    if (raster.width > std::numeric_limits<std::size_t>::max() / raster.height ||
        raster.luminance.size() != raster.width * raster.height) {
        invalid("assistance raster luminance dimensions do not match the pixel buffer");
    }
}

std::vector<AssistanceProposal> suggest_tracing(const AssistanceRaster& raster,
                                                 AssistanceEngineOptions options) {
    validate_assistance_raster(raster);
    validate_options(options);

    const auto [minimum, maximum] = std::minmax_element(raster.luminance.begin(),
                                                         raster.luminance.end());
    const auto range = static_cast<int>(*maximum) - static_cast<int>(*minimum);
    if (range < 16) return {};
    const auto threshold = static_cast<int>(*minimum) + std::max(8, range * 35 / 100);

    std::size_t dark_count = 0;
    std::size_t min_x = raster.width;
    std::size_t min_y = raster.height;
    std::size_t max_x = 0;
    std::size_t max_y = 0;
    for (std::size_t y = 0; y < raster.height; ++y) {
        for (std::size_t x = 0; x < raster.width; ++x) {
            if (static_cast<int>(raster.luminance[y * raster.width + x]) > threshold) continue;
            ++dark_count;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    if (dark_count < 4 || min_x >= max_x || min_y >= max_y ||
        dark_count * 100 > raster.luminance.size() * 92) {
        return {};
    }

    const auto width_pixels = static_cast<double>(max_x - min_x);
    const auto height_pixels = static_cast<double>(max_y - min_y);
    const auto width_metres = width_pixels * options.metres_per_pixel * options.image_scale;
    const auto height_metres = height_pixels * options.metres_per_pixel * options.image_scale;
    finite_positive(width_metres, "assistance trace width is not representable");
    finite_positive(height_metres, "assistance trace height is not representable");

    const auto id = stable_id("assist-trace", raster.reference_id + ":" +
                                             std::to_string(min_x) + ":" +
                                             std::to_string(min_y) + ":" +
                                             std::to_string(max_x) + ":" +
                                             std::to_string(max_y) + ":" +
                                             std::to_string(options.metres_per_pixel));
    const auto confidence = std::clamp(0.55 + static_cast<double>(range) / 255.0 * 0.35,
                                       0.55, 0.95);
    const auto normalized_x = static_cast<double>(min_x) /
                              static_cast<double>(raster.width);
    const auto normalized_y = static_cast<double>(min_y) /
                              static_cast<double>(raster.height);
    const auto normalized_width = static_cast<double>(max_x - min_x + 1) /
                                  static_cast<double>(raster.width);
    const auto normalized_height = static_cast<double>(max_y - min_y + 1) /
                                   static_cast<double>(raster.height);
    return {proposal(
        id, AssistanceKind::tracing,
        source_for(raster.reference_id, {}, normalized_x, normalized_y,
                   normalized_width, normalized_height, confidence),
        {"add_boundary", {id},
         {{"boundary_id", id},
          {"points", transformed_rectangle_points(
              width_metres, height_metres,
              {options.origin_metres.x + std::cos(options.rotation_radians) *
                       (static_cast<double>(min_x) * options.metres_per_pixel * options.image_scale) -
                       std::sin(options.rotation_radians) *
                       (static_cast<double>(min_y) * options.metres_per_pixel * options.image_scale),
               options.origin_metres.y + std::sin(options.rotation_radians) *
                       (static_cast<double>(min_x) * options.metres_per_pixel * options.image_scale) +
                       std::cos(options.rotation_radians) *
                       (static_cast<double>(min_y) * options.metres_per_pixel * options.image_scale)},
              options.rotation_radians)},
          {"closed", true}, {"classification", "measurement"},
          {"source", "deterministic-raster-bbox-v1"},
          {"source_pixel_bounds", Json::array({min_x, min_y, max_x, max_y})},
          {"metres_per_pixel", options.metres_per_pixel},
          {"image_scale", options.image_scale},
          {"rotation_radians", options.rotation_radians},
           {"origin_metres", point_json(options.origin_metres)}}})};
}

std::vector<AssistanceProposal> suggest_edge_tracing(const AssistanceRaster& raster,
                                                      AssistanceEngineOptions options) {
    validate_assistance_raster(raster);
    validate_options(options);
    if (raster.width * raster.height > maximum_edge_trace_pixels) return {};

    const auto [minimum, maximum] = std::minmax_element(raster.luminance.begin(),
                                                         raster.luminance.end());
    const auto range = static_cast<int>(*maximum) - static_cast<int>(*minimum);
    if (range < 16) return {};
    const auto threshold = static_cast<int>(*minimum) + std::max(8, range * 35 / 100);
    std::size_t dark_count = 0;
    for (const auto value : raster.luminance) {
        if (static_cast<int>(value) <= threshold) ++dark_count;
    }
    if (dark_count < 4 || dark_count * 100 > raster.luminance.size() * 92) return {};
    const auto components = trace_components(raster, threshold);
    std::vector<AssistanceProposal> result;
    result.reserve(std::min<std::size_t>(components.size(), 64));
    const auto cosine = std::cos(options.rotation_radians);
    const auto sine = std::sin(options.rotation_radians);
    const auto transform = [&](Vec2 point, const TraceComponent& component) {
        const auto scale = options.metres_per_pixel * options.image_scale;
        const auto local_x = (point.x - static_cast<double>(component.min_x)) * scale;
        const auto local_y = (point.y - static_cast<double>(component.min_y)) * scale;
        const auto base_x = static_cast<double>(component.min_x) * scale;
        const auto base_y = static_cast<double>(component.min_y) * scale;
        return Vec2{
            options.origin_metres.x + cosine * (base_x + local_x) - sine * (base_y + local_y),
            options.origin_metres.y + sine * (base_x + local_x) + cosine * (base_y + local_y)};
    };

    for (std::size_t component_index = 0; component_index < components.size() &&
                                          result.size() < 64; ++component_index) {
        const auto& component = components[component_index];
        if (component.pixel_count < 4 || component.min_x >= component.max_x ||
            component.min_y >= component.max_y) {
            continue;
        }
        // Tiny connected components are usually text specks or anti-aliasing
        // noise. Keep the filter deterministic and proportional to the image.
        const auto box_width = component.max_x - component.min_x + 1;
        const auto box_height = component.max_y - component.min_y + 1;
        if (box_width * box_height < 12) continue;

        std::vector<Vec2> support_points;
        support_points.reserve(component.support.size());
        for (const auto& support : component.support) {
            if (!support.present) continue;
            const auto pixel_x = support.index % raster.width;
            const auto pixel_y = support.index / raster.width;
            support_points.push_back({static_cast<double>(pixel_x),
                                      static_cast<double>(pixel_y)});
        }
        const auto hull = convex_hull(std::move(support_points));
        if (hull.size() < 3 || std::abs(cross_product(hull[0], hull[1], hull[2])) <= 0.0) {
            continue;
        }

        Json points = Json::array();
        for (const auto point : hull) points.push_back(point_json(transform(point, component)));
        const auto scale = options.metres_per_pixel * options.image_scale;
        const auto width_metres = static_cast<double>(component.max_x - component.min_x) * scale;
        const auto height_metres = static_cast<double>(component.max_y - component.min_y) * scale;
        finite_positive(width_metres, "assistance edge trace width is not representable");
        finite_positive(height_metres, "assistance edge trace height is not representable");
        std::string material = raster.reference_id + ":" + std::to_string(component_index) + ":";
        material += std::to_string(component.min_x) + ":" + std::to_string(component.min_y) + ":" +
                    std::to_string(component.max_x) + ":" + std::to_string(component.max_y) + ":" +
                    std::to_string(options.metres_per_pixel) + ":" +
                    std::to_string(options.image_scale) + ":" +
                    std::to_string(options.rotation_radians);
        const auto id = stable_id("assist-edge-trace", material);
        const auto normalized_x = static_cast<double>(component.min_x) /
                                  static_cast<double>(raster.width);
        const auto normalized_y = static_cast<double>(component.min_y) /
                                  static_cast<double>(raster.height);
        const auto normalized_width = static_cast<double>(box_width) /
                                      static_cast<double>(raster.width);
        const auto normalized_height = static_cast<double>(box_height) /
                                       static_cast<double>(raster.height);
        const auto confidence = std::clamp(0.60 + static_cast<double>(range) / 255.0 * 0.30,
                                           0.60, 0.94);
        result.push_back(proposal(
            id, AssistanceKind::edge_tracing,
            source_for(raster.reference_id, {}, normalized_x, normalized_y,
                       normalized_width, normalized_height, confidence),
            {"add_boundary", {id},
             {{"boundary_id", id}, {"points", std::move(points)}, {"closed", true},
              {"classification", "measurement"}, {"source", "deterministic-raster-contour-v1"},
              {"trace_mode", "connected-components-v1"},
              {"component_index", component_index},
              {"component_pixels", component.pixel_count},
              {"source_pixel_bounds", Json::array({component.min_x, component.min_y,
                                                     component.max_x, component.max_y})},
              {"metres_per_pixel", options.metres_per_pixel},
              {"image_scale", options.image_scale},
              {"rotation_radians", options.rotation_radians},
              {"origin_metres", point_json(options.origin_metres)}}}));
    }
    return result;
}

std::vector<AssistanceProposal> extract_dimensions(const AssistanceRaster& raster,
                                                    AssistanceEngineOptions options,
                                                    std::string target_boundary_id) {
    validate_assistance_raster(raster);
    validate_options(options);
    if (!target_boundary_id.empty() && !valid_identifier(target_boundary_id)) {
        invalid("dimension target boundary ID is invalid");
    }
    if (raster.source_text.empty()) return {};

    std::vector<AssistanceProposal> result;
    for (const auto& match : find_dimension_tokens(raster.source_text)) {
        const auto run = std::find_if(raster.text_runs.begin(), raster.text_runs.end(),
            [&](const AssistanceTextRun& candidate) {
                return match.offset >= candidate.offset &&
                    match.offset + match.text.size() <= candidate.offset + candidate.length;
            });
        if (run == raster.text_runs.end()) continue;
        const auto expression = normalized_quantity_expression(match.text);
        Quantity quantity;
        try {
            quantity = parse_quantity(expression, Unit::metre);
        } catch (const std::exception&) {
            continue;
        }
        if (!(quantity.metres > 0.0) || !std::isfinite(quantity.metres)) continue;
        const auto material = raster.reference_id + ":" + std::to_string(match.offset) + ":" +
                              match.text + ":" + target_boundary_id;
        const auto id = stable_id("assist-dimension", material);
        std::vector<std::string> affected{id};
        if (!target_boundary_id.empty()) affected.push_back(target_boundary_id);
        Json args{{"length_expression", expression}, {"length_metres", quantity.metres},
                  {"source_text", match.text}, {"source_offset", match.offset},
                  {"target_boundary_id", target_boundary_id}};
        if (!target_boundary_id.empty()) args["target_segment_id"] = "";
        result.push_back(proposal(
            id, AssistanceKind::dimension_extraction,
            source_for(raster.reference_id, match.text, run->x, run->y, run->width, run->height,
                       0.86),
            {"add_dimension_suggestion", std::move(affected), std::move(args)}));
    }
    return result;
}

std::vector<AssistanceProposal> suggest_label_placements(
    std::span<const AssistanceAnchor> anchors) {
    std::set<std::string, std::less<>> seen;
    std::vector<AssistanceProposal> result;
    result.reserve(anchors.size());
    for (const auto& anchor : anchors) {
        if (!valid_identifier(anchor.entity_id)) invalid("label anchor ID is invalid");
        if (!seen.insert(anchor.entity_id).second) invalid("label anchors contain a duplicate ID");
        if (!std::isfinite(anchor.position.x) || !std::isfinite(anchor.position.y)) {
            invalid("label anchor position is not finite");
        }
        if (anchor.label.empty() || anchor.label.size() > 4096 ||
            anchor.label.find_first_of("\r\n\t\0") != std::string::npos) {
            invalid("label anchor text is empty or unsafe");
        }
        const auto id = stable_id("assist-label", anchor.entity_id + ":" + anchor.label);
        result.push_back(proposal(
            id, AssistanceKind::label_placement,
            source_for(anchor.entity_id, anchor.label, 0.0, 0.0, 1.0, 1.0, 0.78),
            {"add_label", {id, anchor.entity_id},
             {{"annotation_id", id}, {"template_id", "note"}, {"content", anchor.label},
              {"position", point_json(anchor.position)},
              {"anchor_entity_id", anchor.entity_id}}}));
    }
    return result;
}

std::vector<AssistanceProposal> parse_natural_language(std::string_view command) {
    if (command.size() > 4096 || command.find_first_of("\r\n\t\0") != std::string_view::npos) {
        invalid("natural-language command is empty, oversized, or contains a control character");
    }
    const auto original = trim_copy(command);
    if (original.empty()) invalid("natural-language command is empty");
    const auto lowered = lowercase(original);

    static const std::regex label_pattern(
        R"(^label\s+(.+?)\s+at\s+([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+))\s*,\s*([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+))$)",
        std::regex_constants::icase);
    std::smatch label_match;
    if (std::regex_match(original, label_match, label_pattern)) {
        const auto content = trim_copy(label_match[1].str());
        if (content.empty() || content.size() > 4096 ||
            content.find_first_of("\r\n\t\0") != std::string::npos) {
            invalid("natural-language label content is empty or unsafe");
        }
        const Vec2 position{parse_coordinate(label_match[2].str()),
                            parse_coordinate(label_match[3].str())};
        const auto id = stable_id("assist-language-label", original);
        return {proposal(
            id, AssistanceKind::natural_language,
            source_for("natural-language", original, 0.0, 0.0, 1.0, 1.0, 0.93),
            {"add_label", {id},
             {{"annotation_id", id}, {"template_id", "note"}, {"content", content},
              {"position", point_json(position)}}})};
    }

    if (lowered == "set workspace measurement" || lowered == "set workspace architectural") {
        const auto workspace = lowered.ends_with("measurement") ? "measurement" : "architectural";
        const auto id = stable_id("assist-language-workspace", original);
        return {proposal(
            id, AssistanceKind::natural_language,
            source_for("natural-language", original, 0.0, 0.0, 1.0, 1.0, 0.99),
            {"set_workspace", {id}, {{"workspace", workspace}}})};
    }

    static const std::regex rectangle_pattern(R"(^draw\s+rectangle\s+(.+)\s+x\s+(.+)$)",
                                               std::regex_constants::icase);
    std::smatch rectangle_match;
    if (std::regex_match(original, rectangle_match, rectangle_pattern)) {
        Quantity width;
        Quantity height;
        try {
            width = parse_quantity(rectangle_match[1].str(), Unit::metre);
            height = parse_quantity(rectangle_match[2].str(), Unit::metre);
        } catch (const std::exception&) {
            invalid("natural-language rectangle dimensions are invalid");
        }
        finite_positive(width.metres, "natural-language rectangle width is invalid");
        finite_positive(height.metres, "natural-language rectangle height is invalid");
        const auto id = stable_id("assist-language-rectangle", original);
        return {proposal(
            id, AssistanceKind::natural_language,
            source_for("natural-language", original, 0.0, 0.0, 1.0, 1.0, 0.94),
            {"add_boundary", {id},
             {{"boundary_id", id}, {"points", rectangle_points(width.metres, height.metres)},
              {"closed", true}, {"classification", "measurement"},
              {"width_metres", width.metres}, {"height_metres", height.metres}}})};
    }

    invalid("natural-language command is not supported by the offline grammar");
}

std::vector<std::string> default_assistance_resource_ids() {
    return {"assistance-engine-v1", "Property-Studio-LICENSE"};
}

}  // namespace sketch
