#pragma once

#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/building_view_projection.hpp"
#include "sketch/sheet_view_model.hpp"
#include "plan_canvas.hpp"

namespace sketch::desktop {

// Shared saved-view input. This contains presentation values only; the actual
// captured DocumentSnapshot remains the sole source/history authority.
struct ArchitecturalViewContext {
    BuildingViewFrame frame;
    BuildingViewDepth depth;
    ViewPresentation presentation;
    std::vector<std::string> object_ids;
    std::string view_id;
    std::vector<SectionOverlay> overlays;
    std::optional<BuildingViewCrop> crop;
    bool restrict_to_objects{false};
};

struct PhaseWallCanvasProjection {
    std::vector<CanvasEntity> entities;
    std::vector<CanvasLabel> labels;
};

// Shared with settled canvas rendering: the default frame/default depth is
// analytical even though its conventional far depth is finite.
[[nodiscard]] bool analytical_canvas_plan_context(
    BuildingViewKind kind, const ArchitecturalViewContext& context) noexcept;

// Detached overrides, matched to the source scene by original owner/child ID
// and captured presentation key. Physical stage IDs are never changed. Empty
// overrides remove captured presentations which leave the saved depth/crop.
// Unsupported affected geometry throws std::invalid_argument; no stale source
// presentation is silently substituted. Unreviewed physical room facts and
// their room labels deliberately remain outside this physical-stage preview.
[[nodiscard]] PhaseWallCanvasProjection project_phase_wall_canvas(
    const DocumentSnapshot& source,
    const PhaseWallReplacementAuthoringPreview& physical,
    const std::vector<CanvasEntity>& retained,
    const std::vector<CanvasEntity>& eligible,
    const std::vector<CanvasLabel>& labels,
    bool metric_units,
    const std::optional<ArchitecturalViewContext>& view_context = std::nullopt);

} // namespace sketch::desktop
