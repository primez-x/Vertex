#include "sketch/boundary_authoring_session.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sketch {

namespace {

constexpr std::uint32_t automatic_placement_version = 1;

// Returned same-type copy prvalues use mandatory elision. PublishOnSuccess
// commits only after their construction, including Debug iterator proxies.

std::atomic_size_t live_history_entries;
std::atomic_size_t live_redo_nodes;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool finite_point(Vec2 point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    return left.x == right.x && left.y == right.y;
}

double point_distance(Vec2 left, Vec2 right) {
    const auto result = std::hypot(left.x - right.x, left.y - right.y);
    if (!std::isfinite(result)) invalid("world point distance is not finite");
    return result;
}

void require_point(Vec2 point, std::string_view label) {
    if (!finite_point(point)) invalid(std::string(label) + " must be finite");
}

void require_identifier_prefix(std::string_view prefix, std::string_view label) {
    if (prefix.empty() || prefix.size() > 120 ||
        !std::all_of(prefix.begin(), prefix.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(label) + " must be a nonempty identifier prefix");
    }
}

void require_editable(const BoundaryAuthoringSession& session) {
    if (session.phase() == BoundaryAuthoringPhase::cancelled) {
        invalid("boundary authoring session is cancelled");
    }
}

std::string allocate_id(std::string_view prefix, std::string_view identity_namespace,
                        std::uint64_t& counter) {
    if (counter == 0 || counter == std::numeric_limits<std::uint64_t>::max()) {
        invalid("authoring identity counter is exhausted");
    }
    std::string result(prefix);
    result.push_back('-');
    result.append(identity_namespace);
    result.push_back('-');
    result += std::to_string(counter++);
    if (result.size() > 128) invalid("authoring identity exceeds the 128-byte identifier limit");
    return result;
}

struct ArcGeometry {
    Vec2 center;
    double radius{};
    double start_angle{};
};

ArcGeometry arc_geometry(const Segment& segment) {
    if (segment.sweep_radians == 0.0) invalid("line segment has no arc geometry");
    const auto chord_x = segment.end.x - segment.start.x;
    const auto chord_y = segment.end.y - segment.start.y;
    const auto chord = std::hypot(chord_x, chord_y);
    if (!std::isfinite(chord) || !(chord > 0.0)) invalid("arc chord is degenerate");
    const auto half_sweep = segment.sweep_radians / 2.0;
    const auto sine = std::sin(std::abs(half_sweep));
    const auto tangent = std::tan(half_sweep);
    if (!(sine > 0.0) || tangent == 0.0 || !std::isfinite(tangent)) {
        invalid("arc sweep cannot be represented");
    }
    const Vec2 midpoint{(segment.start.x + segment.end.x) / 2.0,
                        (segment.start.y + segment.end.y) / 2.0};
    const Vec2 left_normal{-chord_y / chord, chord_x / chord};
    const auto offset = chord / (2.0 * tangent);
    const Vec2 center{midpoint.x + left_normal.x * offset,
                      midpoint.y + left_normal.y * offset};
    const auto radius = chord / (2.0 * sine);
    if (!finite_point(center) || !std::isfinite(radius)) {
        invalid("arc geometry is not finite");
    }
    return {center, radius,
            std::atan2(segment.start.y - center.y, segment.start.x - center.x)};
}

Vec2 arc_point_at(const Segment& segment, double parameter) {
    const auto arc = arc_geometry(segment);
    const auto angle = arc.start_angle + segment.sweep_radians * parameter;
    return {arc.center.x + arc.radius * std::cos(angle),
            arc.center.y + arc.radius * std::sin(angle)};
}

Vec2 automatic_dimension_position(const Segment& segment) {
    const auto length = segment_length(segment);
    if (!std::isfinite(length) || !(length > 0.0)) invalid("cannot place a degenerate dimension");
    double tangent = 0.0;
    Vec2 midpoint{};
    if (segment.sweep_radians == 0.0) {
        midpoint = {(segment.start.x + segment.end.x) / 2.0,
                    (segment.start.y + segment.end.y) / 2.0};
        tangent = std::atan2(segment.end.y - segment.start.y,
                             segment.end.x - segment.start.x);
    } else {
        midpoint = arc_point_at(segment, 0.5);
        const auto arc = arc_geometry(segment);
        const auto middle_angle = arc.start_angle + segment.sweep_radians * 0.5;
        tangent = middle_angle + (segment.sweep_radians > 0.0 ? std::numbers::pi / 2.0
                                                               : -std::numbers::pi / 2.0);
    }
    const auto offset = std::max(0.25, length * 0.1);
    const Vec2 result{midpoint.x - std::sin(tangent) * offset,
                      midpoint.y + std::cos(tangent) * offset};
    require_point(result, "automatic dimension position");
    return result;
}

void validate_strict_chain(const BoundaryDraftChain& chain, double tolerance) {
    if (chain.segments.empty()) invalid("cannot close an empty boundary chain");
    if (!finite_point(chain.anchor)) invalid("boundary anchor is not finite");
    if (!can_recognize_boundary_entity_type(chain.type)) {
        invalid("boundary entity type is unsupported");
    }
    if (chain.classified && chain.classification.empty()) {
        invalid("classified boundary chain has no area classification");
    }

    Boundary geometry;
    geometry.reserve(chain.segments.size());
    std::set<std::string, std::less<>> segment_ids;
    std::set<std::string, std::less<>> vertex_ids;
    for (std::size_t index = 0; index < chain.segments.size(); ++index) {
        const auto& edge = chain.segments[index];
        if (!segment_ids.insert(edge.segment_id).second) {
            invalid("boundary chain contains duplicate segment identities");
        }
        if (!vertex_ids.insert(edge.start_vertex_id).second) {
            invalid("boundary chain revisits a vertex identity");
        }
        if (!finite_point(edge.segment.start) || !finite_point(edge.segment.end) ||
            !std::isfinite(edge.segment.sweep_radians)) {
            invalid("boundary chain contains non-finite geometry");
        }
        if (!same_point(edge.segment.start, chain.anchor)) {
            // The first edge is the only edge whose start is compared to the
            // captured anchor. Later starts are checked against prior ends.
            if (index == 0) invalid("boundary chain does not retain its anchor");
        }
        if (index > 0 && !same_point(chain.segments[index - 1].segment.end,
                                     edge.segment.start)) {
            invalid("boundary chain has a distorted internal join");
        }
        if (index > 0 && chain.segments[index - 1].end_vertex_id != edge.start_vertex_id) {
            invalid("boundary chain has a broken vertex identity join");
        }
        geometry.push_back(edge.segment);
    }
    if (!same_point(chain.segments.back().segment.end, chain.anchor)) {
        invalid("boundary chain is not exactly closed");
    }
    if (chain.segments.back().end_vertex_id != chain.segments.front().start_vertex_id) {
        invalid("boundary closure does not reuse the starting vertex identity");
    }
    const auto diagnostics = validate_boundary(geometry, tolerance);
    if (!diagnostics.empty()) {
        invalid("boundary chain geometry: " + diagnostics.front().message);
    }
}

void validate_active_for_edge(const BoundaryAuthoringSession& session,
                              bool has_chain, bool has_pending_dimensions) {
    require_editable(session);
    if (!has_chain) invalid("an anchor is required before edge input");
    if (session.pen_state() != BoundaryPenState::down) invalid("pen must be down for edge input");
    if (session.mode() == BoundaryAuthoringMode::define_first &&
        has_pending_dimensions) {
        invalid("Define First requires dimension placement before the next edge");
    }
}

BoundaryAuthoringActionKind action_kind_for_receipt(BoundaryConstructionKind kind) {
    switch (kind) {
        case BoundaryConstructionKind::line_heading:
            return BoundaryAuthoringActionKind::line_heading;
        case BoundaryConstructionKind::line_rise_run:
            return BoundaryAuthoringActionKind::line_rise_run;
        case BoundaryConstructionKind::line_relative_turn:
            return BoundaryAuthoringActionKind::line_relative_turn;
        case BoundaryConstructionKind::line_closure:
            return BoundaryAuthoringActionKind::line_closure;
        case BoundaryConstructionKind::line_to_point:
            return BoundaryAuthoringActionKind::line_to_point;
        case BoundaryConstructionKind::arc_chord_angle:
            return BoundaryAuthoringActionKind::arc_chord_angle;
        case BoundaryConstructionKind::arc_chord_height:
            return BoundaryAuthoringActionKind::arc_chord_height;
        case BoundaryConstructionKind::arc_chord_length:
            return BoundaryAuthoringActionKind::arc_chord_length;
        case BoundaryConstructionKind::arc_start_tangent:
            return BoundaryAuthoringActionKind::arc_start_tangent;
    }
    invalid("unsupported construction receipt action kind");
}

BoundaryAuthoringChainRecord chain_record(const AcceptedBoundaryChain& source) {
    BoundaryAuthoringChainRecord result;
    result.anchor = source.anchor;
    result.boundary_id = source.boundary.id;
    result.type = source.boundary.type;
    result.classification = source.classification;
    result.classified = source.classified;
    result.dimensions = source.dimensions;
    if (source.receipts.size() != source.boundary.segments.size()) {
        invalid("accepted chain receipt count does not match segment count");
    }
    result.edges.reserve(source.boundary.segments.size());
    for (std::size_t index = 0; index < source.boundary.segments.size(); ++index) {
        const auto& edge = source.boundary.segments[index];
        result.edges.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id,
                                source.receipts[index]});
    }
    return result;
}

