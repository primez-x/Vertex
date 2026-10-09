#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace sketch {

using PhysicalWallJoinRemovalEntities = std::map<std::string, Entity, std::less<>>;
using PhysicalWallJoinRemovalAdditionalIdentities =
    std::map<std::string, std::vector<std::string>, std::less<>>;

// Opt-in complete wall-join deletion preflight. Reserves explicit fresh IDs
// against the entire actual retained snapshot, including undone command intent,
// opaque/local tokens, assets and computed component presentation aliases.
// Bounded read-only inspection; throws invalid_argument on collisions or limits.
// An empty map needs no lifetime scan. Replay still validates exact join slots.
void validate_physical_wall_join_removal_identity_lifetime(
    const DocumentSnapshot& source,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities);

// Encoder admission before native cohort inference: these are actual fresh
// typed join upserts, not a fabricated source-join/destination mapping. Exact
// source-derived split slots must still be independently validated afterward.
void validate_physical_wall_join_removal_identity_lifetime(
    const DocumentSnapshot& source,const std::vector<std::string>& fresh_join_ids);

// Document admission/replay variant: actual source plus only the preceding
// retained prefix. The command being replayed and later records are excluded.
// The snapshot overload also reserves actual assets and document metadata.
void validate_physical_wall_join_removal_identity_lifetime(
    const PhysicalWallJoinRemovalEntities& source,
    const std::vector<RevisionRecord>& history,
    std::size_t preceding_records,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities);

struct PhysicalWallJoinRemovalDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const PhysicalWallJoinRemovalDiagnostic&) const = default;
};

struct PhysicalWallJoinRemovalPlan {
    std::vector<std::string> selected_wall_ids;
    std::vector<std::string> source_join_ids;
    std::vector<std::string> retired_join_ids;
    // Components retain original member order and first-member component order.
    // Singletons remain walls but cannot retain a wall-join entity.
    std::map<std::string, std::vector<std::vector<std::string>>, std::less<>> surviving_join_components;
    // Only nonzero counts: the first multiwall component retains the source ID.
    std::map<std::string, std::size_t, std::less<>> additional_identity_counts;
    // Codec-admitted presentation references that the enclosing complete wall
    // removal must review/clean. This producer leaves these entities unchanged.
    std::vector<std::string> presentation_cleanup_entity_ids;
    std::vector<PhysicalWallJoinRemovalDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const PhysicalWallJoinRemovalPlan&) const = default;
};

// Derives actual join consequences while selected walls and their complete
// semantic openings remain in source. Selection is 1..128 unique actual walls;
// source joins are fully admitted before classifying surviving components.
// Required/inactive/shared-baseline/other-alternative owners refuse. Opaque
// affected join references refuse; admitted presentation references are exposed.
[[nodiscard]] PhysicalWallJoinRemovalPlan inspect_physical_wall_join_removal(
    const PhysicalWallJoinRemovalEntities& source,
    const std::vector<std::string>& selected_wall_ids);

// Rederives from actual source; never trusts a supplied candidate or plan.
// Captured fresh identities must exactly fill the source-derived join slots and
// avoid every source JSON string/key, including global/local opaque tokens.
// Complete deletion callers use the snapshot lifetime preflight above before
// native work; the enclosing Document also reserves history/assets. Returns the full
// source map with only joins and their actual phase memberships changed. Walls,
// openings, catalogs, rooms and presentation remain for enclosing removal.
[[nodiscard]] PhysicalWallJoinRemovalEntities replay_physical_wall_join_removal(
    const PhysicalWallJoinRemovalEntities& source,
    const std::vector<std::string>& selected_wall_ids,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities = {});

} // namespace sketch
