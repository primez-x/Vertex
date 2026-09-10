#include "sketch/project_workspace.hpp"

#include "sketch/document_digest.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace sketch {

struct PreparedWorkspaceEdit::State {
    std::string workspace_identity;
    std::uint64_t expected_epoch = 0;
    std::string source_digest;
    std::unique_ptr<Document> candidate;
    bool consumed = false;
};

PreparedWorkspaceEdit::PreparedWorkspaceEdit(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PreparedWorkspaceEdit::PreparedWorkspaceEdit(PreparedWorkspaceEdit&&) noexcept = default;
PreparedWorkspaceEdit& PreparedWorkspaceEdit::operator=(PreparedWorkspaceEdit&&) noexcept = default;
PreparedWorkspaceEdit::~PreparedWorkspaceEdit() = default;

DocumentSnapshot PreparedWorkspaceEdit::preview() const {
    if (!state_ || state_->consumed || !state_->candidate) {
        throw std::invalid_argument("workspace edit is consumed or moved from");
    }
    return state_->candidate->snapshot();
}

ProjectWorkspace::ProjectWorkspace(const DocumentSnapshot& source)
    : document_(new Document(Document::fork(source))), identity_(make_stable_id()) {}

ProjectWorkspace::~ProjectWorkspace() = default;

const std::string& ProjectWorkspace::identity() const noexcept { return identity_; }
std::uint64_t ProjectWorkspace::epoch() const noexcept { return epoch_; }
DocumentSnapshot ProjectWorkspace::snapshot() const { return document_->snapshot(); }

PreparedWorkspaceEdit ProjectWorkspace::prepare(const Command& command) const {
    return prepare_impl(Operation::apply, &command);
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_undo() const {
    return prepare_impl(Operation::undo, nullptr);
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_redo() const {
    return prepare_impl(Operation::redo, nullptr);
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_impl(
    Operation operation, const Command* command) const {
    const auto source = document_->snapshot();
    auto state = std::make_unique<PreparedWorkspaceEdit::State>();
    state->workspace_identity = identity_;
    state->expected_epoch = epoch_;
    state->source_digest = document_snapshot_digest(source);
    state->candidate.reset(new Document(Document::fork(source)));
    switch (operation) {
    case Operation::apply:
        state->candidate->apply(*command);
        break;
    case Operation::undo:
        state->candidate->undo(source.revision());
        break;
    case Operation::redo:
        state->candidate->redo(source.revision());
        break;
    }
    return PreparedWorkspaceEdit(std::move(state));
}

Revision ProjectWorkspace::commit(PreparedWorkspaceEdit& edit) {
    if (!edit.state_ || edit.state_->consumed || !edit.state_->candidate) {
        throw std::invalid_argument("workspace edit is consumed or moved from");
    }
    auto& state = *edit.state_;
    if (state.workspace_identity != identity_) {
        throw std::invalid_argument("workspace edit belongs to another workspace");
    }
    if (state.expected_epoch != epoch_) {
        throw std::invalid_argument("workspace edit epoch is stale");
    }
    if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("workspace epoch is exhausted");
    }
    {
        // Complete all allocating validation and dispose its JSON snapshots
        // before publication. Full history and saved markers are part of CAS.
        const auto current = document_->snapshot();
        if (document_snapshot_digest(current) != state.source_digest) {
            throw std::invalid_argument("workspace edit source digest is stale");
        }
    }
    const Revision revision = state.candidate->revision();
    document_.swap(state.candidate);
    ++epoch_;
    state.consumed = true;
    // The consumed ticket retains the retired Document. Its destruction must
    // not run here: JSON disposal can allocate even after successful mutation.
    return revision;
}

}  // namespace sketch
