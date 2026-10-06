#pragma once

#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>

namespace sketch {
struct Entity;
struct RevisionRecord;

enum class StairChildIdentityRole { flight, landing };
struct StairChildIdentityOwner {
    std::string owner;
    StairChildIdentityRole role;
    bool operator==(const StairChildIdentityOwner&) const = default;
};

// Explicit document-owned retained ledger, never a process/global cache.
// Restore reserves records chronologically, validating authored transitions
// against the preceding ledger; navigation/redo records reserve state only.
// Geometry/state admission is separate; this ledger checks typed identity only.
// Entirely v1/opaque histories stay lazy and store no unrelated entity IDs.
// Before the first authored v2 topology, initialize_if_needed reconstructs ALL
// prior entity/known-stair IDs. Once initialized, reserve EVERY future state,
// even with no live stair, to preserve lifetime evidence.
class StairIdentityHistory final {
public:
    // Builds from already admitted history for a subsequent authored transition.
    // Do not use the future-inclusive result to validate earlier introductions.
    [[nodiscard]] static StairIdentityHistory from_retained_history(
        std::span<const RevisionRecord> retained_history);
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    // Early no-op without scanning the prefix if already initialized OR neither
    // adjacent map has v2 topology. First v2 reconstructs the prefix once.
    void initialize_if_needed(std::span<const RevisionRecord> retained_history,
        const std::map<std::string, Entity, std::less<>>& before,
        const std::map<std::string, Entity, std::less<>>& after);
    // Call only for authored changes, after initialization and state admission.
    void validate_transition(const std::map<std::string, Entity, std::less<>>& before,
        const std::map<std::string, Entity, std::less<>>& after) const;
    // Call for initial state and EVERY admitted record, including Undo/Redo.
    // Historical reappearance is valid with the same typed owner. All logical
    // checks precede mutation; insertion failure rolls back added reservations.
    void reserve_state(const std::map<std::string, Entity, std::less<>>& state);
private:
    bool initialized_{};
    std::map<std::string, StairChildIdentityOwner, std::less<>> children_;
    std::set<std::string, std::less<>> entity_ids_;
    std::set<std::string, std::less<>> stair_owner_ids_;
};
} // namespace sketch
