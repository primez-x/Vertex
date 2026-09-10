#pragma once

#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_authoring_resource_policy.hpp"
#include "sketch/detail/persistent_sequence.hpp"
#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// This is a non-UI semantic authoring surface. A session owns only an
// unfinished drawing and the completed chains accepted by that drawing; it
// never observes or mutates a Document.
enum class BoundaryAuthoringMode { draw_first, define_first };

enum class BoundaryAuthoringPhase {
    awaiting_classification,
    awaiting_anchor,
    drawing,
    awaiting_dimension,
    completed,
    cancelled,
};

enum class BoundaryPenState { up, down };

struct PendingBoundaryDimension {
    std::string boundary_id;
    std::string segment_id;

    bool operator==(const PendingBoundaryDimension&) const = default;
};

struct BoundaryAuthoringOptions {
    std::string default_boundary_type{"measurement_boundary"};
    std::string boundary_id_prefix{"boundary"};
    std::string vertex_id_prefix{"vertex"};
    std::string segment_id_prefix{"segment"};
    std::string dimension_id_prefix{"dimension"};
    bool automatic_dimension_placement{};
    std::uint32_t automatic_placement_version{1};
    double geometry_tolerance_metres{default_geometry_tolerance_metres};
    bool operator==(const BoundaryAuthoringOptions&) const = default;
};

// These are next-ID values, rather than allocation counts. They are retained
// across local undo, redo, reset and branch truncation so an identity that was
// once handed out is never silently reused.
struct BoundaryAuthoringCounters {
    std::uint64_t next_boundary_id{1};
    std::uint64_t next_vertex_id{1};
    std::uint64_t next_segment_id{1};
    std::uint64_t next_dimension_id{1};

    bool operator==(const BoundaryAuthoringCounters&) const = default;
};

struct AcceptedBoundaryChain {
    // The captured anchor is part of the normalized construction record. It
    // remains available after the active draft has been moved to acceptance.
    Vec2 anchor{};
    IdentifiedBoundary boundary;
    std::string classification;
    std::vector<BoundaryDimension> dimensions;
    std::vector<ConstructionReceipt> receipts;
    // Draw First may close measured linework before its final area
    // classification. The recognized boundary entity type remains the
    // session option; classify_last_chain() fills this independent field.
    bool classified{true};

    bool operator==(const AcceptedBoundaryChain& other) const noexcept;
};

struct BoundaryDraftChain {
    std::string boundary_id;
    std::string type;
    std::string classification;
    Vec2 anchor{};
    std::vector<IdentifiedSegment> segments;
    std::vector<BoundaryDimension> dimensions;
    std::vector<ConstructionReceipt> receipts;
    std::vector<PendingBoundaryDimension> pending_dimensions;
    bool classified{};

    bool operator==(const BoundaryDraftChain& other) const noexcept;
};

// view() and snapshot() return copies. In particular, a caller cannot mutate
// the session by changing a returned vector or by retaining a reference to a
// returned geometry object. pending_dimension is a convenience copy of the
// first member of pending_dimensions.
struct BoundaryAuthoringState {
    BoundaryAuthoringMode mode{BoundaryAuthoringMode::draw_first};
    std::string identity_namespace;
    BoundaryAuthoringPhase phase{BoundaryAuthoringPhase::awaiting_anchor};
    BoundaryPenState pen_state{BoundaryPenState::up};
    std::optional<Vec2> pointer;
    std::optional<Vec2> anchor;
    std::optional<std::string> classification;
    std::optional<BoundaryDraftChain> active_chain;
    std::optional<PendingBoundaryDimension> pending_dimension;
    std::vector<PendingBoundaryDimension> pending_dimensions;
    std::vector<AcceptedBoundaryChain> accepted_chains;
    std::size_t semantic_undo_depth{};
    std::size_t semantic_redo_depth{};

    bool operator==(const BoundaryAuthoringState& other) const noexcept;
};

// One normalized semantic transition. The action timeline contains only
// semantic operations; pointer motion is intentionally kept in the enclosing
// checkpoint. Resulting receipts, dimensions and topology are retained where
// needed to compare replay output from the authoritative operations.
enum class BoundaryAuthoringActionKind {
    set_classification,
    classify_current_chain,
    classify_last_chain,
    anchor,
    pen_up,
    pen_down,
    line_heading,
    line_rise_run,
    line_relative_turn,
    line_closure,
    line_to_point,
    arc_chord_angle,
    arc_chord_height,
    arc_chord_length,
    arc_start_tangent,
    manual_dimension,
    automatic_dimension,
    close_chain,
};

