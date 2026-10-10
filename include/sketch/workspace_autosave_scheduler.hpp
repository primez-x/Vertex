#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace sketch {

// Owner-thread scheduling policy for recovery publications. It does not own
// a workspace, start a thread, or write a file; callers capture an immutable
// ProjectWorkspaceSnapshot and submit it to WorkspaceSaveQueue after capture.
class WorkspaceAutosaveScheduler final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Capture final {
        std::uint64_t sequence{};
        std::uint64_t edited_generation{};
        std::uint64_t checkpoint_generation{};
        std::uint64_t source_generation{};

        friend bool operator==(const Capture&, const Capture&) = default;
    };

    explicit WorkspaceAutosaveScheduler(
        std::chrono::milliseconds quiet_interval = std::chrono::seconds(2),
        std::chrono::milliseconds maximum_interval = std::chrono::seconds(30));

    // Generations are monotonic workspace counters. Pointer-only checkpoints
    // may advance checkpoint_generation without advancing edited_generation.
    // A regression is rejected rather than silently dropping recovery state.
    void observe(TimePoint now, std::uint64_t edited_generation,
                 std::uint64_t checkpoint_generation,
                 std::uint64_t autosaved_checkpoint_generation);

    // A replaced authoring source may retain every persisted counter. Track
    // that change transiently; only its own successful capture can cover it.
    // Exhaustion latches dirty state and throws rather than wrapping.
    void invalidate_source(TimePoint now);

    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool in_flight() const noexcept;
    [[nodiscard]] bool due(TimePoint now) const;

    // Captures current persisted and transient generations once when due.
    // The value is sealed by value and may be paired with a queue ticket.
    [[nodiscard]] std::optional<Capture> capture(TimePoint now);

    // Completion is accepted only for the current in-flight sequence. A
    // successful stale capture advances the autosaved watermark to its own
    // checkpoint/source generation but never clears newer dirty state.
    // Failures leave the current state dirty and eligible for retry.
    void complete(const Capture&, bool success, TimePoint now);

private:
    std::chrono::milliseconds quiet_interval_;
    std::chrono::milliseconds maximum_interval_;
    std::uint64_t edited_generation_ = 0;
    std::uint64_t checkpoint_generation_ = 0;
    std::uint64_t autosaved_checkpoint_generation_ = 0;
    std::uint64_t source_generation_ = 0;
    std::uint64_t autosaved_source_generation_ = 0;
    bool source_generation_exhausted_ = false;
    std::optional<TimePoint> dirty_since_;
    std::optional<TimePoint> last_change_;
    std::uint64_t next_sequence_ = 1;
    std::optional<Capture> in_flight_capture_;
};

}  // namespace sketch