void require_recovery_identifier(std::string_view value, std::string_view label) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(label) + " must contain 1..128 supported ASCII characters");
    }
}

void require_recovery_counter_values(const BoundaryAuthoringCounters& counters,
                                     std::string_view label) {
    if (counters.next_boundary_id == 0 || counters.next_vertex_id == 0 ||
        counters.next_segment_id == 0 || counters.next_dimension_id == 0) {
        invalid(std::string(label) + " must contain nonzero next-ID values");
    }
}

bool counters_dominate(const BoundaryAuthoringCounters& left,
                       const BoundaryAuthoringCounters& right) noexcept {
    return left.next_boundary_id >= right.next_boundary_id &&
           left.next_vertex_id >= right.next_vertex_id &&
           left.next_segment_id >= right.next_segment_id &&
           left.next_dimension_id >= right.next_dimension_id;
}

}  // namespace

bool BoundaryAuthoringChainRecord::operator==(
    const BoundaryAuthoringChainRecord& other) const noexcept {
    return anchor.x == other.anchor.x && anchor.y == other.anchor.y &&
           boundary_id == other.boundary_id && type == other.type &&
           classification == other.classification && edges == other.edges &&
           dimensions == other.dimensions && classified == other.classified;
}

bool BoundaryAuthoringAction::operator==(const BoundaryAuthoringAction& other) const noexcept {
    const auto equal_point = [](const std::optional<Vec2>& left,
                                const std::optional<Vec2>& right) noexcept {
        if (left.has_value() != right.has_value()) return false;
        return !left.has_value() ||
               (left->x == right->x && left->y == right->y);
    };
    return kind == other.kind && counters_before == other.counters_before &&
           counters_after == other.counters_after && generated_ids == other.generated_ids &&
           classification == other.classification && equal_point(point, other.point) &&
           receipt == other.receipt && dimension == other.dimension && chain == other.chain;
}

bool BoundaryAuthoringCheckpoint::operator==(
    const BoundaryAuthoringCheckpoint& other) const noexcept {
    const auto equal_point = [](const std::optional<Vec2>& left,
                                const std::optional<Vec2>& right) noexcept {
        if (left.has_value() != right.has_value()) return false;
        return !left.has_value() ||
               (left->x == right->x && left->y == right->y);
    };
    return version == other.version && replay_version == other.replay_version &&
           mode == other.mode &&
           options == other.options &&
           identity_namespace == other.identity_namespace && equal_point(pointer, other.pointer) &&
           actions == other.actions && history_position == other.history_position &&
           counters == other.counters && extensions == other.extensions;
}

std::string BoundaryAuthoringSession::append_edge(SemanticRoot& semantic, IdCounters& ids,
                                                  Segment segment,
                                                  ConstructionReceipt receipt,
                                                  std::vector<std::string>* generated_ids) {
    auto& chain = *semantic.active_chain;
    const auto mode = mode_;
    const auto& options = options_;
    if (!finite_point(segment.start) || !finite_point(segment.end) ||
        !std::isfinite(segment.sweep_radians)) {
        invalid("edge geometry must be finite");
    }
    const auto length = segment_length(segment);
    if (!std::isfinite(length) || !(length > options.geometry_tolerance_metres)) {
        invalid("edge geometry is degenerate");
    }
    const auto chord_length = point_distance(segment.start, segment.end);
    if (!(chord_length > options.geometry_tolerance_metres)) {
        invalid("edge chord is at or below the geometry tolerance");
    }
    const auto expected_start = chain.segments.empty() ? chain.anchor
                                                         : chain.segments.back().segment.end;
    if (!same_point(expected_start, segment.start)) {
        invalid("edge start does not exactly match the current anchor");
    }

    const auto allocate = [&](std::string_view prefix, std::uint64_t& counter) {
        auto result = allocate_id(prefix, identity_namespace_, counter);
        if (generated_ids != nullptr) generated_ids->push_back(result);
        return result;
    };
    const auto segment_id = allocate(options.segment_id_prefix, ids.segment);
    const auto start_vertex_id = chain.segments.empty()
                                     ? allocate(options.vertex_id_prefix, ids.vertex)
                                     : chain.segments.back().end_vertex_id;
    // An edge that reaches the captured anchor is already an explicit
    // topological closure. Reuse the first vertex identity while allocating
    // the edge, so the ordered endpoint pair is never rewritten at
    // close_chain() time. A later edge after this point will fail strict
    // closure validation because it would revisit that vertex.
    const auto end_vertex_id = !chain.segments.empty() && same_point(segment.end, chain.anchor)
                                    ? chain.segments.front().start_vertex_id
                                    : allocate(options.vertex_id_prefix, ids.vertex);
    receipt.segment_id = segment_id;
    receipt.start = segment.start;
    chain.segments.push_back({segment_id, start_vertex_id, end_vertex_id, segment});
    chain.receipts.push_back(std::move(receipt));

    if (options.automatic_dimension_placement) {
        const auto text_position = automatic_dimension_position(segment);
        const auto dimension_id = allocate(options.dimension_id_prefix, ids.dimension);
        if (generated_ids != nullptr) generated_ids->push_back(dimension_id);
        chain.dimensions.push_back(
            {dimension_id, *chain.boundary_id,
             segment_id, text_position, BoundaryDimensionPlacement::automatic,
             automatic_placement_version});
    } else if (mode == BoundaryAuthoringMode::define_first) {
        chain.pending_dimensions.push_back({*chain.boundary_id, segment_id});
    }
    semantic.phase = !chain.pending_dimensions.empty()
                         ? BoundaryAuthoringPhase::awaiting_dimension
                         : BoundaryAuthoringPhase::drawing;
    return segment_id;
}

void require_mode(BoundaryAuthoringMode mode) {
    if (mode != BoundaryAuthoringMode::draw_first && mode != BoundaryAuthoringMode::define_first) {
        invalid("unknown boundary authoring mode");
    }
}

bool AcceptedBoundaryChain::operator==(const AcceptedBoundaryChain& other) const noexcept {
    return same_point(anchor, other.anchor) && boundary == other.boundary &&
           classification == other.classification && dimensions == other.dimensions &&
           receipts == other.receipts && classified == other.classified;
}

bool BoundaryDraftChain::operator==(const BoundaryDraftChain& other) const noexcept {
    const auto equal_point = [](Vec2 left, Vec2 right) {
        return same_point(left, right);
    };
    if (boundary_id != other.boundary_id || type != other.type ||
        classification != other.classification ||
        !equal_point(anchor, other.anchor) || segments.size() != other.segments.size() ||
        dimensions != other.dimensions || receipts != other.receipts ||
        pending_dimensions != other.pending_dimensions || classified != other.classified) {
        return false;
    }
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const auto& left = segments[index];
        const auto& right = other.segments[index];
        if (left.segment_id != right.segment_id || left.start_vertex_id != right.start_vertex_id ||
            left.end_vertex_id != right.end_vertex_id ||
            !equal_point(left.segment.start, right.segment.start) ||
            !equal_point(left.segment.end, right.segment.end) ||
            left.segment.sweep_radians != right.segment.sweep_radians) {
            return false;
        }
    }
    return true;
}

bool BoundaryAuthoringState::operator==(const BoundaryAuthoringState& other) const noexcept {
    const auto equal_point = [](const std::optional<Vec2>& left,
                                const std::optional<Vec2>& right) {
        if (left.has_value() != right.has_value()) return false;
        return !left.has_value() || same_point(*left, *right);
    };
    return mode == other.mode && identity_namespace == other.identity_namespace &&
           phase == other.phase && pen_state == other.pen_state &&
           equal_point(pointer, other.pointer) && equal_point(anchor, other.anchor) &&
           classification == other.classification && active_chain == other.active_chain &&
           pending_dimension == other.pending_dimension &&
           pending_dimensions == other.pending_dimensions &&
           accepted_chains == other.accepted_chains &&
           semantic_undo_depth == other.semantic_undo_depth &&
           semantic_redo_depth == other.semantic_redo_depth;
}

BoundaryAuthoringSession::HistoryEntry::HistoryEntry(
    BoundaryAuthoringAction source_action, SemanticRoot source_semantic,
    std::size_t source_position, std::size_t source_retained_sequence_chunks,
    std::shared_ptr<const HistoryEntry> source_previous)
    : action(std::move(source_action)),
      semantic(std::move(source_semantic)),
      position(source_position),
      retained_sequence_chunks(source_retained_sequence_chunks),
      previous(std::move(source_previous)) {
    live_history_entries.fetch_add(1, std::memory_order_relaxed);
}

BoundaryAuthoringSession::HistoryEntry::~HistoryEntry() {
    live_history_entries.fetch_sub(1, std::memory_order_relaxed);
    auto cursor = std::move(previous);
    // Only the mutable ownership link is detached, and only for the last
    // strong owner. No weak references or mutable entry roots escape this
    // private history. The action and semantic payload remain immutable.
    while (cursor && cursor.use_count() == 1) {
        auto next = std::move(cursor->previous);
        cursor.reset();
        cursor = std::move(next);
    }
}

