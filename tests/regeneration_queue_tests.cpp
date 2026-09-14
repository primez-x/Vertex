#include "sketch/workspace_regeneration_queue.hpp"
#include "support/noninteractive_errors.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace sketch;
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<WorkspaceRegenerationQueue::Completion> wait_for(
    WorkspaceRegenerationQueue& queue, std::size_t expected) {
    std::vector<WorkspaceRegenerationQueue::Completion> result;
    for (int attempt = 0; attempt != 2000 && result.size() < expected; ++attempt) {
        std::this_thread::sleep_for(1ms);
        auto next = queue.take_completed();
        for (auto& completion : next) result.push_back(std::move(completion));
    }
    return result;
}

void cancellation_discards_stale_result_and_preserves_source_revision() {
    WorkspaceRegenerationQueue queue;
    std::atomic<bool> started = false;
    constexpr std::uint64_t source_revision = 41;
    const auto sequence = queue.enqueue([&](const RegenerationCancellationToken& token) {
        started = true;
        while (!token.is_cancelled()) std::this_thread::yield();
        return RegenerationReceipt{source_revision, "stale-output"};
    });
    for (int attempt = 0; attempt != 2000 && !started; ++attempt) {
        std::this_thread::sleep_for(1ms);
    }
    require(started, "regeneration did not start");
    require(queue.cancel(sequence), "active regeneration was not cancellable");
    const auto completions = wait_for(queue, 1);
    require(completions.size() == 1, "cancelled regeneration did not complete");
    require(completions.front().sequence == sequence &&
                completions.front().kind == RegenerationCompletionKind::cancelled &&
                !completions.front().receipt.has_value() &&
                !completions.front().succeeded(),
            "cancelled regeneration published a stale result");
    // The caller only applies completed receipts whose source revision still
    // matches the live workspace.  Cancellation therefore leaves that valid
    // revision untouched.
    std::uint64_t current_revision = source_revision;
    if (completions.front().succeeded()) current_revision = completions.front().receipt->source_revision;
    require(current_revision == source_revision,
            "cancellation changed the valid source revision");
}

void pending_cancellation_does_not_run_operation_and_completion_order_is_fifo() {
    WorkspaceRegenerationQueue queue;
    std::atomic<bool> first_started = false;
    std::atomic<bool> release_first = false;
    std::atomic<bool> second_ran = false;
    const auto first = queue.enqueue([&](const RegenerationCancellationToken& token) {
        first_started = true;
        while (!release_first && !token.is_cancelled()) std::this_thread::yield();
        return RegenerationReceipt{7, "first"};
    });
    const auto second = queue.enqueue([&](const RegenerationCancellationToken&) {
        second_ran = true;
        return RegenerationReceipt{7, "second"};
    });
    for (int attempt = 0; attempt != 2000 && !first_started; ++attempt) {
        std::this_thread::sleep_for(1ms);
    }
    require(first_started, "first regeneration did not start");
    require(queue.cancel(second), "pending regeneration was not cancellable");
    release_first = true;
    const auto completions = wait_for(queue, 2);
    require(completions.size() == 2 && completions[0].sequence == first &&
                completions[1].sequence == second,
            "regeneration completions were not FIFO");
    require(completions[0].kind == RegenerationCompletionKind::completed &&
                completions[0].receipt.has_value(),
            "uncancelled regeneration did not publish its receipt");
    require(completions[1].kind == RegenerationCompletionKind::cancelled &&
                !completions[1].receipt.has_value() && !second_ran,
            "pending cancellation ran or published an output");
}

void failures_are_reported_and_shutdown_without_drain_cancels_all_work() {
    WorkspaceRegenerationQueue failure_queue;
    const auto failed = failure_queue.enqueue([](const RegenerationCancellationToken&) -> RegenerationReceipt {
        throw std::runtime_error("injected regeneration failure");
    });
    const auto failure = wait_for(failure_queue, 1);
    require(failure.size() == 1 && failure.front().sequence == failed &&
                failure.front().kind == RegenerationCompletionKind::failed &&
                failure.front().error && !failure.front().succeeded(),
            "regeneration failure was not captured");

    WorkspaceRegenerationQueue queue;
    std::atomic<bool> started = false;
    std::atomic<bool> pending_ran = false;
    const auto active = queue.enqueue([&](const RegenerationCancellationToken& token) {
        started = true;
        while (!token.is_cancelled()) std::this_thread::yield();
        return RegenerationReceipt{9, "discarded"};
    });
    const auto pending = queue.enqueue([&](const RegenerationCancellationToken&) {
        pending_ran = true;
        return RegenerationReceipt{9, "must-not-run"};
    });
    for (int attempt = 0; attempt != 2000 && !started; ++attempt) {
        std::this_thread::sleep_for(1ms);
    }
    require(started, "shutdown fixture did not start active regeneration");
    queue.shutdown(false);
    const auto completions = queue.take_completed();
    require(completions.size() == 2 && completions[0].sequence == active &&
                completions[1].sequence == pending,
            "shutdown cancellation did not retain ordered completions");
    require(completions[0].kind == RegenerationCompletionKind::cancelled &&
                completions[1].kind == RegenerationCompletionKind::cancelled &&
                !pending_ran,
            "non-draining shutdown ran or published regeneration work");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        cancellation_discards_stale_result_and_preserves_source_revision();
        pending_cancellation_does_not_run_operation_and_completion_order_is_fifo();
        failures_are_reported_and_shutdown_without_drain_cancels_all_work();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
