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
    // A merged-away physical wall remains reserved across retained branches.
    // Navigation may restore it; an ordinary insertion cannot reuse its ID.
    bool wall_merge_reserved{};
};
using BoundaryIdentityHistory =
    std::map<std::string, BoundaryIdentityLifetime, std::less<>>;
} // namespace sketch