BoundaryAuthoringSession::RedoNode::RedoNode(
    std::shared_ptr<const HistoryEntry> source_entry,
    std::shared_ptr<const RedoNode> source_next)
    : entry(std::move(source_entry)),
      next(std::move(source_next)),
      depth((next ? next->depth : 0) + 1),
      retained_sequence_chunks(next ? next->retained_sequence_chunks
                                    : entry->retained_sequence_chunks),
      usage(next ? next->usage : entry->usage) {
    live_redo_nodes.fetch_add(1, std::memory_order_relaxed);
}

BoundaryAuthoringSession::RedoNode::~RedoNode() {
    live_redo_nodes.fetch_sub(1, std::memory_order_relaxed);
    auto cursor = std::move(next);
    // As with HistoryEntry, detach only the uniquely owned mutable link.
    while (cursor && cursor.use_count() == 1) {
        auto next_node = std::move(cursor->next);
        cursor.reset();
        cursor = std::move(next_node);
    }
}

BoundaryAuthoringSession::SharedString BoundaryAuthoringSession::shared_string(
    std::string value) {
    return std::make_shared<const std::string>(std::move(value));
}

BoundaryAuthoringSession::SemanticRoot BoundaryAuthoringSession::initial_semantic() const noexcept {
    SemanticRoot result;
    result.phase = mode_ == BoundaryAuthoringMode::define_first
                       ? BoundaryAuthoringPhase::awaiting_classification
                       : BoundaryAuthoringPhase::awaiting_anchor;
    return result;
}

bool BoundaryAuthoringSession::same_semantic_root(const SemanticRoot& left,
                                                  const SemanticRoot& right) noexcept {
    if (left.phase != right.phase || left.pen_state != right.pen_state ||
        left.classification != right.classification ||
        !left.accepted_chains.same_version(right.accepted_chains) ||
        left.active_chain.has_value() != right.active_chain.has_value()) {
        return false;
    }
    if (!left.active_chain) return true;
    const auto& lhs = *left.active_chain;
    const auto& rhs = *right.active_chain;
    return lhs.boundary_id == rhs.boundary_id && lhs.type == rhs.type &&
           lhs.classification == rhs.classification && same_point(lhs.anchor, rhs.anchor) &&
           lhs.segments.same_version(rhs.segments) &&
           lhs.dimensions.same_version(rhs.dimensions) &&
           lhs.receipts.same_version(rhs.receipts) &&
           lhs.pending_dimensions.same_version(rhs.pending_dimensions) &&
           lhs.classified == rhs.classified;
}

std::size_t BoundaryAuthoringSession::count_new_sequence_chunks(
    const SemanticRoot& current, const SemanticRoot& candidate) noexcept {
    std::size_t result = 0;
    const auto count_changed_root = [&](const auto& before, const auto& after) {
        if (after.root_identity() != nullptr &&
            after.root_identity() != before.root_identity()) {
            ++result;
        }
    };
    count_changed_root(current.accepted_chains, candidate.accepted_chains);
    if (candidate.active_chain) {
        if (current.active_chain) {
            count_changed_root(current.active_chain->segments,
                               candidate.active_chain->segments);
            count_changed_root(current.active_chain->dimensions,
                               candidate.active_chain->dimensions);
            count_changed_root(current.active_chain->receipts,
                               candidate.active_chain->receipts);
            count_changed_root(current.active_chain->pending_dimensions,
                               candidate.active_chain->pending_dimensions);
        } else {
            result += candidate.active_chain->segments.chunk_count();
            result += candidate.active_chain->dimensions.chunk_count();
            result += candidate.active_chain->receipts.chunk_count();
            result += candidate.active_chain->pending_dimensions.chunk_count();
        }
    }
    return result;
}

BoundaryDraftChain BoundaryAuthoringSession::materialize(const CompactDraftChain& source) {
    BoundaryDraftChain result;
    result.boundary_id = *source.boundary_id;
    result.type = *source.type;
    result.classification = source.classification ? *source.classification : std::string{};
    result.anchor = source.anchor;
    result.segments = source.segments.materialize();
    result.dimensions = source.dimensions.materialize();
    result.receipts = source.receipts.materialize();
    result.pending_dimensions = source.pending_dimensions.materialize();
    result.classified = source.classified;
    return result;
}

std::size_t BoundaryAuthoringSession::new_retained_bytes(const SemanticRoot& candidate) const {
    std::size_t bytes = 0;
    const auto add = [&](std::size_t amount) {
        if (!boundary_authoring_recovery_checked_add(bytes, amount, bytes))
            invalid("compact retained byte accounting overflow");
    };
    // There are only a handful of independently shared strings per semantic
    // transition. Charge a new identity once even if several fields share it.
    std::vector<const std::string*> existing;
    const auto remember = [&](const SharedString& s) { if (s) existing.push_back(s.get()); };
    remember(semantic_.classification);
    if (semantic_.active_chain) {
        remember(semantic_.active_chain->boundary_id); remember(semantic_.active_chain->type);
        remember(semantic_.active_chain->classification);
    }
    if (!semantic_.accepted_chains.empty()) {
        const auto& c = semantic_.accepted_chains.back();
        remember(c.boundary_id); remember(c.type); remember(c.classification);
    }
    const auto charge_string = [&](const SharedString& s) {
        if (s && std::find(existing.begin(), existing.end(), s.get()) == existing.end()) {
            add(sizeof(std::string) + 32); add(s->capacity()); add(1);
            existing.push_back(s.get());
        }
    };
    charge_string(candidate.classification);
    const auto dynamic = [](const auto& value) { return detail::authoring_dynamic_bytes(value); };
    const auto charge_sequence = [&](const auto& old, const auto& next) {
        if (old.root_identity() != next.root_identity()) add(next.tail_allocation_bytes(dynamic));
    };
    if (candidate.active_chain) {
        const auto& next = *candidate.active_chain;
        charge_string(next.boundary_id); charge_string(next.type); charge_string(next.classification);
        const CompactDraftChain empty;
        const auto& old = semantic_.active_chain ? *semantic_.active_chain : empty;
        charge_sequence(old.segments, next.segments);
        charge_sequence(old.receipts, next.receipts);
        charge_sequence(old.dimensions, next.dimensions);
        charge_sequence(old.pending_dimensions, next.pending_dimensions);
    }
    if (semantic_.accepted_chains.root_identity() != candidate.accepted_chains.root_identity()) {
        add(candidate.accepted_chains.tail_allocation_bytes(
            [](const CompactAcceptedChain&) -> std::size_t { return 0; }));
        if (!candidate.accepted_chains.empty()) {
            const auto& next = candidate.accepted_chains.back();
            charge_string(next.boundary_id); charge_string(next.type); charge_string(next.classification);
        }
    }
    return bytes;
}

AcceptedBoundaryChain BoundaryAuthoringSession::materialize(
    const CompactAcceptedChain& source) {
    AcceptedBoundaryChain result;
    result.anchor = source.anchor;
    result.boundary = {*source.boundary_id, *source.type, source.segments.materialize()};
    result.classification = source.classification ? *source.classification : std::string{};
    result.dimensions = source.dimensions.materialize();
    result.receipts = source.receipts.materialize();
    result.classified = source.classified;
    return result;
}

BoundaryAuthoringSession::BoundaryAuthoringSession(BoundaryAuthoringMode mode,
                                                   BoundaryAuthoringOptions options,
                                                   BoundaryAuthoringResourcePolicy policy)
    : mode_(mode), options_(std::move(options)), resource_policy_(policy),
      identity_namespace_(make_stable_id()) {
    require_mode(mode);
    if (!std::isfinite(options_.geometry_tolerance_metres) ||
        !(options_.geometry_tolerance_metres > 0.0)) {
        invalid("geometry tolerance must be finite and positive");
    }
    if (options_.automatic_placement_version != automatic_placement_version) {
        invalid("only automatic dimension placement version one is supported");
    }
    require_identifier_prefix(options_.boundary_id_prefix, "boundary id prefix");
    require_identifier_prefix(options_.vertex_id_prefix, "vertex id prefix");
    require_identifier_prefix(options_.segment_id_prefix, "segment id prefix");
    require_identifier_prefix(options_.dimension_id_prefix, "dimension id prefix");
    constexpr std::size_t maximum_counter_text = 20;
    if (identity_namespace_.size() + maximum_counter_text + 2 > 128) {
        invalid("authoring identity namespace exceeds the 128-byte identifier limit");
    }
    const auto maximum_prefix_size =
        128 - identity_namespace_.size() - maximum_counter_text - 2;
    if (options_.boundary_id_prefix.size() > maximum_prefix_size ||
        options_.vertex_id_prefix.size() > maximum_prefix_size ||
        options_.segment_id_prefix.size() > maximum_prefix_size ||
        options_.dimension_id_prefix.size() > maximum_prefix_size) {
        invalid("authoring identity prefix exceeds the 128-byte identifier limit");
    }
    if (options_.boundary_id_prefix == options_.vertex_id_prefix ||
        options_.boundary_id_prefix == options_.segment_id_prefix ||
        options_.boundary_id_prefix == options_.dimension_id_prefix ||
        options_.vertex_id_prefix == options_.segment_id_prefix ||
        options_.vertex_id_prefix == options_.dimension_id_prefix ||
        options_.segment_id_prefix == options_.dimension_id_prefix) {
        invalid("authoring identity prefixes must be distinct");
    }
    if (!can_recognize_boundary_entity_type(options_.default_boundary_type)) {
        invalid("default boundary entity type is unsupported");
    }

    semantic_ = initial_semantic();
    base_resource_usage_ = detail::authoring_context_usage(
        options_, mode_, identity_namespace_, nlohmann::json::object(), resource_policy_);
}

