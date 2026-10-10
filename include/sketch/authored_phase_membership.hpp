#pragma once

#include "sketch/document.hpp"

#include <set>

namespace sketch {

// Shared deterministic phase portion of desktop authoring registration.
// Caller owns admission of the supplied command, actual source and identities;
// this function confers no arbitrary registry or fresh-object authority.
void register_new_phase_memberships(
    const std::map<std::string, Entity, std::less<>>& actual, ApplyEntityChanges& command,
    const std::set<std::string, std::less<>>& admitted_destinations = {},
    const std::string& registry_id = {});

} // namespace sketch

