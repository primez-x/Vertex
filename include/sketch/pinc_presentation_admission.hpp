#pragma once

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/pinc_measurement_admission.hpp"

#include <span>
#include <string_view>

namespace sketch {
enum class PincDoorArtworkTransform { none, swing_left_positive_svg_y, side_positive_svg_y, symmetric_source_garage };
struct PincSymbolBinding {
    std::string source_kind;
    SymbolDefinition definition;
    std::string pinned_svg;
    std::string fidelity_note;
    PincDoorArtworkTransform door_transform{PincDoorArtworkTransform::none};
    // Opening midpoint relative to footprint center in canonical model axes,
    // expressed as fractions of imported width/depth, before flips/rotation.
    Vec2 opening_anchor_fraction{};
};
struct PincPresentationAdmissionLimits {
    std::size_t max_bindings{80};
    std::size_t max_symbols{10'000};
    std::size_t max_labels{10'000};
    std::size_t max_annotation_records{100'000};
    std::size_t max_pinned_svg_bytes{32*1024*1024};
    std::size_t max_string_bytes{32*1024*1024};
    // One total reservation shared by independent geometry re-admission and
    // native page detection. Four graph passes per populated page: two source
    // admission graphs, detector graph, detector's exact lineage-check graph.
    // Builder must partition its combined budget before either module runs;
    // measurement plus presentation conservatively require nine pair passes.
    PincGeometryAdmissionLimits geometry_verification;
};
struct PincPresentationAdmission {
    // One new page-scoped annotation carrier per source page. These supplement,
    // rather than replace, the complete measurement admission entities.
    std::vector<Entity> entities;
    std::vector<PincImportDiagnostic> diagnostics;
};

// Detached, atomic preparation against the complete private measurement preview.
// Independently rebuilds source geometry correspondence; mapping fields are not
// authority. Area callouts refer to native area owners and contain no cached
// calculation strings. Symbol wall references are visual provenance only.
// Bindings must contain reviewed bundled definitions and exact SVG bytes; no
// asset lookup, source artwork execution, or physical hosting occurs here.
// Limits may only reduce the hard defaults. Invalid inputs throw invalid_argument.
[[nodiscard]] PincPresentationAdmission prepare_pinc_presentation_admission(
    const PincImportProject&, const PincMeasurementAdmission&,
    const DocumentSnapshot& private_measurement_candidate,
    std::span<const PincSymbolBinding>, std::string_view fresh_namespace,
    const PincPresentationAdmissionLimits& limits = {});
} // namespace sketch
