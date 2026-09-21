#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// The IFC mapper is a bounded, deterministic STEP subset. It is intentionally
// independent from the process broker: callers can run it behind the reviewed
// worker boundary and retain the original bytes whenever diagnostics exist.
struct IfcExchangeLimits {
    std::size_t max_bytes{64 * 1024 * 1024};
    std::size_t max_records{250'000};
    std::size_t max_arguments{2'000'000};
    std::size_t max_string_bytes{4'096};
};

struct IfcProjectDiagnostic {
    std::string source_id;
    std::string source_kind;
    std::string code;

    bool operator==(const IfcProjectDiagnostic&) const = default;
};

struct IfcProjectExportResult {
    std::string step;
    std::vector<IfcProjectDiagnostic> diagnostics;
};

struct IfcProjectImportResult {
    std::vector<Entity> entities;
    std::vector<IfcProjectDiagnostic> diagnostics;
    // The desktop adapter must retain the original IFC bytes when this is true
    // because one or more identifiable records were not reconstructed.
    bool source_retention_required{};

    bool complete() const noexcept { return diagnostics.empty(); }
};

// Maps the immutable native snapshot into a bounded IFC4 STEP subset. Linear
// boundaries remain analytical polylines; straight constant-height walls and
// closed footprints with an explicit thickness become swept solids in a default
// project/site/building/storey hierarchy. Native wall layer stacks use IFC types
// and material relationships. Unsupported required objects retain native payload
// references with diagnostics; this subset does not claim MVD conformance.
[[nodiscard]] IfcProjectExportResult export_project_ifc(
    const DocumentSnapshot& document,
    const IfcExchangeLimits& limits = {});

// Parses IFC4 STEP records in memory and reconstructs reliable, typed native
// walls, slabs, and rectangular hosted openings when the required geometry and
// Vertex property-set metadata are present. Other reliable footprints remain
// editable boundary candidates with source metadata. The function never opens
// a path or mutates a Document. Malformed input throws before returning any
// partial result; unsupported records produce stable diagnostics and require
// source retention.
[[nodiscard]] IfcProjectImportResult import_project_ifc(
    std::string_view bytes,
    const IfcExchangeLimits& limits = {});

} // namespace sketch
