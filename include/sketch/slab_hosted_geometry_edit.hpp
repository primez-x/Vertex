#pragma once

#include "sketch/slab_geometry_edit.hpp"

namespace sketch {

// Ordinary actual-source horizontal edits. Physical replay remains authoritative;
// only instances in actual catalogs bound to a changed target follow its rigid
// plan or physical model transform. Host and instance identities remain exact.
// Vertex/axis edits retain placements and admit the resulting actual host solids.
// Hosted plan-only nonunit scale refuses: that operation does not scale profile Z.
// Unaffected catalog payloads remain opaque. Affected catalogs require active,
// resolved context, strict typed admission and bounded source/candidate geometry.
// No copied hosts, catalog allocation, phase enrollment or quantities are authored.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_slab_geometry_with_hosted_entities(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const std::vector<SlabGeometryEditIntent>& intents);

} // namespace sketch
