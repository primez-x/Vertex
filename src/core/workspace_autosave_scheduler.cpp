#include "sketch/workspace_autosave_scheduler.hpp"

#include <algorithm>
#include <stdexcept>

namespace sketch {

WorkspaceAutosaveScheduler::WorkspaceAutosaveScheduler(
    std::chrono::milliseconds quiet_interval, std::chrono::milliseconds maximum_interval)
    : quiet_interval_(quiet_interval), maximum_interval_(maximum_interval) {
    if (quiet_interval_.count() <= 0 || maximum_interval_.count() <= 0 ||
        quiet_interval_ > maximum_interval_)
        throw std::invalid_argument("autosave intervals must be positive and ordered");
}

void WorkspaceAutosaveScheduler::observe(TimePoint now, std::uint64_t edited_generation,
                                         std::uint64_t checkpoint_generation,
                                         std::uint64_t autosaved_checkpoint_generation) {
    if (edited_generation < edited_generation_ || checkpoint_generation < checkpoint_generation_ ||
        autosaved_checkpoint_generation < autosaved_checkpoint_generation_ ||
        autosaved_checkpoint_generation > checkpoint_generation)
        throw std::invalid_argument("autosave generations are not monotonic");
    const bool changed = edited_generation != edited_generation_ ||
        checkpoint_generation != checkpoint_generation_ ||
        autosaved_checkpoint_generation != autosaved_checkpoint_generation_;
    edited_generation_ = edited_generation;
    checkpoint_generation_ = checkpoint_generation;
    autosaved_checkpoint_generation_ = autosaved_checkpoint_generation;
    if (changed) {
        last_change_ = now;
        if (dirty() && !dirty_since_) dirty_since_ = now;
        if (!dirty()) dirty_since_.reset();
    }
}

bool WorkspaceAutosaveScheduler::dirty() const noexcept {
    return checkpoint_generation_ > autosaved_checkpoint_generation_;
}

bool WorkspaceAutosaveScheduler::in_flight() const noexcept {
    return in_flight_capture_.has_value();
}

bool WorkspaceAutosaveScheduler::due(TimePoint now) const {
    if (!dirty() || in_flight_capture_ || !last_change_ || !dirty_since_) return false;
    return now - *last_change_ >= quiet_interval_ || now - *dirty_since_ >= maximum_interval_;
}

std::optional<WorkspaceAutosaveScheduler::Capture> WorkspaceAutosaveScheduler::capture(TimePoint now) {
    if (!due(now)) return std::nullopt;
    Capture result{next_sequence_++, edited_generation_, checkpoint_generation_};
    in_flight_capture_ = result;
    return result;
}

void WorkspaceAutosaveScheduler::complete(const Capture& capture, bool success, TimePoint now) {
    if (!in_flight_capture_ || *in_flight_capture_ != capture)
        throw std::invalid_argument("autosave completion does not match in-flight capture");
    in_flight_capture_.reset();
    if (success) {
        // Workspace counters are monotonic, so this also handles a stale
        // completion that finished after a newer pointer or semantic change.
        autosaved_checkpoint_generation_ = std::max(
            autosaved_checkpoint_generation_, capture.checkpoint_generation);
    }
    if (!dirty()) {
        dirty_since_.reset();
        last_change_.reset();
    } else {
        // A failed or stale completion must not postpone the maximum interval.
        if (!dirty_since_) dirty_since_ = now;
        if (!last_change_) last_change_ = now;
    }
}

}  // namespace sketch