// A close action stores construction receipts and stable topology identities,
// but no displayed Segment geometry. Geometry is always rebuilt by the
// receipt replay authority during restore.
struct BoundaryAuthoringChainRecord {
    Vec2 anchor{};
    std::string boundary_id;
    std::string type;
    std::string classification;
    std::vector<ConstructionTopologyEdge> edges;
    std::vector<BoundaryDimension> dimensions;
    bool classified{true};

    bool operator==(const BoundaryAuthoringChainRecord&) const noexcept;
};

struct BoundaryAuthoringAction {
    BoundaryAuthoringActionKind kind{};
    BoundaryAuthoringCounters counters_before{};
    BoundaryAuthoringCounters counters_after{};
    std::vector<std::string> generated_ids;
    std::optional<std::string> classification;
    std::optional<Vec2> point;
    std::optional<ConstructionReceipt> receipt;
    std::optional<BoundaryDimension> dimension;
    std::optional<BoundaryAuthoringChainRecord> chain;

    bool operator==(const BoundaryAuthoringAction&) const noexcept;
};

inline constexpr std::uint32_t boundary_authoring_recovery_version = 1;
inline constexpr std::uint32_t boundary_authoring_recovery_replay_version = 1;
// Compatibility constant for the default linear-action plus quadratic-closure
// work ceiling. Sessions and typed recovery use the stored resource policy.
inline constexpr std::size_t boundary_authoring_recovery_max_replay_work = 20'000'000;

struct BoundaryAuthoringCheckpoint {
    std::uint32_t version{boundary_authoring_recovery_version};
    std::uint32_t replay_version{boundary_authoring_recovery_replay_version};
    BoundaryAuthoringMode mode{BoundaryAuthoringMode::draw_first};
    BoundaryAuthoringOptions options{};
    std::string identity_namespace;
    std::optional<Vec2> pointer;
    std::vector<BoundaryAuthoringAction> actions;
    std::size_t history_position{};
    BoundaryAuthoringCounters counters{};
    nlohmann::json extensions = nlohmann::json::object();

    bool operator==(const BoundaryAuthoringCheckpoint&) const noexcept;
};

enum class BoundaryAuthoringFaultPoint {
    before_semantic_mutation,
    before_action_append,
    before_publication,
};

using BoundaryAuthoringFaultHook =
    std::function<void(BoundaryAuthoringFaultPoint)>;

// Read-only structural evidence for compact-history regression tests. Live and
// allocation counts cover this process's boundary-authoring sequence
// specializations, so tests compare them with a captured baseline.
struct BoundaryAuthoringStructuralStats {
    std::size_t action_count{};
    std::size_t history_position{};
    std::size_t redo_node_count{};
    std::size_t retained_sequence_chunks{};
    std::size_t active_segment_count{};
    std::size_t active_receipt_count{};
    std::size_t active_dimension_count{};
    std::size_t active_pending_dimension_count{};
    std::size_t accepted_chain_count{};
    // Process-wide instrumentation below is intended only for isolated test
    // deltas. Resource admission must use the per-session fields above.
    std::size_t live_history_entries{};
    std::size_t live_redo_nodes{};
    std::size_t live_sequence_chunks{};
    std::size_t sequence_chunk_allocations{};
    const void* history_root_identity{};
    const void* redo_root_identity{};
};

class BoundaryAuthoringSession final {
public:
    explicit BoundaryAuthoringSession(
        BoundaryAuthoringMode mode,
        BoundaryAuthoringOptions options = {},
        BoundaryAuthoringResourcePolicy policy = boundary_authoring_default_resource_policy);

    BoundaryAuthoringSession(const BoundaryAuthoringSession&) = default;
    BoundaryAuthoringSession& operator=(const BoundaryAuthoringSession&) = default;
    BoundaryAuthoringSession(BoundaryAuthoringSession&&) noexcept = default;
    BoundaryAuthoringSession& operator=(BoundaryAuthoringSession&&) noexcept = default;
    ~BoundaryAuthoringSession() = default;

