#include "sketch/project_store.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation, std::string_view diagnostic = {}) {
    try { operation(); } catch (const StorageError& error) {
        require(diagnostic.empty() || std::string_view(error.what()).find(diagnostic) != std::string_view::npos,
            "storage rejection did not contain the expected diagnostic"); return;
    }
    throw std::runtime_error("invalid archive operation accepted");
}
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("property-archive-" + make_stable_id());
    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
struct Database {
    sqlite3* db{};
    explicit Database(const std::filesystem::path& path) {
        require(sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "open fixture database");
    }
    ~Database() { sqlite3_close(db); }
    void execute(const char* sql) { require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "execute fixture SQL"); }
    void bind_execute(const char* sql, const std::vector<std::string>& values) {
        sqlite3_stmt* statement{};
        require(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK, "prepare fixture SQL");
        for (std::size_t i = 0; i < values.size(); ++i)
            sqlite3_bind_text(statement, static_cast<int>(i + 1), values[i].data(), static_cast<int>(values[i].size()), SQLITE_TRANSIENT);
        const auto status = sqlite3_step(statement); sqlite3_finalize(statement);
        require(status == SQLITE_DONE, "execute bound fixture SQL");
    }
    std::string scalar(const char* sql) {
        sqlite3_stmt* statement{};
        require(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW, "query fixture SQL");
        const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        std::string result(bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement, 0)));
        sqlite3_finalize(statement); return result;
    }
};
ProjectArchiveSnapshot fixture(BoundaryAuthoringMode mode, ArchiveRole role, bool saved = false) {
    auto document = Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}}, {"l", "layer", {{"floor_id", "f"}}}});
    if (saved) document.mark_saved(document.revision());
    ProjectWorkspace workspace(document.snapshot());
    BoundaryAuthoringSession session(mode); session.set_classification("living_area"); (void)session.anchor({0, 0});
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint(), {{"number", 1.0}}};
    auto ticket = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(ticket);
    ticket = workspace.prepare_discard_boundary(); (void)workspace.commit(ticket);
    ticket = workspace.prepare_undo(); (void)workspace.commit(ticket);
    const auto snapshot = workspace.capture();
    auto history = capture_workspace_history_record(snapshot); history.extensions = {{"preserve", Json::array({nullptr, 1.0, "x"})}};
    RecoveryLedger rows{{"z-history", "workspace_history", encode_workspace_history_record(snapshot.document(), history, snapshot.active_boundary())},
        {"a-active", "boundary_active", encode_boundary_active_recovery(*snapshot.active_boundary())}};
    if (role == ArchiveRole::recovery_copy) {
        RecoveryCopyRecord copy; copy.archive_id = "recovery"; copy.owner_token = "owner";
        copy.document_id = snapshot.document().document_id(); copy.workspace_epoch = history.workspace_epoch;
        copy.edited_generation = history.edited_generation; copy.checkpoint_generation = history.checkpoint_generation;
        copy.explicitly_saved_document_revision = snapshot.document().saved_revision_optional();
        rows.push_back({"m-copy", "recovery_copy", encode_recovery_copy_record(copy)});
    }
    return {snapshot.document(), std::move(rows), role};
}
Json ledger_json(RecoveryLedger rows) {
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.record_id < b.record_id; });
    auto result = Json::array();
    for (const auto& row : rows) result.push_back({{"record_id", row.record_id}, {"record_kind", row.record_kind}, {"envelope", row.envelope}});
    return result;
}
// Independent contract encoder for checksum-valid corruption and unknown fixtures.
std::string digest(const DocumentSnapshot& snapshot, const RecoveryLedger& rows) {
    Json saved = nullptr; if (snapshot.saved_revision_optional()) saved = *snapshot.saved_revision_optional();
    Json manifest{{"format_version", 4}, {"document_id", snapshot.document_id()}, {"head_revision", snapshot.revision()},
        {"saved_revision", saved}, {"named_revisions", snapshot.named_revisions()}, {"history", Json::array()},
        {"recovery_records", ledger_json(rows)}};
    for (const auto& revision : snapshot.history()) {
        Json value{{"revision", revision.revision}, {"parent_revision", revision.parent_revision}, {"source_revision", revision.source_revision},
            {"action", revision.action}, {"name", revision.name}, {"undo_stack", revision.undo_stack}, {"redo_stack", revision.redo_stack},
            {"entities", Json::array()}, {"assets", Json::array()}};
        for (const auto& [id, entity] : revision.entities) value["entities"].push_back({{"id", id}, {"type", entity.type},
            {"required", entity.required}, {"properties", entity.properties}, {"extensions", entity.extensions}});
        for (const auto& [id, asset] : revision.assets) value["assets"].push_back({{"id", id}, {"media_type", asset.media_type},
            {"sha256", asset.sha256}, {"size", asset.bytes.size()}, {"metadata", asset.metadata}});
        manifest["history"].push_back(std::move(value));
    }
    const auto encoded = manifest.dump();
    return sha256_hex(std::as_bytes(std::span(encoded.data(), encoded.size())));
}
void replace_rows(const std::filesystem::path& path, const DocumentSnapshot& document, const RecoveryLedger& rows) {
    Database db(path); db.execute("DELETE FROM project_recovery_records");
    for (const auto& row : rows) db.bind_execute("INSERT INTO project_recovery_records VALUES(?1,?2,?3)",
        {row.record_id, row.record_kind, row.envelope.dump()});
    db.bind_execute("UPDATE metadata SET value=?1 WHERE key='logical_digest'", {digest(document, rows)});
}
void round_trip(BoundaryAuthoringMode mode, ArchiveRole role, bool saved) {
    TemporaryDirectory temporary; const auto path = temporary.path / "workspace.bldproj";
    const auto source = fixture(mode, role, saved);
    const auto receipt = ProjectStore::save_archive(path, source);
    const auto loaded = ProjectStore::load_archive(path, role);
    require(loaded.supported() && !loaded.opaque() && loaded.file_sha256 == receipt.file_sha256, "archive must reopen with exact published hash");
    require(document_snapshot_digest(loaded.archive->document()) == document_snapshot_digest(source.document()), "archive must preserve exact document state and saved marker");
    require(ledger_json(loaded.archive->recovery()).dump() == ledger_json(source.recovery()).dump(), "archive must preserve complete recovery JSON");
    { Database db(path);
      require(db.scalar("PRAGMA user_version") == "4" && db.scalar("SELECT value FROM metadata WHERE key='format_version'") == "4", "v4 markers");
      require(db.scalar("SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'") == "6", "v4 exact six-table schema");
      require(db.scalar("SELECT value FROM metadata WHERE key='logical_digest'") == digest(source.document(), source.recovery()), "v4 independent logical digest"); }
    rejects([&] { (void)ProjectStore::load(path); });
    rejects([&] { (void)ProjectStore::save(path, source.document(), SaveOptions{.expected_destination_sha256 = receipt.file_sha256}); });
    require(ProjectStore::file_sha256(path) == receipt.file_sha256, "document-only routes cannot alter archive");
    const auto wrong_role = role == ArchiveRole::ordinary ? ArchiveRole::recovery_copy : ArchiveRole::ordinary;
    const auto mismatched = ProjectStore::load_archive(path, wrong_role);
    require(mismatched.opaque() && !mismatched.supported(), "role mismatch must remain opaque");
    require(!mismatched.editable() && !mismatched.archive, "opaque result must not expose a forkable snapshot");
    rejects([&] { (void)ProjectStore::save_archive(temporary.path / "wrong-role.bldproj",
        {source.document(), *mismatched.recovery.original_ledger, wrong_role}); });
    for (const auto fault : {SaveFaultStage::after_journal_creation, SaveFaultStage::after_database_write,
                            SaveFaultStage::after_validation, SaveFaultStage::before_publish}) {
        rejects([&] { (void)ProjectStore::save_archive(path, source,
            SaveOptions{.expected_destination_sha256 = receipt.file_sha256, .fault_stage = fault}); });
        require(ProjectStore::file_sha256(path) == receipt.file_sha256, "failed aggregate save must preserve original");
    }
    const auto next = ProjectStore::save_archive(path, source, SaveOptions{.expected_destination_sha256 = receipt.file_sha256});
    require(next.backup_path && ProjectStore::file_sha256(*next.backup_path) == receipt.file_sha256, "aggregate overwrite needs exact backup");
    rejects([&] { (void)ProjectStore::save_archive(path, source, SaveOptions{.expected_destination_sha256 = std::string(64, '0')}); });
}
void unknown_and_corruption() {
    TemporaryDirectory temporary; const auto path = temporary.path / "unknown.bldproj";
    const auto source = fixture(BoundaryAuthoringMode::draw_first, ArchiveRole::ordinary);
    (void)ProjectStore::save_archive(path, source);
    auto rows = source.recovery(); rows.push_back({"future", "future_kind", {{"unknown", 1.0}, {"nested", Json::array({nullptr, true})}}});
    replace_rows(path, source.document(), rows);
    const auto loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
    require(loaded.opaque() && !loaded.archive && ledger_json(*loaded.recovery.original_ledger).dump() == ledger_json(rows).dump(), "checksum-valid unknown records must survive whole without a forkable snapshot");
    rejects([&] { (void)ProjectStore::save_archive(temporary.path / "rewrite.bldproj",
        {source.document(), *loaded.recovery.original_ledger, ArchiveRole::ordinary}); });
    rejects([&] { (void)ProjectStore::save_archive(path, source, SaveOptions{.expected_destination_sha256 = loaded.file_sha256}); });
    require(ProjectStore::file_sha256(path) == loaded.file_sha256, "known archive cannot erase unknown destination records");
    { Database db(path); db.execute("UPDATE project_recovery_records SET envelope_json='{}' WHERE record_id='future'"); }
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); });
    auto future = source.recovery(); future[0].envelope["version"] = 99;
    replace_rows(path, source.document(), future);
    require(ProjectStore::load_archive(path, ArchiveRole::ordinary).opaque(), "future known record version stays opaque after disk round trip");
    auto duplicate = source.recovery(); duplicate.push_back(duplicate[0]); duplicate.back().record_id = "duplicate-kind";
    replace_rows(path, source.document(), duplicate);
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); });
    replace_rows(path, source.document(), {});
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); });
    replace_rows(path, source.document(), source.recovery());
    { Database db(path); db.execute("UPDATE project_recovery_records SET envelope_json='{\"version\":1,\"version\":99}' WHERE record_id='z-history'"); }
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); }, "duplicate");
    replace_rows(path, source.document(), source.recovery());
    { Database db(path); db.execute("PRAGMA user_version=3"); }
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); });
    rejects([&] { (void)ProjectStore::load(path); });
}
void roles_and_legacy() {
    TemporaryDirectory temporary; const auto path = temporary.path / "upgrade.bldproj";
    const auto source = fixture(BoundaryAuthoringMode::draw_first, ArchiveRole::ordinary);
    const auto legacy = ProjectStore::save(path, source.document());
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); });
    const auto upgraded = ProjectStore::save_archive(path, source, SaveOptions{.expected_destination_sha256 = legacy.file_sha256});
    require(upgraded.backup_path && ProjectStore::load(*upgraded.backup_path).file_sha256 == legacy.file_sha256, "upgrade retains valid legacy backup");
    const auto recovery = fixture(BoundaryAuthoringMode::draw_first, ArchiveRole::recovery_copy);
    rejects([&] { (void)ProjectStore::save_archive(path, recovery, SaveOptions{.expected_destination_sha256 = upgraded.file_sha256}); });
    require(ProjectStore::file_sha256(path) == upgraded.file_sha256, "ordinary destination cannot be changed to recovery role implicitly");
    rejects([&] { (void)ProjectStore::save_archive(temporary.path / "empty.bldproj", {source.document(), {}, ArchiveRole::ordinary}); });
}
void required_entity_and_limits() {
    TemporaryDirectory temporary;
    auto document = Document::create({{"future-object", "future_required_entity", Json::object(), true}});
    ProjectWorkspace workspace(document.snapshot());
    const auto snapshot = workspace.capture();
    const auto history = capture_workspace_history_record(snapshot);
    const ProjectArchiveSnapshot source(snapshot.document(), {{"history", "workspace_history",
        encode_workspace_history_record(snapshot.document(), history, {})}}, ArchiveRole::ordinary);
    const auto path = temporary.path / "required.bldproj";
    (void)ProjectStore::save_archive(path, source);
    const auto loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
    require(loaded.supported() && !loaded.editable() && !loaded.archive->document().is_editable(),
        "known ledger must not erase unsupported document read-only state");
    // Recovery DOM construction must reject excessive depth before hashing/replay.
    std::string deep(100, '['); deep += "null"; deep += std::string(100, ']');
    { Database db(path); db.bind_execute("UPDATE project_recovery_records SET envelope_json=?1", {deep}); }
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); }, "nesting");
    { Database db(path); db.bind_execute("UPDATE project_recovery_records SET envelope_json=?1",
        {Json({{"oversized", std::string(20U * 1024U * 1024U, 'x')}}).dump()}); }
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); }, "string budget");
    { Database db(path); db.execute("UPDATE project_recovery_records SET envelope_json=CAST(zeroblob(67108865) AS TEXT)"); }
    rejects([&] { (void)ProjectStore::load_archive(path, ArchiveRole::ordinary); }, "bytes");
}
void depth_boundary() {
    TemporaryDirectory temporary;
    const auto base = fixture(BoundaryAuthoringMode::draw_first, ArchiveRole::ordinary);
    auto rows = base.recovery();
    Json nested = Json::array();
    for (int i = 0; i < 62; ++i) nested = Json::array({std::move(nested)});
    rows[0].envelope["extensions"]["depth"] = nested; // root=0, extensions=1, empty leaf=64.
    const ProjectArchiveSnapshot source(base.document(), rows, base.role());
    const auto path = temporary.path / "depth64.bldproj";
    (void)ProjectStore::save_archive(path, source);
    require(ProjectStore::load_archive(path, source.role()).supported(), "codec-admitted depth64 must survive disk reopen");
    rows[0].envelope["extensions"]["depth"] = Json::array({std::move(nested)});
    replace_rows(path, base.document(), rows);
    rejects([&] { (void)ProjectStore::load_archive(path, source.role()); }, "nesting");
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--emit-fixtures") {
            const std::filesystem::path directory(argv[2]); std::filesystem::create_directories(directory);
            for (const auto role : {ArchiveRole::ordinary, ArchiveRole::recovery_copy})
                (void)ProjectStore::save_archive(directory / (role == ArchiveRole::ordinary ? "ordinary-v4.bldproj" : "recovery-v4.bldproj"),
                    fixture(BoundaryAuthoringMode::draw_first, role));
            return 0;
        }
        for (const auto mode : {BoundaryAuthoringMode::draw_first, BoundaryAuthoringMode::define_first})
            for (const auto role : {ArchiveRole::ordinary, ArchiveRole::recovery_copy})
                for (const bool saved : {false, true}) round_trip(mode, role, saved);
        unknown_and_corruption(); roles_and_legacy(); required_entity_and_limits(); depth_boundary();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
