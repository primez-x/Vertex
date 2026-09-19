#pragma once

#include "sketch/document.hpp"
#include <cstddef>

namespace sketch {
enum class PerformanceWorkloadKind { drawing, architecture, sheets };
struct PerformanceWorkloadOptions {
    PerformanceWorkloadKind kind{PerformanceWorkloadKind::drawing};
    std::size_t count{50000};
    std::size_t asset_bytes_per_sheet{12500000};
};
// Fresh document identity on every invocation, deterministic semantic payloads.
// Bounds: 50k drawing entities, 10k objects, or 20 sheets / 250 MB asset bytes.
// Each sheet explicitly selects one visible reference entity in its viewport;
// the measured image asset is also the renderer's input, with no duplicate copy.
[[nodiscard]] Document make_performance_workload(const PerformanceWorkloadOptions& options);
// Counts are decoded from actual content. Mesh triangles are counted from OCCT
// faces at the reported fixed tessellation settings, never estimated per object.
// Sheet placements are resolved through persisted viewport/view/object/asset
// relationships. placed_asset_bytes counts unique assets, not placement copies.
// Semantic hash excludes document identity/save markers; archive verification
// must additionally compare document_authoring_source_digest_v1 across reopen.
[[nodiscard]] nlohmann::json inspect_performance_workload(const DocumentSnapshot& snapshot);
}
