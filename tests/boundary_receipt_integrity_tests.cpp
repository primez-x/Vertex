#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& operation, const char* message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
Entity fixture(std::string id = "boundary") {
    BoundaryConstructionRecord record;
    record.boundary_id = id;
    record.anchor = {0, 0};
    const Vec2 points[]{{0, 0}, {2, 0}, {2, 1}, {0, 1}};
    const char* rises[]{"0 m", "1 m", "0 m", "-1 m"};
    const char* runs[]{"2 m", "0 m", "-2 m", "0 m"};
    IdentifiedBoundary boundary{id, "measurement_boundary", {}};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto segment_id = "edge-" + std::to_string(i);
        const auto start_id = "vertex-" + std::to_string(i);
        const auto end_id = "vertex-" + std::to_string((i + 1) % 4);
        ConstructionReceipt receipt;
        receipt.segment_id = segment_id;
        receipt.kind = BoundaryConstructionKind::line_rise_run;
        receipt.start = points[i];
        receipt.rise = parse_quantity(rises[i]);
        receipt.run = parse_quantity(runs[i]);
        record.edges.push_back({segment_id, start_id, end_id, receipt});
        boundary.segments.push_back({segment_id, start_id, end_id,
            {points[i], points[(i + 1) % 4], 0}});
    }
    auto result = encode_identified_boundary_entity(boundary);
    result.properties["boundary_authoring"] = encode_boundary_receipt_envelope(record);
    return result;
}

Entity receiptless_fixture(std::string id) {
    auto result = fixture(std::move(id));
    result.properties.erase("boundary_authoring");
    return result;
}

struct ExactFileCleanup {
    std::vector<std::filesystem::path> files;

    ~ExactFileCleanup() {
        for (const auto& file : files) {
            std::error_code error;
            const bool exists = std::filesystem::exists(file, error);
            if (error) {
                std::cerr << "boundary receipt cleanup could not inspect " << file << ": "
                          << error.message() << '\n';
                continue;
            }
            if (!exists) continue;
            std::filesystem::remove(file, error);
            if (error) {
                std::cerr << "boundary receipt cleanup could not remove " << file << ": "
                          << error.message() << '\n';
            }
        }
    }
};

std::optional<std::filesystem::path> receipt_lifecycle_capture_directory() {
#ifdef _WIN32
    wchar_t* capture_value = nullptr;
    std::size_t capture_length = 0;
    require(_wdupenv_s(&capture_value, &capture_length,
                       L"SKETCH_CAPTURE_RECEIPT_LIFECYCLE") == 0,
            "cannot read receipt lifecycle fixture capture directory");
    const std::unique_ptr<wchar_t, decltype(&std::free)> capture(capture_value, &std::free);
    if (!capture || capture_length <= 1) return std::nullopt;

    const std::filesystem::path directory(capture.get());
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        throw std::runtime_error("receipt lifecycle capture directory must already exist");
    }
    return directory;
#else
    return std::nullopt;
#endif
}

