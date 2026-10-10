#include "sketch/workspace_save_queue.hpp"
#include "sketch/document_digest.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sketch {

struct WorkspaceSaveQueue::State final {
    struct Pending final {
        std::uint64_t sequence{};
        CompletionKind kind{CompletionKind::save};
        std::optional<SavePublicationTicket> ticket;
        SaveOperation operation;
        PreparedOperation prepared;
        TaskOperation task;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Pending> pending;
    std::deque<Completion> completed;
    std::uint64_t next_sequence = 1;
    bool stopping = false;
    bool drain = true;
    std::thread worker;

    void run() {
        for (;;) {
            Pending job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !pending.empty(); });
                if (pending.empty()) {
                    if (stopping) return;
                    continue;
                }
                job = std::move(pending.front());
                pending.pop_front();
            }

            Completion completion;
            completion.sequence = job.sequence;
            completion.kind = job.kind;
            completion.ticket = std::move(job.ticket);
            try {
                if (job.kind == CompletionKind::save) {
                    completion.receipt = job.operation();
                } else if (job.kind == CompletionKind::prepared_save) {
                    completion.prepared = job.prepared();
                } else if (job.kind == CompletionKind::task) {
                    job.task();
                }
            } catch (...) {
                completion.error = std::current_exception();
            }
            {
                std::lock_guard lock(mutex);
                completed.push_back(std::move(completion));
            }
        }
    }
};

WorkspaceSaveQueue::SourceProof::SourceProof(DocumentSnapshot source, std::string digest) noexcept
    : source_(std::move(source)), digest_(std::move(digest)) {}
WorkspaceSaveQueue::SourceProof WorkspaceSaveQueue::SourceProof::capture(const DocumentSnapshot& source) {
    return SourceProof(source, document_authoring_source_digest_v2(source));
}

WorkspaceSaveQueue::PreparedSave::PreparedSave(SavePublicationTicket ticket, SaveReceipt receipt,
    DocumentSnapshot source, std::string digest, SaveAcknowledgementDescriptor descriptor,
    std::optional<PreparedProjectWorkspace> workspace) noexcept
    : ticket_(std::move(ticket)), receipt_(std::move(receipt)), source_(std::move(source)),
      source_digest_(std::move(digest)), descriptor_(std::move(descriptor)), workspace_(std::move(workspace)) {}

WorkspaceSaveQueue::PreparedSave WorkspaceSaveQueue::PreparedSave::publish(
    const ProjectWorkspaceSnapshot& snapshot, const SavePublicationBinding& binding,
    SourceProof proof, SaveOperation operation,
    std::optional<PreparedProjectWorkspace> workspace) {
    if (!operation) throw std::invalid_argument("save queue: empty prepared storage operation");
    auto descriptor = WorkspaceSaveCoordinator::describe(snapshot);
    if (!proof.source().shares_authoring_source_with(snapshot.document()) &&
        proof.digest() != document_authoring_source_digest_v2(snapshot.document()))
        throw std::invalid_argument("save queue: source proof does not match publication");
    if (workspace && WorkspaceSaveCoordinator::describe(workspace->capture()) != descriptor)
        throw std::invalid_argument("save queue: prepared workspace does not match publication");
    // All result allocation and sealing precedes filesystem publication.
    auto ticket = WorkspaceSaveCoordinator::capture(snapshot, binding, proof.digest());
    auto captured_source = proof.source();
    auto digest = proof.digest();
    auto receipt = operation();
    return PreparedSave(std::move(ticket), std::move(receipt), std::move(captured_source),
        std::move(digest), std::move(descriptor), std::move(workspace));
}
std::unique_ptr<ProjectWorkspace> WorkspaceSaveQueue::PreparedSave::adopt_workspace() noexcept {
    if (!workspace_ || !acknowledged_) return {};
    auto result = ProjectWorkspace::adopt_prepared(std::move(*workspace_));
    workspace_.reset();
    return result;
}
bool WorkspaceSaveQueue::PreparedSave::publication_valid() const {
    return WorkspaceSaveCoordinator::publication_valid(ticket_, receipt_);
}
SaveAcknowledgementResult WorkspaceSaveQueue::PreparedSave::acknowledge(
    const SaveAcknowledgementDescriptor& current, const SavePublicationBinding& binding, std::string_view digest) {
    const auto result = WorkspaceSaveCoordinator::accept(ticket_, receipt_, current, binding, digest);
    acknowledged_ = result.acknowledged();
    return result;
}

WorkspaceSaveQueue::WorkspaceSaveQueue() : state_(std::make_unique<State>()) {
    state_->worker = std::thread([state = state_.get()] { state->run(); });
}

WorkspaceSaveQueue::~WorkspaceSaveQueue() {
    shutdown(true);
}

std::uint64_t WorkspaceSaveQueue::enqueue(SavePublicationTicket ticket, SaveOperation operation) {
    if (!operation) throw std::invalid_argument("save queue: empty operation");
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) throw std::logic_error("save queue: shutdown");
    const auto sequence = state.next_sequence++;
    state.pending.push_back(State::Pending{sequence, CompletionKind::save,
        std::optional<SavePublicationTicket>(std::move(ticket)), std::move(operation)});
    state.wake.notify_one();
    return sequence;
}

std::uint64_t WorkspaceSaveQueue::enqueue_barrier() {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) throw std::logic_error("save queue: shutdown");
    const auto sequence = state.next_sequence++;
    state.pending.push_back(State::Pending{sequence, CompletionKind::barrier, std::nullopt, {}});
    state.wake.notify_one();
    return sequence;
}

std::uint64_t WorkspaceSaveQueue::enqueue_prepared(PreparedOperation operation) {
    if (!operation) throw std::invalid_argument("save queue: empty preparation operation");
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) throw std::logic_error("save queue: shutdown");
    const auto sequence = state.next_sequence++;
    State::Pending job;
    job.sequence = sequence;
    job.kind = CompletionKind::prepared_save;
    job.prepared = std::move(operation);
    state.pending.push_back(std::move(job));
    state.wake.notify_one();
    return sequence;
}
std::uint64_t WorkspaceSaveQueue::enqueue_task(TaskOperation operation) {
    if (!operation) throw std::invalid_argument("save queue: empty task operation");
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) throw std::logic_error("save queue: shutdown");
    const auto sequence = state.next_sequence++;
    State::Pending job;
    job.sequence = sequence;
    job.kind = CompletionKind::task;
    job.task = std::move(operation);
    state.pending.push_back(std::move(job));
    state.wake.notify_one();
    return sequence;
}

std::vector<WorkspaceSaveQueue::Completion> WorkspaceSaveQueue::take_completed() {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    std::vector<Completion> result;
    result.reserve(state.completed.size());
    while (!state.completed.empty()) {
        result.push_back(std::move(state.completed.front()));
        state.completed.pop_front();
    }
    return result;
}
std::optional<WorkspaceSaveQueue::Completion> WorkspaceSaveQueue::take_completed_one() {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (state.completed.empty()) return std::nullopt;
    auto result = std::move(state.completed.front());
    state.completed.pop_front();
    return result;
}

void WorkspaceSaveQueue::shutdown(bool drain) {
    if (!state_) return;
    auto& state = *state_;
    {
        std::lock_guard lock(state.mutex);
        if (!state.stopping) {
            state.stopping = true;
            state.drain = drain;
            if (!drain) state.pending.clear();
        }
    }
    state.wake.notify_all();
    if (state.worker.joinable()) state.worker.join();
}

}  // namespace sketch
