#include "sketch/workspace_regeneration_queue.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sketch {

struct RegenerationCancellationToken::State final {
    std::atomic<bool> cancelled{false};
};

RegenerationCancellationToken::RegenerationCancellationToken(
    std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

bool RegenerationCancellationToken::is_cancelled() const noexcept {
    return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
}

struct WorkspaceRegenerationQueue::State final {
    struct Pending final {
        std::uint64_t sequence{};
        std::shared_ptr<RegenerationCancellationToken::State> token;
        RegenerationOperation operation;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Pending> pending;
    std::deque<RegenerationCompletion> completed;
    std::map<std::uint64_t, std::shared_ptr<RegenerationCancellationToken::State>, std::less<>>
        tokens;
    std::uint64_t next_sequence = 1;
    bool stopping = false;
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

            RegenerationCompletion completion;
            completion.sequence = job.sequence;
            const RegenerationCancellationToken token(job.token);
            if (token.is_cancelled()) {
                completion.kind = RegenerationCompletionKind::cancelled;
            } else {
                try {
                    auto receipt = job.operation(token);
                    if (token.is_cancelled()) {
                        completion.kind = RegenerationCompletionKind::cancelled;
                    } else {
                        completion.kind = RegenerationCompletionKind::completed;
                        completion.receipt = std::move(receipt);
                    }
                } catch (...) {
                    if (token.is_cancelled()) {
                        completion.kind = RegenerationCompletionKind::cancelled;
                    } else {
                        completion.kind = RegenerationCompletionKind::failed;
                        completion.error = std::current_exception();
                    }
                }
            }
            {
                std::lock_guard lock(mutex);
                tokens.erase(job.sequence);
                completed.push_back(std::move(completion));
            }
        }
    }
};

WorkspaceRegenerationQueue::WorkspaceRegenerationQueue()
    : state_(std::make_unique<State>()) {
    state_->worker = std::thread([state = state_.get()] { state->run(); });
}

WorkspaceRegenerationQueue::~WorkspaceRegenerationQueue() {
    shutdown(true);
}

std::uint64_t WorkspaceRegenerationQueue::enqueue(RegenerationOperation operation) {
    if (!operation) throw std::invalid_argument("regeneration queue: empty operation");
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) throw std::logic_error("regeneration queue: shutdown");
    if (state.next_sequence == 0) throw std::overflow_error("regeneration queue: sequence overflow");
    const auto sequence = state.next_sequence++;
    auto token = std::make_shared<RegenerationCancellationToken::State>();
    state.tokens.emplace(sequence, token);
    state.pending.push_back(State::Pending{sequence, std::move(token), std::move(operation)});
    state.wake.notify_one();
    return sequence;
}

bool WorkspaceRegenerationQueue::cancel(std::uint64_t sequence) {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    const auto found = state.tokens.find(sequence);
    if (found == state.tokens.end()) return false;
    found->second->cancelled.store(true, std::memory_order_release);
    return true;
}

std::vector<RegenerationCompletion> WorkspaceRegenerationQueue::take_completed() {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    std::vector<RegenerationCompletion> result;
    result.reserve(state.completed.size());
    while (!state.completed.empty()) {
        result.push_back(std::move(state.completed.front()));
        state.completed.pop_front();
    }
    return result;
}

void WorkspaceRegenerationQueue::shutdown(bool drain) {
    if (!state_) return;
    auto& state = *state_;
    {
        std::lock_guard lock(state.mutex);
        if (!state.stopping) {
            state.stopping = true;
            if (!drain) {
                for (const auto& [sequence, token] : state.tokens) {
                    (void)sequence;
                    token->cancelled.store(true, std::memory_order_release);
                }
            }
        }
    }
    state.wake.notify_all();
    if (state.worker.joinable()) state.worker.join();
}

}  // namespace sketch
