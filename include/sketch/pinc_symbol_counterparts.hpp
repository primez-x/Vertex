#pragma once

#include <string>
#include <vector>

namespace sketch {

// A semantic Pinc symbol name and the closest bundled Vertex SVG asset.
// The note records known differences; it does not set import dimensions.
struct PincSymbolCounterpart {
    std::string source_kind;
    std::string catalog_id;
    std::string fidelity_note;
};

[[nodiscard]] std::vector<PincSymbolCounterpart> default_pinc_symbol_counterparts();

} // namespace sketch
