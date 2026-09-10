#include "sketch/workspace_history_record.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument("workspace history: " + std::string(message));
}
void keys(const Json& v, std::initializer_list<const char*> names) {
    if (!v.is_object() || v.size() != names.size()) invalid("unexpected object fields");
    for (auto name : names) if (!v.contains(name)) invalid("missing required field");
}
const Json& array(const Json& v) {
    if (!v.is_array()) invalid("expected array");
    return v;
}
const std::string& string(const Json& v) {
    if (!v.is_string()) invalid("expected string");
    return v.get_ref<const std::string&>();
}
void identifier(std::string_view v, bool empty = false) {
    if ((!empty && v.empty()) || v.size() > 128 || v.find('\0') != std::string_view::npos)
        invalid("invalid identifier");
}
std::string id(const Json& v, bool empty = false) {
    const auto& s = string(v); identifier(s, empty); return s;
}
std::uint64_t integer(const Json& v) {
    if (v.is_number_unsigned()) return v.get<std::uint64_t>();
    if (!v.is_number_integer() || v.get<std::int64_t>() < 0) invalid("expected unsigned integer");
    return static_cast<std::uint64_t>(v.get<std::int64_t>());
}
bool future(const Json& v) {
    if (!v.is_object() || !v.contains("version")) invalid("missing version discriminator");
    const auto version = integer(v.at("version"));
    if (!version) invalid("version must be positive");
    if (version != 1) return true;
    if (!v.contains("replay_version")) invalid("missing replay version");
    const auto replay = integer(v.at("replay_version"));
    if (!replay) invalid("replay version must be positive");
    return replay != 1;
}
void add(std::size_t& total, std::size_t n, std::size_t limit) {
    if (!boundary_authoring_recovery_checked_add(total, n, total) || total > limit)
        invalid("aggregate resource budget exceeded");
}
BoundaryAuthoringResourcePolicy wire_policy(const BoundaryAuthoringResourcePolicy& p,
                                           const WorkspaceRecoveryLimits& l) {
    auto result = p;
    result.max_encoded_bytes = l.max_encoded_bytes;
    result.max_json_values = l.max_json_values;
    result.max_string_bytes = l.max_string_bytes;
    return result;
}
void charge_wire(WorkspaceRecoveryUsage& u, const BoundaryAuthoringResourceUsage& w,
                 const WorkspaceRecoveryLimits& l) {
    add(u.encoded_bytes, w.encoded_bytes, l.max_encoded_bytes);
    add(u.json_values, w.json_values, l.max_json_values);
    add(u.string_bytes, w.string_bytes, l.max_string_bytes);
}
std::optional<std::string> optional_id(const Json& v) {
    return v.is_null() ? std::nullopt : std::optional<std::string>(id(v));
}
Json optional_id(const std::optional<std::string>& v) {
    if (!v) return nullptr;
    identifier(*v); return *v;
}
constexpr const char* lifecycle_names[] = {"document_edit", "boundary_activate", "boundary_discard",
    "boundary_finish", "undo", "redo", "clear_redo"};
