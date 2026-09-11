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

    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool in_flight() const noexcept;
    [[nodiscard]] bool due(TimePoint now) const;

    // Captures the current generations exactly once when due. The returned
    // value is sealed by value and may be paired with a queue ticket.
    [[nodiscard]] std::optional<Capture> capture(TimePoint now);

    // Completion is accepted only for the current in-flight sequence. A
    // successful stale capture advances the autosaved watermark to its own
    // checkpoint but never clears newer dirty state. Failures leave the
    // current state dirty and eligible for retry.
    void complete(const Capture&, bool success, TimePoint now);

private:
    std::chrono::milliseconds quiet_interval_;
    std::chrono::milliseconds maximum_interval_;
    std::uint64_t edited_generation_ = 0;
    std::uint64_t checkpoint_generation_ = 0;
    std::uint64_t autosaved_checkpoint_generation_ = 0;
    std::optional<TimePoint> dirty_since_;
    std::optional<TimePoint> last_change_;
    std::uint64_t next_sequence_ = 1;
    std::optional<Capture> in_flight_capture_;
};

}  // namespace sketch
