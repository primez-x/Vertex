#pragma once

#include "sketch/document.hpp"
#include "sketch/sheet_view_model.hpp"

namespace sketch {

struct ResolvedSectionDimension {
    // Actual source silhouette support points or selected corner-leg jambs
    // for bound dimensions, explicit endpoints for detached dimensions.
    // Extension lines start here.
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

// The map must be the caller's complete actual captured/edited source cohort;
// no fabricated snapshot or cached geometry grants corner admission.
[[nodiscard]] SectionDimensionResolution resolve_corner_window_leg_view_dimension(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const CoordinatedView& view, const SectionDimensionBinding& binding);

// Pure derived measurement: resolves complete authoritative source geometry
// and level placement in the owning plan, elevation or section frame, with no
// crop, depth, visibility, detail or cut-plane filtering. Does not modify source
// quantities.
// Binding minimum/maximum handles remain semantic across source edits.
// Aligned owner-plus-leg bindings resolve the actual corner cut endpoint and
// outer jamb at the common resolved sill, with complete two-host/two-cut native
// assembly admission. Measurement is their projected view-plane distance and
// line_offset_m follows its left normal. A collapsed projection is unresolved.
// The supplied view is the projection frame (section displacement is already
// applied by the caller), in the same source coordinates as whole-owner extents.
// Independent analytical rooms are supported only in horizontal plan frames;
// other frames require explicit, valid physical volume geometry. The legacy
// section name is retained for existing callers and persisted overlay types.
[[nodiscard]] SectionDimensionResolution resolve_section_dimension(
    const DocumentSnapshot& source, const CoordinatedView& view,
    const SectionOverlay& overlay);

} // namespace sketch
