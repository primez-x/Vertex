#include "sketch/boundary_authoring_session.hpp"
#include "support/noninteractive_errors.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
std::atomic_bool fail_next_allocation{false};
const char* active_operation = "allocator setup";

void allocation_probe() {
    if (fail_next_allocation.exchange(false, std::memory_order_relaxed)) {
        std::fprintf(stderr, "allocation probe: %s, inside mutator publication/return\n",
                     active_operation);
        throw std::bad_alloc();
    }
}

void* allocate(std::size_t size) {
    allocation_probe();
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::size_t alignment) {
    allocation_probe();
    void* result = nullptr;
#ifdef _WIN32
    result = _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    if (posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0) result = nullptr;
#endif
    if (result) return result;
    throw std::bad_alloc();
}

void free_aligned(void* pointer) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
} // namespace

// These replacements belong only to this isolated test executable.
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { free_aligned(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(pointer);
}

namespace {
using namespace sketch;

void require(bool condition, const char* message) {
    if (!condition) {
        fail_next_allocation.store(false, std::memory_order_relaxed);
        std::fprintf(stderr, "boundary_authoring_return_safety_tests: %s: %s\n",
                     active_operation, message);
        std::exit(1);
    }
}

template <typename Operation>
auto check_return(BoundaryAuthoringSession& session, const char* label, Operation operation) {
    using Result = std::invoke_result_t<Operation>;
    static_assert(std::is_nothrow_move_constructible_v<Result>);
    const auto checkpoint = session.recovery_checkpoint();
    const auto view = session.view();
    const auto counters = session.counters();
    const auto roots = session.structural_stats();
    active_operation = label;
    bool reached_publication = false;
    session.set_fault_hook([&](BoundaryAuthoringFaultPoint point) {
        if (point == BoundaryAuthoringFaultPoint::before_publication) {
            reached_publication = true;
            fail_next_allocation.store(true, std::memory_order_relaxed);
        }
    });
    std::optional<Result> result;
    bool threw = false;
    bool allocation_unconsumed = false;
    try {
        // The operation lambda returns a prvalue directly. C++17 mandatory
        // elision initializes this object without an extra harness move, even
        // when optional NRVO in the mutator is disabled by the compiler.
        Result returned = operation();
        allocation_unconsumed =
            fail_next_allocation.exchange(false, std::memory_order_relaxed);
        // Debug STL moves may allocate iterator proxies. This harness move
        // must happen only after the mutator-return probe is disarmed.
        result.emplace(std::move(returned));
    } catch (...) {
        fail_next_allocation.store(false, std::memory_order_relaxed);
        threw = true;
    }
    // Never let the probe leak into assertions, diagnostics or snapshot copies.
    session.clear_fault_hook();
    if (threw) {
        const auto after = session.structural_stats();
        require(session.recovery_checkpoint() == checkpoint && session.view() == view &&
                    session.counters() == counters &&
                    after.history_root_identity == roots.history_root_identity &&
                    after.redo_root_identity == roots.redo_root_identity &&
                    after.action_count == roots.action_count &&
                    after.retained_sequence_chunks == roots.retained_sequence_chunks &&
                    after.live_history_entries == roots.live_history_entries &&
                    after.live_redo_nodes == roots.live_redo_nodes &&
                    after.live_sequence_chunks == roots.live_sequence_chunks,
                "an exception after publication changed the checkpoint, view, counters or roots");
    }
    require(reached_publication, "operation did not reach publication");
    if (threw) {
        // A return-object copy may allocate before a scope-success guard
        // publishes. The unchanged-state checks above prove that this is a
        // safe rejected transaction. Retrying without the allocator fault
        // must then commit exactly once.
        result.emplace(operation());
    } else {
        require(allocation_unconsumed, "an armed allocation was silently consumed");
    }
    require(session.structural_stats().action_count == roots.action_count + 1,
            "successful return did not commit exactly one action");
    return std::move(*result);
}

BoundaryAuthoringOptions long_ids(bool automatic = false) {
    BoundaryAuthoringOptions options;
    options.boundary_id_prefix = "boundary_with_a_long_identifier_prefix";
    options.segment_id_prefix = "segment_with_a_long_identifier_prefix";
    options.dimension_id_prefix = "dimension_with_a_long_identifier_prefix";
    options.automatic_dimension_placement = automatic;
    return options;
}

void test_anchor_edge_dimensions() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::define_first, long_ids());
    session.set_classification("living_area");
    const auto boundary = check_return(session, "anchor", [&] { return session.anchor({0, 0}); });
    require(boundary.size() > 32 && session.active_chain()->boundary_id == boundary,
            "anchor returned the wrong long identity");
    const auto distance = parse_quantity("4 m", Unit::metre);
    const auto edge = check_return(session, "edge", [&] { return session.add_line(distance, 0.0); });
    require(edge.size() > 32 && session.active_chain()->segments.back().segment_id == edge,
            "edge returned the wrong long identity");
    const auto manual = check_return(session, "manual dimension", [&] { return session.place_manual_dimension({2, 1}); });
    require(manual == session.active_chain()->dimensions.back(), "manual dimension return differs");
    (void)session.add_line(distance, 1.0);
    const auto automatic = check_return(session, "automatic dimension", [&] { return session.place_automatic_dimension(); });
    require(automatic == session.active_chain()->dimensions.back(), "automatic dimension return differs");
}

