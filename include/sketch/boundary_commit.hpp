#pragma once

#include "sketch/boundary_authoring_session.hpp"
#include "sketch/document.hpp"
#include "sketch/project_organization.hpp"

#include <map>
#include <string>
#include <vector>

namespace sketch {

class BoundaryCommitBuilder;

// A commit intent is a value copied out of an authoring session. The commit
// adapter owns the copy retained by a preview; callers may continue editing
// their session or the original intent after preview creation.
struct BoundaryCommitIntent {
    BoundaryAuthoringOptions options{};
    std::vector<AcceptedBoundaryChain> chains;
    DrawingContext context;
    std::string message{"Commit boundary authoring"};
};

// A preview is a sealed, copyable display value. Its public candidate map and
// diagnostics are integrity checked by apply_boundary_commit(), while the
// normalized intent retained privately remains the reconstruction authority.
class BoundaryCommitPreview final {
public:
    BoundaryCommitPreview(const BoundaryCommitPreview&) = default;
    BoundaryCommitPreview& operator=(const BoundaryCommitPreview&) = default;
    BoundaryCommitPreview(BoundaryCommitPreview&&) noexcept = default;
    BoundaryCommitPreview& operator=(BoundaryCommitPreview&&) noexcept = default;

    [[nodiscard]] bool accepted() const noexcept;
    [[nodiscard]] const std::string& document_id() const noexcept;
    [[nodiscard]] Revision expected_revision() const noexcept;
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept;
    [[nodiscard]] const std::string& candidate_digest() const noexcept;
    [[nodiscard]] const std::map<std::string, Entity, std::less<>>&
    candidate_entities() const noexcept;
    [[nodiscard]] const std::vector<std::string>& created_boundary_ids() const noexcept;
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept;

private:
    BoundaryCommitPreview() = default;

    bool accepted_{};
    std::string document_id_;
    Revision expected_revision_{};
    std::string source_snapshot_digest_;
    std::string candidate_digest_;
    std::string trial_snapshot_digest_;
    std::map<std::string, Entity, std::less<>> candidate_entities_;
    std::vector<std::string> created_boundary_ids_;
    std::vector<std::string> diagnostics_;
    BoundaryCommitIntent normalized_intent_;

    friend BoundaryCommitPreview preview_boundary_commit(
        const DocumentSnapshot&, const BoundaryCommitIntent&);
    friend Revision apply_boundary_commit(Document&, const BoundaryCommitPreview&);
    friend class BoundaryCommitBuilder;
};

// Preview is side-effect free. Invalid, incomplete, contradictory, or stale
// authoring data returns accepted()==false with the source entity map and a
// diagnostic explaining the first rejected invariant.
[[nodiscard]] BoundaryCommitPreview preview_boundary_commit(
    const DocumentSnapshot& snapshot,
    const BoundaryCommitIntent& intent);

// Apply verifies the complete source snapshot and the public display values,
// recomputes from the sealed normalized intent, then commits all entities as
// one atomic Document command. Rejected, stale, foreign, or modified previews
// throw without mutating the document.
Revision apply_boundary_commit(Document& document,
                               const BoundaryCommitPreview& preview);

}  // namespace sketch