const BoundaryAuthoringResourcePolicy& BoundaryAuthoringSession::resource_policy() const noexcept {
    return resource_policy_;
}

BoundaryAuthoringResourceUsage BoundaryAuthoringSession::resource_usage() const noexcept {
    return redo_ ? redo_->usage : history_ ? history_->usage : base_resource_usage_;
}

BoundaryAuthoringMode BoundaryAuthoringSession::mode() const noexcept {
    return mode_;
}

BoundaryAuthoringOptions BoundaryAuthoringSession::options() const {
    return options_;
}

BoundaryAuthoringPhase BoundaryAuthoringSession::phase() const noexcept {
    return semantic_.phase;
}

BoundaryPenState BoundaryAuthoringSession::pen_state() const noexcept {
    return semantic_.pen_state;
}

BoundaryAuthoringState BoundaryAuthoringSession::view() const {
    BoundaryAuthoringState result;
    result.mode = mode_;
    result.identity_namespace = identity_namespace_;
    result.phase = semantic_.phase;
    result.pen_state = semantic_.pen_state;
    result.pointer = pointer_;
    if (semantic_.classification) result.classification = *semantic_.classification;
    if (semantic_.active_chain) {
        result.active_chain = materialize(*semantic_.active_chain);
        result.anchor = semantic_.active_chain->anchor;
        result.pending_dimensions = semantic_.active_chain->pending_dimensions.materialize();
        if (!result.pending_dimensions.empty()) {
            result.pending_dimension = result.pending_dimensions.front();
        }
    }
    const auto accepted = semantic_.accepted_chains.materialize();
    result.accepted_chains.reserve(accepted.size());
    for (const auto& chain : accepted) result.accepted_chains.push_back(materialize(chain));
    result.semantic_undo_depth = history_position();
    result.semantic_redo_depth = redo_depth();
    return result;
}

BoundaryAuthoringState BoundaryAuthoringSession::snapshot() const {
    return view();
}

std::vector<AcceptedBoundaryChain> BoundaryAuthoringSession::accepted_chains() const {
    std::vector<AcceptedBoundaryChain> result;
    const auto accepted = semantic_.accepted_chains.materialize();
    result.reserve(accepted.size());
    for (const auto& chain : accepted) result.push_back(materialize(chain));
    return result;
}

std::optional<BoundaryDraftChain> BoundaryAuthoringSession::active_chain() const {
    if (!semantic_.active_chain) return std::nullopt;
    return materialize(*semantic_.active_chain);
}

std::optional<PendingBoundaryDimension> BoundaryAuthoringSession::pending_dimension() const {
    if (!semantic_.active_chain || semantic_.active_chain->pending_dimensions.empty()) {
        return std::nullopt;
    }
    return semantic_.active_chain->pending_dimensions.front();
}

bool BoundaryAuthoringSession::can_undo() const noexcept {
    return history_ != nullptr;
}

bool BoundaryAuthoringSession::can_redo() const noexcept {
    return redo_ != nullptr;
}

std::size_t BoundaryAuthoringSession::history_position() const noexcept {
    return history_ ? history_->position : 0;
}

std::size_t BoundaryAuthoringSession::redo_depth() const noexcept {
    return redo_ ? redo_->depth : 0;
}

BoundaryAuthoringStructuralStats BoundaryAuthoringSession::structural_stats() const noexcept {
    BoundaryAuthoringStructuralStats result;
    result.history_position = history_position();
    result.redo_node_count = redo_depth();
    result.action_count = result.history_position + result.redo_node_count;
    result.retained_sequence_chunks =
        redo_ ? redo_->retained_sequence_chunks
              : (history_ ? history_->retained_sequence_chunks : 0);
    if (semantic_.active_chain) {
        result.active_segment_count = semantic_.active_chain->segments.size();
        result.active_receipt_count = semantic_.active_chain->receipts.size();
        result.active_dimension_count = semantic_.active_chain->dimensions.size();
        result.active_pending_dimension_count =
            semantic_.active_chain->pending_dimensions.size();
    }
    result.accepted_chain_count = semantic_.accepted_chains.size();
    result.live_history_entries = live_history_entries.load(std::memory_order_relaxed);
    result.live_redo_nodes = live_redo_nodes.load(std::memory_order_relaxed);
    result.live_sequence_chunks =
        Sequence<IdentifiedSegment>::live_chunk_count() +
        Sequence<BoundaryDimension>::live_chunk_count() +
        Sequence<ConstructionReceipt>::live_chunk_count() +
        Sequence<PendingBoundaryDimension>::live_chunk_count() +
        Sequence<CompactAcceptedChain>::live_chunk_count();
    result.sequence_chunk_allocations =
        Sequence<IdentifiedSegment>::chunk_allocation_count() +
        Sequence<BoundaryDimension>::chunk_allocation_count() +
        Sequence<ConstructionReceipt>::chunk_allocation_count() +
        Sequence<PendingBoundaryDimension>::chunk_allocation_count() +
        Sequence<CompactAcceptedChain>::chunk_allocation_count();
    result.history_root_identity = history_.get();
    result.redo_root_identity = redo_.get();
    return result;
}

void BoundaryAuthoringSession::set_pointer(Vec2 world_position) {
    require_editable(*this);
    require_point(world_position, "pointer position");
    pointer_ = world_position;
}

void BoundaryAuthoringSession::set_classification(std::string category) {
    set_classification_impl(std::move(category), BoundaryAuthoringActionKind::set_classification);
}

void BoundaryAuthoringSession::set_classification_impl(
    std::string category, BoundaryAuthoringActionKind action_kind) {
    const auto trimmed = trim(category);
    if (trimmed.empty() || trimmed.size() > 128 ||
        std::any_of(trimmed.begin(), trimmed.end(), [](unsigned char character) {
            return std::iscntrl(character) != 0;
        })) {
        invalid("boundary area classification must be a nonempty printable value");
    }
    category.assign(trimmed);
    require_editable(*this);
    auto candidate = semantic_;
    bool changed = false;
    if (candidate.active_chain.has_value()) {
        if (mode() == BoundaryAuthoringMode::define_first &&
            candidate.active_chain->classified &&
            *candidate.active_chain->classification != category) {
            invalid("Define First classification cannot change after anchoring");
        }
        if (!candidate.active_chain->classification ||
            *candidate.active_chain->classification != category ||
            !candidate.active_chain->classified || !candidate.classification ||
            *candidate.classification != category) {
            auto classification = shared_string(category);
            candidate.active_chain->classification = classification;
            candidate.active_chain->classified = true;
            candidate.classification = std::move(classification);
            changed = true;
        }
    } else if (mode() == BoundaryAuthoringMode::draw_first &&
               !candidate.accepted_chains.empty() &&
               !candidate.accepted_chains.back().classified) {
        auto accepted = candidate.accepted_chains.back();
        accepted.classification = shared_string(category);
        accepted.classified = true;
        candidate.accepted_chains.replace_back(std::move(accepted));
        // Draw First classification belongs to the selected closed chain;
        // leave the next measured chain unclassified unless the caller
        // explicitly selects a category again.
        candidate.classification.reset();
        changed = true;
    } else {
        if (!candidate.classification || *candidate.classification != category) {
            candidate.classification = shared_string(category);
            changed = true;
        }
        if (mode() == BoundaryAuthoringMode::define_first) {
            changed = changed || candidate.phase != BoundaryAuthoringPhase::awaiting_anchor;
            candidate.phase = BoundaryAuthoringPhase::awaiting_anchor;
        } else if (!candidate.active_chain.has_value()) {
            changed = changed || candidate.phase != BoundaryAuthoringPhase::awaiting_anchor;
            candidate.phase = BoundaryAuthoringPhase::awaiting_anchor;
        }
    }
    if (!changed) return;
    const auto ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                next_dimension_id_};
    BoundaryAuthoringAction action;
    action.kind = action_kind;
    action.counters_before = public_counters();
    action.counters_after = action.counters_before;
    action.classification = category;
    commit_semantic(std::move(candidate), ids, std::move(action));
}

void BoundaryAuthoringSession::classify_current_chain(std::string category) {
    if (!semantic_.active_chain.has_value()) invalid("no active chain is available for classification");
    set_classification_impl(std::move(category),
                            BoundaryAuthoringActionKind::classify_current_chain);
}

void BoundaryAuthoringSession::classify_last_chain(std::string category) {
    if (mode() != BoundaryAuthoringMode::draw_first) {
        invalid("only Draw First supports post-closure classification");
    }
    if (semantic_.active_chain.has_value() || semantic_.accepted_chains.empty()) {
        invalid("a closed Draw First chain is required for post-closure classification");
    }
    if (semantic_.accepted_chains.back().classified) {
        invalid("the most recent chain is already classified");
    }
    const auto trimmed = trim(category);
    if (trimmed.empty() || trimmed.size() > 128 ||
        std::any_of(trimmed.begin(), trimmed.end(), [](unsigned char character) {
            return std::iscntrl(character) != 0;
        })) {
        invalid("boundary area classification must be a nonempty printable value");
    }
    auto candidate = semantic_;
    auto accepted = candidate.accepted_chains.back();
    accepted.classification = shared_string(std::string(trimmed));
    accepted.classified = true;
    candidate.accepted_chains.replace_back(std::move(accepted));
    candidate.classification.reset();
    candidate.phase = BoundaryAuthoringPhase::completed;
    const auto ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                next_dimension_id_};
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::classify_last_chain;
    action.counters_before = public_counters();
    action.counters_after = action.counters_before;
    action.classification = std::string(trimmed);
    commit_semantic(std::move(candidate), ids, std::move(action));
}