constexpr const char* document_names[] = {"edit", "undo", "redo"};
template<class E, std::size_t N> E read_kind(const Json& v, const char* const (&names)[N]) {
    const auto& s = string(v);
    for (std::size_t i = 0; i < N; ++i) if (s == names[i]) return static_cast<E>(i);
    invalid("unknown kind");
}
template<class E, std::size_t N> const char* write_kind(E v, const char* const (&names)[N]) {
    const auto i = static_cast<std::size_t>(v);
    if (i >= N) invalid("unknown kind");
    return names[i];
}
constexpr const char* operation_names[] = {"document_edit", "boundary_activate", "boundary_discard", "boundary_finish"};
Json write_source(const BoundaryRecoverySource& s) {
    identifier(s.document_id);
    for (const auto* v : {&s.context.property_id, &s.context.building_id, &s.context.floor_id, &s.context.layer_id})
        identifier(*v, true);
    if (s.authoring_digest.size() != 64 || !std::all_of(s.authoring_digest.begin(), s.authoring_digest.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) invalid("invalid source digest");
    return {{"document_id", s.document_id}, {"revision", s.revision}, {"authoring_digest", s.authoring_digest},
        {"context", {{"property_id", s.context.property_id}, {"building_id", s.context.building_id},
        {"floor_id", s.context.floor_id}, {"layer_id", s.context.layer_id}}}};
}
BoundaryRecoverySource read_source(const Json& v) {
    keys(v, {"document_id", "revision", "authoring_digest", "context"});
    const auto& c = v.at("context"); keys(c, {"property_id", "building_id", "floor_id", "layer_id"});
    BoundaryRecoverySource s{id(v.at("document_id")), integer(v.at("revision")), string(v.at("authoring_digest")),
        {id(c.at("property_id"), true), id(c.at("building_id"), true), id(c.at("floor_id"), true), id(c.at("layer_id"), true)}};
    (void)write_source(s); return s;
}
Json write_counters(const BoundaryAuthoringCounters& c) {
    return {{"next_boundary_id", c.next_boundary_id}, {"next_vertex_id", c.next_vertex_id},
        {"next_segment_id", c.next_segment_id}, {"next_dimension_id", c.next_dimension_id}};
}
BoundaryAuthoringCounters read_counters(const Json& v) {
    keys(v, {"next_boundary_id", "next_vertex_id", "next_segment_id", "next_dimension_id"});
    return {integer(v.at("next_boundary_id")), integer(v.at("next_vertex_id")),
        integer(v.at("next_segment_id")), integer(v.at("next_dimension_id"))};
}
Json write_target(const WorkspaceNavigationTarget& t) {
    if (const auto* r = std::get_if<Revision>(&t.identity)) return {{"kind", "baseline"}, {"id", *r}};
    const auto& s = std::get<std::string>(t.identity); identifier(s);
    return {{"kind", "event"}, {"id", s}};
}
WorkspaceNavigationTarget read_target(const Json& v) {
    keys(v, {"kind", "id"});
    if (string(v.at("kind")) == "baseline") return {integer(v.at("id"))};
    if (string(v.at("kind")) == "event") return {id(v.at("id"))};
    invalid("unknown target kind");
}
Json write_session(const WorkspaceSessionIdentity& s) {
    identifier(s.identity_namespace);
    const char* mode;
    switch (s.mode) {
    case BoundaryAuthoringMode::draw_first: mode = "draw_first"; break;
    case BoundaryAuthoringMode::define_first: mode = "define_first"; break;
    default: invalid("unknown session mode");
    }
    return {{"source", write_source(s.source)}, {"identity_namespace", s.identity_namespace}, {"mode", mode},
        {"initial_counters", write_counters(s.initial_counters)}, {"revised_from_namespace", optional_id(s.revised_from_namespace)},
        {"revised_from_finish_event_id", optional_id(s.revised_from_finish_event_id)}};
}
WorkspaceSessionIdentity read_session(const Json& v) {
    keys(v, {"source", "identity_namespace", "mode", "initial_counters", "revised_from_namespace", "revised_from_finish_event_id"});
    const auto& mode = string(v.at("mode"));
    if (mode != "draw_first" && mode != "define_first") invalid("unknown session mode");
    return {read_source(v.at("source")), id(v.at("identity_namespace")),
        mode == "draw_first" ? BoundaryAuthoringMode::draw_first : BoundaryAuthoringMode::define_first,
        read_counters(v.at("initial_counters")), optional_id(v.at("revised_from_namespace")), optional_id(v.at("revised_from_finish_event_id"))};
}
Json write_input(const WorkspaceArchivedInput& input, bool full, const BoundaryAuthoringResourcePolicy& p) {
    identifier(input.owner_event_id);
    Json point = nullptr;
    if (input.pointer_override && *input.pointer_override) {
        const auto v = **input.pointer_override;
        if (!std::isfinite(v.x) || !std::isfinite(v.y)) invalid("nonfinite pointer");
        point = Json::array({v.x, v.y});
    }
    if (!input.value) invalid("missing input owner");
    if (input.status != WorkspaceInputStatus::active && input.status != WorkspaceInputStatus::retired) invalid("unknown input status");
    return {{"owner_event_id", input.owner_event_id},
        {"value", full ? encode_boundary_active_recovery(*input.value, p) : Json(nullptr)},
        {"pointer_override", {{"present", input.pointer_override.has_value()}, {"point", std::move(point)}}},
        {"status", input.status == WorkspaceInputStatus::active ? "active" : "retired"},
        {"finish_event_id", optional_id(input.finish_event_id)}};
}
using Owners = std::map<std::string, std::shared_ptr<const BoundaryActiveRecovery>, std::less<>>;
WorkspaceArchivedInput read_input(const Json& v, const std::string* event, Owners& owners,
                                 const BoundaryAuthoringResourcePolicy& p) {
    keys(v, {"owner_event_id", "value", "pointer_override", "status", "finish_event_id"});
    WorkspaceArchivedInput result;
    result.owner_event_id = id(v.at("owner_event_id"));
    const bool full = event && *event == result.owner_event_id;
    if (full) {
        if (v.at("value").is_null()) invalid("owner payload missing");
        auto decoded = decode_boundary_active_recovery(v.at("value"), p);
        if (!decoded.supported()) invalid("unsupported input escaped version scan");
        result.value = std::make_shared<const BoundaryActiveRecovery>(std::move(*decoded.active));
        if (!owners.emplace(result.owner_event_id, result.value).second) invalid("duplicate owner");
    } else {
        if (!v.at("value").is_null()) invalid("reference must not contain payload");
        const auto found = owners.find(result.owner_event_id);
        if (found == owners.end()) invalid("input reference must name a prior direct owner");
        result.value = found->second;
    }
    const auto& pointer = v.at("pointer_override"); keys(pointer, {"present", "point"});
    if (!pointer.at("present").is_boolean()) invalid("pointer presence must be boolean");
    const auto& point = pointer.at("point");
    if (!pointer.at("present").get<bool>()) {
        if (!point.is_null()) invalid("absent pointer must be null");
    } else {
        result.pointer_override.emplace();
        if (!point.is_null()) {
            if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) invalid("invalid pointer");
            Vec2 position{point[0].get<double>(), point[1].get<double>()};
            if (!std::isfinite(position.x) || !std::isfinite(position.y)) invalid("nonfinite pointer");
            *result.pointer_override = position;
        }
    }
    const auto& status = string(v.at("status"));
    if (status != "active" && status != "retired") invalid("unknown input status");
    result.status = status == "active" ? WorkspaceInputStatus::active : WorkspaceInputStatus::retired;
    result.finish_event_id = optional_id(v.at("finish_event_id"));
    return result;
}
std::vector<Revision> read_revisions(const Json& v) {
    std::vector<Revision> result;
    for (const auto& r : array(v)) result.push_back(integer(r));
    return result;
}
void scan_actions(const Json& input, WorkspaceRecoveryUsage& usage, const WorkspaceRecoveryLimits& limits) {
    const auto& checkpoint = input.at("checkpoint");
    const auto& actions = array(checkpoint.at("actions"));
    add(usage.inputs, 1, limits.max_inputs);
    add(usage.actions, actions.size(), limits.max_actions);
    for (const auto& action : actions) {
        add(usage.replay_work, 1, limits.max_replay_work);
        if (action.is_object() && action.contains("chain")) {
            const auto& chain = action.at("chain");
            if (!chain.is_object() || !chain.contains("edges")) invalid("invalid action chain");
            const auto edges = array(chain.at("edges")).size();
            std::size_t work;
            if (!boundary_authoring_recovery_checked_multiply(edges, edges, work)) invalid("closure accounting overflow");
            add(usage.replay_work, work, limits.max_replay_work);
        }
    }
}
void preflight_wire_history(const DocumentSnapshot& document, const Json& envelope,
    const std::optional<BoundaryActiveRecovery>& active, const BoundaryAuthoringResourcePolicy& p,
    const WorkspaceRecoveryLimits& limits, const BoundaryAuthoringResourceUsage& wire) {
    const auto& event_rows = array(envelope.at("events"));
    const auto& retired_rows = array(envelope.at("retired"));
    auto usage = preflight_workspace_recovery(document, {}, {}, {}, active, {}, p, limits);
    charge_wire(usage, wire, limits);
    add(usage.events, event_rows.size(), limits.max_events);
    if (retired_rows.size() > limits.max_inputs) invalid("too many retired inputs");
    for (const auto& row : event_rows) {
        const auto& input = row.at("input");
        if (!input.is_null() && !input.at("value").is_null()) scan_actions(input.at("value"), usage, limits);
    }
    // Include the borrowed history before any owner is canonically replayed.
    std::size_t base = usage.entity_rows;
    for (const auto value : {usage.asset_rows, usage.replay_work, usage.events,
                             usage.json_values, usage.asset_bytes / 1024})
        add(base, value, limits.max_validation_work);
    std::size_t passes = usage.events, work;
    add(passes, 8, limits.max_validation_work);
    if (!boundary_authoring_recovery_checked_multiply(base, passes, work) ||
        work > limits.max_validation_work) invalid("aggregate validation work budget exceeded");
}
} // namespace