void capture_fixture_if_requested(const std::optional<std::filesystem::path>& directory,
                                  const std::filesystem::path& source,
                                  std::string_view filename) {
    if (!directory) return;
    const auto destination = *directory / std::filesystem::path(std::string(filename));
    std::error_code error;
    const bool exists = std::filesystem::exists(destination, error);
    if (error) {
        throw std::runtime_error("cannot inspect receipt lifecycle fixture destination: " +
                                 error.message());
    }
    if (exists) {
        throw std::runtime_error("receipt lifecycle fixture already exists and will not be overwritten: " +
                                 destination.string());
    }
    if (!std::filesystem::copy_file(source, destination,
                                    std::filesystem::copy_options::none, error) || error) {
        throw std::runtime_error("cannot capture receipt lifecycle fixture: " + error.message());
    }
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void require_storage_format(const std::filesystem::path& file, int expected,
                            const char* message) {
    sqlite3* database = nullptr;
    const auto encoded_path = path_utf8(file);
    if (sqlite3_open_v2(encoded_path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) !=
        SQLITE_OK) {
        if (database) sqlite3_close(database);
        throw std::runtime_error(message);
    }

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    const bool sqlite_matches = sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_int(statement, 0) == expected;
    sqlite3_finalize(statement);
    statement = nullptr;
    if (!sqlite_matches || sqlite3_prepare_v2(
            database, "SELECT value FROM metadata WHERE key='format_version'", -1,
            &statement, nullptr) != SQLITE_OK) {
        if (statement) sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    const auto* value = sqlite3_step(statement) == SQLITE_ROW
        ? sqlite3_column_text(statement, 0)
        : nullptr;
    const bool metadata_matches = value != nullptr &&
        std::string(reinterpret_cast<const char*>(value)) == std::to_string(expected);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (!metadata_matches) throw std::runtime_error(message);
}

void require_supported_receipt(const DocumentSnapshot& snapshot, std::string_view id,
                               const char* message) {
    const auto found = snapshot.entities().find(std::string(id));
    require(found != snapshot.entities().end(), message);
    const auto decoded = decode_boundary_receipt_envelope(
        found->second.properties.at("boundary_authoring"));
    require(decoded.supported(), message);
    require(decoded.record->boundary_id == id, message);
}

void rejected_edit(Document& document, Entity changed, const char* message) {
    const auto before = document_snapshot_digest(document.snapshot());
    rejects([&] { document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(changed)}}); }, message);
    require(before == document_snapshot_digest(document.snapshot()), "rejection mutated retained document state");
}
void test_bound_geometry_and_envelope_survive_metadata_edits_and_navigation() {
    const auto original = fixture();
    auto document = Document::create({original});
    auto styled = original;
    styled.properties["classification"] = "garage";
    styled.properties["fill"] = "blue";
    styled.properties["segments"][0]["vendor_note"] = "retained";
    styled.extensions["vendor"] = Json{{"count", 1.0}};
    styled.required = true;
    document.apply(ApplyEntityChanges{.expected_revision = 0,
        .entity_changes = {EntityChange::upsert(styled)}});
    require(document.snapshot().entities().at(original.id) == styled, "unrelated metadata edit must remain usable");
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase(original.id)}});
    require(document.snapshot().entities().empty(), "whole boundary deletion must be allowed");
    document.undo(document.revision());
    require(document.snapshot().entities().at(original.id).properties.dump() == styled.properties.dump(),
            "undo must restore exact construction envelope and metadata");
    document.redo(document.revision());
    require(document.snapshot().entities().empty(), "redo must reproduce deletion");
}
void test_raw_geometry_receipt_removal_attachment_and_rewriting_reject() {
    const auto original = fixture();
    auto document = Document::create({original});
    auto removed = original;
    removed.properties.erase("boundary_authoring");
    rejected_edit(document, removed, "raw receipt removal must reject");
    auto rewritten = original;
    rewritten.properties["boundary_authoring"]["extensions"]["extra"] = true;
    rejected_edit(document, rewritten, "even opaque receipt extensions cannot be rewritten by a raw edit");
    auto changed = original;
    for (auto& segment : changed.properties["segments"]) {
        segment["start"][0] = segment["start"][0].get<double>() + 1;
        segment["end"][0] = segment["end"][0].get<double>() + 1;
    }
    rejected_edit(document, changed, "geometry cannot contradict its input derivation");
    auto rerecorded = *decode_boundary_receipt_envelope(original.properties.at("boundary_authoring")).record;
    rerecorded.anchor.x += 1;
    for (auto& edge : rerecorded.edges) edge.receipt.start.x += 1;
    changed.properties["boundary_authoring"] = encode_boundary_receipt_envelope(rerecorded);
    require(Document::create({changed}).is_editable(), "translated replacement fixture must be independently valid");
    rejected_edit(document, changed, "coordinated geometry and input rewriting requires typed derivation semantics");
    auto obscured = original;
    obscured.properties["boundary_authoring"] = Json{{"version", 999}};
    rejected_edit(document, obscured, "an unknown envelope cannot replace known input semantics");
    auto relabeled = original;
    relabeled.type = "room_boundary";
    rejected_edit(document, relabeled, "receipt-bound owner subtype cannot change through a raw edit");
    auto plain = original;
    plain.properties.erase("boundary_authoring");
    auto existing = Document::create({plain});
    rejected_edit(existing, original, "surviving receiptless geometry cannot acquire a receipt through raw attachment");
}
void test_owner_qualification_and_unknown_versions() {
    auto opaque = fixture();
    opaque.properties["boundary_authoring"] = Json{{"version", 999}, {"opaque", Json{{"value", 1.0}}}};
    const auto future_receipt = Document::create({opaque});
    require(!future_receipt.is_editable(), "unknown receipt semantics must make the document read-only");
    auto malformed = fixture("bad");
    malformed.properties["boundary_authoring"]["version"] = 0;
    rejects([&] { (void)Document::create({opaque, malformed}); },
            "opaque receipt must not suppress known malformed data validation");
    auto future_model = malformed;
    future_model.properties["boundary_model_version"] = 999;
    const auto unknown_owner = Document::create({future_model});
    require(!unknown_owner.is_editable(), "future model keeps its receipt-looking data opaque and read-only");
    Entity label{"label", "label", Json{{"boundary_authoring", Json{{"version", 0}}}}, false, Json::object()};
    const auto unrelated = Document::create({label});
    require(unrelated.is_editable(), "generic metadata collisions must not acquire receipt semantics");
    auto legacy = fixture("legacy");
    legacy.properties.erase("boundary_model_version");
    legacy.properties["boundary_authoring"] = Json{{"version", 0}};
    for (auto& segment : legacy.properties["segments"]) {
        segment.erase("segment_id"); segment.erase("start_vertex_id"); segment.erase("end_vertex_id");
    }
    require(Document::create({legacy}).is_editable(), "legacy opaque metadata must retain its original meaning");
    rejects([&] { (void)upgrade_legacy_boundary_entity(legacy); }, "legacy receipt-key collision must block implicit promotion");
}

