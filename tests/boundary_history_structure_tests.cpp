#include "sketch/boundary_authoring_recovery.hpp"
#include "support/noninteractive_errors.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace sketch;
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

void deep_history_releases_iteratively() {
    // This probes the representation's 100k-action teardown independently of
    // the smaller default wire envelope. It is not a supported-project claim.
    auto policy = boundary_authoring_recovery_default_limits;
    policy.max_json_values = 10'000'000;
    policy.max_string_bytes = 128U * 1024U * 1024U;
    policy.max_encoded_bytes = 256U * 1024U * 1024U;
    policy.max_retained_history_bytes = 1024U * 1024U * 1024U;
    policy.max_operation_bytes = 2U * 1024U * 1024U * 1024U;
    policy.max_materialization_bytes = 1024U * 1024U * 1024U;
    policy.max_cumulative_replay_copy_bytes = 2U * 1024U * 1024U * 1024U;
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first, {}, policy);
    const auto baseline = source.structural_stats();
    constexpr std::size_t count = 100000;
    for (std::size_t index = 0; index < count; ++index) {
        source.set_classification(index % 2 == 0 ? "one" : "two");
    }
    const auto full = source.structural_stats();
    require(full.live_history_entries == baseline.live_history_entries + count &&
                full.retained_sequence_chunks == 0 && full.active_segment_count == 0,
            "topology-free actions must retain one history node each and no geometry chunks");
    auto copy = source;
    for (std::size_t index = 0; index < count; ++index) {
        require(source.undo(), "all deep-history actions must remain undoable");
    }
    require(source.structural_stats().live_history_entries == full.live_history_entries &&
                source.structural_stats().redo_node_count == count,
            "deep undo must create only a redo zipper over the existing entries");
    copy.reset();
    require(source.structural_stats().live_history_entries == full.live_history_entries,
            "a separate copy must not own the lifetime of the remaining redo history");
    source.cancel();
    const auto released = source.structural_stats();
    require(released.live_history_entries == baseline.live_history_entries &&
                released.live_redo_nodes == baseline.live_redo_nodes &&
                released.live_sequence_chunks == baseline.live_sequence_chunks,
            "last-owner cancellation must release the complete deep zipper without leaking nodes");

    source.reset();
    for (std::size_t index = 0; index < count; ++index) {
        source.set_classification(index % 2 == 0 ? "one" : "two");
    }
    source.reset();
    require(source.structural_stats().live_history_entries == baseline.live_history_entries,
            "last-owner reset must release a deep past-only history iteratively");
}

void independent_copies_can_release_shared_chunks_concurrently() {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first, options);
    (void)source.anchor({0.0, 0.0});
    for (std::size_t index = 0; index < 64; ++index) {
        (void)source.add_line_to({static_cast<double>(index + 1), 0.0});
    }
    const auto expected = source.recovery_checkpoint();
    const auto baseline = source.structural_stats();
    std::exception_ptr failures[2];
    const auto exercise = [&](std::size_t worker) {
        try {
            // The original is read-only during both copies. Each thread then
            // mutates only its own session; this does not promise same-session
            // concurrent mutation safety.
            auto copy = source;
            for (std::size_t index = 0; index < 512; ++index) {
                (void)copy.add_line_to({65.0 + static_cast<double>(index),
                                        static_cast<double>(worker + 1)});
            }
            while (copy.undo()) {}
            while (copy.redo()) {}
            copy.reset();
        } catch (...) {
            failures[worker] = std::current_exception();
        }
    };
    std::jthread first(exercise, 0);
    std::jthread second(exercise, 1);
    first.join();
    second.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    require(source.recovery_checkpoint() == expected,
            "independent threaded copies must not mutate the shared source history");
    const auto after = source.structural_stats();
    require(after.live_history_entries == baseline.live_history_entries &&
                after.live_redo_nodes == baseline.live_redo_nodes &&
                after.live_sequence_chunks == baseline.live_sequence_chunks,
            "thread-local branches must release every node beyond the original roots");
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        deep_history_releases_iteratively();
        independent_copies_can_release_shared_chunks_concurrently();
        std::cout << "Deep history teardown and independent shared-root concurrency passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_history_structure_tests: " << error.what() << '\n';
        return 1;
    }
}
