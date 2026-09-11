#include "sketch/workspace_save_queue.hpp"
#include "sketch/workspace_save_coordinator.hpp"
#include "support/noninteractive_errors.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace sketch;
using namespace std::chrono_literals;

const std::string digest(64, 'a');

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

Document fixture() {
    return Document::create({
        {"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}},
        {"label", "label", {{"text", "Original"}}}});
}

SavePublicationBinding binding() {
    return {"owner", ArchiveRole::ordinary, "destination", "C:/projects/example.sketch"};
}

void edit(ProjectWorkspace& workspace) {
    auto label = workspace.snapshot().entities().at("label");
    label.properties["text"] = "Edited";
    auto staged = workspace.prepare(ApplyEntityChanges{
        .expected_revision = workspace.snapshot().revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Edit"});
    (void)workspace.commit(staged);
}

void matching_completion_acknowledges_on_owner_thread() {
    ProjectWorkspace workspace(fixture().snapshot());
    edit(workspace);
    const auto captured = workspace.capture();
    auto ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    WorkspaceSaveQueue queue;
    std::mutex ids_mutex;
    std::vector<std::thread::id> operation_threads;
    const auto owner_thread = std::this_thread::get_id();
    const auto sequence = queue.enqueue(std::move(ticket), [&] {
        std::lock_guard lock(ids_mutex);
        operation_threads.push_back(std::this_thread::get_id());
        return SaveReceipt{captured.document().revision(), std::string(64, 'b'), std::nullopt};
    });
    const auto barrier = queue.enqueue_barrier();
    std::vector<WorkspaceSaveQueue::Completion> completions;
    for (int attempt = 0; attempt != 100 && completions.size() != 2; ++attempt) {
        std::this_thread::sleep_for(1ms);
        auto next = queue.take_completed();
        for (auto& item : next) completions.push_back(std::move(item));
    }
    require(completions.size() == 2, "queue did not deliver save and barrier");
    require(completions[0].sequence == sequence && completions[1].sequence == barrier,
            "completion order did not follow FIFO sequence");
    require(completions[0].succeeded() && completions[1].succeeded(), "successful queue work failed");
    require(completions[0].ticket.has_value() && completions[0].receipt.has_value(),
            "save completion dropped ticket or receipt");
    const auto acknowledged = WorkspaceSaveCoordinator::accept(
        *completions[0].ticket, *completions[0].receipt, captured, binding(), digest);
    require(acknowledged.acknowledged(), "owner-thread acknowledgement rejected queue result");
    require(!operation_threads.empty() && operation_threads.front() != owner_thread,
            "save operation ran on owner thread");
}

void failure_is_captured_and_worker_continues() {
    ProjectWorkspace workspace(fixture().snapshot());
    const auto captured = workspace.capture();
    WorkspaceSaveQueue queue;
    auto first_ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    auto second_ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    (void)queue.enqueue(std::move(first_ticket), []() -> SaveReceipt {
        throw std::runtime_error("injected save failure");
    });
    (void)queue.enqueue(std::move(second_ticket), [captured] {
        return SaveReceipt{captured.document().revision(), std::string(64, 'c'), std::nullopt};
    });
    std::vector<WorkspaceSaveQueue::Completion> completions;
    for (int attempt = 0; attempt != 100 && completions.size() != 2; ++attempt) {
        std::this_thread::sleep_for(1ms);
        auto next = queue.take_completed();
        for (auto& item : next) completions.push_back(std::move(item));
    }
    require(completions.size() == 2, "queue stopped after a failed save");
    require(completions[0].error && !completions[0].succeeded(), "save exception was not captured");
    require(completions[1].succeeded(), "queue did not execute work after failure");
    bool rethrown = false;
    try { std::rethrow_exception(completions[0].error); }
    catch (const std::runtime_error& error) { rethrown = std::string(error.what()) == "injected save failure"; }
    require(rethrown, "captured save exception changed type or message");
}

void shutdown_without_drain_discards_pending_work() {
    WorkspaceSaveQueue queue;
    std::atomic<bool> started = false;
    std::atomic<bool> release = false;
    ProjectWorkspace workspace(fixture().snapshot());
    const auto captured = workspace.capture();
    auto running = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    auto pending = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    (void)queue.enqueue(std::move(running), [&] {
        started = true;
        while (!release) std::this_thread::yield();
        return SaveReceipt{captured.document().revision(), std::string(64, 'd'), std::nullopt};
    });
    (void)queue.enqueue(std::move(pending), [captured] {
        return SaveReceipt{captured.document().revision(), std::string(64, 'e'), std::nullopt};
    });
    for (int attempt = 0; attempt != 100 && !started; ++attempt) std::this_thread::sleep_for(1ms);
    require(started, "running save did not start");
    std::thread stop([&] { queue.shutdown(false); });
    std::this_thread::sleep_for(1ms);
    release = true;
    stop.join();
    const auto completions = queue.take_completed();
    require(completions.size() == 1, "non-draining shutdown ran pending work");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        matching_completion_acknowledges_on_owner_thread();
        failure_is_captured_and_worker_continues();
        shutdown_without_drain_discards_pending_work();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