WorkspaceHistoryRecord capture_workspace_history_record(const ProjectWorkspaceSnapshot& snapshot) {
    return {snapshot.document_history(), snapshot.lifecycle_history(), snapshot.navigation(), snapshot.retired_boundaries(),
        snapshot.epoch(), snapshot.edited_generation(), snapshot.checkpoint_generation(), Json::object()};
}

void validate_workspace_history_record(const DocumentSnapshot& document, const WorkspaceHistoryRecord& record,
    const std::optional<BoundaryActiveRecovery>& active, const BoundaryAuthoringResourcePolicy& p,
    const WorkspaceRecoveryLimits& limits) {
    auto usage = preflight_workspace_recovery(document, record.document_history, record.events, record.navigation,
        active, record.retired, p, limits);
    if (!record.extensions.is_object()) invalid("extensions must be an object");
    const auto extension_usage = detail::measure_authoring_recovery_json(record.extensions, wire_policy(p, limits));
    charge_wire(usage, extension_usage, limits);
    std::size_t work;
    if (!boundary_authoring_recovery_checked_multiply(extension_usage.json_values, record.events.size() + 8, work))
        invalid("extension accounting overflow");
    add(usage.validation_work, work, limits.max_validation_work);
    identifier(record.document_history.baseline.document_id);
    for (const auto& event : record.document_history.events) {
        identifier(event.event_id); (void)write_kind(event.kind, document_names);
    }
    for (const auto& event : record.events) {
        identifier(event.event_id); (void)write_kind(event.kind, lifecycle_names);
        if (event.target) (void)write_target(*event.target);
        if (event.session) (void)write_session(*event.session);
        if (event.input) {
            identifier(event.input->owner_event_id);
            (void)optional_id(event.input->finish_event_id);
        }
    }
    for (const auto& [name, input] : record.retired) { identifier(name); identifier(input.owner_event_id); (void)optional_id(input.finish_event_id); }
    for (const auto& [name, kind] : record.navigation.operations) { identifier(name); (void)write_kind(kind, operation_names); }
    for (const auto* stack : {&record.navigation.undo_stack, &record.navigation.redo_stack})
        for (const auto& target : *stack) (void)write_target(target);
    validate_workspace_lifecycle_slots(document, record.document_history, record.events, record.navigation, active, record.retired, p);
}

