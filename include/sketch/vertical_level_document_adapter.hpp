#pragma once

#include "sketch/document.hpp"
#include "sketch/vertical_levels.hpp"

namespace sketch {

struct ConnectedStairRiseChange {
    std::string stair_id;
    double old_rise_m{};
    double new_rise_m{};
    bool operator==(const ConnectedStairRiseChange&) const = default;
};

struct VerticalLevelEditReceipt {
    Revision revision{};
    std::vector<ConnectedStairRiseChange> affected_stairs;
};

// Immutable preview and command authority, bound to the complete source snapshot.
class VerticalLevelEditCandidate {
public:
    [[nodiscard]] const DocumentSnapshot& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const std::vector<ConnectedStairRiseChange>& affected_stairs() const noexcept {
        return affected_stairs_;
    }
private:
    friend VerticalLevelEditCandidate prepare_vertical_level_edit(
        const DocumentSnapshot&, const std::string&, const VerticalLevelGraph&);
    friend VerticalLevelEditReceipt apply_vertical_level_edit(Document&, const VerticalLevelEditCandidate&);
    VerticalLevelEditCandidate(DocumentSnapshot snapshot, ApplyEntityChanges command,
        std::string source_digest, std::vector<ConnectedStairRiseChange> affected_stairs);
    DocumentSnapshot snapshot_;
    ApplyEntityChanges command_;
    std::string source_digest_;
    std::vector<ConnectedStairRiseChange> affected_stairs_;
};

// Only connected, endpoint-preserving links may drive canonical straight stairs.
// Graph metadata, stair placement and every unrelated field are preserved.
[[nodiscard]] VerticalLevelEditCandidate prepare_vertical_level_edit(
    const DocumentSnapshot& source, const std::string& graph_entity_id,
    const VerticalLevelGraph& replacement);
[[nodiscard]] VerticalLevelEditReceipt apply_vertical_level_edit(
    Document& document, const VerticalLevelEditCandidate& candidate);

} // namespace sketch
