#include "sketch/boundary_authoring_recovery.hpp"

#include "support/noninteractive_errors.hpp"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <psapi.h>
#endif

namespace {

using namespace sketch;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool exact_point(Vec2 left, Vec2 right) {
    return std::bit_cast<std::uint64_t>(left.x) == std::bit_cast<std::uint64_t>(right.x) &&
           std::bit_cast<std::uint64_t>(left.y) == std::bit_cast<std::uint64_t>(right.y);
}

void copied_history_isolation() {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first, options);
    (void)source.anchor({0.0, 0.0});
    (void)source.add_line_to({2.0, 0.0});
    const auto old_segment = source.add_line_to({2.0, 3.0});
    require(source.undo(), "copy fixture must have an existing redo tail");
    const auto original = source.recovery_checkpoint();

    auto copy = source;
    const auto source_roots = source.structural_stats();
    const auto copy_roots = copy.structural_stats();
    require(source_roots.history_root_identity == copy_roots.history_root_identity &&
                source_roots.redo_root_identity == copy_roots.redo_root_identity,
            "a copied session must initially share immutable history and redo roots");
    require(copy.redo(), "copied session must retain its independent redo position");
    require(copy.undo(), "copied redo must remain undoable");
    const auto replacement = copy.add_line_to({4.0, 1.0});
    require(replacement != old_segment && !copy.can_redo(),
            "branching a copied session must allocate a fresh identity and truncate its own redo");
    require(source.recovery_checkpoint() == original && source.can_redo(),
            "editing a session copy must not alter retained history in its source");
    const auto branched = copy.recovery_checkpoint();
    source.reset();
    require(copy.recovery_checkpoint() == branched,
            "resetting the source must not destroy history shared by an independent copy");
    auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(branched);
    require(restored.undo() && restored.redo() && restored.recovery_checkpoint() == branched,
            "copied and branched immutable history must survive recovery and navigation");
}

void report_memory(std::string_view stage) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    require(K32GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0,
            "process memory measurement must succeed");
    std::cout << "Memory " << stage << ": private_bytes=" << counters.PrivateUsage
              << " peak_working_set_bytes=" << counters.PeakWorkingSetSize << '\n';
#else
    (void)stage;
#endif
}