    [[nodiscard]] BoundaryAuthoringMode mode() const noexcept;
    [[nodiscard]] BoundaryAuthoringOptions options() const;
    [[nodiscard]] BoundaryAuthoringPhase phase() const noexcept;
    [[nodiscard]] BoundaryPenState pen_state() const noexcept;
    [[nodiscard]] BoundaryAuthoringState view() const;
    [[nodiscard]] BoundaryAuthoringState snapshot() const;
    [[nodiscard]] std::vector<AcceptedBoundaryChain> accepted_chains() const;
    [[nodiscard]] std::optional<BoundaryDraftChain> active_chain() const;
    [[nodiscard]] std::optional<PendingBoundaryDimension> pending_dimension() const;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] BoundaryAuthoringCounters counters() const noexcept;
    [[nodiscard]] BoundaryAuthoringStructuralStats structural_stats() const noexcept;
    [[nodiscard]] const BoundaryAuthoringResourcePolicy& resource_policy() const noexcept;
    [[nodiscard]] BoundaryAuthoringResourceUsage resource_usage() const noexcept;

    // Export and restore are document independent. Restore replays the
    // normalized action timeline through the same canonical authoring methods
    // used by interactive callers, then positions local history at the saved
    // history_position. It never closes an open chain or invokes finish.
    [[nodiscard]] BoundaryAuthoringCheckpoint recovery_checkpoint() const;
    [[nodiscard]] BoundaryAuthoringCheckpoint export_recovery_checkpoint() const {
        return recovery_checkpoint();
    }
    void restore_recovery_checkpoint(const BoundaryAuthoringCheckpoint& checkpoint);
    [[nodiscard]] static BoundaryAuthoringSession from_recovery_checkpoint(
        const BoundaryAuthoringCheckpoint& checkpoint,
        BoundaryAuthoringResourcePolicy policy = boundary_authoring_default_resource_policy);
    // Validate the original checkpoint, then replay its measurements into a
    // fresh namespace. Preserves local undo/redo and allocation high-water marks;
    // regenerated topology and dimension IDs never alias the original session.
    [[nodiscard]] static BoundaryAuthoringSession revise_recovery_checkpoint(
        const BoundaryAuthoringCheckpoint& checkpoint,
        BoundaryAuthoringResourcePolicy policy = boundary_authoring_default_resource_policy);

    // Fault injection is a test-only seam for proving the transaction's strong
    // exception guarantee. Hooks are never serialized and are not invoked by
    // pointer-only updates.
    void set_fault_hook(BoundaryAuthoringFaultHook hook);
    void clear_fault_hook() noexcept;

    // Pointer motion is intentionally ephemeral and never enters semantic
    // undo history. It still validates coordinates so invalid world points
    // cannot leak into a later anchor operation.
    void set_pointer(Vec2 world_position);

    // Define First uses this before anchor(). Draw First can set it before or
    // after closing measured linework; classify_last_chain() handles the
    // latter case explicitly.
    void set_classification(std::string category);
    void classify_current_chain(std::string category);
    void classify_last_chain(std::string category);

    [[nodiscard]] std::string anchor(Vec2 world_position);
    [[nodiscard]] std::string anchor();
    void pen_up();
    void pen_down();

    // Straight line entry. Heading is absolute world orientation. Rise/run
    // are world-coordinate components, and relative turn is measured from
    // the preceding edge's analytical end tangent.
    [[nodiscard]] std::string add_line(const Quantity& distance,
                                       const AngleInput& heading);
    [[nodiscard]] std::string add_line(const Quantity& distance, double heading_radians);
    // Point-native line entry copies the measured endpoint exactly. Its
    // receipt has no synthetic Quantity or AngleInput fields.
    [[nodiscard]] std::string add_line_to(Vec2 end);
    [[nodiscard]] std::string add_line_rise_run(const Quantity& rise, const Quantity& run);
    [[nodiscard]] std::string add_line_relative_turn(const Quantity& distance,
                                                     const AngleInput& turn);
    [[nodiscard]] std::string add_line_relative_turn(const Quantity& distance,
                                                     double turn_radians);

    // The endpoint is measured world geometry. Each overload delegates to
    // the corresponding canonical constructor in geometry.hpp.
    [[nodiscard]] std::string add_arc_chord_angle(Vec2 end,
                                                  const AngleInput& sweep);
    [[nodiscard]] std::string add_arc_chord_angle(Vec2 end, double sweep_radians);
    [[nodiscard]] std::string add_arc_chord_height(Vec2 end, const Quantity& signed_height);
    [[nodiscard]] std::string add_arc_chord_arc_length(Vec2 end,
                                                       const Quantity& arc_length,
                                                       bool clockwise = false);
    [[nodiscard]] std::string add_arc_start_tangent(const AngleInput& tangent,
                                                    const Quantity& arc_length,
                                                    const AngleInput& sweep);
    [[nodiscard]] std::string add_arc_start_tangent(const Quantity& arc_length,
                                                    const AngleInput& tangent,
                                                    const AngleInput& sweep);
    [[nodiscard]] std::string add_arc_start_tangent(double tangent_radians,
                                                    const Quantity& arc_length,
                                                    double sweep_radians);

    // Define First requires an explicit dimension phase for every edge unless
    // automatic placement is enabled. Draw First measured linework has no
    // pending dimensions by default; enabling automatic placement emits the
    // deterministic version-one dimension for each edge.
    [[nodiscard]] BoundaryDimension place_manual_dimension(Vec2 world_position);
    [[nodiscard]] BoundaryDimension place_automatic_dimension();

    // close_chain requires all pending dimensions to be resolved, then performs
    // exact endpoint joining and strict analytical simple-cycle validation. It
    // never snaps or otherwise changes coordinates.
    [[nodiscard]] AcceptedBoundaryChain close_chain();
    // Adds a real, receipt-bearing final line to the captured anchor. The
    // endpoint is the existing anchor and is never used to snap an earlier
    // segment. Define First still requires placing the new edge's dimension
    // before close_chain().
    [[nodiscard]] std::string add_closing_segment();

    // These operations only affect this local session. Undo/redo restores
    // semantic snapshots while retaining monotonic ID allocation, and pointer
    // motion remains untouched by either operation.
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    void discard_redo_branch() noexcept;
    void cancel() noexcept;
    void reset() noexcept;

