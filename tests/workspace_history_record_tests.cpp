#include "sketch/workspace_history_record.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("malformed workspace history record was accepted");
}
void commit(ProjectWorkspace& w, PreparedWorkspaceEdit ticket) { (void)w.commit(ticket); }
Document fixture() {
    return Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}});
}
void run(BoundaryAuthoringMode mode) {
    auto doc = fixture(); ProjectWorkspace w(doc.snapshot());
    BoundaryAuthoringSession session(mode);
    session.set_classification("living_area"); (void)session.anchor({0, 0});
    const auto dimension = [&] { if (mode == BoundaryAuthoringMode::define_first)
        (void)session.place_automatic_dimension(); };
    (void)session.add_line_to({4, 0}); dimension();
    (void)session.add_line_to({4, 3}); dimension();
    (void)session.add_line_to({0, 3}); dimension();
    (void)session.add_closing_segment(); dimension(); (void)session.close_chain();
    BoundaryActiveRecovery active{capture_boundary_recovery_source(w.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint(), {{"opaque_number", 1.0}}};
    commit(w, w.prepare_boundary_checkpoint(active));
    // Active input is a separate record, but required for complete validation.
    auto s = w.capture(); auto r = capture_workspace_history_record(s);
    auto wire = encode_workspace_history_record(s.document(), r, s.active_boundary());
    auto decoded = decode_workspace_history_record(s.document(), wire, s.active_boundary());
    require(decoded.supported(), "active-only history must round trip");
    rejects([&] { (void)decode_workspace_history_record(s.document(), wire, std::nullopt); });
    commit(w, w.prepare_discard_boundary()); commit(w, w.prepare_undo());
    active.checkpoint.pointer = Vec2{9, 8}; commit(w, w.prepare_boundary_checkpoint(active));
    commit(w, w.prepare_redo()); commit(w, w.prepare_undo());
    active.checkpoint.pointer.reset(); commit(w, w.prepare_boundary_checkpoint(active));
    commit(w, w.prepare_redo()); commit(w, w.prepare_undo());
    commit(w, w.prepare_finish_boundary()); commit(w, w.prepare_undo());
    s = w.capture(); r = capture_workspace_history_record(s);
    r.extensions = {{"future", {{"number", 1.0}, {"values", Json::array({nullptr, "x"})}}}};
    r.workspace_epoch = std::numeric_limits<std::uint64_t>::max();
    r.edited_generation = 0; r.checkpoint_generation = 3; // No invented cross-counter ordering.
    wire = encode_workspace_history_record(s.document(), r, s.active_boundary());
    decoded = decode_workspace_history_record(s.document(), wire, s.active_boundary());
    require(decoded.supported() && decoded.record->retired.size() == 1 &&
            decoded.record->workspace_epoch == std::numeric_limits<std::uint64_t>::max(),
            "retired history and full-width counters must round trip");
    require(encode_workspace_history_record(s.document(), *decoded.record, s.active_boundary()).dump() == wire.dump(),
            "canonical re-encoding must preserve references and opaque number representation");
    const BoundaryActiveRecovery* owner = nullptr;
    for (const auto& e : decoded.record->events) if (e.input) {
        if (!owner) owner = e.input->value.get();
        require(e.input->value.get() == owner, "decoded references must share their immutable payload owner");
    }
    bool explicit_clear = false;
    for (const auto& e : wire.at("events")) if (!e.at("input").is_null()) {
        const auto& input = e.at("input");
        if (input.at("pointer_override").at("present") == true &&
            input.at("pointer_override").at("point").is_null()) explicit_clear = true;
    }
    require(explicit_clear, "wire format must preserve explicit pointer clearing");
    const auto bad = [&](auto mutate) { auto copy = wire; mutate(copy);
        rejects([&] { (void)decode_workspace_history_record(s.document(), copy, s.active_boundary()); }); };
    bad([](auto& j) { j["extra"] = 1; });
    bad([](auto& j) { j["version"] = 0; });
    bad([](auto& j) { j["replay_version"] = 1.0; });
    bad([](auto& j) { j["workspace_epoch"] = -1; });
    bad([](auto& j) { j["edited_generation"] = true; });
    bad([](auto& j) { j["events"][0]["event_id"] = std::string("event\0suffix", 12); });
    bad([](auto& j) { j["events"][0]["event_id"] = std::string(129, 'x'); });
    bad([](auto& j) { j["events"][0]["sequence"] = 999; });
    bad([](auto& j) { j["navigation"]["operations"].push_back(j["navigation"]["operations"][0]); });
    bad([](auto& j) { j["retired"].push_back(j["retired"][0]); });
    bad([](auto& j) { j["retired"][0]["identity_namespace"] = "wrong-session"; });
    bad([](auto& j) { for (auto& e : j["events"]) if (!e["input"].is_null()) {
        e["input"]["pointer_override"] = {{"present", false}, {"point", Json::array({1, 2})}}; break;
    }});
    bad([](auto& j) { for (auto& e : j["events"]) if (!e["input"].is_null() && e["input"]["value"].is_null()) {
        e["input"]["owner_event_id"] = "missing"; break;
    }});
    for (const auto* discriminator : {"version", "replay_version"}) {
        auto future = wire; future[discriminator] = 99;
        const auto unknown = decode_workspace_history_record(s.document(), future, s.active_boundary());
        require(unknown.opaque() && unknown.original_envelope->dump() == future.dump(), "future history must remain wholly opaque");
    }
    auto nested = wire;
    for (auto& e : nested["events"]) if (!e["input"].is_null() && !e["input"]["value"].is_null()) {
        e["input"]["value"]["checkpoint"]["replay_version"] = 99; break;
    }
    require(decode_workspace_history_record(s.document(), nested, s.active_boundary()).opaque(),
            "unknown nested replay must preserve the whole history");
    WorkspaceRecoveryLimits limits; limits.max_events = 0;
    rejects([&] { (void)decode_workspace_history_record(s.document(), wire, s.active_boundary(), s.resource_policy(), limits); });
    limits = {}; limits.max_actions = 0;
    rejects([&] { (void)decode_workspace_history_record(s.document(), wire, s.active_boundary(), s.resource_policy(), limits); });
    auto invalid_owner = wire;
    for (auto& e : invalid_owner["events"]) if (!e["input"].is_null() && !e["input"]["value"].is_null()) {
        e["input"]["value"]["checkpoint"]["actions"][0]["kind"] = "invalid-action"; break;
    }
    limits = {};
    limits.max_validation_work = preflight_workspace_recovery(s.document(), {}, {}, {},
        s.active_boundary(), {}, s.resource_policy()).validation_work;
    bool budget_first = false;
    try { (void)decode_workspace_history_record(s.document(), invalid_owner, s.active_boundary(), s.resource_policy(), limits); }
    catch (const std::invalid_argument& e) { budget_first = std::string(e.what()).find("budget") != std::string::npos; }
    require(budget_first, "aggregate validation work must be bounded before owner replay");
    auto excessive_future = wire; excessive_future["version"] = 99;
    excessive_future["extensions"]["large"] = std::string(100'000, 'x');
    limits = {}; limits.max_encoded_bytes = 64'000;
    rejects([&] { (void)decode_workspace_history_record(s.document(), excessive_future, s.active_boundary(), s.resource_policy(), limits); });
}
void admission_symmetry() {
    auto document = fixture(); ProjectWorkspace workspace(document.snapshot());
    const auto snapshot = workspace.capture();
    const auto record = capture_workspace_history_record(snapshot);
    WorkspaceRecoveryLimits limits;
    limits.max_validation_work = preflight_workspace_recovery(snapshot.document(), record.document_history,
        record.events, record.navigation, snapshot.active_boundary(), record.retired).validation_work + 8;
    // Typed admission alone fits, but cannot account for all emitted wire keys.
    validate_workspace_history_record(snapshot.document(), record, snapshot.active_boundary(),
        snapshot.resource_policy(), limits);
    rejects([&] { (void)encode_workspace_history_record(snapshot.document(), record, snapshot.active_boundary(),
        snapshot.resource_policy(), limits); });
    limits = {};
    const auto wire = encode_workspace_history_record(snapshot.document(), record, snapshot.active_boundary(),
        snapshot.resource_policy(), limits);
    require(decode_workspace_history_record(snapshot.document(), wire, snapshot.active_boundary(),
        snapshot.resource_policy(), limits).supported(), "admitted empty history must round trip");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { run(BoundaryAuthoringMode::draw_first); run(BoundaryAuthoringMode::define_first); admission_symmetry(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