std::string BoundaryAuthoringSession::anchor(Vec2 world_position) {
    require_point(world_position, "anchor position");
    require_editable(*this);
    if (semantic_.active_chain.has_value()) invalid("an active chain must be closed before a new anchor");
    if (semantic_.pen_state == BoundaryPenState::down) invalid("pen must be up before anchoring");
    if (mode() == BoundaryAuthoringMode::define_first && !semantic_.classification) {
        invalid("Define First requires classification before anchoring");
    }

    auto candidate = semantic_;
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    CompactDraftChain chain;
    std::vector<std::string> generated_ids;
    auto boundary_id = allocate_id(options_.boundary_id_prefix, identity_namespace_, ids.boundary);
    generated_ids.push_back(boundary_id);
    chain.boundary_id = shared_string(boundary_id);
    chain.anchor = world_position;
    chain.type = shared_string(options_.default_boundary_type);
    chain.classification = candidate.classification;
    chain.classified = chain.classification != nullptr;
    candidate.active_chain = std::move(chain);
    candidate.phase = BoundaryAuthoringPhase::drawing;
    candidate.pen_state = BoundaryPenState::down;
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::anchor;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.point = world_position;
    auto transaction = prepare_semantic(std::move(candidate), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(boundary_id);
}

std::string BoundaryAuthoringSession::anchor() {
    if (!pointer_.has_value()) invalid("anchor requires a world position or pointer position");
    return anchor(*pointer_);
}

void BoundaryAuthoringSession::pen_up() {
    require_editable(*this);
    if (semantic_.pen_state == BoundaryPenState::up) return;
    if (mode_ == BoundaryAuthoringMode::define_first && semantic_.active_chain.has_value() &&
        !semantic_.active_chain->pending_dimensions.empty()) {
        invalid("Define First requires dimension placement before lifting the pen");
    }
    auto candidate = semantic_;
    candidate.pen_state = BoundaryPenState::up;
    const auto ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                next_dimension_id_};
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::pen_up;
    action.counters_before = public_counters();
    action.counters_after = action.counters_before;
    commit_semantic(std::move(candidate), ids, std::move(action));
}

void BoundaryAuthoringSession::pen_down() {
    require_editable(*this);
    if (!semantic_.active_chain.has_value()) invalid("pen down requires an active chain");
    if (semantic_.pen_state == BoundaryPenState::down) return;
    auto candidate = semantic_;
    candidate.pen_state = BoundaryPenState::down;
    candidate.phase = candidate.active_chain->pending_dimensions.empty()
                          ? BoundaryAuthoringPhase::drawing
                          : BoundaryAuthoringPhase::awaiting_dimension;
    const auto ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                next_dimension_id_};
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::pen_down;
    action.counters_before = public_counters();
    action.counters_after = action.counters_before;
    commit_semantic(std::move(candidate), ids, std::move(action));
}

