#pragma once

#include "sketch/document.hpp"
#include "sketch/sheet_view_model.hpp"

namespace sketch {

struct ResolvedSectionDimension {
    // Actual silhouette support points for bound dimensions, explicit
    // endpoints for detached dimensions. Extension lines start here.
    std::array<double, 2> start_m;
    std::array<double, 2> end_m;
    // Dimension line endpoints; placement has no effect on measured_metres.
    std::array<double, 2> line_start_m;
    std::array<double, 2> line_end_m;
    double measured_metres{};
    bool associative{};
};

struct SectionDimensionResolution {
    std::optional<ResolvedSectionDimension> dimension;
    // Nonempty when unresolved. A present binding never falls back to its
    // old detached endpoints, even if the source is missing or unsupported.
    std::string diagnostic;
};

// Pure derived measurement: resolves complete authoritative source geometry
// and level placement in the owning section frame, with no crop, depth,
// visibility, detail or cut-plane filtering. Does not modify source quantities.
// Binding minimum/maximum handles remain semantic across source edits.
[[nodiscard]] SectionDimensionResolution resolve_section_dimension(
    const DocumentSnapshot& source, const CoordinatedView& view,
    const SectionOverlay& overlay);

} // namespace sketch