void test_accepted_chain() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, long_ids(true));
    (void)session.anchor({0, 0});
    (void)session.add_line_to({4, 0});
    (void)session.add_line_to({4, 3});
    (void)session.add_line_to({0, 3});
    (void)session.add_line_to({0, 0});
    const auto accepted = check_return(session, "accepted chain", [&] { return session.close_chain(); });
    require(accepted == session.accepted_chains().back(), "accepted chain return differs");
}

void test_pointer_anchor_forwarding() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, long_ids());
    session.set_pointer({1.0, 2.0});
    const auto identity = check_return(session, "pointer anchor wrapper", [&] { return session.anchor(); });
    require(identity == session.active_chain()->boundary_id,
            "pointer anchor wrapper must forward the prepared result exactly");
}
void test_restore_publication() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first, long_ids(true));
    (void)source.anchor({3, 4});
    (void)source.add_line_to({7, 4});
    auto checkpoint = source.recovery_checkpoint();
    checkpoint.extensions = nlohmann::json::parse(
        R"({"nested":[{"text":"retained extension payload","values":[null,true,-9223372036854775808,9223372036854775807,18446744073709551615,0.1,-0.0,1.7976931348623157e308,5e-324]}]})");
    checkpoint.extensions["signed_positive"] = std::int64_t{42};
    BoundaryAuthoringSession target(BoundaryAuthoringMode::define_first);
    auto previous = checkpoint;
    previous.extensions["retired"] = nlohmann::json::array({1, 2, 3});
    target.restore_recovery_checkpoint(previous);
    require(target.undo(), "restore setup must retain a redo branch");
    active_operation = "restore publication";
    bool reached_publication = false;
    target.set_fault_hook([&](BoundaryAuthoringFaultPoint point) {
        if (point == BoundaryAuthoringFaultPoint::before_publication) {
            reached_publication = true;
            fail_next_allocation.store(true, std::memory_order_relaxed);
        }
    });
    target.restore_recovery_checkpoint(checkpoint);
    const bool allocation_unconsumed =
        fail_next_allocation.exchange(false, std::memory_order_relaxed);
    target.clear_fault_hook();
    require(reached_publication && allocation_unconsumed,
            "restore publication must not allocate");
    require(target.recovery_checkpoint() == checkpoint,
            "restore did not publish the complete checkpoint");
    require(target.recovery_checkpoint().extensions.dump() == checkpoint.extensions.dump(),
            "restore changed the extension wire representation");
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_anchor_edge_dimensions();
        test_accepted_chain();
        test_pointer_anchor_forwarding();
        test_restore_publication();
    } catch (const std::exception& error) {
        fail_next_allocation.store(false, std::memory_order_relaxed);
        std::fprintf(stderr, "boundary_authoring_return_safety_tests: %s\n", error.what());
        return 1;
    }
    std::puts("boundary_authoring_return_safety_tests passed");
}
