#pragma once

#include "sketch/slab_geometry_edit.hpp"

namespace sketch {

// Ordinary actual-source horizontal edits. Physical replay remains authoritative;
// only instances in actual catalogs bound to a changed target follow its rigid
// plan or physical model transform. Host and instance identities remain exact.
// Historical vertex/axis edits retain placements and admit the actual host solids.
// Flagged v4 directional resize maps complete root/nested own profiles through
// their exact world poses, retaining type definitions, topology, Z and all poses.
// Changed rosters become row-local v9 overrides; inherited raw fields survive.
// Legacy native host copies materialize an anonymous actual resolved host body
// under the unchanged placement, preserving their actual native source body.
// Both source forms then use the same captured Site-framed world operation;
// materialization does not change the meaning of a subsequent resize.
// Unit directional operations authenticate both snapshots without materializing.
// Historical operation placement and profile-scaling policies remain unchanged.
// Unaffected catalog payloads remain opaque. Affected catalogs require active,
// resolved context, strict typed admission and bounded source/candidate geometry.
// No copied hosts, catalog allocation, phase enrollment or quantities are authored.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_slab_geometry_with_hosted_entities(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const std::vector<SlabGeometryEditIntent>& intents);

} // namespace sketch
