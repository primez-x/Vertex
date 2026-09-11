#include "sketch/recovery_ledger.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"

#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument("recovery ledger: " + std::string(message));
}
void add(std::size_t& total, std::size_t amount, std::size_t limit) {
    if (!boundary_authoring_recovery_checked_add(total, amount, total) || total > limit)
        invalid("aggregate resource budget exceeded");
}
std::size_t multiply(std::size_t a, std::size_t b) {
    std::size_t result;
    if (!boundary_authoring_recovery_checked_multiply(a, b, result)) invalid("resource accounting overflow");
    return result;
}
void identifier(std::string_view value) {
    if (value.empty() || value.size() > 128 || value.find('\0') != std::string_view::npos)
        invalid("record identifier or kind must contain 1 to 128 bytes without NUL");
    // Validate borrowed UTF-8 without constructing a JSON string first.
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        unsigned count;
        std::uint32_t code, minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 0x07; minimum = 0x10000; }
        else invalid("invalid UTF-8 record identifier or kind");
        if (count > value.size() - i) invalid("truncated UTF-8 record identifier or kind");
        while (count--) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) invalid("invalid UTF-8 continuation");
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            invalid("invalid UTF-8 scalar");
    }
}
bool future(const Json& value) {
    if (!value.is_object()) invalid("known envelope must be an object");
    bool result = false;
    for (const auto* field : {"version", "replay_version"}) {
        if (!value.contains(field)) invalid("missing version discriminator");
        const auto& v = value.at(field);
        std::uint64_t number;
        if (v.is_number_unsigned()) number = v.get<std::uint64_t>();
        else if (v.is_number_integer() && v.get<std::int64_t>() > 0)
            number = static_cast<std::uint64_t>(v.get<std::int64_t>());
        else invalid("version discriminator must be a positive integer");
        if (!number) invalid("version discriminator must be positive");
        result = result || number != 1;
    }
    return result;
}
const Json& array(const Json& value) {
    if (!value.is_array()) invalid("expected array");
    return value;
}
bool scan_input_versions(const Json& value) {
    if (future(value)) return true;
    return future(value.at("checkpoint"));
}
bool scan_history_versions(const Json& history) {
    bool unsupported = false;
    for (const auto* field : {"events", "retired"}) {
        for (const auto& row : array(history.at(field))) {
            const auto& input = row.at("input");
            if (input.is_null()) continue;
            const auto& value = input.at("value");
            if (!value.is_null()) {
                const bool nested = scan_input_versions(value);
                unsupported = unsupported || nested;
            }
        }
    }
    return unsupported;
}
void scan_actions(const Json& input, WorkspaceRecoveryUsage& usage, const WorkspaceRecoveryLimits& limits) {
    const auto& actions = array(input.at("checkpoint").at("actions"));
    add(usage.inputs, 1, limits.max_inputs);
    add(usage.actions, actions.size(), limits.max_actions);
    for (const auto& action : actions) {
        add(usage.replay_work, 1, limits.max_replay_work);
        if (action.is_object() && action.contains("chain")) {
            const auto edges = array(action.at("chain").at("edges")).size();
            add(usage.replay_work, multiply(edges, edges), limits.max_replay_work);
        }
    }
}
void validation_work(const WorkspaceRecoveryUsage& usage, const WorkspaceRecoveryLimits& limits) {
    std::size_t base = usage.entity_rows;
    for (const auto amount : {usage.asset_rows, usage.replay_work, usage.events,
                             usage.json_values, usage.asset_bytes / 1024})
        add(base, amount, limits.max_validation_work);
    auto passes = usage.events;
    add(passes, 8, limits.max_validation_work);
    if (multiply(base, passes) > limits.max_validation_work) invalid("aggregate validation work budget exceeded");
}
} // namespace

WorkspaceRecoveryUsage preflight_recovery_ledger(const DocumentSnapshot& document,
    const RecoveryLedger& ledger, const BoundaryAuthoringResourcePolicy& policy,
    const WorkspaceRecoveryLimits& limits) {
        if (ledger.empty()) invalid("ledger must not be empty");
        auto usage = preflight_workspace_recovery(document, {}, {}, {}, {}, {}, policy, limits);
        auto wire_policy = policy;
        wire_policy.max_encoded_bytes = limits.max_encoded_bytes;
        wire_policy.max_json_values = limits.max_json_values;
        wire_policy.max_string_bytes = limits.max_string_bytes;
        // Reserve the ledger array and row objects, their keys, punctuation and
        // the two bounded strings. Envelope trees remain borrowed throughout.
        add(usage.encoded_bytes, 2, limits.max_encoded_bytes);
        add(usage.json_values, 1, limits.max_json_values);
        for (const auto& row : ledger) {
            identifier(row.record_id); identifier(row.record_kind);
            add(usage.encoded_bytes, 64, limits.max_encoded_bytes);
            add(usage.json_values, 6, limits.max_json_values);
            add(usage.string_bytes, 29, limits.max_string_bytes); // wrapper key bytes
            for (const auto* text : {&row.record_id, &row.record_kind}) {
                add(usage.string_bytes, text->size(), limits.max_string_bytes);
                add(usage.encoded_bytes, multiply(text->size(), 6), limits.max_encoded_bytes);
            }
            const auto wire = detail::measure_authoring_recovery_json(row.envelope, wire_policy);
            add(usage.encoded_bytes, wire.encoded_bytes, limits.max_encoded_bytes);
            add(usage.json_values, wire.json_values, limits.max_json_values);
            add(usage.string_bytes, wire.string_bytes, limits.max_string_bytes);
        }
        validation_work(usage, limits);
        return usage;
}

