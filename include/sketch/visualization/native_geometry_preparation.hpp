#pragma once

#include "sketch/document.hpp"
#include "sketch/site_frame.hpp"
#include <Quantity_Color.hxx>
#include <TopoDS_Shape.hxx>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sketch::visualization {
using NativeGeometryVisibleIds = std::set<std::string, std::less<>>;

// Fresh, worker-owned topology. No AIS handles or live-view shapes are shared
// with preparation. Ownership transfers to the owner thread on completion;
// with triangulation already prepared for presentation.
struct PreparedNativeRoofOpeningTarget {
    std::string roof_id;
    std::string opening_id;
    bool operator==(const PreparedNativeRoofOpeningTarget&) const = default;
};
struct PreparedNativeMaterialRegion {
    std::string source_id;
    TopoDS_Shape shape; // empty for a fully occluded member; never mesh/display it
    Quantity_Color color;
    std::optional<std::string> material_color;
    std::optional<std::string> catalog_id;
    std::optional<std::string> material_id;
    double gross_volume{};
    double net_volume{};
    // Versioned assembly profile key; empty for other native material regions.
    std::string presentation_key;
    // Explicit semantic provenance for an actual skylight fill. source_id is
    // also used by material/assembly regions and grants no child-pick authority.
    std::optional<PreparedNativeRoofOpeningTarget> roof_opening;
};
struct PreparedNativeSolid {
    std::string content;
    TopoDS_Shape shape;
    Quantity_Color color;
    std::optional<std::string> material_color;
    bool visible{};
    // Ordered regions, freshly meshed with this candidate. Joined roof regions
    // are nonoverlapping; independent assembly profiles retain authored overlap.
    // Keep shape as fused truth; construct colored presentation from these
    // actual shapes instead of matching their faces to a cached fusion.
    std::vector<PreparedNativeMaterialRegion> material_regions;
    // Includes source bindings and resolved colors. Geometry content remains
    // stable on catalog color edits; publication must compare this separately.
    std::string appearance_content;
    // Authored source -> world, applied once to shape and every material region.
    // Selection/manipulator presentations therefore use world geometry. Retain
    // the captured placement for command-edge conjugation; it does not replace
    // the controller's complete document-snapshot authority guard. Legacy
    // catalog host copies have no enrollment and remain world by default.
    std::optional<SitePresentationPlacement> presentation_placement;
};
struct PreparedNativeGeometry {
    Revision revision{};
    std::optional<NativeGeometryVisibleIds> visible_ids;
    std::map<std::string, PreparedNativeSolid, std::less<>> solids;
    std::vector<std::string> errors;
    std::vector<std::string> pending;
};

// Cancellation discards the entire candidate. Polls bracket objects and join
// input members; the existing wall/roof join builder (including its internal
// boolean sequence) is not interruptible by this API.
// progress is called on the worker thread after each successfully built solid.
[[nodiscard]] std::optional<PreparedNativeGeometry> prepare_native_geometry(
    const DocumentSnapshot& snapshot,
    std::optional<NativeGeometryVisibleIds> visible_ids,
    const std::function<bool()>& cancelled = {},
    const std::function<void(std::size_t)>& progress = {});

// request/take_completed/shutdown belong to one owner thread. New requests
// supersede even equal document revisions (document switch or visibility edit).
// At most one running and one waiting snapshot are retained. The worker owns
// detached snapshots and accesses only its implementation state, never a widget.
class NativeGeometryRegenerator final {
public:
    using Preparation = std::function<std::optional<PreparedNativeGeometry>(
        const DocumentSnapshot&, std::optional<NativeGeometryVisibleIds>,
        const std::function<bool()>&)>;
    NativeGeometryRegenerator();
    explicit NativeGeometryRegenerator(Preparation preparation);
    ~NativeGeometryRegenerator();
    NativeGeometryRegenerator(const NativeGeometryRegenerator&) = delete;
    NativeGeometryRegenerator& operator=(const NativeGeometryRegenerator&) = delete;
    void request(DocumentSnapshot snapshot,
                 std::optional<NativeGeometryVisibleIds> visible_ids);
    [[nodiscard]] bool is_pending() const noexcept;
    // Includes the running request and the single replaceable waiting snapshot.
    [[nodiscard]] std::size_t retained_snapshot_count() const;
    // Re-throws a worker failure on the owner thread, leaving its scene intact.
    [[nodiscard]] std::optional<PreparedNativeGeometry> take_completed();
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sketch::visualization