void test_known_receipt_storage_lifecycle_and_v3_retention() {
    const auto capture_directory = receipt_lifecycle_capture_directory();
    const auto file = std::filesystem::temp_directory_path() /
        ("boundary-receipt-lifecycle-" + make_stable_id() + ".bldproj");
    ExactFileCleanup cleanup{{file}};

    const std::string receiptless_id = "receiptless-v2";
    const std::string known_id = "known-receipt-v3";
    auto document = Document::create({receiptless_fixture(receiptless_id)});
    const auto original_snapshot = document.snapshot();
    require(ProjectStore::required_format_version(original_snapshot) == 2,
            "receiptless identified boundary must begin at storage format v2");
    const auto original_save = ProjectStore::save(file, original_snapshot);
    require_storage_format(file, 2, "receiptless boundary did not persist storage format v2");

    const auto known = fixture(known_id);
    document.apply(ApplyEntityChanges{
        .expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase(receiptless_id), EntityChange::upsert(known)},
        .message = "replace receiptless boundary with construction receipt"});
    const auto known_snapshot = document.snapshot();
    require(ProjectStore::required_format_version(known_snapshot) == 3,
            "retained known construction receipt must require storage format v3");

    bool injected = false;
    try {
        (void)ProjectStore::save(file, known_snapshot,
            SaveOptions{.expected_destination_sha256 = original_save.file_sha256,
                        .fault_stage = SaveFaultStage::after_validation});
    } catch (const StorageError& error) {
        injected = error.code() == StorageErrorCode::injected_failure;
    }
    require(injected, "after-validation fault injection did not reject the receipt upgrade");
    require(ProjectStore::file_sha256(file) == original_save.file_sha256,
            "failed receipt upgrade changed the original v2 file");
    require_storage_format(file, 2, "failed receipt upgrade changed the original v2 markers");

    const auto known_save = ProjectStore::save(
        file, known_snapshot,
        SaveOptions{.expected_destination_sha256 = original_save.file_sha256});
    require(known_save.backup_path.has_value(),
            "receipt upgrade did not retain the replaced v2 project");
    cleanup.files.push_back(*known_save.backup_path);
    require(ProjectStore::file_sha256(*known_save.backup_path) == original_save.file_sha256,
            "receipt upgrade backup is not byte-identical to the original v2 project");
    require_storage_format(*known_save.backup_path, 2,
                           "receipt upgrade backup was rewritten with v3 markers");
    const auto original_reopened = ProjectStore::load(*known_save.backup_path);
    require(original_reopened.document.snapshot().entities() == original_snapshot.entities() &&
                ProjectStore::required_format_version(original_reopened.document.snapshot()) == 2,
            "retained v2 backup did not reopen as the original receiptless project");

    require_storage_format(file, 3, "known receipt project did not persist storage format v3");
    auto known_reopened = ProjectStore::load(file);
    require(known_reopened.file_sha256 == known_save.file_sha256,
            "known receipt project changed while reopening");
    require_supported_receipt(known_reopened.document.snapshot(), known_id,
                              "known receipt did not survive save and reopen");
    require(known_reopened.document.can_undo(),
            "known receipt replacement did not retain undo history");
    capture_fixture_if_requested(capture_directory, file, "known-v3.bldproj");
    capture_fixture_if_requested(capture_directory, *known_save.backup_path,
                                 "original-v2.bldproj");

    known_reopened.document.undo(known_reopened.document.revision());
    const auto undone_snapshot = known_reopened.document.snapshot();
    require(undone_snapshot.entities().contains(receiptless_id) &&
                !undone_snapshot.entities().contains(known_id),
            "undo did not restore the receiptless v2 boundary head");
    require(ProjectStore::required_format_version(undone_snapshot) == 3,
            "undoing the known receipt dropped the retained v3 requirement");
    const auto undone_save = ProjectStore::save(
        file, undone_snapshot,
        SaveOptions{.expected_destination_sha256 = known_save.file_sha256});
    require(undone_save.backup_path.has_value(), "saving the undone receipt state lost its backup");
    cleanup.files.push_back(*undone_save.backup_path);
    require_storage_format(file, 3, "undone known receipt project did not retain storage format v3");
    capture_fixture_if_requested(capture_directory, file, "undone-v3.bldproj");

    auto undone_reopened = ProjectStore::load(file);
    require(undone_reopened.document.can_redo(),
            "reopened v3 receipt history did not retain redo navigation");
    undone_reopened.document.redo(undone_reopened.document.revision());
    const auto redone_snapshot = undone_reopened.document.snapshot();
    require_supported_receipt(redone_snapshot, known_id,
                              "redo did not restore the known construction receipt");
    require(ProjectStore::required_format_version(redone_snapshot) == 3,
            "redo of the known receipt did not preserve storage format v3");
    const auto redone_save = ProjectStore::save(
        file, redone_snapshot,
        SaveOptions{.expected_destination_sha256 = undone_save.file_sha256});
    require(redone_save.backup_path.has_value(), "saving the redone receipt state lost its backup");
    cleanup.files.push_back(*redone_save.backup_path);
    require_storage_format(file, 3, "redone known receipt project did not retain storage format v3");

    auto redone_reopened = ProjectStore::load(file);
    redone_reopened.document.apply(ApplyEntityChanges{
        .expected_revision = redone_reopened.document.revision(),
        .entity_changes = {EntityChange::erase(known_id)},
        .message = "delete construction receipt boundary"});
    const auto deleted_snapshot = redone_reopened.document.snapshot();
    require(deleted_snapshot.entities().empty(),
            "deleting the known receipt boundary did not produce an empty head");
    require(ProjectStore::required_format_version(deleted_snapshot) == 3,
            "deleting the known receipt dropped the retained v3 requirement");
    const auto deleted_save = ProjectStore::save(
        file, deleted_snapshot,
        SaveOptions{.expected_destination_sha256 = redone_save.file_sha256});
    require(deleted_save.backup_path.has_value(), "saving the deleted receipt state lost its backup");
    cleanup.files.push_back(*deleted_save.backup_path);
    require_storage_format(file, 3, "deleted known receipt project did not retain storage format v3");
    capture_fixture_if_requested(capture_directory, file, "deleted-v3.bldproj");

    auto deleted_reopened = ProjectStore::load(file);
    require(ProjectStore::required_format_version(deleted_reopened.document.snapshot()) == 3,
            "reopened deleted receipt history was downgraded below storage format v3");
    deleted_reopened.document.undo(deleted_reopened.document.revision());
    require_supported_receipt(deleted_reopened.document.snapshot(), known_id,
                              "undo after receipt deletion did not restore the known receipt");
    require(ProjectStore::required_format_version(deleted_reopened.document.snapshot()) == 3,
            "undo after receipt deletion dropped storage format v3");
    deleted_reopened.document.redo(deleted_reopened.document.revision());
    require(deleted_reopened.document.snapshot().entities().empty() &&
                ProjectStore::required_format_version(deleted_reopened.document.snapshot()) == 3,
            "redo after receipt deletion lost retained v3 history");
}
void test_unknown_and_malformed_receipts_in_abandoned_history() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(fixture())}});
    document.undo(document.revision());
    const Entity note{"note", "label", Json{{"text", "new branch"}}, false, Json::object()};
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(note)}});

    // Model a file written by a future version: its unknown record is retained
    // only in the abandoned revision, not in the visible current entities.
    auto future = document.snapshot();
    auto& future_record = const_cast<std::vector<RevisionRecord>&>(future.history()).at(1);
    future_record.entities.at("boundary").properties["boundary_authoring"] =
        Json{{"version", 999}, {"opaque", Json{{"number", 1.0}}}};
    const auto file = std::filesystem::temp_directory_path() /
        ("boundary-receipt-future-history-" + make_stable_id() + ".bldproj");
    ExactFileCleanup cleanup{{file}};
    (void)ProjectStore::save(file, future);
    require_storage_format(file, 3, "abandoned receipt history must retain format v3");
    auto loaded = ProjectStore::load(file);
    require(!loaded.document.is_editable(), "unknown abandoned receipt must make the project read-only");
    require(loaded.document.snapshot().entities() == document.snapshot().entities(),
            "unknown abandoned receipt must not alter the current visible entities");
    require(loaded.document.snapshot().history().at(1).entities.at("boundary").properties.dump() ==
                future_record.entities.at("boundary").properties.dump(),
            "unknown abandoned receipt must retain the exact opaque record");
    const auto before = document_snapshot_digest(loaded.document.snapshot());
    rejects([&] { loaded.document.undo(loaded.document.revision()); },
            "unsupported abandoned semantics must block editing through history navigation");
    require(document_snapshot_digest(loaded.document.snapshot()) == before,
            "read-only navigation rejection must leave the complete history unchanged");

    auto malformed = document.snapshot();
    auto& bad_record = const_cast<std::vector<RevisionRecord>&>(malformed.history()).at(1);
    bad_record.entities.at("boundary").properties["boundary_authoring"]["segments"][0]["receipt"].erase("rise");
    const auto original_hash = ProjectStore::file_sha256(file);
    bool invalid_snapshot = false;
    try {
        (void)ProjectStore::save(file, malformed,
            SaveOptions{.expected_destination_sha256 = original_hash});
    } catch (const StorageError& error) {
        invalid_snapshot = error.code() == StorageErrorCode::invalid_snapshot;
    }
    require(invalid_snapshot, "malformed abandoned known receipt must reject before publication");
    require(ProjectStore::file_sha256(file) == original_hash,
            "rejected malformed history must preserve the previous valid file");
}

