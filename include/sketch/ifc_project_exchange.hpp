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
    // Total native tessellation storage per exchange, independent of STEP limits.
    std::size_t max_mesh_vertices{250'000};
    std::size_t max_mesh_triangles{500'000};
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
// and material relationships. When the architecture bridge is enabled, curved
// wall/void solids and door/window fill parts use closed native-kernel meshes
// with maximum 1 mm deviation; fills remain separate from void features.
// Fill meshes are opening-local: X spans the jamb chord, Z points up, and the
// right-handed Y points toward the door swing side (host left for windows).
// Explicit product placements preserve their actual native world geometry.
// Canonical panel/gable/hip roofs and authored room volumes (including cuts,
// curves and holes) use validated native solids as IFCROOF/IFCSPACE tessellations
// when the bridge is enabled. Current source-bound physical rooms are IFCSPACE
// net footprints with inner loops and their actual plane; no height is invented.
// Spaces decompose the storey through IfcRelAggregates. Stale source-bound room
// geometry is withheld. Complete roof/room authoring metadata is retained in a
// bounded property envelope, including nested extension metadata.
// Canonical v1/v2 stairs and railings likewise use real IFCSTAIR/IFCRAILING
// native solid compounds. Hosted railings aggregate under their source stair;
// individual flight/landing solids do not become duplicate active products.
// IFC fill OverallWidth is the opening body's local X envelope; retained native
// curved width_m continues to measure stations along the host arc.
// Unsupported required objects retain native payload
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
// With the optional native bridge, native curved hosts and exact fill profiles
// activate only after their regenerated geometry, dimensions, contexts, and
// host/void/fill relationships agree. Foreign tessellations remain diagnosed
// source data; metadata alone never activates native manufacturing semantics.
// Fill placements compose bounded proper rigid Z-up frames and must agree with
// the opening and handing; unsupported placement bases remain inactive.
// Roof/room activation additionally requires exact regenerated native meshes,
// IFC type, the actual project's linked metre length-unit assignment, and a
// supported representation context. Orphan unit declarations cannot authorize
// reconstruction. Their rigid
// placement frames are composed before comparison. Original level-relative
// authoring is retained but activation uses resolved world coordinates. Source
// organization/level links and required state remain in the retained envelope;
// detached editable candidates diagnose that unreconstructed context and share
// the worker's bounded pure admission rules. An exchange-wide attempt ledger
// charges analytical and expected construction work before native regeneration,
// including failed/mismatching carriers. Foreign roof/space geometries and
// physical-room source descriptors remain conservative retained candidates.
// Stair/railing recovery also checks the captured original authoring envelope
// and exact meshes; hosted rails require one actual aggregate to a proved
// same-model stair. Only validated identities are remapped; source organization,
// level context and extensions remain opaque retained provenance.
[[nodiscard]] IfcProjectImportResult import_project_ifc(
    std::string_view bytes,
    const IfcExchangeLimits& limits = {});

} // namespace sketch