Json encode_workspace_history_record(const DocumentSnapshot& document, const WorkspaceHistoryRecord& record,
    const std::optional<BoundaryActiveRecovery>& active, const BoundaryAuthoringResourcePolicy& p,
    const WorkspaceRecoveryLimits& limits) {
    validate_workspace_history_record(document, record, active, p, limits);
    const auto& b = record.document_history.baseline;
    Json history_events = Json::array(), events = Json::array(), retired = Json::array();
    for (const auto& e : record.document_history.events)
        history_events.push_back({{"event_id", e.event_id}, {"sequence", e.sequence}, {"kind", write_kind(e.kind, document_names)},
            {"before_revision", e.before_revision}, {"after_revision", e.after_revision}});
    for (const auto& e : record.events)
        events.push_back({{"event_id", e.event_id}, {"sequence", e.sequence}, {"kind", write_kind(e.kind, lifecycle_names)},
            {"before_revision", e.before_revision}, {"after_revision", e.after_revision},
            {"target", e.target ? write_target(*e.target) : Json(nullptr)},
            {"session", e.session ? write_session(*e.session) : Json(nullptr)},
            {"input", e.input ? write_input(*e.input, e.input->owner_event_id == e.event_id, p) : Json(nullptr)}});
    for (const auto& [name, input] : record.retired)
        retired.push_back({{"identity_namespace", name}, {"input", write_input(input, false, p)}});
    Json undo = Json::array(), redo = Json::array(), operations = Json::array();
    for (const auto& t : record.navigation.undo_stack) undo.push_back(write_target(t));
    for (const auto& t : record.navigation.redo_stack) redo.push_back(write_target(t));
    for (const auto& [name, kind] : record.navigation.operations)
        operations.push_back({{"event_id", name}, {"kind", write_kind(kind, operation_names)}});
    Json result{{"version", 1}, {"replay_version", 1},
        {"document_history", {{"baseline", {{"document_id", b.document_id}, {"baseline_revision", b.baseline_revision},
            {"source_digest", b.source_digest}, {"undo_stack", b.undo_stack}, {"redo_stack", b.redo_stack}}}, {"events", std::move(history_events)}}},
        {"events", std::move(events)}, {"navigation", {{"undo_stack", std::move(undo)}, {"redo_stack", std::move(redo)}, {"operations", std::move(operations)}}},
        {"retired", std::move(retired)}, {"workspace_epoch", record.workspace_epoch}, {"edited_generation", record.edited_generation},
        {"checkpoint_generation", record.checkpoint_generation}, {"extensions", record.extensions}};
    const auto wire = detail::measure_authoring_recovery_json(result, wire_policy(p, limits));
    preflight_wire_history(document, result, active, p, limits, wire);
    return result;
}

