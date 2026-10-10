#pragma once

#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/building_view_projection.hpp"
#include "sketch/sheet_view_model.hpp"
#include "plan_canvas.hpp"

#include <utility>
#include <tuple>

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
    // View IDs are local to a sheet-view model. Retain the captured owner
    // instead of resolving the same spelling across unrelated models.
    std::string sheet_view_entity_id;
};

struct PhaseWallCanvasProjection {
    std::vector<CanvasEntity> entities;
    std::vector<CanvasLabel> labels;
};

// Opt-in coordinated physical projection. These are the actual coordinator's
// entity/owned-child mappings, not persisted render aliases. Hosted instances
// have a catalog-qualified namespace and therefore retain a separate mapping.
struct PhaseWallCanvasCoordinatedPhysicalInput {
    PhaseWallReplacementIdentityMap original_to_proposed;
    std::map<std::pair<std::string, std::string>, std::string>
        original_to_hosted_instance_proposed;
    // A shared source catalog can produce separate private catalogs for
    // different family leaves. Destination is (catalog, instance); the older
    // single-catalog input above remains supported for existing callers.
    std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>>
        original_to_hosted_destination_proposed;
    std::map<std::tuple<std::string, std::string, std::string>, std::string>
        original_to_overlay_proposed;
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
// Supplying coordinated input additionally projects actual changed roof/slab,
// stair/railing and column/beam owners, their joins and hosted catalog
// presentations from edited_entities. Hosted railings use the complete raw
// stage's stair placement, including when their own envelope is unchanged.
// Retained/eligible presentations must come from the captured source visibility
// and object roster. Their original IDs/keys remain gesture rendering aliases.
// Corner-window owners use the actual final two-host manufactured assembly;
// their managed cuts remain wall-roster data without independent hit targets.
// Both leg controls retain source snapshot revisions and local host baselines,
// and require complete captured source controls plus complete final depth/crop.
// As with wall bodies, the caller applies captured Site placement exactly once.
[[nodiscard]] PhaseWallCanvasProjection project_phase_wall_canvas(
    const DocumentSnapshot& source,
    const PhaseWallReplacementAuthoringPreview& physical,
    const std::vector<CanvasEntity>& retained,
    const std::vector<CanvasEntity>& eligible,
    const std::vector<CanvasLabel>& labels,
    bool metric_units,
    const std::optional<ArchitecturalViewContext>& view_context = std::nullopt,
    const std::optional<PhaseWallCanvasCoordinatedPhysicalInput>& coordinated = std::nullopt);

} // namespace sketch::desktop
