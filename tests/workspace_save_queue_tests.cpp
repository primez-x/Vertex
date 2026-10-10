#include "sketch/workspace_save_queue.hpp"
#include "sketch/workspace_save_coordinator.hpp"
#include "sketch/document_digest.hpp"
#include "support/detached_document_snapshot.hpp"
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

void deferred_publication_and_late_proof_share_fifo() {
    auto document = fixture();
    const auto source = document.snapshot();
    WorkspaceSaveQueue queue;
    std::atomic<int> stage = 0;
    const auto sequence = queue.enqueue_prepared([source, &stage] {
        require(stage.fetch_add(1) == 0, "preparation was reordered");
        auto prepared = ProjectWorkspace::prepare_detached(source);
        const auto snapshot = prepared.capture();
        return WorkspaceSaveQueue::PreparedSave::publish(snapshot, binding(),
            WorkspaceSaveQueue::SourceProof::capture(source), [&] {
                require(stage.fetch_add(1) == 1, "storage preceded preparation");
                return SaveReceipt{source.revision(), std::string(64, 'e'), std::nullopt};
            }, std::move(prepared));
    });
    const auto first_barrier = queue.enqueue_barrier();
    std::vector<WorkspaceSaveQueue::Completion> results;
    for (int attempt = 0; attempt != 1000 && results.size() != 2; ++attempt) {
        auto batch = queue.take_completed();
        for (auto& item : batch) results.push_back(std::move(item));
        if (results.size() != 2) std::this_thread::sleep_for(1ms);
    }
    require(results.size() == 2 && results[0].sequence == sequence &&
        results[1].sequence == first_barrier, "deferred save/barrier order changed");
    require(results[0].succeeded() && results[0].prepared && !results[0].ticket && !results[0].receipt,
        "deferred result was split from its sealed publication");
    auto& publication = *results[0].prepared;
    require(publication.has_workspace() && publication.receipt().file_sha256 == std::string(64, 'e'),
        "prepared result lost candidate or actual receipt");
    require(publication.publication_valid() && !publication.adopt_workspace(),
        "valid deferred file fact refused");
    const auto descriptor = publication.descriptor();
    const auto acknowledgement = publication.acknowledge(descriptor, binding(), document_authoring_source_digest_v2(source));
    require(acknowledgement.acknowledged(), "sealed prepared publication refused");
    auto adopted = publication.adopt_workspace();
    require(adopted && WorkspaceSaveCoordinator::describe(*adopted) == descriptor &&
        !publication.adopt_workspace(), "candidate adoption mismatched or replayed");
    // An owner proof enqueued after the first worker barrier needs its own final
    // barrier. This intentionally reproduces the reviewed interleaving.
    const auto proof = queue.enqueue_task([&] { require(stage.fetch_add(1) == 2, "proof preceded storage"); });
    const auto final_barrier = queue.enqueue_barrier();
    queue.shutdown(true);
    auto tail = queue.take_completed();
    require(tail.size() == 2 && tail[0].sequence == proof && tail[0].kind == WorkspaceSaveQueue::CompletionKind::task &&
        tail[0].succeeded() && tail[1].sequence == final_barrier && stage == 3,
        "final barrier/shutdown did not cover late proof");
}

void deferred_faults_and_mismatched_candidates_refuse_before_storage() {
    const auto source = fixture().snapshot();
    WorkspaceSaveQueue queue;
    std::atomic<int> stored = 0;
    (void)queue.enqueue_prepared([]() -> WorkspaceSaveQueue::PreparedSave {
        throw std::runtime_error("injected preparation failure");
    });
    (void)queue.enqueue_prepared([source, &stored] {
        auto first = ProjectWorkspace::prepare_detached(source);
        auto other = ProjectWorkspace::prepare_detached(source);
        return WorkspaceSaveQueue::PreparedSave::publish(first.capture(), binding(),
            WorkspaceSaveQueue::SourceProof::capture(source), [&] {
                ++stored;
                return SaveReceipt{source.revision(), std::string(64, 'f'), std::nullopt};
            }, std::move(other));
    });
    (void)queue.enqueue_task([] { throw std::runtime_error("injected proof failure"); });
    (void)queue.enqueue_prepared([source, &stored] {
        auto candidate = ProjectWorkspace::prepare_detached(source);
        const auto mismatched = test::DetachedDocumentSnapshotFixture::mutate(source, [](auto& fixture) {
            fixture.entities().at("label").properties["text"] = "Same revision, different full source";
        });
        const auto snapshot = candidate.capture();
        return WorkspaceSaveQueue::PreparedSave::publish(snapshot, binding(),
            WorkspaceSaveQueue::SourceProof::capture(mismatched), [&] {
                ++stored;
                return SaveReceipt{source.revision(), std::string(64, 'e'), std::nullopt};
            }, std::move(candidate));
    });
    (void)queue.enqueue_barrier();
    queue.shutdown(true);
    const auto results = queue.take_completed();
    require(results.size() == 5 && results[0].error && results[1].error && results[2].error && results[3].error &&
        results[4].succeeded() && stored == 0, "preparation/proof failure escaped or touched storage");
    bool refused = false;
    try { (void)queue.enqueue_task([] {}); } catch (const std::logic_error&) { refused = true; }
    require(refused, "proof accepted after shutdown");
    refused = false;
    try { (void)queue.enqueue_prepared({}); } catch (const std::invalid_argument&) { refused = true; }
    require(refused, "empty preparation accepted");
}

void malformed_prepared_receipt_cannot_install_candidate() {
    const auto source = fixture().snapshot();
    auto candidate = ProjectWorkspace::prepare_detached(source);
    const auto snapshot = candidate.capture();
    auto publication = WorkspaceSaveQueue::PreparedSave::publish(snapshot, binding(),
        WorkspaceSaveQueue::SourceProof::capture(source), [&] {
            return SaveReceipt{source.revision() + 1, std::string(64, 'e'), std::nullopt};
        }, std::move(candidate));
    require(!publication.publication_valid() && !publication.adopt_workspace(),
        "malformed prepared receipt granted publication/adoption authority");
    require(publication.acknowledge(publication.descriptor(), binding(),
        document_authoring_source_digest_v2(source)).status == SaveAcknowledgementStatus::invalid_receipt &&
        !publication.adopt_workspace(), "invalid actual receipt installed its prepared candidate");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        matching_completion_acknowledges_on_owner_thread();
        failure_is_captured_and_worker_continues();
        shutdown_without_drain_discards_pending_work();
        deferred_publication_and_late_proof_share_fifo();
        deferred_faults_and_mismatched_candidates_refuse_before_storage();
        malformed_prepared_receipt_cannot_install_candidate();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
