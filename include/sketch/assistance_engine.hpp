#pragma once

#include "sketch/assistance_contract.hpp"
#include "sketch/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// A bounded grayscale view supplied by a local reference decoder. The engine
// intentionally accepts pixels, rather than a file path, so it cannot reach
// outside the project or introduce a hidden network dependency.
struct AssistanceRaster {
    std::string reference_id;
    std::string source_text;
    std::size_t width{};
    std::size_t height{};
    std::vector<std::uint8_t> luminance;
};

struct AssistanceEngineOptions {
    double metres_per_pixel{0.01};
    Vec2 origin_metres{};
    double rotation_radians{};
    double image_scale{1.0};
};

struct AssistanceAnchor {
    std::string entity_id;
    std::string label;
    Vec2 position;
};

void validate_assistance_raster(const AssistanceRaster&);

// All results are deterministic proposal envelopes. They are never document
// mutations and remain unverified until a user accepts them through the
// normal command dispatcher.
[[nodiscard]] std::vector<AssistanceProposal> suggest_tracing(
    const AssistanceRaster&, AssistanceEngineOptions options = {});
[[nodiscard]] std::vector<AssistanceProposal> suggest_edge_tracing(
    const AssistanceRaster&, AssistanceEngineOptions options = {});
[[nodiscard]] std::vector<AssistanceProposal> extract_dimensions(
    const AssistanceRaster&, AssistanceEngineOptions options = {},
    std::string target_boundary_id = {});
[[nodiscard]] std::vector<AssistanceProposal> suggest_label_placements(
    std::span<const AssistanceAnchor>);
[[nodiscard]] std::vector<AssistanceProposal> parse_natural_language(std::string_view command);

// IDs supplied to AssistanceSession by a package loader after it has verified
// the corresponding files and licenses. The engine itself does not inspect
// the filesystem.
[[nodiscard]] std::vector<std::string> default_assistance_resource_ids();

}  // namespace sketch
