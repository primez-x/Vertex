#pragma once

#include "sketch/drawing_selection_removal.hpp"

namespace sketch {

// Complete one already admitted geometry/removal operation with independently
// selected drawing owners and qualified annotation children. The original
// operation and selection replay the actual captured source. Reviewed wall/room
// commands retain their complete original proof under additive envelope 41;
// ordinary asset-free changes retain the existing raw version-one grammar.
// The controller owns publication, selection and displayed-source fencing.
[[nodiscard]] Command complete_drawing_removal_command(
    const DocumentSnapshot& source, const Command& original,
    const DrawingSelectionRemovalIntent& intent);

} // namespace sketch
