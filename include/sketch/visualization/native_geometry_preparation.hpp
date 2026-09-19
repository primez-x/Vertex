#pragma once

#include "sketch/document.hpp"
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
struct PreparedNativeSolid {
    std::string content;
    TopoDS_Shape shape;
    Quantity_Color color;
    std::optional<std::string> material_color;
    bool visible{};
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
