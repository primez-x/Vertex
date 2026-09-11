#include "sketch/workspace_autosave_scheduler.hpp"
#include "support/noninteractive_errors.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void quiet_debounce_and_successful_completion() {
    WorkspaceAutosaveScheduler scheduler;
    const auto start = WorkspaceAutosaveScheduler::TimePoint{};
    scheduler.observe(start, 1, 1, 0);
    require(scheduler.dirty() && !scheduler.due(start + 1999ms), "quiet debounce fired early");
    require(scheduler.due(start + 2s), "quiet debounce did not fire");
    const auto capture = scheduler.capture(start + 2s);
    require(capture && scheduler.in_flight(), "due capture was not sealed");
    scheduler.complete(*capture, true, start + 2s);
    require(!scheduler.dirty() && !scheduler.in_flight(), "successful autosave stayed dirty");
}

void stale_completion_retains_newer_dirty_state() {
    WorkspaceAutosaveScheduler scheduler;
    const auto start = WorkspaceAutosaveScheduler::TimePoint{};
    scheduler.observe(start, 1, 1, 0);
    auto capture = scheduler.capture(start + 2s);
    require(capture.has_value(), "stale fixture did not capture");
    scheduler.observe(start + 2100ms, 1, 2, 0);
    scheduler.complete(*capture, true, start + 2200ms);
    require(scheduler.dirty() && !scheduler.due(start + 3s),
            "stale completion cleared or immediately re-fired newer state");
    require(scheduler.due(start + 4100ms), "newer pointer state missed quiet debounce");
}

void maximum_interval_and_failure_retry() {
    WorkspaceAutosaveScheduler scheduler;
    const auto start = WorkspaceAutosaveScheduler::TimePoint{};
    scheduler.observe(start, 1, 1, 0);
    scheduler.observe(start + 10s, 2, 2, 0);
    require(!scheduler.due(start + 11s), "maximum interval ignored quiet reset");
    require(scheduler.due(start + 40s), "maximum interval did not force a flush");
    auto capture = scheduler.capture(start + 40s);
    require(capture.has_value(), "maximum interval capture missing");
    scheduler.complete(*capture, false, start + 40s);
    require(scheduler.dirty() && scheduler.due(start + 42s), "failed autosave was not retryable");
}

void malformed_transitions_are_rejected() {
    bool rejected = false;
    try { WorkspaceAutosaveScheduler invalid(3s, 2s); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "inverted intervals accepted");
    WorkspaceAutosaveScheduler scheduler;
    const auto start = WorkspaceAutosaveScheduler::TimePoint{};
    scheduler.observe(start, 2, 2, 0);
    rejected = false;
    try { scheduler.observe(start + 1s, 1, 2, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "generation regression accepted");
    scheduler.observe(start + 2s, 2, 2, 1);
    rejected = false;
    try { scheduler.observe(start + 3s, 2, 2, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "autosaved watermark regression accepted");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        quiet_debounce_and_successful_completion();
        stale_completion_retains_newer_dirty_state();
        maximum_interval_and_failure_retry();
        malformed_transitions_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