RecoveryLedgerDecodeResult decode_recovery_ledger(const DocumentSnapshot& document,
    const RecoveryLedger& ledger, ArchiveRole role, const BoundaryAuthoringResourcePolicy& policy,
    const WorkspaceRecoveryLimits& limits) {
    try {
        if (role != ArchiveRole::ordinary && role != ArchiveRole::recovery_copy) invalid("unknown archive role");
        auto usage = preflight_recovery_ledger(document, ledger, policy, limits);
        std::set<std::string_view> ids;
        const Json *active = nullptr, *history = nullptr, *copy = nullptr;
        bool unsupported = false;
        for (const auto& row : ledger) {
            if (!ids.insert(row.record_id).second) invalid("duplicate record ID");
            const Json** slot = nullptr;
            if (row.record_kind == "boundary_active") slot = &active;
            else if (row.record_kind == "workspace_history") slot = &history;
            else if (row.record_kind == "recovery_copy") slot = &copy;
            else { unsupported = true; continue; }
            if (*slot) invalid("duplicate known record kind");
            *slot = &row.envelope;
            const bool outer_future = future(row.envelope);
            unsupported = unsupported || outer_future;
            if (!outer_future) {
                if (slot == &active) {
                    const bool nested = future(row.envelope.at("checkpoint"));
                    unsupported = unsupported || nested;
                } else if (slot == &history) {
                    const bool nested = scan_history_versions(row.envelope);
                    unsupported = unsupported || nested;
                }
            }
        }
        if ((active || copy) && !history) invalid("active and recovery-copy records require workspace history");
        const bool role_mismatch = (role == ArchiveRole::ordinary && copy) ||
                                   (role == ArchiveRole::recovery_copy && !copy);
        if (unsupported || role_mismatch)
            return {std::nullopt, ledger, unsupported ? "unsupported recovery ledger kind or version" : "archive role and recovery records disagree"};

        // Charge all raw owners together before even the active checkpoint is
        // canonically replayed. References have null values and cost no replay.
        if (active) scan_actions(*active, usage, limits);
        if (history) {
            const auto& events = array(history->at("events"));
            add(usage.events, events.size(), limits.max_events);
            if (array(history->at("retired")).size() > limits.max_inputs) invalid("retired input budget exceeded");
            if (array(history->at("document_history").at("events")).size() > limits.max_events)
                invalid("document event budget exceeded");
            const auto& baseline = history->at("document_history").at("baseline");
            for (const auto* field : {"undo_stack", "redo_stack"})
                if (array(baseline.at(field)).size() > limits.max_document_revisions)
                    invalid("baseline stack budget exceeded");
            const auto& navigation = history->at("navigation");
            for (const auto* field : {"undo_stack", "redo_stack", "operations"})
                if (array(navigation.at(field)).size() > limits.max_events)
                    invalid("navigation budget exceeded");
            for (const auto* field : {"events", "retired"}) for (const auto& row : history->at(field)) {
                const auto& input = row.at("input");
                if (!input.is_null() && !input.at("value").is_null()) scan_actions(input.at("value"), usage, limits);
            }
        }
        validation_work(usage, limits);
        DecodedRecoveryLedger result;
        if (active) {
            auto value = decode_boundary_active_recovery(*active, policy);
            if (!value.supported()) invalid("unsupported active escaped version scan");
            result.active = std::move(value.active);
        }
        if (history) {
            auto value = decode_workspace_history_record(document, *history, result.active, policy, limits);
            if (!value.supported()) invalid("unsupported history escaped version scan");
            result.history = std::move(value.record);
        }
        if (copy) {
            auto value = decode_recovery_copy_record(*copy, policy);
            if (!value.supported()) invalid("unsupported recovery copy escaped version scan");
            result.recovery_copy = std::move(value.record);
            validate_recovery_copy_document(*result.recovery_copy, document, policy);
            const auto& c = *result.recovery_copy;
            const auto& h = *result.history;
            if (c.workspace_epoch != h.workspace_epoch || c.edited_generation != h.edited_generation ||
                c.checkpoint_generation != h.checkpoint_generation)
                invalid("recovery copy counters disagree with workspace history");
        }
        return {std::move(result), std::nullopt, {}};
    } catch (const Json::exception& error) {
        invalid(error.what());
    }
}
} // namespace sketch
