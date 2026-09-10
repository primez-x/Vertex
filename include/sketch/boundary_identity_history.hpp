#pragma once
#include <map>
#include <set>
#include <string>

namespace sketch {
// Derived from all retained revisions, including abandoned branches. Never
// serialized separately; exact navigation may restore these reserved IDs.
struct BoundaryIdentityLifetime {
    std::string entity_type;
    bool protected_identity{};
    bool seen_legacy{};
    bool legacy_reused_or_retyped{};
    std::set<std::string, std::less<>> segments;
    std::set<std::string, std::less<>> vertices;
};
using BoundaryIdentityHistory =
    std::map<std::string, BoundaryIdentityLifetime, std::less<>>;
} // namespace sketch