void open_draft_recovery(std::size_t edge_count) {
    constexpr std::size_t undone_count = 32;
    const Vec2 anchor{-0.0, -0.0};
    const Vec2 pointer{-0.0, std::nextafter(10.0, 11.0)};
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first, options);
    (void)source.anchor(anchor);
    std::vector<Vec2> endpoints;
    std::vector<std::string> segment_ids;
    endpoints.reserve(edge_count);
    segment_ids.reserve(edge_count);
    const auto start = std::chrono::steady_clock::now();
    const auto structure_before = source.structural_stats();
    report_memory("before-construction");
    for (std::size_t index = 0; index < edge_count; ++index) {
        const Vec2 point{index == 0 ? std::nextafter(1.0, 2.0)
                                   : static_cast<double>(index + 1),
                         index % 2 == 0 ? -0.0 : 1.0};
        endpoints.push_back(point);
        segment_ids.push_back(source.add_line_to(point));
    }
    report_memory("after-construction");
    const auto constructed = source.structural_stats();
    require(constructed.action_count == edge_count + 1 &&
                constructed.active_segment_count == edge_count &&
                constructed.active_receipt_count == edge_count &&
                constructed.active_dimension_count == edge_count,
            "compact roots must retain every action, edge, receipt and automatic dimension");
    require(constructed.live_history_entries == structure_before.live_history_entries + edge_count &&
                constructed.retained_sequence_chunks <= 4 * edge_count &&
                constructed.sequence_chunk_allocations <=
                    structure_before.sequence_chunk_allocations + 4 * edge_count,
            "retained history and new chunk allocations must have a linear per-edge bound");
    const auto highwater = source.counters();
    for (std::size_t index = 0; index < undone_count; ++index) {
        require(source.undo(), "source draft must retain the expected undo depth");
    }
    source.set_pointer(pointer);
    const auto checkpoint = source.recovery_checkpoint();
    const auto encoded = encode_boundary_authoring_recovery(checkpoint).dump();
    const auto decoded = decode_boundary_authoring_recovery(nlohmann::json::parse(encoded));
    report_memory("after-codec");
    require(decoded.supported(), "ordinary multi-hundred-edge draft must remain supported");
    const auto restore_start = std::chrono::steady_clock::now();
    auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(*decoded.checkpoint);
    const auto restore_end = std::chrono::steady_clock::now();
    report_memory("after-restore");
    auto view = restored.view();
    require(view.active_chain && view.accepted_chains.empty(),
            "recovery must preserve the unfinished chain without implicitly closing it");
    require(view.active_chain->segments.size() == edge_count - undone_count &&
                view.active_chain->dimensions.size() == edge_count - undone_count,
            "recovery must restore the current edge and automatic dimension counts");
    require(view.semantic_redo_depth == undone_count && restored.counters() == highwater,
            "recovery must preserve redo depth and all previously allocated identities");
    require(view.pointer && exact_point(*view.pointer, pointer) &&
                exact_point(view.active_chain->anchor, anchor),
            "serialized pointer and anchor must preserve negative zero and adjacent doubles");
    require(restored.recovery_checkpoint() == checkpoint,
            "restored history must re-export the full checkpoint without truncation");

    const auto navigation_structure = restored.structural_stats();
    const auto require_stable_navigation_storage = [&] {
        const auto observed = restored.structural_stats();
        require(observed.live_history_entries == navigation_structure.live_history_entries &&
                    observed.live_sequence_chunks == navigation_structure.live_sequence_chunks &&
                    observed.sequence_chunk_allocations == navigation_structure.sequence_chunk_allocations &&
                    observed.retained_sequence_chunks == navigation_structure.retained_sequence_chunks,
                "undo/redo must reuse the retained history and geometry chunks");
    };

    std::size_t undo_count = 0;
    while (restored.undo()) {
        ++undo_count;
        require_stable_navigation_storage();
    }
    require(undo_count == edge_count - undone_count + 1,
            "restored undo must reach the original state including the anchor action");
    require(!restored.view().active_chain && restored.view().accepted_chains.empty(),
            "complete local undo must not leave geometry behind");
    std::size_t redo_count = 0;
    while (restored.redo()) {
        ++redo_count;
        require_stable_navigation_storage();
    }
    require(redo_count == edge_count + 1,
            "restored redo must include the tail already undone before serialization");
    view = restored.view();
    require(view.active_chain && view.active_chain->segments.size() == edge_count &&
                view.accepted_chains.empty(),
            "redo must restore every open edge without converting it into an accepted area");
    require(view.active_chain->receipts.size() == edge_count,
            "each restored open edge must retain its original construction receipt");
    for (std::size_t index = 0; index < edge_count; ++index) {
        const auto& receipt = view.active_chain->receipts[index];
        require(receipt.kind == BoundaryConstructionKind::line_to_point &&
                    receipt.segment_id == segment_ids[index] && receipt.chord_end &&
                    exact_point(*receipt.chord_end, endpoints[index]),
                "redo must preserve point-native receipt identities and endpoint bits");
        require(exact_point(receipt.start, index == 0 ? anchor : endpoints[index - 1]),
                "receipt starts must match the captured endpoint bits without drift");
    }
    require(view.pointer && exact_point(*view.pointer, pointer) &&
                restored.counters() == highwater,
            "local navigation must not alter the pointer or rewind allocation watermarks");
    const auto end = std::chrono::steady_clock::now();
    report_memory("after-navigation");
    std::cout << "Structure: " << constructed.action_count << " history entries, "
              << constructed.retained_sequence_chunks << " retained chunks\n";
    std::cout << "Open draft: " << edge_count << " edges, " << encoded.size()
              << " encoded bytes; restore "
              << std::chrono::duration_cast<std::chrono::milliseconds>(restore_end - restore_start).count()
              << " ms; construction, codec and full navigation "
              << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
              << " ms\n";
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        // The normal CTest fixture stays fixed. Explicit isolated measurements
        // accept only these bounded sizes, never arbitrary allocation requests.
        std::size_t edge_count = 256;
        if (argc == 2) {
            const std::string_view argument(argv[1]);
            if (argument == "512") edge_count = 512;
            else if (argument == "1024") edge_count = 1024;
            else if (argument == "2048") edge_count = 2048;
            else require(argument == "256", "measurement size must be 256, 512, 1024 or 2048");
        } else {
            require(argc == 1, "at most one measurement size is accepted");
        }
        copied_history_isolation();
        open_draft_recovery(edge_count);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_recovery_scaling_tests: " << error.what() << '\n';
        return 1;
    }
}
