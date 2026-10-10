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

// Maps the snapshot's saved active design into the supported DXF R2013 drawing
// model. Retained inactive owners, their hosted openings and bound annotations
// are omitted with fidelity diagnostics; wall cuts and native hosted IDs use
// the same active graph. The complete source snapshot remains unchanged. DXF
// does not preserve phase registries or alternatives. Without a phase registry,
// the existing whole-project mapping applies. The result remains in memory;
// use export_dxf_ascii separately when ready to serialize it to a local path.
// Current physical-room and wall-axis lengths resolve from the complete saved
// inventory. Single curved lengths use analytical ARC_DIMENSION; straight
// lengths use aligned DIMENSION. Bent/curved multi-edge totals retain their
// measured quantity as a named text callout with an explicit fidelity diagnostic.
// Angles retain their admitted vertex tangents as three-point angular DIMENSION;
// area quantities retain named callouts with their association loss diagnosed.
// Inline boundary/slab holes retain their analytical loops as ordinary curves;
// native hole ownership is not represented. Boundary edge/vertex topology,
// native boundary types and declared area classifications report their loss.
// Polyline vertices require exact authored joins. Distinct consecutive endpoints
// retain independent primitives with a connection-loss diagnostic; a merely
// tolerance-close final endpoint remains an open polyline without snapping.
[[nodiscard]] DxfProjectExportResult export_project_dxf(
    const DocumentSnapshot& document,
    const DxfExchangeLimits& limits = {});

// Parses a bounded DXF R2013 drawing and reconstructs editable native
// boundary/annotation entities and validated native wall/opening graphs. The
// boundary candidates carry primitive classifications, not the source's native
// area classifications, stable edge/vertex identities or outer/hole ownership.
// Separate exported hole curves therefore reconstruct as independent candidates.
// Native VERTEX_ENTITY_V1 block metadata requires matching plan primitives and metre
// units with identity INSERTs. Manufactured opening blocks additionally require
// depiction MANUFACTURED_PLAN_V1 and an architecture-enabled mapper that admits
// the host/assembly solids and regenerates their exact horizontal plan section.
// Core-only builds retain symbolic visual fallback with explicit diagnostics;
// they never activate manufactured native metadata. Legacy blocks without an
// assembly profile retain their original plan contract. The
// returned entities are unparented import candidates; a desktop adapter is
// responsible for assigning the active floor/layer and committing one atomic
// document command. Malformed transport input throws and returns no partial
// result. Unsupported records are reported in diagnostics.
// Inches, feet, millimetres, centimetres, metres and kilometres are normalized
// to native SI metres before mapping. Missing/unitless or other source units
// return no candidates, an explicit diagnostic and required source retention.
// Linear/arc/angular dimensions reconstruct their measured geometry and actual text,
// retaining their presentation metadata. Foreign owner association remains
// unbound and diagnosed; a dimension picture block supplies no native authority.
[[nodiscard]] DxfProjectImportResult import_project_dxf(
    std::string_view bytes,
    const DxfExchangeLimits& limits = {});

} // namespace sketch
