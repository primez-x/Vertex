#include "roof_derivation_cache.hpp"

#include <cstddef>
#include <list>
#include <mutex>

namespace sketch {
namespace {

constexpr std::size_t maximum_entries = 64;
constexpr std::size_t maximum_wire_bytes = 8 * 1024 * 1024;
constexpr std::size_t maximum_archive_bytes = 1024 * 1024;

struct Entry {
    std::string dialect;
    std::string wire;
};

struct Cache {
    std::mutex mutex;
    // Most recently used first; entries own all bytes used as cache authority.
    std::list<Entry> entries;
    std::size_t wire_bytes = 0;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

bool find_and_touch(Cache& state, std::string_view dialect,
                    const std::string& wire) {
    for (auto it = state.entries.begin(); it != state.entries.end(); ++it) {
        // Exact equality is authoritative, without hash collisions or borrowed keys.
        if (it->dialect == dialect && it->wire == wire) {
            state.entries.splice(state.entries.begin(), state.entries, it);
            return true;
        }
    }
    return false;
}

} // namespace

void validate_roof_derivation_cached(
    std::string_view dialect,
    const std::string& exact_archive_wire,
    const std::function<void()>& validate) {
    if (dialect.empty() || dialect.size() > 128 || exact_archive_wire.empty() ||
        exact_archive_wire.size() > maximum_archive_bytes) {
        validate();
        return;
    }

    auto& state = cache();
    {
        std::lock_guard lock(state.mutex);
        if (find_and_touch(state, dialect, exact_archive_wire)) {
            return;
        }
    }

    // Native derivation can be expensive or throw. Never hold the cache lock
    // across it, and never retain a failed validation.
    validate();

    std::lock_guard lock(state.mutex);
    // Another thread may have validated these exact bytes while we were outside.
    if (find_and_touch(state, dialect, exact_archive_wire)) {
        return;
    }
    state.entries.push_front(Entry{std::string(dialect), exact_archive_wire});
    state.wire_bytes += exact_archive_wire.size();
    while (state.entries.size() > maximum_entries ||
           state.wire_bytes > maximum_wire_bytes) {
        state.wire_bytes -= state.entries.back().wire.size();
        state.entries.pop_back();
    }
}

} // namespace sketch
