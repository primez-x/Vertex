#pragma once

#include "plan_canvas.hpp"
#include "sketch/site_frame.hpp"

#include <cstddef>
#include <map>
#include <utility>

namespace sketch::desktop {

// Geometry aliases need both the render ID and the exact derived profile key.
// Auxiliary labels/references/grids retain their own render-ID namespace.
// A captured type is checked at every geometry lookup; there is no ID fallback.
struct SiteCanvasPresentationFrames {
    using GeometryKey = std::pair<QString, QString>;
    struct GeometryFrame {
        QString type;
        SitePresentationPlacement placement;
    };
    std::map<GeometryKey, GeometryFrame> geometry;
    std::map<QString, SitePresentationPlacement> auxiliary;

    void clear();
    void insertGeometry(const CanvasEntity& entity, SitePresentationPlacement placement);
    [[nodiscard]] const SitePresentationPlacement* findGeometry(const CanvasEntity& entity) const;
    [[nodiscard]] const SitePresentationPlacement* findGeometry(
        const CanvasEntityPresentationIdentity& identity) const;
    [[nodiscard]] const SitePresentationPlacement& geometryAt(const CanvasEntity& entity) const;
    [[nodiscard]] const SitePresentationPlacement& geometryAt(
        const CanvasEntityPresentationIdentity& identity) const;
};

// Limits for the retained presentation values. Geometry and grid entries are
// bounded before a derived copy is made; callers may lower either cap.
struct SiteCanvasPresentationLimits {
    std::size_t maximum_geometry_entries{1000000};
    std::size_t maximum_grid_lines{1000000};
};

// Pure source-plan <-> presented-plan coordinate helpers. They use the
// captured rigid transform and never change model-space lengths or directions.
[[nodiscard]] Vec2 site_presented_plan_point(
    Vec2 point, const SitePresentationPlacement& placement);
[[nodiscard]] Vec2 site_presented_plan_delta(
    Vec2 delta, const SitePresentationPlacement& placement);
[[nodiscard]] Vec2 site_source_plan_point(
    Vec2 point, const SitePresentationPlacement& placement);
[[nodiscard]] Vec2 site_source_plan_delta(
    Vec2 delta, const SitePresentationPlacement& placement);

// Conservative obstacle bounds in another captured source frame. Every corner
// travels through presentation world once; equal container IDs never imply
// equal coordinates. This also handles already-world embedded profiles.
[[nodiscard]] Bounds2 site_reframed_plan_bounds(
    const Bounds2& bounds, const SitePresentationPlacement& source,
    const SitePresentationPlacement& destination);

// Each function returns a transformed derived value and leaves its input
// untouched. Spatial positions and orientations move once with the frame;
// dimensions, calibration, style and source artwork remain intact.
[[nodiscard]] CanvasEntity site_presented_canvas_entity(
    const CanvasEntity& source, const SitePresentationPlacement& placement,
    const SiteCanvasPresentationLimits& limits = {});
[[nodiscard]] CanvasLabel site_presented_canvas_label(
    const CanvasLabel& source, const SitePresentationPlacement& placement);
[[nodiscard]] CanvasReference site_presented_canvas_reference(
    const CanvasReference& source, const SitePresentationPlacement& placement);
[[nodiscard]] CanvasReferenceGrid site_presented_canvas_reference_grid(
    const CanvasReferenceGrid& source, const SitePresentationPlacement& placement,
    const SiteCanvasPresentationLimits& limits = {});

} // namespace sketch::desktop
