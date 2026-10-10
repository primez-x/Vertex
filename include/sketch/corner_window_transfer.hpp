#pragma once

#include "sketch/document.hpp"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace sketch {

// Passive content, ordered by the owner's two legs. Hosts supply source
// geometry only; cloning never imports or changes their entity envelopes.
struct CornerWindowTransfer {
    Entity owner;
    std::array<Entity, 2> walls;
    std::array<Entity, 2> cuts;
    // Supported owner-bound corner-leg dimensions only. Appended so existing
    // three-field aggregate callers retain their source compatibility.
    std::vector<Entity> dimensions;
};

// Checks bounded portable envelopes, reciprocal ownership, raw placement,
// structural geometry, supported quantity cores and canonical-reference
// portability before clipboard arming or source deletion. Unchanged future or
// core-free quantity payloads remain opaque; changed bound values require a
// supported replayable core. This carries no Document/history or native authority.
void validate_corner_window_transfer(const CornerWindowTransfer& transfer);

// Stages the owner, cuts and transported dimensions atomically, retaining
// metadata, extensions and unchanged numeric representations. Each dimension
// requires one explicit fresh identity keyed by its source identity. Its text
// follows the same leg's actual endpoint-to-jamb frame in destination metres.
// The caller must complete phase/catalog changes, preview the complete Document command,
// validate actual architectural geometry, fence the source and then apply.
// Fresh identities are checked against the complete retained/undone history,
// including nested source envelopes. Unsupported external canonical bindings
// refuse; raw context is inherited from the destination hosts.
[[nodiscard]] ApplyEntityChanges corner_window_clone_command(
    const DocumentSnapshot& destination, const CornerWindowTransfer& transfer,
    const std::string& owner_id, const std::array<std::string, 2>& opening_ids,
    const std::array<std::string, 2>& wall_ids, const std::array<bool, 2>& at_start,
    Revision expected_revision,
    const std::map<std::string, std::string, std::less<>>& dimension_ids = {});

} // namespace sketch