std::string BoundaryAuthoringSession::add_line(const Quantity& distance,
                                               const AngleInput& heading) {
    const auto normalized_distance = normalize_exact_quantity(distance, "line distance");
    const auto normalized_heading = normalize_exact_angle(heading, "line heading");
    if (!(normalized_distance.metres > options_.geometry_tolerance_metres)) {
        invalid("line distance must exceed the geometry tolerance");
    }
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    const auto start = semantic.active_chain->segments.empty()
                           ? semantic.active_chain->anchor
                           : semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::line_heading;
    receipt.start = start;
    receipt.distance = normalized_distance;
    receipt.heading = normalized_heading;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::line_heading;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_line(const Quantity& distance, double heading_radians) {
    return add_line(distance, AngleInput::from_radians(heading_radians));
}

std::string BoundaryAuthoringSession::add_line_to(Vec2 end) {
    require_point(end, "line endpoint");
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    const auto start = semantic.active_chain->segments.empty()
                           ? semantic.active_chain->anchor
                           : semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::line_to_point;
    receipt.start = start;
    receipt.chord_end = end;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::line_to_point;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_line_rise_run(const Quantity& rise,
                                                         const Quantity& run) {
    const auto normalized_rise = normalize_exact_quantity(rise, "line rise");
    const auto normalized_run = normalize_exact_quantity(run, "line run");
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    if (!std::isfinite(normalized_rise.metres) || !std::isfinite(normalized_run.metres) ||
        !(std::hypot(normalized_rise.metres, normalized_run.metres) >
          options_.geometry_tolerance_metres)) {
        invalid("line rise and run must define a non-degenerate vector");
    }
    const auto start = semantic.active_chain->segments.empty()
                           ? semantic.active_chain->anchor
                           : semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::line_rise_run;
    receipt.start = start;
    receipt.rise = normalized_rise;
    receipt.run = normalized_run;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::line_rise_run;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_line_relative_turn(const Quantity& distance,
                                                             const AngleInput& turn) {
    const auto normalized_distance = normalize_exact_quantity(distance, "relative line distance");
    const auto normalized_turn = normalize_exact_angle(turn, "relative line turn");
    if (!(normalized_distance.metres > options_.geometry_tolerance_metres)) {
        invalid("relative line distance must exceed the geometry tolerance");
    }
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    if (semantic.active_chain->segments.empty()) {
        invalid("relative turn requires a preceding edge");
    }
    const auto start = semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::line_relative_turn;
    receipt.start = start;
    receipt.distance = normalized_distance;
    receipt.turn = normalized_turn;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start,
                                           semantic.active_chain->segments.back().segment,
                                           std::nullopt, options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::line_relative_turn;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_line_relative_turn(const Quantity& distance,
                                                             double turn_radians) {
    return add_line_relative_turn(distance, AngleInput::from_radians(turn_radians));
}

std::string BoundaryAuthoringSession::add_arc_chord_angle(Vec2 end,
                                                          const AngleInput& sweep) {
    require_point(end, "arc endpoint");
    const auto normalized_sweep = normalize_exact_angle(sweep, "arc sweep");
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    const auto start = semantic.active_chain->segments.empty()
                           ? semantic.active_chain->anchor
                           : semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::arc_chord_angle;
    receipt.start = start;
    receipt.chord_end = end;
    receipt.angle = normalized_sweep;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::arc_chord_angle;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_arc_chord_angle(Vec2 end, double sweep_radians) {
    return add_arc_chord_angle(end, AngleInput::from_radians(sweep_radians));
}

std::string BoundaryAuthoringSession::add_arc_chord_height(Vec2 end,
                                                           const Quantity& signed_height) {
    require_point(end, "arc endpoint");
    const auto normalized_height = normalize_exact_quantity(signed_height, "arc chord height");
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    const auto start = semantic.active_chain->segments.empty()
                           ? semantic.active_chain->anchor
                           : semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::arc_chord_height;
    receipt.start = start;
    receipt.chord_end = end;
    receipt.height = normalized_height;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::arc_chord_height;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_arc_chord_arc_length(Vec2 end,
                                                               const Quantity& arc_length,
                                                               bool clockwise) {
    require_point(end, "arc endpoint");
    const auto normalized_length = normalize_exact_quantity(arc_length, "arc length");
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    const auto start = semantic.active_chain->segments.empty()
                           ? semantic.active_chain->anchor
                           : semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::arc_chord_length;
    receipt.start = start;
    receipt.chord_end = end;
    receipt.arc_length = normalized_length;
    receipt.clockwise = clockwise;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::arc_chord_length;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_arc_start_tangent(const AngleInput& tangent,
                                                             const Quantity& arc_length,
                                                             const AngleInput& sweep) {
    const auto normalized_tangent = normalize_exact_angle(tangent, "arc tangent");
    const auto normalized_length = normalize_exact_quantity(arc_length, "arc length");
    const auto normalized_sweep = normalize_exact_angle(sweep, "arc sweep");
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    const auto start = semantic.active_chain->segments.empty()
                           ? semantic.active_chain->anchor
                           : semantic.active_chain->segments.back().segment.end;
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::arc_start_tangent;
    receipt.start = start;
    receipt.tangent = normalized_tangent;
    receipt.arc_length = normalized_length;
    receipt.sweep = normalized_sweep;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::arc_start_tangent;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

std::string BoundaryAuthoringSession::add_arc_start_tangent(const Quantity& arc_length,
                                                             const AngleInput& tangent,
                                                             const AngleInput& sweep) {
    return add_arc_start_tangent(tangent, arc_length, sweep);
}

std::string BoundaryAuthoringSession::add_arc_start_tangent(double tangent_radians,
                                                             const Quantity& arc_length,
                                                             double sweep_radians) {
    return add_arc_start_tangent(AngleInput::from_radians(tangent_radians), arc_length,
                                 AngleInput::from_radians(sweep_radians));
}

std::string BoundaryAuthoringSession::add_closing_segment() {
    require_editable(*this);
    if (!semantic_.active_chain.has_value()) invalid("no active chain is available for closure");
    auto semantic = semantic_;
    validate_active_for_edge(*this, semantic.active_chain.has_value(),
                             semantic.active_chain &&
                                 !semantic.active_chain->pending_dimensions.empty());
    auto& chain = *semantic.active_chain;
    const auto current = chain.segments.empty() ? chain.anchor : chain.segments.back().segment.end;
    if (same_point(current, chain.anchor)) {
        invalid("the active chain is already at its anchor");
    }
    const auto delta = Vec2{chain.anchor.x - current.x, chain.anchor.y - current.y};
    require_point(delta, "closing segment delta");
    if (!(point_distance(current, chain.anchor) > options_.geometry_tolerance_metres)) {
        invalid("closing segment is degenerate");
    }
    ConstructionReceipt receipt;
    receipt.kind = BoundaryConstructionKind::line_closure;
    receipt.start = current;
    receipt.closure_delta = delta;
    const auto replay = replay_construction_receipt(
        receipt, ConstructionReplayContext{current, std::nullopt, chain.anchor,
                                           options_.geometry_tolerance_metres});
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    auto id = append_edge(semantic, ids, replay.segment, replay.receipt, &generated_ids);
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::line_closure;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.receipt = semantic.active_chain->receipts.back();
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return std::string(id);
}

BoundaryDimension BoundaryAuthoringSession::place_manual_dimension(Vec2 world_position) {
    require_point(world_position, "dimension position");
    require_editable(*this);
    if (!semantic_.active_chain.has_value() ||
        semantic_.active_chain->pending_dimensions.empty()) {
        invalid("no pending dimension is available");
    }
    auto semantic = semantic_;
    auto& chain = *semantic.active_chain;
    const auto pending = chain.pending_dimensions.front();
    if (pending.boundary_id != *chain.boundary_id) {
        invalid("pending dimension boundary identity changed");
    }
    if (chain.segments.find_last_if([&](const IdentifiedSegment& edge) {
            return edge.segment_id == pending.segment_id;
        }) == nullptr) {
        invalid("pending dimension target edge no longer exists");
    }
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    BoundaryDimension dimension{
        [&] {
            auto id = allocate_id(options_.dimension_id_prefix, identity_namespace_, ids.dimension);
            generated_ids.push_back(id);
            return id;
        }(),
        pending.boundary_id,
        pending.segment_id, world_position, BoundaryDimensionPlacement::manual, std::nullopt};
    chain.pending_dimensions.pop_front();
    chain.dimensions.push_back(dimension);
    semantic.phase = chain.pending_dimensions.empty() ? BoundaryAuthoringPhase::drawing
                                                       : BoundaryAuthoringPhase::awaiting_dimension;
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::manual_dimension;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.dimension = dimension;
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return BoundaryDimension(dimension);
}

BoundaryDimension BoundaryAuthoringSession::place_automatic_dimension() {
    require_editable(*this);
    if (!semantic_.active_chain.has_value() ||
        semantic_.active_chain->pending_dimensions.empty()) {
        invalid("no pending dimension is available");
    }
    auto semantic = semantic_;
    auto& chain = *semantic.active_chain;
    const auto pending = chain.pending_dimensions.front();
    if (pending.boundary_id != *chain.boundary_id) {
        invalid("pending dimension boundary identity changed");
    }
    const auto* found = chain.segments.find_last_if([&](const IdentifiedSegment& edge) {
            return edge.segment_id == pending.segment_id;
        });
    if (found == nullptr) invalid("pending dimension target edge no longer exists");
    const auto before_ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                       next_dimension_id_};
    auto ids = before_ids;
    std::vector<std::string> generated_ids;
    const auto dimension_id = allocate_id(options_.dimension_id_prefix, identity_namespace_,
                                          ids.dimension);
    generated_ids.push_back(dimension_id);
    const auto text_position = automatic_dimension_position(found->segment);
    BoundaryDimension dimension{
        dimension_id,
        pending.boundary_id,
        pending.segment_id,
        text_position,
        BoundaryDimensionPlacement::automatic,
        automatic_placement_version};
    chain.pending_dimensions.pop_front();
    chain.dimensions.push_back(dimension);
    semantic.phase = chain.pending_dimensions.empty() ? BoundaryAuthoringPhase::drawing
                                                       : BoundaryAuthoringPhase::awaiting_dimension;
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::automatic_dimension;
    action.counters_before = public_counters(before_ids);
    action.counters_after = public_counters(ids);
    action.generated_ids = std::move(generated_ids);
    action.dimension = dimension;
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return BoundaryDimension(dimension);
}

AcceptedBoundaryChain BoundaryAuthoringSession::close_chain() {
    require_editable(*this);
    if (!semantic_.active_chain.has_value()) invalid("no active chain is available for closure");
    if (!semantic_.active_chain->pending_dimensions.empty()) {
        invalid("all edge dimensions must be placed before closure");
    }
    auto semantic = semantic_;
    auto& chain = *semantic.active_chain;
    if (mode() == BoundaryAuthoringMode::define_first && !chain.classified) {
        invalid("Define First requires classification before closure");
    }
    if (chain.segments.empty()) invalid("cannot close an empty boundary chain");
    if (!same_point(chain.segments.back().segment.end, chain.anchor)) {
        invalid("boundary chain is not exactly closed");
    }
    auto closure_usage = history_ ? history_->usage : base_resource_usage_;
    std::size_t closure_work;
    if (!boundary_authoring_recovery_checked_multiply(chain.segments.size(), chain.segments.size(), closure_work) ||
        !boundary_authoring_recovery_checked_add(closure_work, 1, closure_work) ||
        !boundary_authoring_recovery_checked_add(closure_usage.replay_work, closure_work, closure_usage.replay_work))
        invalid("closure work accounting overflow");
    detail::validate_authoring_usage(closure_usage, resource_policy_);
    const auto public_draft = materialize(chain);
    validate_strict_chain(public_draft, options_.geometry_tolerance_metres);
    CompactAcceptedChain compact;
    compact.anchor = chain.anchor;
    compact.boundary_id = chain.boundary_id;
    compact.type = chain.type;
    compact.classification = chain.classification;
    compact.segments = chain.segments;
    compact.dimensions = chain.dimensions;
    compact.receipts = chain.receipts;
    compact.classified = chain.classified;
    auto accepted = materialize(compact);
    semantic.accepted_chains.push_back(std::move(compact));
    semantic.active_chain.reset();
    semantic.pen_state = BoundaryPenState::up;
    semantic.phase = BoundaryAuthoringPhase::completed;
    if (mode() == BoundaryAuthoringMode::draw_first) semantic.classification.reset();
    const auto ids = IdCounters{next_boundary_id_, next_vertex_id_, next_segment_id_,
                                next_dimension_id_};
    BoundaryAuthoringAction action;
    action.kind = BoundaryAuthoringActionKind::close_chain;
    action.counters_before = public_counters();
    action.counters_after = action.counters_before;
    action.chain = chain_record(accepted);
    auto transaction = prepare_semantic(std::move(semantic), ids, std::move(action));
    PublishOnSuccess publication(*this, transaction);
    return AcceptedBoundaryChain(accepted);
}

bool BoundaryAuthoringSession::undo() {
    if (!history_) return false;
    invoke_fault(BoundaryAuthoringFaultPoint::before_semantic_mutation);
    MutationCandidate candidate;
    candidate.ids = {next_boundary_id_, next_vertex_id_, next_segment_id_, next_dimension_id_};
    candidate.history = history_->previous;
    candidate.semantic = candidate.history ? candidate.history->semantic : initial_semantic();
    candidate.redo = std::make_shared<const RedoNode>(history_, redo_);
    invoke_fault(BoundaryAuthoringFaultPoint::before_publication);
    publish_candidate(std::move(candidate));
    return true;
}

bool BoundaryAuthoringSession::redo() {
    if (!redo_) return false;
    if (redo_->entry->previous != history_ ||
        redo_->entry->position != history_position() + 1) {
        invalid("authoring redo zipper is inconsistent with history");
    }
    invoke_fault(BoundaryAuthoringFaultPoint::before_semantic_mutation);
    MutationCandidate candidate;
    candidate.ids = {next_boundary_id_, next_vertex_id_, next_segment_id_, next_dimension_id_};
    candidate.history = redo_->entry;
    candidate.semantic = candidate.history->semantic;
    candidate.redo = redo_->next;
    invoke_fault(BoundaryAuthoringFaultPoint::before_publication);
    publish_candidate(std::move(candidate));
    return true;
}

void BoundaryAuthoringSession::discard_redo_branch() noexcept {
    redo_.reset();
}

void BoundaryAuthoringSession::cancel() noexcept {
    auto replacement = initial_semantic();
    replacement.phase = BoundaryAuthoringPhase::cancelled;
    std::swap(semantic_, replacement);
    history_.reset();
    redo_.reset();
}

void BoundaryAuthoringSession::reset() noexcept {
    auto replacement = initial_semantic();
    std::swap(semantic_, replacement);
    pointer_.reset();
    history_.reset();
    redo_.reset();
}

void BoundaryAuthoringSession::commit_semantic(SemanticRoot candidate, IdCounters ids,
                                               BoundaryAuthoringAction action) {
    if (same_semantic_root(candidate, semantic_)) return;
    auto transaction = prepare_semantic(std::move(candidate), ids, std::move(action));
    publish_candidate(std::move(transaction));
}

BoundaryAuthoringSession::MutationCandidate BoundaryAuthoringSession::prepare_semantic(
    SemanticRoot candidate, IdCounters ids, BoundaryAuthoringAction action) {
    auto usage = history_ ? history_->usage : base_resource_usage_;
    auto delta = detail::authoring_action_usage(action, resource_policy_);
    const auto add = [](std::size_t& total, std::size_t amount) {
        if (!boundary_authoring_recovery_checked_add(total, amount, total))
            invalid("boundary authoring accounting overflow");
    };
    add(delta.retained_history_bytes, new_retained_bytes(candidate));
    add(delta.retained_history_bytes, sizeof(HistoryEntry) + sizeof(RedoNode) + 64);
    add(usage.action_count, delta.action_count);
    add(usage.generated_ids, delta.generated_ids);
    add(usage.encoded_bytes, delta.encoded_bytes);
    add(usage.json_values, delta.json_values);
    add(usage.string_bytes, delta.string_bytes);
    usage.json_depth = std::max(usage.json_depth, delta.json_depth);
    add(usage.replay_work, delta.replay_work);
    add(usage.retained_history_bytes, delta.retained_history_bytes);
    add(usage.materialization_bytes, delta.materialization_bytes);
    usage.materialization_bytes = std::max(usage.materialization_bytes, usage.retained_history_bytes);
    add(usage.cumulative_replay_copy_bytes, delta.retained_history_bytes);
    usage.operation_bytes = usage.retained_history_bytes;
    add(usage.operation_bytes, usage.materialization_bytes);
    if (candidate.active_chain &&
        (candidate.active_chain->segments.size() > resource_policy_.max_chain_edges ||
         candidate.active_chain->dimensions.size() > resource_policy_.max_dimensions_per_chain))
        invalid("active chain exceeds resource policy");
    detail::validate_authoring_usage(usage, resource_policy_);
    // The old redo branch remains reachable until publication. Its transient
    // charge is checked separately so discarded history never contaminates
    // the canonical prefix usage stored below or subsequent replay results.
    auto transient_usage = usage;
    auto old_and_candidate_bytes = resource_usage().retained_history_bytes;
    add(old_and_candidate_bytes, usage.materialization_bytes);
    transient_usage.operation_bytes = std::max(usage.operation_bytes, old_and_candidate_bytes);
    detail::validate_authoring_usage(transient_usage, resource_policy_);
    const auto new_sequence_chunks = count_new_sequence_chunks(semantic_, candidate);
    invoke_fault(BoundaryAuthoringFaultPoint::before_semantic_mutation);
    MutationCandidate transaction;
    transaction.ids = ids;
    invoke_fault(BoundaryAuthoringFaultPoint::before_action_append);
    const auto retained_sequence_chunks =
        (history_ ? history_->retained_sequence_chunks : 0) + new_sequence_chunks;
    auto entry = std::make_shared<HistoryEntry>(
        std::move(action), std::move(candidate), history_position() + 1,
        retained_sequence_chunks, history_);
    entry->usage = usage;
    transaction.history = std::move(entry);
    transaction.semantic = transaction.history->semantic;
    invoke_fault(BoundaryAuthoringFaultPoint::before_publication);
    return transaction;
}

void BoundaryAuthoringSession::publish_candidate(MutationCandidate&& candidate) noexcept {
    std::swap(semantic_, candidate.semantic);
    history_.swap(candidate.history);
    redo_.swap(candidate.redo);
    next_boundary_id_ = candidate.ids.boundary;
    next_vertex_id_ = candidate.ids.vertex;
    next_segment_id_ = candidate.ids.segment;
    next_dimension_id_ = candidate.ids.dimension;
}

void BoundaryAuthoringSession::publish_restored(BoundaryAuthoringSession&& candidate) noexcept {
    std::swap(mode_, candidate.mode_);
    // Aggregate moves allocate iterator proxies in the MSVC Debug STL.
    // Publication must exchange existing storage without allocating.
    options_.default_boundary_type.swap(candidate.options_.default_boundary_type);
    options_.boundary_id_prefix.swap(candidate.options_.boundary_id_prefix);
    options_.vertex_id_prefix.swap(candidate.options_.vertex_id_prefix);
    options_.segment_id_prefix.swap(candidate.options_.segment_id_prefix);
    options_.dimension_id_prefix.swap(candidate.options_.dimension_id_prefix);
    std::swap(options_.automatic_dimension_placement,
              candidate.options_.automatic_dimension_placement);
    std::swap(options_.automatic_placement_version,
              candidate.options_.automatic_placement_version);
    std::swap(options_.geometry_tolerance_metres,
              candidate.options_.geometry_tolerance_metres);
    std::swap(base_resource_usage_, candidate.base_resource_usage_);
    identity_namespace_.swap(candidate.identity_namespace_);
    pointer_.swap(candidate.pointer_);
    std::swap(semantic_, candidate.semantic_);
    history_.swap(candidate.history_);
    redo_.swap(candidate.redo_);
    std::swap(next_boundary_id_, candidate.next_boundary_id_);
    std::swap(next_vertex_id_, candidate.next_vertex_id_);
    std::swap(next_segment_id_, candidate.next_segment_id_);
    std::swap(next_dimension_id_, candidate.next_dimension_id_);
    recovery_extensions_.swap(candidate.recovery_extensions_);
}

BoundaryAuthoringCounters BoundaryAuthoringSession::public_counters() const noexcept {
    return {next_boundary_id_, next_vertex_id_, next_segment_id_, next_dimension_id_};
}

BoundaryAuthoringCounters BoundaryAuthoringSession::public_counters(
    const IdCounters& ids) noexcept {
    return {ids.boundary, ids.vertex, ids.segment, ids.dimension};
}

BoundaryAuthoringCounters BoundaryAuthoringSession::counters() const noexcept {
    return public_counters();
}

void BoundaryAuthoringSession::set_counters(const BoundaryAuthoringCounters& counters) noexcept {
    next_boundary_id_ = counters.next_boundary_id;
    next_vertex_id_ = counters.next_vertex_id;
    next_segment_id_ = counters.next_segment_id;
    next_dimension_id_ = counters.next_dimension_id;
}

void BoundaryAuthoringSession::invoke_fault(BoundaryAuthoringFaultPoint point) const {
    if (fault_hook_) fault_hook_(point);
}

void BoundaryAuthoringSession::set_fault_hook(BoundaryAuthoringFaultHook hook) {
    fault_hook_.swap(hook);
}

void BoundaryAuthoringSession::clear_fault_hook() noexcept {
    fault_hook_ = nullptr;
}

BoundaryAuthoringCheckpoint BoundaryAuthoringSession::recovery_checkpoint() const {
    if (semantic_.phase == BoundaryAuthoringPhase::cancelled) {
        invalid("cancelled authoring sessions have no active recovery checkpoint");
    }
    BoundaryAuthoringCheckpoint result;
    result.version = boundary_authoring_recovery_version;
    result.replay_version = boundary_authoring_recovery_replay_version;
    result.mode = mode_;
    result.options = options_;
    result.identity_namespace = identity_namespace_;
    result.pointer = pointer_;
    result.history_position = history_position();
    result.actions.resize(result.history_position + redo_depth());
    auto action_index = result.history_position;
    for (auto entry = history_; entry; entry = entry->previous) {
        if (action_index == 0) invalid("authoring history positions are inconsistent");
        result.actions[--action_index] = entry->action;
    }
    if (action_index != 0) invalid("authoring history is shorter than its position");
    action_index = result.history_position;
    for (auto node = redo_; node; node = node->next) {
        if (action_index >= result.actions.size()) {
            invalid("authoring redo is longer than its recorded depth");
        }
        result.actions[action_index++] = node->entry->action;
    }
    if (action_index != result.actions.size()) {
        invalid("authoring redo is shorter than its recorded depth");
    }
    result.counters = public_counters();
    result.extensions = nlohmann::json::parse(recovery_extensions_);
    return result;
}

void BoundaryAuthoringSession::apply_recovery_action(
    const BoundaryAuthoringAction& action) {
    const auto require_classification = [&]() -> std::string {
        if (!action.classification.has_value()) {
            invalid("classification action is missing its category");
        }
        return *action.classification;
    };
    const auto require_point_payload = [&]() -> Vec2 {
        if (!action.point.has_value()) invalid("anchor action is missing its point");
        return *action.point;
    };
    const auto require_receipt = [&]() -> const ConstructionReceipt& {
        if (!action.receipt.has_value()) invalid("edge action is missing its receipt");
        return *action.receipt;
    };
    const auto require_dimension = [&]() -> const BoundaryDimension& {
        if (!action.dimension.has_value()) {
            invalid("dimension action is missing its dimension");
        }
        return *action.dimension;
    };
    const auto require_chain = [&]() -> const BoundaryAuthoringChainRecord& {
        if (!action.chain.has_value()) invalid("close action is missing its chain record");
        return *action.chain;
    };

    switch (action.kind) {
        case BoundaryAuthoringActionKind::set_classification:
            set_classification(require_classification());
            return;
        case BoundaryAuthoringActionKind::classify_current_chain:
            classify_current_chain(require_classification());
            return;
        case BoundaryAuthoringActionKind::classify_last_chain:
            classify_last_chain(require_classification());
            return;
        case BoundaryAuthoringActionKind::anchor:
            (void)anchor(require_point_payload());
            return;
        case BoundaryAuthoringActionKind::pen_up:
            pen_up();
            return;
        case BoundaryAuthoringActionKind::pen_down:
            pen_down();
            return;
        case BoundaryAuthoringActionKind::line_heading: {
            const auto& receipt = require_receipt();
            if (!receipt.distance.has_value() || !receipt.heading.has_value()) {
                invalid("line heading action receipt is incomplete");
            }
            (void)add_line(*receipt.distance, *receipt.heading);
            return;
        }
        case BoundaryAuthoringActionKind::line_rise_run: {
            const auto& receipt = require_receipt();
            if (!receipt.rise.has_value() || !receipt.run.has_value()) {
                invalid("line rise/run action receipt is incomplete");
            }
            (void)add_line_rise_run(*receipt.rise, *receipt.run);
            return;
        }
        case BoundaryAuthoringActionKind::line_relative_turn: {
            const auto& receipt = require_receipt();
            if (!receipt.distance.has_value() || !receipt.turn.has_value()) {
                invalid("relative line action receipt is incomplete");
            }
            (void)add_line_relative_turn(*receipt.distance, *receipt.turn);
            return;
        }
        case BoundaryAuthoringActionKind::line_closure:
            (void)require_receipt();
            (void)add_closing_segment();
            return;
        case BoundaryAuthoringActionKind::line_to_point: {
            const auto& receipt = require_receipt();
            if (!receipt.chord_end.has_value()) {
                invalid("point-native line action receipt is incomplete");
            }
            (void)add_line_to(*receipt.chord_end);
            return;
        }
        case BoundaryAuthoringActionKind::arc_chord_angle: {
            const auto& receipt = require_receipt();
            if (!receipt.chord_end.has_value() || !receipt.angle.has_value()) {
                invalid("chord angle action receipt is incomplete");
            }
            (void)add_arc_chord_angle(*receipt.chord_end, *receipt.angle);
            return;
        }
        case BoundaryAuthoringActionKind::arc_chord_height: {
            const auto& receipt = require_receipt();
            if (!receipt.chord_end.has_value() || !receipt.height.has_value()) {
                invalid("chord height action receipt is incomplete");
            }
            (void)add_arc_chord_height(*receipt.chord_end, *receipt.height);
            return;
        }
        case BoundaryAuthoringActionKind::arc_chord_length: {
            const auto& receipt = require_receipt();
            if (!receipt.chord_end.has_value() || !receipt.arc_length.has_value()) {
                invalid("chord length action receipt is incomplete");
            }
            (void)add_arc_chord_arc_length(*receipt.chord_end, *receipt.arc_length,
                                           receipt.clockwise);
            return;
        }
        case BoundaryAuthoringActionKind::arc_start_tangent: {
            const auto& receipt = require_receipt();
            if (!receipt.tangent.has_value() || !receipt.arc_length.has_value() ||
                !receipt.sweep.has_value()) {
                invalid("start tangent action receipt is incomplete");
            }
            (void)add_arc_start_tangent(*receipt.tangent, *receipt.arc_length,
                                        *receipt.sweep);
            return;
        }
        case BoundaryAuthoringActionKind::manual_dimension:
            (void)place_manual_dimension(require_dimension().text_position);
            return;
        case BoundaryAuthoringActionKind::automatic_dimension:
            (void)require_dimension();
            (void)place_automatic_dimension();
            return;
        case BoundaryAuthoringActionKind::close_chain:
            (void)require_chain();
            (void)close_chain();
            return;
    }
    invalid("unknown boundary authoring action kind");
}

void BoundaryAuthoringSession::restore_recovery_checkpoint(
    const BoundaryAuthoringCheckpoint& checkpoint) {
    if (checkpoint.version != boundary_authoring_recovery_version) {
        invalid("unsupported boundary authoring recovery version");
    }
    if (checkpoint.replay_version != boundary_authoring_recovery_replay_version) {
        invalid("unsupported boundary authoring recovery replay version");
    }
    if (!checkpoint.extensions.is_object()) {
        invalid("boundary authoring recovery extensions must be an object");
    }
    detail::validate_authoring_checkpoint_raw(checkpoint, resource_policy_);
    if (checkpoint.history_position > checkpoint.actions.size()) {
        invalid("boundary authoring recovery history position exceeds its actions");
    }
    require_recovery_identifier(checkpoint.identity_namespace, "recovery identity namespace");
    constexpr std::size_t maximum_counter_text = 20;
    if (checkpoint.identity_namespace.size() + maximum_counter_text + 2 > 128) {
        invalid("recovery identity namespace exceeds the identifier limit");
    }
    const auto maximum_prefix_size =
        128 - checkpoint.identity_namespace.size() - maximum_counter_text - 2;
    if (checkpoint.options.boundary_id_prefix.size() > maximum_prefix_size ||
        checkpoint.options.vertex_id_prefix.size() > maximum_prefix_size ||
        checkpoint.options.segment_id_prefix.size() > maximum_prefix_size ||
        checkpoint.options.dimension_id_prefix.size() > maximum_prefix_size) {
        invalid("recovery authoring prefix exceeds the identifier limit");
    }
    require_recovery_counter_values(checkpoint.counters, "recovery counters");
    if (checkpoint.pointer.has_value()) require_point(*checkpoint.pointer, "recovery pointer");

    BoundaryAuthoringSession candidate(checkpoint.mode, checkpoint.options, resource_policy_);
    candidate.identity_namespace_ = checkpoint.identity_namespace;
    candidate.next_boundary_id_ = 1;
    candidate.next_vertex_id_ = 1;
    candidate.next_segment_id_ = 1;
    candidate.next_dimension_id_ = 1;
    candidate.recovery_extensions_ = checkpoint.extensions.dump();
    candidate.base_resource_usage_ = detail::authoring_context_usage(
        candidate.options_, candidate.mode_, candidate.identity_namespace_,
        checkpoint.extensions, candidate.resource_policy_);

    for (const auto& action : checkpoint.actions) {
        require_recovery_counter_values(action.counters_before, "action counters_before");
        require_recovery_counter_values(action.counters_after, "action counters_after");
        if (!counters_dominate(action.counters_after, action.counters_before)) {
            invalid("action counters decrease during recovery replay");
        }
        const auto current = candidate.public_counters();
        if (!counters_dominate(action.counters_before, current)) {
            invalid("action counters move backward during recovery replay");
        }
        candidate.set_counters(action.counters_before);
        const auto previous_action_count = candidate.history_position();
        candidate.apply_recovery_action(action);
        if (candidate.history_position() != previous_action_count + 1 ||
            !candidate.history_ || !(candidate.history_->action == action)) {
            invalid("recovery action differs from canonical authoring replay");
        }
    }

    if (!counters_dominate(checkpoint.counters, candidate.public_counters())) {
        invalid("recovery counters do not dominate the action timeline");
    }
    candidate.set_counters(checkpoint.counters);
    while (candidate.history_position() > checkpoint.history_position) {
        if (!candidate.undo()) invalid("recovery history could not reach its saved position");
    }
    if (candidate.history_position() != checkpoint.history_position ||
        candidate.redo_depth() != checkpoint.actions.size() - checkpoint.history_position) {
        invalid("recovery history position is inconsistent");
    }
    candidate.pointer_ = checkpoint.pointer;
    invoke_fault(BoundaryAuthoringFaultPoint::before_publication);
    publish_restored(std::move(candidate));
}

BoundaryAuthoringSession BoundaryAuthoringSession::revise_recovery_checkpoint(
    const BoundaryAuthoringCheckpoint& checkpoint, BoundaryAuthoringResourcePolicy policy) {
    // Authenticate all original receipts and identifiers before treating their
    // semantic payloads as instructions for a new session.
    (void)from_recovery_checkpoint(checkpoint, policy);
    BoundaryAuthoringSession candidate(checkpoint.mode, checkpoint.options, policy);
    if (candidate.identity_namespace_ == checkpoint.identity_namespace)
        invalid("revised session identity collides with the original");
    candidate.recovery_extensions_ = checkpoint.extensions.dump();
    candidate.base_resource_usage_ = detail::authoring_context_usage(
        candidate.options_, candidate.mode_, candidate.identity_namespace_,
        checkpoint.extensions, policy);
    for (const auto& action : checkpoint.actions) {
        candidate.set_counters(action.counters_before);
        candidate.apply_recovery_action(action);
    }
    candidate.set_counters(checkpoint.counters);
    while (candidate.history_position() > checkpoint.history_position) {
        if (!candidate.undo()) invalid("revised history could not reach its saved position");
    }
    candidate.pointer_ = checkpoint.pointer;
    return candidate;
}

BoundaryAuthoringSession BoundaryAuthoringSession::from_recovery_checkpoint(
    const BoundaryAuthoringCheckpoint& checkpoint, BoundaryAuthoringResourcePolicy policy) {
    BoundaryAuthoringSession result(checkpoint.mode, checkpoint.options, policy);
    result.restore_recovery_checkpoint(checkpoint);
    return result;
}

}  // namespace sketch
