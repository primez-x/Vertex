#pragma once

#include "sketch/document.hpp"
#include "sketch/dxf_exchange.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Project mapping is deliberately separate from the bounded transport codec.
// It translates the native document graph to and from the representable DXF
// subset without opening files, mutating a Document, or inventing a hosted
// service. Callers can retain the original bytes whenever diagnostics are
// present and decide how/where to commit the returned entities.
struct DxfProjectDiagnostic {
    std::string source_id;
    std::string source_kind;
    std::string code;

    bool operator==(const DxfProjectDiagnostic&) const = default;
};

struct DxfProjectExportResult {
    DxfDrawing drawing;
    std::vector<DxfProjectDiagnostic> diagnostics;
};

struct DxfProjectImportResult {
    std::vector<Entity> entities;
    std::vector<DxfProjectDiagnostic> diagnostics;
    // A caller must retain the original input bytes when this is true if
    // unsupported transport/project records need to remain recoverable.
    bool source_retention_required{};

    bool complete() const noexcept { return diagnostics.empty(); }
};

// Maps the current native project snapshot into the supported DXF R2013
// drawing model. The result remains in memory; use export_dxf_ascii separately
// when a caller is ready to serialize it to a local path.
[[nodiscard]] DxfProjectExportResult export_project_dxf(
    const DocumentSnapshot& document,
    const DxfExchangeLimits& limits = {});

// Parses a bounded DXF R2013 drawing and reconstructs editable native
// boundary/annotation entities where their semantics are representable. The
// returned entities are unparented import candidates; a desktop adapter is
// responsible for assigning the active floor/layer and committing one atomic
// document command. Malformed transport input throws and returns no partial
// result. Unsupported records are reported in diagnostics.
[[nodiscard]] DxfProjectImportResult import_project_dxf(
    std::string_view bytes,
    const DxfExchangeLimits& limits = {});

} // namespace sketch
