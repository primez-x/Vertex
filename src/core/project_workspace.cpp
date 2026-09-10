#include "sketch/project_workspace.hpp"

#include "sketch/document_digest.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace detail {
struct WorkspaceDocumentState {
    std::unique_ptr<Document> document;
    WorkspaceDocumentHistory history;
};
}

struct PreparedWorkspaceEdit::State {
    std::string workspace_identity;
    std::uint64_t expected_epoch = 0;
    std::string source_digest;
    std::unique_ptr<detail::WorkspaceDocumentState> candidate;
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
    return state_->candidate->document->snapshot();
}

ProjectWorkspace::ProjectWorkspace(const DocumentSnapshot& source)
    : state_(std::make_unique<detail::WorkspaceDocumentState>()), identity_(make_stable_id()) {
    state_->document.reset(new Document(Document::fork(source)));
    state_->history = capture_workspace_document_history(source);
}

ProjectWorkspace::~ProjectWorkspace() = default;

const std::string& ProjectWorkspace::identity() const noexcept { return identity_; }
std::uint64_t ProjectWorkspace::epoch() const noexcept { return epoch_; }
std::uint64_t ProjectWorkspace::edited_generation() const noexcept { return edited_generation_; }
std::uint64_t ProjectWorkspace::checkpoint_generation() const noexcept { return checkpoint_generation_; }
DocumentSnapshot ProjectWorkspace::snapshot() const { return state_->document->snapshot(); }
WorkspaceDocumentHistory ProjectWorkspace::document_history() const { return state_->history; }

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
    const auto source = state_->document->snapshot();
    auto state = std::make_unique<PreparedWorkspaceEdit::State>();
    state->workspace_identity = identity_;
    state->expected_epoch = epoch_;
    state->source_digest = document_snapshot_digest(source);
    state->candidate = std::make_unique<detail::WorkspaceDocumentState>();
    state->candidate->document.reset(new Document(Document::fork(source)));
    state->candidate->history = state_->history;
    auto kind = WorkspaceDocumentEventKind::edit;
    switch (operation) {
    case Operation::apply:
        state->candidate->document->apply(*command);
        break;
    case Operation::undo:
        state->candidate->document->undo(source.revision());
        kind = WorkspaceDocumentEventKind::undo;
        break;
    case Operation::redo:
        state->candidate->document->redo(source.revision());
        kind = WorkspaceDocumentEventKind::redo;
        break;
    }
    auto& events = state->candidate->history.events;
    if (events.size() == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("workspace history sequence is exhausted");
    events.push_back({make_stable_id(), static_cast<std::uint64_t>(events.size()) + 1,
                      kind, source.revision(), state->candidate->document->revision()});
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
    if (edited_generation_ == std::numeric_limits<std::uint64_t>::max() ||
        checkpoint_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("workspace content generation is exhausted");
    }
    {
        // Complete all allocating validation and dispose its JSON snapshots
        // before publication. Full history and saved markers are part of CAS.
        const auto current = state_->document->snapshot();
        if (document_snapshot_digest(current) != state.source_digest) {
            throw std::invalid_argument("workspace edit source digest is stale");
        }
    }
    const Revision revision = state.candidate->document->revision();
    state_.swap(state.candidate);
    ++epoch_;
    ++edited_generation_;
    ++checkpoint_generation_;
    state.consumed = true;
    // The consumed ticket retains the retired Document. Its destruction must
    // not run here: JSON disposal can allocate even after successful mutation.
    return revision;
}

}  // namespace sketch