private:
    using SharedString = std::shared_ptr<const std::string>;
    template <typename T>
    using Sequence = detail::PersistentSequence<T>;

    struct IdCounters {
        std::uint64_t boundary{1};
        std::uint64_t vertex{1};
        std::uint64_t segment{1};
        std::uint64_t dimension{1};
    };

    struct CompactDraftChain {
        SharedString boundary_id;
        SharedString type;
        SharedString classification;
        Vec2 anchor{};
        Sequence<IdentifiedSegment> segments;
        Sequence<BoundaryDimension> dimensions;
        Sequence<ConstructionReceipt> receipts;
        Sequence<PendingBoundaryDimension> pending_dimensions;
        bool classified{};
    };

    struct CompactAcceptedChain {
        Vec2 anchor{};
        SharedString boundary_id;
        SharedString type;
        SharedString classification;
        Sequence<IdentifiedSegment> segments;
        Sequence<BoundaryDimension> dimensions;
        Sequence<ConstructionReceipt> receipts;
        bool classified{true};
    };

    struct SemanticRoot {
        std::optional<CompactDraftChain> active_chain;
        Sequence<CompactAcceptedChain> accepted_chains;
        BoundaryAuthoringPhase phase{BoundaryAuthoringPhase::awaiting_anchor};
        BoundaryPenState pen_state{BoundaryPenState::up};
        SharedString classification;
    };

    struct HistoryEntry final {
        BoundaryAuthoringAction action;
        SemanticRoot semantic;
        std::size_t position{};
        std::size_t retained_sequence_chunks{};
        BoundaryAuthoringResourceUsage usage{};
        mutable std::shared_ptr<const HistoryEntry> previous;

        HistoryEntry(BoundaryAuthoringAction source_action, SemanticRoot source_semantic,
                     std::size_t source_position, std::size_t source_retained_sequence_chunks,
                     std::shared_ptr<const HistoryEntry> source_previous);
        ~HistoryEntry();
    };

    struct RedoNode final {
        std::shared_ptr<const HistoryEntry> entry;
        mutable std::shared_ptr<const RedoNode> next;
        std::size_t depth{};
        std::size_t retained_sequence_chunks{};
        BoundaryAuthoringResourceUsage usage{};

        RedoNode(std::shared_ptr<const HistoryEntry> source_entry,
                 std::shared_ptr<const RedoNode> source_next);
        ~RedoNode();
    };

    struct MutationCandidate {
        SemanticRoot semantic;
        std::shared_ptr<const HistoryEntry> history;
        std::shared_ptr<const RedoNode> redo;
        IdCounters ids;
    };

    // Constructed after the prepared candidate. A returned copy is initialized
    // before local destruction; a throwing copy therefore skips publication.
    class PublishOnSuccess final {
    public:
        PublishOnSuccess(BoundaryAuthoringSession& owner, MutationCandidate& candidate) noexcept
            : owner_(owner), candidate_(candidate), exceptions_(std::uncaught_exceptions()) {}
        PublishOnSuccess(const PublishOnSuccess&) = delete;
        PublishOnSuccess& operator=(const PublishOnSuccess&) = delete;
        ~PublishOnSuccess() noexcept {
            if (std::uncaught_exceptions() == exceptions_)
                owner_.publish_candidate(std::move(candidate_));
        }
    private:
        BoundaryAuthoringSession& owner_;
        MutationCandidate& candidate_;
        int exceptions_;
    };

    BoundaryAuthoringMode mode_{BoundaryAuthoringMode::draw_first};
    BoundaryAuthoringOptions options_;
    BoundaryAuthoringResourcePolicy resource_policy_;
    BoundaryAuthoringResourceUsage base_resource_usage_;
    std::string identity_namespace_;
    std::optional<Vec2> pointer_;
    SemanticRoot semantic_;
    std::shared_ptr<const HistoryEntry> history_;
    std::shared_ptr<const RedoNode> redo_;
    std::uint64_t next_boundary_id_{1};
    std::uint64_t next_vertex_id_{1};
    std::uint64_t next_segment_id_{1};
    std::uint64_t next_dimension_id_{1};
    // Retired state must be destructible without allocation. The JSON DOM's
    // destructor uses a heap traversal stack, so retain validated wire text.
    std::string recovery_extensions_{"{}"};
    BoundaryAuthoringFaultHook fault_hook_;

    [[nodiscard]] SemanticRoot initial_semantic() const noexcept;
    [[nodiscard]] static bool same_semantic_root(const SemanticRoot& left,
                                                 const SemanticRoot& right) noexcept;
    [[nodiscard]] static BoundaryDraftChain materialize(const CompactDraftChain& source);
    [[nodiscard]] static AcceptedBoundaryChain materialize(
        const CompactAcceptedChain& source);
    [[nodiscard]] static SharedString shared_string(std::string value);
    [[nodiscard]] static std::size_t count_new_sequence_chunks(
        const SemanticRoot& current, const SemanticRoot& candidate) noexcept;
    [[nodiscard]] std::size_t history_position() const noexcept;
    [[nodiscard]] std::size_t new_retained_bytes(const SemanticRoot& candidate) const;
    [[nodiscard]] std::size_t redo_depth() const noexcept;
    void commit_semantic(SemanticRoot candidate, IdCounters ids,
                         BoundaryAuthoringAction action);
    [[nodiscard]] MutationCandidate prepare_semantic(SemanticRoot candidate, IdCounters ids,
                                                      BoundaryAuthoringAction action);
    void set_classification_impl(std::string category,
                                 BoundaryAuthoringActionKind action_kind);
    void invoke_fault(BoundaryAuthoringFaultPoint point) const;
    void publish_candidate(MutationCandidate&& candidate) noexcept;
    void publish_restored(BoundaryAuthoringSession&& candidate) noexcept;
    [[nodiscard]] BoundaryAuthoringCounters public_counters() const noexcept;
    [[nodiscard]] static BoundaryAuthoringCounters public_counters(
        const IdCounters& ids) noexcept;
    void set_counters(const BoundaryAuthoringCounters& counters) noexcept;
    void apply_recovery_action(const BoundaryAuthoringAction& action);
    [[nodiscard]] std::string append_edge(SemanticRoot& semantic, IdCounters& ids,
                                          Segment segment, ConstructionReceipt receipt,
                                          std::vector<std::string>* generated_ids = nullptr);
};

}  // namespace sketch
