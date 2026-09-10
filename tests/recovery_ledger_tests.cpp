#include "sketch/recovery_ledger.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid ledger accepted");
}
void same_opaque(const RecoveryLedgerDecodeResult& decoded, const RecoveryLedger& source) {
    require(decoded.opaque() && !decoded.supported() && decoded.original_ledger->size() == source.size(),
        "unknown ledger must remain wholly opaque");
    for (std::size_t i = 0; i < source.size(); ++i) {
        const auto& actual = decoded.original_ledger->at(i);
        require(actual.record_id == source[i].record_id && actual.record_kind == source[i].record_kind &&
            actual.envelope.dump() == source[i].envelope.dump(), "opaque ledger must preserve row order and exact JSON values");
    }
}
void run(BoundaryAuthoringMode mode) {
    auto document = Document::create({{"p", "property", {{"name", "Property"}}}, {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}}, {"l", "layer", {{"floor_id", "f"}}}});
    ProjectWorkspace workspace(document.snapshot());
    BoundaryAuthoringSession session(mode); session.set_classification("living_area");
    (void)session.anchor({0, 0});
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint()};
    auto activation = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(activation);
    const auto snapshot = workspace.capture();
    auto history = capture_workspace_history_record(snapshot);
    RecoveryLedger ledger{{"history", "workspace_history", encode_workspace_history_record(snapshot.document(), history, active)},
        {"active", "boundary_active", encode_boundary_active_recovery(active)}};
    auto decoded = decode_recovery_ledger(snapshot.document(), ledger, ArchiveRole::ordinary);
    require(decoded.supported() && decoded.decoded->active && decoded.decoded->history && !decoded.decoded->recovery_copy,
        "ordinary active workspace must decode as one aggregate");
    RecoveryCopyRecord copy; copy.archive_id = "archive"; copy.owner_token = "owner";
    copy.document_id = snapshot.document().document_id();
    copy.workspace_epoch = history.workspace_epoch; copy.edited_generation = history.edited_generation;
    copy.checkpoint_generation = history.checkpoint_generation;
    ledger.push_back({"copy", "recovery_copy", encode_recovery_copy_record(copy)});
    decoded = decode_recovery_ledger(snapshot.document(), ledger, ArchiveRole::recovery_copy);
    require(decoded.supported() && decoded.decoded->recovery_copy, "recovery role requires its matching copy record");
    same_opaque(decode_recovery_ledger(snapshot.document(), ledger, ArchiveRole::ordinary), ledger);
    auto ordinary = ledger; ordinary.pop_back();
    same_opaque(decode_recovery_ledger(snapshot.document(), ordinary, ArchiveRole::recovery_copy), ordinary);
    const auto bad = [&](auto mutate) { auto rows = ledger; mutate(rows);
        rejects([&] { (void)decode_recovery_ledger(snapshot.document(), rows, ArchiveRole::recovery_copy); }); };
    bad([](auto& rows) { rows.clear(); });
    bad([](auto& rows) { rows[1].record_id = rows[0].record_id; });
    bad([](auto& rows) { auto duplicate = rows[0]; duplicate.record_id = "duplicate"; rows.push_back(duplicate); });
    bad([](auto& rows) { rows[0].record_id = std::string("bad\0id", 6); });
    bad([](auto& rows) { rows[0].record_kind = std::string(129, 'x'); });
    bad([](auto& rows) { rows[0].record_id = std::string("\xC0\xAF", 2); });
    bad([](auto& rows) { rows.erase(rows.begin()); });
    for (const auto* field : {"workspace_epoch", "edited_generation", "checkpoint_generation"})
        bad([&](auto& rows) { rows[2].envelope[field] = 9000; });
    bad([](auto& rows) { rows[2].envelope["document_id"] = "another-document"; });
    bad([](auto& rows) { rows[2].envelope["explicitly_saved_document_revision"] = 9000; });
    for (const auto invalid : {Json(0), Json(-1), Json(true), Json(1.0)})
        bad([&](auto& rows) { rows[0].envelope["version"] = invalid; });
    for (std::size_t row = 0; row < ledger.size(); ++row) {
        auto future = ledger; future[row].envelope["version"] = 99;
        same_opaque(decode_recovery_ledger(snapshot.document(), future, ArchiveRole::recovery_copy), future);
    }
    auto future = ledger; future[1].envelope["checkpoint"]["replay_version"] = 99;
    same_opaque(decode_recovery_ledger(snapshot.document(), future, ArchiveRole::recovery_copy), future);
    future = ledger; future.push_back({"unknown", "future_kind", {{"number", 1.0}, {"values", Json::array({nullptr, "x"})}}});
    // Unknown aggregate must be recognized before canonical replay of an otherwise malformed known payload.
    future[1].envelope["checkpoint"]["actions"][0]["kind"] = "invalid-action";
    same_opaque(decode_recovery_ledger(snapshot.document(), future, ArchiveRole::recovery_copy), future);
    WorkspaceRecoveryLimits limits; limits.max_encoded_bytes = 1;
    rejects([&] { (void)decode_recovery_ledger(snapshot.document(), future, ArchiveRole::recovery_copy,
        snapshot.resource_policy(), limits); });
    limits = {}; limits.max_actions = 0;
    rejects([&] { (void)decode_recovery_ledger(snapshot.document(), ledger, ArchiveRole::recovery_copy,
        snapshot.resource_policy(), limits); });
    auto cumulative = ledger;
    cumulative.push_back({"future-a", "future_a", {{"text", std::string(40'000, 'a')}}});
    cumulative.push_back({"future-b", "future_b", {{"text", std::string(40'000, 'b')}}});
    limits = {}; limits.max_encoded_bytes = 64'000;
    rejects([&] { (void)decode_recovery_ledger(snapshot.document(), cumulative, ArchiveRole::recovery_copy,
        snapshot.resource_policy(), limits); });
    // Full owner inside history must participate in unknown-version detection too.
    auto discard = workspace.prepare_discard_boundary(); (void)workspace.commit(discard);
    const auto discarded = workspace.capture();
    RecoveryLedger historical{{"history", "workspace_history", encode_workspace_history_record(discarded.document(),
        capture_workspace_history_record(discarded), discarded.active_boundary())}};
    for (auto& event : historical[0].envelope["events"]) if (!event["input"].is_null()) {
        event["input"]["value"]["checkpoint"]["replay_version"] = 99; break;
    }
    same_opaque(decode_recovery_ledger(discarded.document(), historical, ArchiveRole::ordinary), historical);
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { run(BoundaryAuthoringMode::draw_first); run(BoundaryAuthoringMode::define_first); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
