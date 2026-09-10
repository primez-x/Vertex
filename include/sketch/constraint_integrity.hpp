#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Solver-free validation of persisted hard relations and their semantic wall
// hosts. Invalid known data throws std::invalid_argument. A nonempty result
// means well-formed unsupported lock semantics must be preserved read-only.
[[nodiscard]] std::optional<std::string> validate_constraint_integrity(
    const std::map<std::string, Entity, std::less<>>& entities);

// A named wall endpoint cannot silently change identity during reversal.
// Removing its constraints or remapping every surviving binding is explicit
// in the same atomic candidate state.
void validate_constraint_transition(
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after);

} // namespace sketch
