#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Prepare a supported sheet/view entity to retain an explicitly empty source
// restriction. Copies the source envelope and raw collections; only missing
// defaults required by the existing v6 reader and its model version are added.
// Already supported v6+ entities are returned unchanged. Refuses any legacy
// source whose raw data cannot be retained with identical interpreted meaning.
[[nodiscard]] Entity upgrade_sheet_view_entity_for_empty_restriction(const Entity& source);

} // namespace sketch
