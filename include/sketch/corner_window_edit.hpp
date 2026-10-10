#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <string>

namespace sketch {

// Complete existing, same-identity corner owners and their two real cuts after
// every hosted wall edit has been staged. This is geometry/receipt completion,
// not Document, phase, native or snapshot authority. The caller must admit the
// complete transaction afterward. Host scaling requires equal explicit positive
// factors on both hosts and an otherwise unchanged owner geometry.
// Existing participant identities remain fixed; unrelated entity creation or
// removal is allowed. Coherent owner-and-both-cut removal is left to the final
// Document admission, as are fresh corner owners. Failure leaves candidate
// unchanged. Baseline replacement and identity remap are outside this contract.
void complete_corner_window_geometry(
    const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate,
    const std::map<std::string, double, std::less<>>& wall_scales = {});

// Stage one relative leg resize with its common corner fixed. Retains the other
// leg, hosts, profile, metadata and receipt siblings; derives both cuts through
// the same completion path. No Document preview or native admission is performed.
[[nodiscard]] ApplyEntityChanges corner_window_leg_resize_command(
    const DocumentSnapshot& source, const std::string& owner_id,
    std::size_t leg, double relative_width_scale);

} // namespace sketch
