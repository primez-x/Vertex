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

// V2 carries one standalone boundary, not an appraisal/source dependency graph.
// Document's generic reference vocabulary does not cover these consumer-owned
// links. Both mapper and isolated-response admission must reject active links
// rather than accidentally resolving a source ID in the destination project.
[[nodiscard]] inline bool native_dxf_boundary_has_untransported_links(const Entity& entity) {
    const auto& properties = entity.properties;
    if (!properties.is_object() || properties.contains("parent_id") ||
        properties.contains("wall_measurement_source") ||
        entity.extensions.contains("measurement_linework_sources") ||
        entity.extensions.contains("measurement_linework_group"))
        return true;
    const auto nonempty_ids = [](const nlohmann::json& object, const char* key) {
        const auto value = object.find(key);
        return value != object.end() && (!value->is_array() || !value->empty());
    };
    if (nonempty_ids(properties, "deduction_ids")) return true;
    const auto facts = properties.find("appraisal_facts");
    if (facts == properties.end()) return false;
    if (!facts->is_object()) return true;
    const auto ansi = facts->find("ansi");
    if (ansi == facts->end()) return false;
    if (!ansi->is_object()) return true;
    const auto ceiling = ansi->find("ceiling");
    if (ceiling == ansi->end()) return false;
    if (!ceiling->is_object() || nonempty_ids(*ceiling, "below_5ft_deduction_ids")) return true;
    for (const auto* key : {"room_boundary_id", "stair_from_floor_id"}) {
        const auto value = ceiling->find(key);
        if (value != ceiling->end() && (!value->is_string() || !value->get_ref<const std::string&>().empty()))
            return true;
    }
    return false;
}

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
// Valid closed standalone boundaries use a V2 native block: ordinary outer/hole
// curves accompany bounded source properties, classifications and topology.
// Organizational bindings detach on import; unavailable dependent source graphs
// cannot activate. Other boundaries and slabs retain ordinary analytical loops
// and diagnose unrepresented hole ownership, topology and classifications.
// Polyline vertices require exact authored joins. Distinct consecutive endpoints
// retain independent primitives with a connection-loss diagnostic; a merely
// tolerance-close final endpoint remains an open polyline without snapping.
[[nodiscard]] DxfProjectExportResult export_project_dxf(
    const DocumentSnapshot& document,
    const DxfExchangeLimits& limits = {});

// Parses a bounded DXF R2013 drawing and reconstructs editable native
// boundary/annotation entities and validated native boundary/wall/opening graphs.
// Foreign primitive candidates carry CAD classifications and separate hole loops.
// The existing VERTEX_ENTITY_V1 XDATA carrier admits unchanged V1 wall/opening
// JSON and V2 closed-boundary JSON with depiction BOUNDARY_PLAN_V1. V2 requires
// full native document/geometry validation and exact regenerated outer/hole plan
// agreement before retaining native classifications and local topology IDs.
// Entity identities are fresh; organizational bindings remain source evidence.
// Every native carrier requires metre units and identity INSERT/base placement.
// Manufactured opening blocks additionally require
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