void test_point_receipt_storage_and_future_replay() {
    auto point_entity = fixture("point-schema2");
    auto record = *decode_boundary_receipt_envelope(
        point_entity.properties.at("boundary_authoring")).record;
    record.schema_version = boundary_receipt_schema_version_v2;
    const auto boundary = decode_identified_boundary_entity(point_entity);
    for (std::size_t i = 0; i < record.edges.size(); ++i) {
        auto& receipt = record.edges[i].receipt;
        receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.rise.reset();
        receipt.run.reset();
        receipt.chord_end = boundary.segments[i].segment.end;
    }
    point_entity.properties["boundary_authoring"] = encode_boundary_receipt_envelope(record);
    auto document = Document::create({point_entity});
    const auto file = std::filesystem::temp_directory_path() /
        ("boundary-point-storage-" + make_stable_id() + ".bldproj");
    ExactFileCleanup cleanup{{file}};
    const auto capture_directory = receipt_lifecycle_capture_directory();
    (void)ProjectStore::save(file, document.snapshot());
    require_storage_format(file, 3, "schema2 receipt must use storage format3");
    auto loaded = ProjectStore::load(file);
    require(loaded.document.is_editable(), "schema2 point receipt must reopen editable");
    require(loaded.document.snapshot().entities() == document.snapshot().entities(),
            "point receipt and geometry must round-trip exactly");
    capture_fixture_if_requested(capture_directory, file, "point-schema2-v3.bldproj");

    // An older reader must still discover the future dialect when the owning
    // boundary exists only in retained history, after deletion or undo.
    auto history_document = Document::create();
    history_document.apply(ApplyEntityChanges{.expected_revision = history_document.revision(),
        .entity_changes = {EntityChange::upsert(point_entity)}});
    history_document.undo(history_document.revision());
    const auto history_file = std::filesystem::temp_directory_path() /
        ("boundary-point-history-" + make_stable_id() + ".bldproj");
    cleanup.files.push_back(history_file);
    const auto undone_save = ProjectStore::save(history_file, history_document.snapshot());
    require_storage_format(history_file, 3, "undone schema2 receipt must retain storage format3");
    capture_fixture_if_requested(capture_directory, history_file, "point-undone-v3.bldproj");
    history_document.redo(history_document.revision());
    history_document.apply(ApplyEntityChanges{.expected_revision = history_document.revision(),
        .entity_changes = {EntityChange::erase(point_entity.id)}});
    const auto deleted_save = ProjectStore::save(history_file, history_document.snapshot(),
        SaveOptions{.expected_destination_sha256 = undone_save.file_sha256});
    if (deleted_save.backup_path) cleanup.files.push_back(*deleted_save.backup_path);
    require_storage_format(history_file, 3, "deleted schema2 receipt must retain storage format3");
    auto history_loaded = ProjectStore::load(history_file);
    require(history_loaded.document.is_editable() && history_loaded.document.snapshot().entities().empty(),
            "deleted schema2 boundary history must reopen editable with an empty head");
    history_loaded.document.undo(history_loaded.document.revision());
    require(history_loaded.document.snapshot().entities().at(point_entity.id) == point_entity,
            "undo after reopening deleted schema2 history must restore the exact point receipt");
    capture_fixture_if_requested(capture_directory, history_file, "point-deleted-v3.bldproj");

    auto future_entity = point_entity;
    future_entity.properties["boundary_authoring"]["replay_version"] = 999;
    auto future_document = Document::create({future_entity});
    require(!future_document.is_editable(), "future replay must make project read-only");
    const auto save = ProjectStore::save(file, future_document.snapshot(),
        SaveOptions{.expected_destination_sha256 = loaded.file_sha256});
    if (save.backup_path) cleanup.files.push_back(*save.backup_path);
    auto future_loaded = ProjectStore::load(file);
    require(!future_loaded.document.is_editable(), "future replay must reopen read-only");
    require(future_loaded.document.snapshot().entities() == future_document.snapshot().entities(),
            "future replay payload must round-trip opaquely");
    capture_fixture_if_requested(capture_directory, file, "future-replay-v3.bldproj");
}
} // namespace
int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_bound_geometry_and_envelope_survive_metadata_edits_and_navigation();
        test_raw_geometry_receipt_removal_attachment_and_rewriting_reject();
        test_owner_qualification_and_unknown_versions();
        test_unknown_and_malformed_receipts_in_abandoned_history();
        test_point_receipt_storage_and_future_replay();
        test_known_receipt_storage_lifecycle_and_v3_retention();
        std::cout << "Boundary receipt integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
