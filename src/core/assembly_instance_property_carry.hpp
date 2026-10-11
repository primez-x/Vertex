#pragma once

#include "sketch/assembly_instance_property_edit.hpp"

namespace sketch::detail {

// Internal mathematical composition only. The enclosing authoring workflow
// must independently replay its sealed original component and host/base
// preparations before supplying derived_carried and this exact selected map.
// Neither borrowed raw maps nor the returned map establish source authority,
// replacement permission, a Document preparation or publication authority.
// Every original selected target occurs once, including identical destinations;
// destinations retain its exact part path and root kind. Roots are injective.
[[nodiscard]] AssemblyDocumentEntities calculate_carried_assembly_instance_property_entities(
    const AssemblyDocumentEntities& original,
    std::span<const AssemblyInstancePropertyEditIntent> original_intents,
    const AssemblyDocumentEntities& derived_carried,
    const std::map<AssemblyInstancePropertyEditTarget, AssemblyInstancePropertyEditTarget>& selected_destinations);

} // namespace sketch::detail
