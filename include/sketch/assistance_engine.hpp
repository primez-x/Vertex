#pragma once

#include "sketch/assistance_contract.hpp"
#include "sketch/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

struct AssistanceTextRun {
    std::size_t offset{}; // UTF-8 byte range in source_text.
    std::size_t length{};
    double x{}, y{}, width{}, height{}; // Normalized source page selection.
    std::optional<double> confidence; // Recognizer confidence in [0, 1]; absent for embedded text.
};

// A bounded grayscale view supplied by a local reference decoder. The engine
// intentionally accepts pixels, rather than a file path, so it cannot reach
// outside the project or introduce a hidden network dependency.
struct AssistanceRaster {
    std::string reference_id;
    std::string source_text;
    std::size_t width{};
    std::size_t height{};
    std::vector<std::uint8_t> luminance;
    std::vector<AssistanceTextRun> text_runs;
    std::vector<AssistanceResource> text_resources;
    std::string text_producer;
};

struct AssistanceEngineOptions {
    double metres_per_pixel{0.01};
    Vec2 origin_metres{};
    double rotation_radians{};
    double image_scale{1.0};
    // The desktop reference origin is its centre. Public engine defaults
    // retain the historical top-left origin and positive source Y direction.
    bool centered_source{false};
    bool flip_horizontal{false};
    bool flip_vertical{false};
};

struct AssistanceAnchor {
    std::string entity_id;
    std::string label;
    Vec2 position;
};

void validate_assistance_raster(const AssistanceRaster&);

// Map RAW unmirrored image coordinates (including pixel-edge corners) to
// model coordinates, matching CanvasReference's centred drawImage rectangle.
// Source selection/OCR bounds remain raw; only geometry is transformed.
[[nodiscard]] Vec2 assistance_source_point_to_model(
    Vec2 source_pixel, std::size_t source_width, std::size_t source_height,
    AssistanceEngineOptions options = {});

// All results are deterministic proposal envelopes. They are never document
// mutations and remain unverified until a user accepts them through the
// normal command dispatcher.
[[nodiscard]] std::vector<AssistanceProposal> suggest_tracing(
    const AssistanceRaster&, AssistanceEngineOptions options = {});
[[nodiscard]] std::vector<AssistanceProposal> suggest_edge_tracing(
    const AssistanceRaster&, AssistanceEngineOptions options = {});
[[nodiscard]] std::vector<AssistanceProposal> extract_dimensions(
    const AssistanceRaster&, AssistanceEngineOptions options = {},
    std::string target_boundary_id = {}, std::string target_segment_id = {});
[[nodiscard]] std::vector<AssistanceProposal> suggest_label_placements(
    std::span<const AssistanceAnchor>);
[[nodiscard]] std::vector<AssistanceProposal> parse_natural_language(std::string_view command);

// IDs supplied to AssistanceSession by a package loader after it has verified
// the corresponding files and licenses. The engine itself does not inspect
// the filesystem.
[[nodiscard]] std::vector<std::string> default_assistance_resource_ids();

}  // namespace sketch