WorkspaceHistoryDecodeResult decode_workspace_history_record(const DocumentSnapshot& document, const Json& envelope,
    const std::optional<BoundaryActiveRecovery>& active, const BoundaryAuthoringResourcePolicy& p,
    const WorkspaceRecoveryLimits& limits) {
    try {
        const auto wire = detail::measure_authoring_recovery_json(envelope, wire_policy(p, limits));
        const auto opaque = [&]() -> WorkspaceHistoryDecodeResult {
            return {std::nullopt, envelope, "unsupported workspace history or nested recovery version"};
        };
        if (future(envelope)) return opaque();
        keys(envelope, {"version", "replay_version", "document_history", "events", "navigation", "retired",
            "workspace_epoch", "edited_generation", "checkpoint_generation", "extensions"});
        const auto& event_rows = array(envelope.at("events"));
        const auto& retired_rows = array(envelope.at("retired"));
        // Scan every embedded dialect before interpreting or replaying any owner.
        for (const auto* rows : {&event_rows, &retired_rows}) for (const auto& row : *rows) {
            if (!row.is_object() || !row.contains("input")) invalid("missing input field");
            const auto& input = row.at("input");
            if (input.is_null()) continue;
            if (!input.is_object() || !input.contains("value")) invalid("missing input value");
            const auto& value = input.at("value");
            if (value.is_null()) continue;
            if (future(value)) return opaque();
            if (!value.contains("checkpoint")) invalid("missing checkpoint");
            if (future(value.at("checkpoint"))) return opaque();
        }
        preflight_wire_history(document, envelope, active, p, limits, wire);
        WorkspaceHistoryRecord record;
        const auto& history = envelope.at("document_history"); keys(history, {"baseline", "events"});
        const auto& baseline = history.at("baseline");
        keys(baseline, {"document_id", "baseline_revision", "source_digest", "undo_stack", "redo_stack"});
        for (const auto* stack : {"undo_stack", "redo_stack"})
            if (array(baseline.at(stack)).size() > limits.max_document_revisions) invalid("baseline stack budget exceeded");
        record.document_history.baseline = {id(baseline.at("document_id")), integer(baseline.at("baseline_revision")),
            string(baseline.at("source_digest")), read_revisions(baseline.at("undo_stack")), read_revisions(baseline.at("redo_stack"))};
        if (array(history.at("events")).size() > limits.max_events) invalid("document event budget exceeded");
        for (const auto& e : history.at("events")) {
            keys(e, {"event_id", "sequence", "kind", "before_revision", "after_revision"});
            record.document_history.events.push_back({id(e.at("event_id")), integer(e.at("sequence")),
                read_kind<WorkspaceDocumentEventKind>(e.at("kind"), document_names), integer(e.at("before_revision")), integer(e.at("after_revision"))});
        }
        Owners owners;
        std::set<std::string> event_ids;
        for (const auto& e : event_rows) {
            keys(e, {"event_id", "sequence", "kind", "before_revision", "after_revision", "target", "session", "input"});
            WorkspaceLifecycleEvent event;
            event.event_id = id(e.at("event_id"));
            if (!event_ids.insert(event.event_id).second) invalid("duplicate event ID");
            event.sequence = integer(e.at("sequence")); event.kind = read_kind<WorkspaceLifecycleKind>(e.at("kind"), lifecycle_names);
            event.before_revision = integer(e.at("before_revision")); event.after_revision = integer(e.at("after_revision"));
            if (!e.at("target").is_null()) event.target = read_target(e.at("target"));
            if (!e.at("session").is_null()) event.session = read_session(e.at("session"));
            if (!e.at("input").is_null()) event.input = read_input(e.at("input"), &event.event_id, owners, p);
            record.events.push_back(std::move(event));
        }
        const auto& nav = envelope.at("navigation"); keys(nav, {"undo_stack", "redo_stack", "operations"});
        for (const auto* field : {"undo_stack", "redo_stack", "operations"})
            if (array(nav.at(field)).size() > limits.max_events) invalid("navigation budget exceeded");
        for (const auto& t : nav.at("undo_stack")) record.navigation.undo_stack.push_back(read_target(t));
        for (const auto& t : nav.at("redo_stack")) record.navigation.redo_stack.push_back(read_target(t));
        for (const auto& op : nav.at("operations")) {
            keys(op, {"event_id", "kind"});
            if (!record.navigation.operations.emplace(id(op.at("event_id")), read_kind<WorkspaceOperationKind>(op.at("kind"), operation_names)).second)
                invalid("duplicate operation");
        }
        for (const auto& row : retired_rows) {
            keys(row, {"identity_namespace", "input"});
            auto name = id(row.at("identity_namespace"));
            auto input = read_input(row.at("input"), nullptr, owners, p);
            if (!record.retired.emplace(std::move(name), std::move(input)).second) invalid("duplicate retired namespace");
        }
        record.workspace_epoch = integer(envelope.at("workspace_epoch"));
        record.edited_generation = integer(envelope.at("edited_generation"));
        record.checkpoint_generation = integer(envelope.at("checkpoint_generation"));
        if (!envelope.at("extensions").is_object()) invalid("extensions must be an object");
        record.extensions = envelope.at("extensions");
        validate_workspace_history_record(document, record, active, p, limits);
        return {std::move(record), std::nullopt, {}};
    } catch (const Json::exception& error) {
        invalid(error.what());
    }
}
} // namespace sketch
