#pragma once

#include "sketch/document.hpp"
#include "sketch/sheet_view_model.hpp"

#include <string>

namespace sketch {

// The coordinated sheet/view graph is persisted as one typed Document entity.
// Keeping the codec at the Document boundary prevents generic JSON entities from
// bypassing SheetViewModel's graph, frame, and placement validation.
inline constexpr const char* kSheetViewEntityType = "sheet_view_model";

[[nodiscard]] Entity make_sheet_view_entity(std::string id, const SheetViewModel& model);
[[nodiscard]] SheetViewModel decode_sheet_view_entity(const Entity& entity);
void validate_sheet_view_entity(const Entity& entity);

}  // namespace sketch
