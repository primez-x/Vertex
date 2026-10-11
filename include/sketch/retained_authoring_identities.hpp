#pragma once

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace sketch {

struct RevisionRecord;

// Reserve only allocations and captured clone-source names in actual typed
// retained receipts. The explicit prefix includes undone records when used by
// preparation; replay supplies its actual preceding record count. This grants
// no source, candidate, phase-policy or command replay authority.
void validate_retained_authoring_fresh_identities(
    const std::vector<RevisionRecord>& history, std::size_t preceding_records,
    const std::set<std::string, std::less<>>& fresh);

} // namespace sketch
