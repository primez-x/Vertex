#include "sketch/workspace_save_queue.hpp"

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
            if (job.kind == CompletionKind::save) {
                try {
                    completion.receipt = job.operation();
                } catch (...) {
                    completion.error = std::current_exception();
                }
            }
            {
                std::lock_guard lock(mutex);
                completed.push_back(std::move(completion));
            }
        }
    }
};

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
