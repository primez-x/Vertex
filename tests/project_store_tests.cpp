#include "sketch/project_store.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sketch::ApplyEntityChanges;
using sketch::Asset;
using sketch::AssetChange;
using sketch::Document;
using sketch::Entity;
using sketch::EntityChange;
using sketch::NameRevision;
using sketch::ProjectStore;
using sketch::Revision;
using sketch::SaveFaultStage;
using sketch::SaveOptions;
using sketch::StorageError;
using sketch::StorageErrorCode;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "project_store_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void require_error(Function&& function, StorageErrorCode code, std::string_view message) {
    try {
        function();
    } catch (const StorageError& error) {
        if (error.code() == code) {
            return;
        }
        std::cerr << "project_store_tests: " << message << ": wrong error "
                  << static_cast<int>(error.code()) << " (" << error.what() << ")\n";
        std::exit(1);
    }
    fail(message);
}

template <typename Function>
void require_error_contains(Function&& function, StorageErrorCode code, std::string_view text,
                            std::string_view message) {
    try {
        function();
    } catch (const StorageError& error) {
        if (error.code() == code && std::string_view(error.what()).find(text) != std::string_view::npos) {
            return;
        }
        std::cerr << "project_store_tests: " << message << ": wrong error "
                  << static_cast<int>(error.code()) << " (" << error.what() << ")\n";
        std::exit(1);
    }
    fail(message);
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("property-studio-tests-" + sketch::make_stable_id());

    TempDirectory() { std::filesystem::create_directory(path); }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

Entity entity(std::string id, std::string type, nlohmann::json properties = nlohmann::json::object(),
              bool required = false, nlohmann::json extensions = nlohmann::json::object()) {
    return Entity{std::move(id), std::move(type), std::move(properties), required,
                  std::move(extensions)};
}

void execute_sql(const std::filesystem::path& path, std::string_view sql) {
    sqlite3* database = nullptr;
    require(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) ==
                SQLITE_OK,
            "test should open the project database for controlled mutation");
    char* error = nullptr;
    const auto result = sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        std::cerr << "project_store_tests: controlled SQL mutation failed: "
                  << (error == nullptr ? sqlite3_errmsg(database) : error) << '\n';
        sqlite3_free(error);
        sqlite3_close(database);
        std::exit(1);
    }
    sqlite3_close(database);
}

void insert_json_entities(const std::filesystem::path& path, std::size_t count,
                          const std::string& properties) {
    sqlite3* database = nullptr;
    require(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) ==
                SQLITE_OK,
            "test should open database for JSON budget fixture");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(
                database,
                "INSERT INTO revision_entities(revision,id,type,required,properties_json,"
                "extensions_json) VALUES(0,?1,'label',0,?2,'{}')",
                -1, &statement, nullptr) == SQLITE_OK,
            "test should prepare JSON budget rows");
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = "budget-" + std::to_string(index);
        sqlite3_bind_text(statement, 1, id.c_str(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, properties.c_str(), static_cast<int>(properties.size()),
                          SQLITE_TRANSIENT);
        require(sqlite3_step(statement) == SQLITE_DONE, "test should insert JSON budget row");
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
}

void insert_json_entities_in_revisions(const std::filesystem::path& path, std::size_t count,
                                       const std::string& properties,
                                       std::span<const Revision> revisions) {
    sqlite3* database = nullptr;
    require(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) ==
                SQLITE_OK,
            "test should open database for valid-history JSON budget fixture");
    require(sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK,
            "test should begin valid-history JSON budget fixture");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(
                database,
                "INSERT INTO revision_entities(revision,id,type,required,properties_json,"
                "extensions_json) VALUES(?1,?2,'label',0,?3,'{}')",
                -1, &statement, nullptr) == SQLITE_OK,
            "test should prepare valid-history JSON budget rows");
    for (const auto revision : revisions) {
        for (std::size_t index = 0; index < count; ++index) {
            const auto id = "utf8-budget-" + std::to_string(index);
            sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(revision));
            sqlite3_bind_text(statement, 2, id.c_str(), static_cast<int>(id.size()),
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, properties.c_str(),
                              static_cast<int>(properties.size()), SQLITE_TRANSIENT);
            require(sqlite3_step(statement) == SQLITE_DONE,
                    "test should insert valid-history JSON budget row");
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        }
    }
    sqlite3_finalize(statement);
    require(sqlite3_exec(database, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK,
            "test should commit valid-history JSON budget fixture");
    sqlite3_close(database);
}

std::string sqlite_text(sqlite3_stmt* statement, int column) {
    const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return {value, static_cast<std::size_t>(sqlite3_column_bytes(statement, column))};
}

std::string metadata_value(sqlite3* database, const char* key) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, "SELECT value FROM metadata WHERE key=?1", -1,
                               &statement, nullptr) == SQLITE_OK,
            "test should prepare metadata query");
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
    require(sqlite3_step(statement) == SQLITE_ROW, "test metadata should exist");
    auto result = sqlite_text(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

int sqlite_user_version(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK,
            "test should prepare SQLite user_version query");
    require(sqlite3_step(statement) == SQLITE_ROW &&
                sqlite3_column_type(statement, 0) == SQLITE_INTEGER,
            "test SQLite user_version should be an integer row");
    const auto result = sqlite3_column_int(statement, 0);
    require(sqlite3_step(statement) == SQLITE_DONE,
            "test SQLite user_version should contain one row");
    sqlite3_finalize(statement);
    return result;
}

nlohmann::json nullable_revision(sqlite3_stmt* statement, int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
               ? nlohmann::json(nullptr)
               : nlohmann::json(static_cast<std::uint64_t>(sqlite3_column_int64(statement, column)));
}

void rewrite_logical_digest(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    require(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) ==
                SQLITE_OK,
            "test should open database to recompute digest");
    const auto format = std::stoul(metadata_value(database, "format_version"));
    nlohmann::json manifest = {
        {"format_version", format},
        {"document_id", metadata_value(database, "document_id")},
        {"head_revision", std::stoull(metadata_value(database, "head_revision"))},
        {"saved_revision", std::stoull(metadata_value(database, "saved_revision"))},
        {"history", nlohmann::json::array()},
        {"named_revisions", nlohmann::json::object()},
    };

    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(
                database,
                format >= 6
                    ? "SELECT revision,parent_revision,source_revision,action,name,undo_stack_json,"
                      "redo_stack_json,boundary_translation_json,boundary_transform_json FROM revisions ORDER BY revision"
                    : format == 5
                    ? "SELECT revision,parent_revision,source_revision,action,name,undo_stack_json,"
                      "redo_stack_json,boundary_translation_json FROM revisions ORDER BY revision"
                    : "SELECT revision,parent_revision,source_revision,action,name,undo_stack_json,"
                      "redo_stack_json FROM revisions ORDER BY revision",
                -1, &statement, nullptr) == SQLITE_OK,
            "test should read revisions for digest");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        manifest["history"].push_back({
            {"revision", static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0))},
            {"parent_revision", nullable_revision(statement, 1)},
            {"source_revision", nullable_revision(statement, 2)},
            {"action", sqlite_text(statement, 3)},
            {"name", sqlite3_column_type(statement, 4) == SQLITE_NULL
                         ? nlohmann::json(nullptr)
                         : nlohmann::json(sqlite_text(statement, 4))},
            {"undo_stack", nlohmann::json::parse(sqlite_text(statement, 5))},
            {"redo_stack", nlohmann::json::parse(sqlite_text(statement, 6))},
            {"entities", nlohmann::json::array()},
            {"assets", nlohmann::json::array()},
        });
        if (format >= 5 && sqlite3_column_type(statement, 7) != SQLITE_NULL)
            manifest["history"].back()["boundary_translation"] =
                nlohmann::json::parse(sqlite_text(statement, 7));
        if (format >= 6 && sqlite3_column_type(statement, 8) != SQLITE_NULL)
            manifest["history"].back()["boundary_transform"] =
                nlohmann::json::parse(sqlite_text(statement, 8));
    }
    sqlite3_finalize(statement);

    require(sqlite3_prepare_v2(
                database,
                "SELECT revision,id,type,required,properties_json,extensions_json "
                "FROM revision_entities ORDER BY revision,id",
                -1, &statement, nullptr) == SQLITE_OK,
            "test should read entities for digest");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto revision = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
        manifest["history"].at(revision)["entities"].push_back({
            {"id", sqlite_text(statement, 1)},
            {"type", sqlite_text(statement, 2)},
            {"required", sqlite3_column_int(statement, 3) != 0},
            {"properties", nlohmann::json::parse(sqlite_text(statement, 4))},
            {"extensions", nlohmann::json::parse(sqlite_text(statement, 5))},
        });
    }
    sqlite3_finalize(statement);

    require(sqlite3_prepare_v2(
                database,
                "SELECT revision,asset_id,media_type,sha256,metadata_json,length(data) "
                "FROM revision_assets ORDER BY revision,asset_id",
                -1, &statement, nullptr) == SQLITE_OK,
            "test should read assets for digest");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto revision = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
        manifest["history"].at(revision)["assets"].push_back({
            {"id", sqlite_text(statement, 1)},
            {"media_type", sqlite_text(statement, 2)},
            {"sha256", sqlite_text(statement, 3)},
            {"size", static_cast<std::uint64_t>(sqlite3_column_int64(statement, 5))},
            {"metadata", nlohmann::json::parse(sqlite_text(statement, 4))},
        });
    }
    sqlite3_finalize(statement);

    require(sqlite3_prepare_v2(database,
                               "SELECT name,revision FROM named_revisions ORDER BY name", -1,
                               &statement, nullptr) == SQLITE_OK,
            "test should read names for digest");
    while (sqlite3_step(statement) == SQLITE_ROW) {
        manifest["named_revisions"][sqlite_text(statement, 0)] =
            static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
    }
    sqlite3_finalize(statement);

    const auto encoded = manifest.dump();
    const auto digest = sketch::sha256_hex(
        std::as_bytes(std::span<const char>(encoded.data(), encoded.size())));
    require(sqlite3_prepare_v2(database,
                               "UPDATE metadata SET value=?1 WHERE key='logical_digest'", -1,
                               &statement, nullptr) == SQLITE_OK,
            "test should prepare digest update");
    sqlite3_bind_text(statement, 1, digest.c_str(), static_cast<int>(digest.size()), SQLITE_TRANSIENT);
    require(sqlite3_step(statement) == SQLITE_DONE, "test should update logical digest");
    sqlite3_finalize(statement);
    sqlite3_close(database);
}

Document populated_document() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {
            EntityChange::upsert(entity("property-1", "property", {{"name", "House"}})),
            EntityChange::upsert(entity("floor-1", "floor", {{"property_id", "property-1"}})),
            EntityChange::upsert(entity(
                "wall-1", "wall",
                {{"floor_id", "floor-1"},
                 {"baseline", {{"start", {0.0, 0.0}}, {"end", {5.0, 0.0}}, {"sweep_radians", 0.0}}},
                 {"thickness_m", 0.14},
                 {"unknown_future_field", {{"preserve", true}}}})),
        },
        .asset_changes = {AssetChange::upsert(
            Asset::create("photo-1", "image/png", {std::byte{0x01}, std::byte{0x7f}, std::byte{0xff}},
                          {{"source", "test"}}))},
        .message = "initial model",
    });
    document.apply(NameRevision{.expected_revision = 1, .name = "surveyed"});
    return document;
}

nlohmann::json opaque_authoring_envelope() {
    return {
        {"version", 999},
        {"opaque", {{"future_receipt", "preserve-me"}, {"sequence", {1, 2, 3}}}},
    };
}

nlohmann::json identified_boundary_properties(bool with_authoring_envelope = false) {
    nlohmann::json properties = {
        {"boundary_model_version", 1},
        {"segments", nlohmann::json::array({
                         nlohmann::json{{"start", {0.0, 0.0}},
                                        {"end", {1.0, 0.0}},
                                        {"sweep_radians", 0.0},
                                        {"segment_id", "segment-a"},
                                        {"start_vertex_id", "vertex-a"},
                                        {"end_vertex_id", "vertex-b"}},
                         nlohmann::json{{"start", {1.0, 0.0}},
                                        {"end", {1.0, 1.0}},
                                        {"sweep_radians", 0.0},
                                        {"segment_id", "segment-b"},
                                        {"start_vertex_id", "vertex-b"},
                                        {"end_vertex_id", "vertex-c"}},
                         nlohmann::json{{"start", {1.0, 1.0}},
                                        {"end", {0.0, 1.0}},
                                        {"sweep_radians", 0.0},
                                        {"segment_id", "segment-c"},
                                        {"start_vertex_id", "vertex-c"},
                                        {"end_vertex_id", "vertex-d"}},
                         nlohmann::json{{"start", {0.0, 1.0}},
                                        {"end", {0.0, 0.0}},
                                        {"sweep_radians", 0.0},
                                        {"segment_id", "segment-d"},
                                        {"start_vertex_id", "vertex-d"},
                                        {"end_vertex_id", "vertex-a"}},
                     })},
    };
    if (with_authoring_envelope) {
        properties["boundary_authoring"] = opaque_authoring_envelope();
    }
    return properties;
}

Entity identified_boundary(std::string id, bool with_authoring_envelope = false) {
    return entity(std::move(id), "boundary",
                  identified_boundary_properties(with_authoring_envelope));
}

nlohmann::json legacy_boundary_properties_with_collision() {
    const auto identified = identified_boundary_properties();
    nlohmann::json properties = {{"boundary", nlohmann::json::array()}};
    for (const auto& segment : identified.at("segments")) {
        properties["boundary"].push_back({
            {"start", segment.at("start")},
            {"end", segment.at("end")},
            {"sweep_radians", segment.at("sweep_radians")},
        });
    }
    properties["boundary_authoring"] = opaque_authoring_envelope();
    return properties;
}

nlohmann::json future_boundary_properties_with_collision() {
    auto properties = identified_boundary_properties();
    properties["boundary_model_version"] = 999;
    properties["boundary_authoring"] = 7;
    return properties;
}

Document document_with_opaque_authoring_receipt() {
    return Document::create({identified_boundary("boundary-a"),
                             identified_boundary("boundary-z", true)});
}

void test_save_reopen_preserves_exact_revision_history_and_assets() {
    TempDirectory temp;
    const auto file = temp.path / "project.psketch";
    auto document = populated_document();
    const auto snapshot = document.snapshot();

    const auto receipt = ProjectStore::save(file, snapshot);
    require(receipt.revision == snapshot.revision(), "save receipt must identify exact snapshot revision");
    require(!receipt.file_sha256.empty(), "save should return a file fingerprint");

    auto loaded = ProjectStore::load(file);
    require(loaded.file_sha256 == receipt.file_sha256, "load fingerprint should match saved file");
    require(loaded.document.revision() == snapshot.revision(), "load should restore exact head revision");
    require(!loaded.document.dirty(), "a reopened immutable snapshot should be clean");
    const auto reopened = loaded.document.snapshot();
    require(reopened.entities() == snapshot.entities(), "entity JSON should round-trip semantically exactly");
    require(reopened.assets() == snapshot.assets(), "asset bytes and metadata should round-trip exactly");
    require(reopened.history().size() == snapshot.history().size(), "history should be durable");
    require(reopened.named_revisions().at("surveyed") == 2, "named revision should be durable");
    require(sketch::document_authoring_source_digest_v1(reopened) ==
                sketch::document_authoring_source_digest_v1(snapshot),
            "save/reopen bookkeeping must preserve the complete authoring source binding");
    require(sketch::document_snapshot_digest(reopened) != sketch::document_snapshot_digest(snapshot),
            "full commit binding must still distinguish the newly saved snapshot");

    const auto undo_revision = loaded.document.undo(loaded.document.revision());
    require(undo_revision > snapshot.revision(), "reopened history should remain undoable monotonically");
    require(loaded.document.can_redo(), "reopened undo should retain redo navigation");
    require(sketch::document_authoring_source_digest_v1(loaded.document.snapshot()) !=
                sketch::document_authoring_source_digest_v1(snapshot),
            "navigation after reopen must invalidate the old authoring source binding");
}

void test_existing_destination_requires_fingerprint_and_creates_backup() {
    TempDirectory temp;
    const auto file = temp.path / "project.psketch";
    auto first = populated_document();
    const auto first_receipt = ProjectStore::save(file, first.snapshot());

    auto second = populated_document();
    second.apply(ApplyEntityChanges{
        .expected_revision = 2,
        .entity_changes = {EntityChange::upsert(entity("label-1", "label", {{"text", "new"}}))},
    });
    require_error(
        [&] { (void)ProjectStore::save(file, second.snapshot()); },
        StorageErrorCode::destination_exists,
        "an existing destination should not be overwritten without its expected fingerprint");

    const auto second_receipt = ProjectStore::save(
        file, second.snapshot(), SaveOptions{.expected_destination_sha256 = first_receipt.file_sha256});
    require(second_receipt.backup_path.has_value(), "replacement should retain the previous file");
    require(std::filesystem::exists(*second_receipt.backup_path), "reported backup should exist");
    require(ProjectStore::file_sha256(*second_receipt.backup_path) == first_receipt.file_sha256,
            "backup bytes should be the exact prior project");
    require(ProjectStore::load(file).document.revision() == 3, "replacement should publish new revision");
}

void test_external_change_and_injected_failure_preserve_original() {
    TempDirectory temp;
    const auto file = temp.path / "project.psketch";
    auto original = populated_document();
    const auto original_receipt = ProjectStore::save(file, original.snapshot());

    auto replacement = populated_document();
    replacement.apply(ApplyEntityChanges{
        .expected_revision = 2,
        .entity_changes = {EntityChange::upsert(entity("label-1", "label"))},
    });
    require_error(
        [&] {
            (void)ProjectStore::save(
                file, replacement.snapshot(), SaveOptions{.expected_destination_sha256 = std::string(64, '0')});
        },
        StorageErrorCode::external_change,
        "a mismatched expected fingerprint should reject replacement");
    require(ProjectStore::file_sha256(file) == original_receipt.file_sha256,
            "external-change rejection must preserve original bytes");

    require_error(
        [&] {
            (void)ProjectStore::save(
                file, replacement.snapshot(),
                SaveOptions{.expected_destination_sha256 = original_receipt.file_sha256,
                            .fault_stage = SaveFaultStage::before_publish});
        },
        StorageErrorCode::injected_failure,
        "fault injection should exercise the pre-publication rollback path");
    require(ProjectStore::file_sha256(file) == original_receipt.file_sha256,
            "pre-publication failure must preserve original bytes");
}

void test_asset_hash_corruption_and_unsupported_format_are_rejected() {
    TempDirectory temp;
    const auto corrupt_asset_file = temp.path / "bad-asset.psketch";
    auto document = populated_document();
    (void)ProjectStore::save(corrupt_asset_file, document.snapshot());

    sqlite3* database = nullptr;
    require(sqlite3_open_v2(corrupt_asset_file.string().c_str(), &database,
                            SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "test should open database for controlled corruption");
    require(sqlite3_exec(database,
                         "UPDATE revision_assets SET data=x'00' WHERE asset_id='photo-1'",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "test should corrupt an asset payload");
    sqlite3_close(database);
    require_error(
        [&] { (void)ProjectStore::load(corrupt_asset_file); },
        StorageErrorCode::integrity_failure,
        "asset checksum mismatch should reject the project");

    const auto unsupported_file = temp.path / "future.psketch";
    (void)ProjectStore::save(unsupported_file, document.snapshot());
    require(sqlite3_open_v2(unsupported_file.string().c_str(), &database,
                            SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "test should reopen database for format mutation");
    require(sqlite3_exec(database,
                         "UPDATE metadata SET value='999' WHERE key='format_version'",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "test should set unsupported format");
    sqlite3_close(database);
    require_error(
        [&] { (void)ProjectStore::load(unsupported_file); },
        StorageErrorCode::unsupported_format,
        "unknown project format should block loading");
}

void test_saving_snapshot_r_does_not_mark_head_r_plus_one_clean() {
    TempDirectory temp;
    const auto file = temp.path / "snapshot.psketch";
    auto document = populated_document();
    const auto snapshot_r = document.snapshot();
    document.apply(ApplyEntityChanges{
        .expected_revision = snapshot_r.revision(),
        .entity_changes = {EntityChange::upsert(entity("label-1", "label", {{"text", "later"}}))},
    });

    const auto receipt = ProjectStore::save(file, snapshot_r);
    document.mark_saved(receipt.revision);
    require(document.revision() == snapshot_r.revision() + 1 && document.dirty(),
            "head R+1 must remain dirty after snapshot R is saved");
    auto loaded = ProjectStore::load(file);
    require(loaded.document.revision() == snapshot_r.revision(),
            "saved file must contain snapshot R rather than current document head");
    require(!loaded.document.snapshot().entities().contains("label-1"),
            "post-snapshot edits must not leak into saved bytes");
}

void test_unknown_required_entity_reopens_read_only() {
    TempDirectory temp;
    const auto file = temp.path / "required-future.psketch";
    auto document = Document::create(
        {entity("future-1", "vendor_future", {{"opaque", 7}}, true)});
    (void)ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    require(!loaded.document.is_editable(), "unknown required stored data must reopen read-only");
    require(loaded.document.snapshot().entities().at("future-1").properties.at("opaque") == 7,
            "read-only fallback must retain unknown required payload");
}

void test_competing_saves_with_one_fingerprint_publish_exactly_once() {
    TempDirectory temp;
    const auto file = temp.path / std::filesystem::path(u8"contended-résumé-日本.psketch");
    auto original = populated_document();
    const auto original_receipt = ProjectStore::save(file, original.snapshot());

    constexpr std::size_t writer_count = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(writer_count));
    std::atomic<std::size_t> successes = 0;
    std::atomic<std::size_t> external_changes = 0;
    std::atomic<std::size_t> unexpected_failures = 0;
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    const auto absolute = std::filesystem::absolute(file).wstring();
    const std::filesystem::path extended_file(L"\\\\?\\" + absolute);
    const std::filesystem::path trailing_dot_file(file.wstring() + L".");
    std::vector<std::filesystem::path> aliases{file, extended_file, trailing_dot_file};
#ifdef _WIN32
    const auto short_required = GetShortPathNameW(file.c_str(), nullptr, 0);
    if (short_required != 0) {
        std::wstring short_name(short_required, L'\0');
        const auto short_written =
            GetShortPathNameW(file.c_str(), short_name.data(), short_required);
        if (short_written != 0 && short_written < short_name.size()) {
            short_name.resize(short_written);
            if (_wcsicmp(short_name.c_str(), file.c_str()) != 0) {
                aliases.emplace_back(short_name);
            }
        }
    }
#endif
    for (std::size_t index = 0; index < writer_count; ++index) {
        writers.emplace_back([&, index] {
            auto replacement = populated_document();
            replacement.apply(ApplyEntityChanges{
                .expected_revision = 2,
                .entity_changes = {EntityChange::upsert(
                    entity("label-1", "label", {{"writer", index}}))},
            });
            start.arrive_and_wait();
            try {
                (void)ProjectStore::save(
                    aliases[index % aliases.size()], replacement.snapshot(),
                    SaveOptions{.expected_destination_sha256 = original_receipt.file_sha256});
                ++successes;
            } catch (const StorageError& error) {
                if (error.code() == StorageErrorCode::external_change) {
                    ++external_changes;
                } else {
                    ++unexpected_failures;
                }
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    require(successes == 1, "one expected fingerprint must authorize exactly one competing save");
    require(external_changes == writer_count - 1 && unexpected_failures == 0,
            "losing competing saves should fail as external changes");

    const auto ambiguous_missing = temp.path / "ambiguous-new.psketch.";
    require_error_contains(
        [&] { (void)ProjectStore::save(ambiguous_missing, populated_document().snapshot()); },
        StorageErrorCode::io_error, "ambiguous",
        "a missing destination with a trailing-dot alias must fail before mutex creation");
    const auto reserved_missing = temp.path / "NUL.psketch";
    require_error_contains(
        [&] { (void)ProjectStore::save(reserved_missing, populated_document().snapshot()); },
        StorageErrorCode::io_error, "standalone Windows file",
        "a DOS device basename must not enter the standalone project protocol");
    const std::filesystem::path alternate_stream(file.wstring() + L":payload");
    require_error_contains(
        [&] {
            (void)ProjectStore::save(
                alternate_stream, populated_document().snapshot(),
                SaveOptions{.expected_destination_sha256 = original_receipt.file_sha256});
        },
        StorageErrorCode::io_error, "standalone Windows file",
        "an alternate data stream must not enter the standalone project protocol");
}

void test_impossible_history_is_rejected_after_digest_recomputation() {
    TempDirectory temp;
    const auto file = temp.path / "impossible-history.psketch";
    auto document = populated_document();
    (void)ProjectStore::save(file, document.snapshot());
    execute_sql(file, "UPDATE revisions SET undo_stack_json='[]' WHERE revision=2");
    rewrite_logical_digest(file);
    require_error_contains(
        [&] { (void)ProjectStore::load(file); }, StorageErrorCode::integrity_failure,
        "named revision transition is impossible",
        "a recomputed checksum must not legitimize impossible history");

    const auto names_file = temp.path / "impossible-names.psketch";
    (void)ProjectStore::save(names_file, document.snapshot());
    execute_sql(names_file, "UPDATE named_revisions SET name='different-name'");
    rewrite_logical_digest(names_file);
    require_error_contains(
        [&] { (void)ProjectStore::load(names_file); }, StorageErrorCode::integrity_failure,
        "bijection", "named revision index must exactly match named history records");
}

Entity translation_fixture() {
    sketch::BoundaryConstructionRecord record;
    record.schema_version = sketch::boundary_receipt_schema_version_v2;
    record.boundary_id = "translated-boundary";
    const sketch::Vec2 points[]{{0, 0}, {2, 0}, {2, 1}, {0, 1}};
    sketch::IdentifiedBoundary boundary{record.boundary_id, "measurement_boundary", {}};
    for (std::size_t index = 0; index < 4; ++index) {
        const auto edge = "edge-" + std::to_string(index);
        const auto start = "vertex-" + std::to_string(index);
        const auto end = "vertex-" + std::to_string((index + 1) % 4);
        sketch::ConstructionReceipt receipt;
        receipt.segment_id = edge;
        receipt.kind = sketch::BoundaryConstructionKind::line_to_point;
        receipt.start = points[index];
        receipt.chord_end = points[(index + 1) % 4];
        record.edges.push_back({edge, start, end, receipt});
        boundary.segments.push_back({edge, start, end, {points[index], points[(index + 1) % 4], 0}});
    }
    auto result = sketch::encode_identified_boundary_entity(boundary);
    result.properties["boundary_authoring"] = sketch::encode_boundary_receipt_envelope(record);
    return result;
}

void test_translation_proof_storage_and_forgery_rejection() {
    TempDirectory temp;
    const auto file = temp.path / "translation-v5.psketch";
    auto document = Document::create({translation_fixture()});
    const auto original = document.snapshot().entities();
    document.apply(sketch::TranslateBoundary{0, {"translated-boundary", {8, -4}}});
    const auto moved = document.snapshot().entities();
    document.undo(document.revision());
    require(ProjectStore::required_format_version(document.snapshot()) == 5,
            "translation in undone history must require v5");
    auto saved = ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    require(loaded.document.snapshot().entities() == original && loaded.document.can_redo(),
            "v5 reopen must preserve undone state and redo");
    const auto reopened_snapshot = loaded.document.snapshot();
    const auto& proof = reopened_snapshot.history().at(1).boundary_translation;
    require(proof && proof->boundary_id == "translated-boundary" &&
                proof->offset.x == 8 && proof->offset.y == -4,
            "translation proof must survive SQLite storage");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == moved, "v5 redo must restore exact translated entities");
    saved = ProjectStore::save(file, loaded.document.snapshot(),
        SaveOptions{.expected_destination_sha256 = saved.file_sha256});
    require(saved.backup_path.has_value(), "v5 document replacement must preserve verified backup");
    require(ProjectStore::load(file).document.snapshot().entities() == moved,
            "replacement v5 document must reopen");

    const std::vector<std::string> malformed{
        R"({"version":2,"boundary_id":"translated-boundary","offset":[8.0,-4.0]})",
        R"({"version":1,"boundary_id":"translated-boundary","offset":[8.0,-4.0],"extra":true})",
        R"({"version":1,"boundary_id":"bad id","offset":[8.0,-4.0]})",
        R"({"version":1,"boundary_id":"translated-boundary","offset":[null,-4.0]})",
        R"({"version":1,"boundary_id":"translated-boundary","offset":[1e999,-4.0]})",
        R"({"version":1,"version":1,"boundary_id":"translated-boundary","offset":[8.0,-4.0]})"};
    for (std::size_t index = 0; index < malformed.size(); ++index) {
        const auto tampered = temp.path / ("malformed-" + std::to_string(index) + ".psketch");
        std::filesystem::copy_file(file, tampered);
        execute_sql(tampered, "UPDATE revisions SET boundary_translation_json='" + malformed[index] + "' WHERE revision=1");
        require_error([&] { (void)ProjectStore::load(tampered); }, StorageErrorCode::integrity_failure,
                      "malformed translation proof must reject before replay");
    }
    for (const auto replacement : {std::string("NULL"),
             std::string("'{\"version\":1,\"boundary_id\":\"translated-boundary\",\"offset\":[9.0,-4.0]}'")}) {
        const auto tampered = temp.path / (replacement == "NULL" ? "missing-proof.psketch" : "forged-offset.psketch");
        std::filesystem::copy_file(file, tampered);
        execute_sql(tampered, "UPDATE revisions SET boundary_translation_json=" + replacement + " WHERE revision=1");
        rewrite_logical_digest(tampered);
        require_error([&] { (void)ProjectStore::load(tampered); }, StorageErrorCode::integrity_failure,
                      "recomputed digest must not authorize missing or forged translation proof");
    }
    const auto downgraded = temp.path / "downgraded-translation.psketch";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "ALTER TABLE revisions DROP COLUMN boundary_translation_json; "
        "PRAGMA user_version=3; UPDATE metadata SET value='3' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::integrity_failure,
                  "downgraded history must fail the unchanged raw receipt guard");
}

void test_transform_proof_storage_and_forgery_rejection() {
    TempDirectory temp;
    const auto file = temp.path / "transform-v6.psketch";
    auto document = Document::create({translation_fixture()});
    sketch::PlanarTransform transform;
    transform.pivot = {1, 0.5};
    transform.rotation_radians = 0.4;
    transform.flip_horizontal = true;
    transform.offset = {3, -2};
    document.apply(sketch::TransformBoundary{document.revision(), {"translated-boundary", transform}});
    const auto transformed = document.snapshot().entities();
    // A later historical command must not reduce the required format to v5.
    document.apply(sketch::TranslateBoundary{document.revision(), {"translated-boundary", {8, -4}}});
    const auto moved = document.snapshot().entities();
    document.undo(document.revision());
    require(ProjectStore::required_format_version(document.snapshot()) == 6,
            "mixed transform/translation history must require v6");
    auto saved = ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    const auto snapshot = loaded.document.snapshot();
    require(snapshot.entities() == transformed && loaded.document.can_redo(),
            "v6 reopen must preserve undone state and redo");
    require(snapshot.history().at(1).boundary_transform &&
                !snapshot.history().at(1).boundary_translation &&
                snapshot.history().at(2).boundary_translation &&
                !snapshot.history().at(2).boundary_transform,
            "mixed history must retain distinct command proofs");
    const auto wire = sketch::encode_boundary_transform(*snapshot.history().at(1).boundary_transform);
    require(wire == sketch::encode_boundary_transform({"translated-boundary", transform}),
            "transform fields must survive SQLite storage");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == moved, "v6 redo must restore exact entities");
    loaded.document.undo(loaded.document.revision());
    loaded.document.undo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == document.snapshot().history().front().entities,
            "v6 transform must undo after reopen");
    saved = ProjectStore::save(file, document.snapshot(),
        SaveOptions{.expected_destination_sha256 = saved.file_sha256});
    require(saved.backup_path && ProjectStore::load(*saved.backup_path).document.can_redo(),
            "v6 replacement must preserve a loadable verified backup");

    auto wrong_version = wire; wrong_version["version"] = 2;
    auto wrong_type = wire; wrong_type["flip_horizontal"] = 1;
    auto extra = wire; extra["extra"] = true;
    auto invalid_coordinate = wire; invalid_coordinate["pivot"][0] = nullptr;
    auto duplicate = wire.dump(); duplicate.insert(1, "\"version\":1,");
    const std::vector<std::string> malformed{wrong_version.dump(), wrong_type.dump(), extra.dump(),
        invalid_coordinate.dump(), duplicate};
    for (std::size_t index = 0; index < malformed.size(); ++index) {
        const auto tampered = temp.path / ("bad-transform-" + std::to_string(index) + ".psketch");
        std::filesystem::copy_file(file, tampered);
        execute_sql(tampered, "UPDATE revisions SET boundary_transform_json='" + malformed[index] + "' WHERE revision=1");
        require_error([&] { (void)ProjectStore::load(tampered); }, StorageErrorCode::integrity_failure,
                      "malformed or duplicate transform proof must reject");
    }
    auto forged = wire; forged["rotation_radians"] = 0.8;
    for (const auto& replacement : {std::string("NULL"), "'" + forged.dump() + "'"}) {
        const auto tampered = temp.path / (replacement == "NULL" ? "missing-transform.psketch" : "forged-transform.psketch");
        std::filesystem::copy_file(file, tampered);
        execute_sql(tampered, "UPDATE revisions SET boundary_transform_json=" + replacement + " WHERE revision=1");
        rewrite_logical_digest(tampered);
        require_error([&] { (void)ProjectStore::load(tampered); }, StorageErrorCode::integrity_failure,
                      "a recomputed digest must not authorize a missing or forged transform");
    }
    const auto downgraded = temp.path / "downgraded-transform.psketch";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "ALTER TABLE revisions DROP COLUMN boundary_transform_json; "
        "PRAGMA user_version=5; UPDATE metadata SET value='5' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::integrity_failure,
                  "downgraded transform history must fail replay");
    const auto overloaded = temp.path / "oversized-transform.psketch";
    std::filesystem::copy_file(file, overloaded);
    execute_sql(overloaded, "UPDATE revisions SET boundary_transform_json=CAST(zeroblob(67108865) AS TEXT) WHERE revision=1");
    require_error([&] { (void)ProjectStore::load(overloaded); }, StorageErrorCode::resource_limit,
                  "transform proof bytes must count toward preallocation budget");
}

void test_boundary_authoring_receipt_after_v2_entity_requires_v3() {
    auto document = document_with_opaque_authoring_receipt();
    const auto snapshot = document.snapshot();
    require(ProjectStore::required_format_version(snapshot) == 3,
            "a retained boundary_authoring property after an identified boundary must require v3");
}

void test_unqualified_authoring_property_collisions_remain_v1_and_opaque() {
    TempDirectory temp;
    const auto label_file = temp.path / "label-collision-v1.psketch";
    const auto legacy_file = temp.path / "legacy-boundary-collision-v1.psketch";
    const auto label = entity("vendor-label", "label",
                              {{"boundary_authoring", opaque_authoring_envelope()}});
    const auto legacy_boundary =
        entity("legacy-boundary", "boundary", legacy_boundary_properties_with_collision());

    const auto label_document = Document::create({label});
    const auto legacy_document = Document::create({legacy_boundary});
    require(ProjectStore::required_format_version(label_document.snapshot()) == 1,
            "a generic entity with a vendor boundary_authoring collision must remain v1");
    require(ProjectStore::required_format_version(legacy_document.snapshot()) == 1,
            "an anonymous legacy boundary with a vendor boundary_authoring collision must remain v1");

    (void)ProjectStore::save(label_file, label_document.snapshot());
    (void)ProjectStore::save(legacy_file, legacy_document.snapshot());
    sqlite3* database = nullptr;
    require(sqlite3_open_v2(label_file.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "test should reopen generic collision project for marker inspection");
    require(metadata_value(database, "format_version") == "1",
            "a generic collision must persist metadata format v1");
    require(sqlite_user_version(database) == 1,
            "a generic collision must persist SQLite user_version 1");
    sqlite3_close(database);
    require(sqlite3_open_v2(legacy_file.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "test should reopen legacy collision project for marker inspection");
    require(metadata_value(database, "format_version") == "1",
            "a legacy collision must persist metadata format v1");
    require(sqlite_user_version(database) == 1,
            "a legacy collision must persist SQLite user_version 1");
    sqlite3_close(database);
    const auto label_reopened = ProjectStore::load(label_file).document.snapshot();
    const auto legacy_reopened = ProjectStore::load(legacy_file).document.snapshot();
    require(label_reopened.entities().at("vendor-label").properties.at("boundary_authoring").dump() ==
                label.properties.at("boundary_authoring").dump(),
            "v1 generic collision property must remain exactly opaque after reopen");
    require(legacy_reopened.entities().at("legacy-boundary").properties.at("boundary_authoring").dump() ==
                legacy_boundary.properties.at("boundary_authoring").dump(),
            "v1 legacy collision property must remain exactly opaque after reopen");
}

void test_unknown_boundary_model_collision_requires_v2() {
    TempDirectory temp;
    const auto file = temp.path / "future-boundary-collision-v2.psketch";
    const auto future_boundary =
        entity("future-boundary", "boundary", future_boundary_properties_with_collision());
    const auto document = Document::create({future_boundary});
    const auto snapshot = document.snapshot();
    require(ProjectStore::required_format_version(snapshot) == 2,
            "an unknown boundary model with a malformed receipt-looking property must require v2");
    (void)ProjectStore::save(file, snapshot);

    sqlite3* database = nullptr;
    require(sqlite3_open_v2(file.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "test should reopen future boundary project for marker inspection");
    require(metadata_value(database, "format_version") == "2",
            "an unknown boundary model collision must persist format v2");
    require(sqlite_user_version(database) == 2,
            "an unknown boundary model collision must persist SQLite user_version 2");
    sqlite3_close(database);

    auto reopened_document = ProjectStore::load(file).document;
    require(!reopened_document.is_editable(),
            "unknown boundary model data must remain read-only because of its own version");
    const auto reopened = reopened_document.snapshot();
    require(reopened.entities().at("future-boundary").properties.at("boundary_authoring") == 7,
            "unknown boundary model data must retain its receipt-looking property opaquely");
}

void test_v3_save_reopen_preserves_opaque_positive_authoring_envelope() {
    TempDirectory temp;
    const auto file = temp.path / "receipt-v3.psketch";
    auto document = document_with_opaque_authoring_receipt();
    const auto snapshot = document.snapshot();
    const auto expected_envelope =
        snapshot.entities().at("boundary-z").properties.at("boundary_authoring");

    (void)ProjectStore::save(file, snapshot);

    sqlite3* database = nullptr;
    require(sqlite3_open_v2(file.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "test should reopen v3 project for marker inspection");
    require(metadata_value(database, "format_version") == "3",
            "receipt-bearing project should persist metadata format v3");
    require(sqlite_user_version(database) == 3,
            "receipt-bearing project should persist SQLite user_version 3");
    sqlite3_close(database);

    const auto reopened = ProjectStore::load(file).document.snapshot();
    require(reopened.entities().at("boundary-z").properties.at("boundary_authoring") ==
                expected_envelope,
            "v3 load should preserve the opaque positive authoring envelope");
    require(reopened.entities().at("boundary-z").properties.at("boundary_authoring").dump() ==
                expected_envelope.dump(),
            "v3 load should preserve the opaque authoring envelope representation");
#ifdef _WIN32
    wchar_t* capture_value = nullptr;
    std::size_t capture_length = 0;
    require(_wdupenv_s(&capture_value, &capture_length, L"SKETCH_CAPTURE_RECEIPT_STORAGE") == 0,
            "cannot read receipt fixture capture directory");
    const std::unique_ptr<wchar_t, decltype(&std::free)> capture(capture_value, &std::free);
    if (capture && capture_length > 1) {
        const std::filesystem::path directory(capture.get());
        require(std::filesystem::is_directory(directory), "receipt capture directory must already exist");
        require(std::filesystem::copy_file(file, directory / "receipt-v3.bldproj"),
                "cannot retain actual receipt fixture; existing files must not be overwritten");
    }
#endif
}

void test_downgraded_v3_receipt_markers_reject_before_receipt_acceptance() {
    TempDirectory temp;
    const auto file = temp.path / "receipt-downgrade.psketch";
    (void)ProjectStore::save(file, document_with_opaque_authoring_receipt().snapshot());

    execute_sql(file,
                "PRAGMA user_version=2; "
                "UPDATE metadata SET value='2' WHERE key='format_version'");
    require_error_contains(
        [&] { (void)ProjectStore::load(file); }, StorageErrorCode::unsupported_format,
        "boundary_authoring", "downgraded v3 markers must reject retained receipt data");
}

void test_document_only_api_refuses_future_recovery_destinations_before_overwrite() {
    const auto snapshot = populated_document().snapshot();
    const std::array<std::pair<std::string_view, int>, 3> future_versions{
        std::pair<std::string_view, int>{"v4", 4},
        std::pair<std::string_view, int>{"v5-archive", 5},
        std::pair<std::string_view, int>{"future999", 999}};

    for (const auto [label, version] : future_versions) {
        TempDirectory temp;
        const auto file = temp.path / ("document-only-" + std::string(label) + ".psketch");
        (void)ProjectStore::save(file, snapshot);
        execute_sql(
            file,
            "PRAGMA user_version=" + std::to_string(version) +
                "; UPDATE metadata SET value='" + std::to_string(version) +
                "' WHERE key='format_version'; "
                "CREATE TABLE project_recovery_records("
                "record_id TEXT PRIMARY KEY, record_kind TEXT NOT NULL, envelope_json TEXT NOT NULL"
                ") STRICT; "
                "INSERT INTO project_recovery_records(record_id,record_kind,envelope_json) "
                "VALUES('opaque-record','future_kind','{\"version\":999,\"opaque\":true}')");

        const auto original_hash = ProjectStore::file_sha256(file);
        require_error(
            [&] { (void)ProjectStore::load(file); }, StorageErrorCode::unsupported_format,
            "document-only load must reject a future recovery archive");
        require_error(
            [&] {
                (void)ProjectStore::save(
                    file, snapshot,
                    SaveOptions{.expected_destination_sha256 = original_hash});
            },
            StorageErrorCode::unsupported_format,
            "document-only save must reject overwriting a future recovery archive with a correct CAS");
        require(ProjectStore::file_sha256(file) == original_hash,
                "rejected document-only save must preserve exact future recovery archive bytes");
        for (const auto& entry : std::filesystem::directory_iterator(temp.path)) {
            require(entry.path() == file,
                    "rejected document-only save must leave no temporary, backup, or journal artifacts");
        }
    }
}

void test_schema_metadata_and_user_version_are_exact() {
    TempDirectory temp;
    auto document = populated_document();

    const auto user_version_file = temp.path / "user-version.psketch";
    (void)ProjectStore::save(user_version_file, document.snapshot());
    execute_sql(user_version_file, "PRAGMA user_version=2");
    require_error(
        [&] { (void)ProjectStore::load(user_version_file); },
        StorageErrorCode::unsupported_format, "unexpected SQLite user_version should be rejected");

    const auto unsupported_marker_file = temp.path / "unsupported-marker.psketch";
    (void)ProjectStore::save(unsupported_marker_file, document.snapshot());
    execute_sql(unsupported_marker_file, "PRAGMA user_version=4");
    require_error(
        [&] { (void)ProjectStore::load(unsupported_marker_file); },
        StorageErrorCode::unsupported_format, "unsupported SQLite user_version should be rejected");

    const auto dimension_file = temp.path / "dimension-only-v1.psketch";
    auto dimension_only = Document::create({Entity::create("dimension", {{"dimension_version", 999}})});
    (void)ProjectStore::save(dimension_file, dimension_only.snapshot());
    execute_sql(dimension_file, "PRAGMA user_version=1; UPDATE metadata SET value='1' WHERE key='format_version'");
    require_error_contains([&] { (void)ProjectStore::load(dimension_file); },
        StorageErrorCode::unsupported_format, "dimension", "dimension-only v2 semantics were accepted as v1");

    const auto metadata_file = temp.path / "metadata.psketch";
    (void)ProjectStore::save(metadata_file, document.snapshot());
    execute_sql(metadata_file, "INSERT INTO metadata(key,value) VALUES('unrecognized','value')");
    require_error_contains(
        [&] { (void)ProjectStore::load(metadata_file); }, StorageErrorCode::integrity_failure,
        "metadata key set", "extra metadata must be rejected for exact format v1");

    const auto columns_file = temp.path / "columns.psketch";
    (void)ProjectStore::save(columns_file, document.snapshot());
    execute_sql(columns_file,
                "ALTER TABLE metadata ADD COLUMN unexpected TEXT");
    require_error_contains(
        [&] { (void)ProjectStore::load(columns_file); }, StorageErrorCode::integrity_failure,
        "columns", "schema column mutation must be rejected");

    const auto foreign_key_file = temp.path / "foreign-key.psketch";
    (void)ProjectStore::save(foreign_key_file, document.snapshot());
    execute_sql(foreign_key_file,
                "ALTER TABLE named_revisions RENAME TO old_names;"
                "CREATE TABLE named_revisions(name TEXT PRIMARY KEY,revision INTEGER NOT NULL) STRICT;"
                "INSERT INTO named_revisions SELECT * FROM old_names;"
                "DROP TABLE old_names");
    require_error_contains(
        [&] { (void)ProjectStore::load(foreign_key_file); },
        StorageErrorCode::integrity_failure, "foreign keys",
        "missing declared foreign key must be rejected");

    const auto strict_file = temp.path / "strict.psketch";
    (void)ProjectStore::save(strict_file, document.snapshot());
    execute_sql(strict_file,
                "ALTER TABLE metadata RENAME TO old_metadata;"
                "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                "INSERT INTO metadata SELECT * FROM old_metadata;"
                "DROP TABLE old_metadata");
    require_error_contains(
        [&] { (void)ProjectStore::load(strict_file); }, StorageErrorCode::integrity_failure,
        "STRICT", "non-STRICT replacement table must be rejected");
}

void test_load_resource_limits_precede_large_allocations() {
    TempDirectory temp;
    const auto oversized = temp.path / "oversized.psketch";
    {
        std::ofstream stream(oversized, std::ios::binary);
        stream.put('\0');
    }
    std::filesystem::resize_file(oversized, ProjectStore::maximum_file_bytes + 1);
    require_error_contains(
        [&] { (void)ProjectStore::load(oversized); }, StorageErrorCode::resource_limit,
        "file size", "oversized sparse project must be rejected before SQLite decode");

    const auto revisions_file = temp.path / "too-many-revisions.psketch";
    auto document = populated_document();
    (void)ProjectStore::save(revisions_file, document.snapshot());
    execute_sql(
        revisions_file,
        "WITH RECURSIVE seq(x) AS (VALUES(3) UNION ALL SELECT x+1 FROM seq WHERE x<10000) "
        "INSERT INTO revisions(revision,parent_revision,source_revision,action,name,"
        "undo_stack_json,redo_stack_json) SELECT x,x-1,NULL,'bulk',NULL,'[]','[]' FROM seq");
    require_error_contains(
        [&] { (void)ProjectStore::load(revisions_file); }, StorageErrorCode::resource_limit,
        "revision count", "revision limit+1 must be rejected before history allocation");
}

void test_aggregate_json_byte_and_value_budgets_are_enforced() {
    TempDirectory temp;
    auto document = populated_document();

    const auto bytes_file = temp.path / "json-bytes.psketch";
    (void)ProjectStore::save(bytes_file, document.snapshot());
    const std::string multibyte_code_point = "\xc3\xa9";
    constexpr std::uint64_t multibyte_count = 425'000;
    constexpr std::size_t entity_count = 40;
    std::string oversized_json = "{\"text\":\"";
    oversized_json.reserve(static_cast<std::size_t>(multibyte_count * 2 + 12));
    for (std::uint64_t index = 0; index < multibyte_count; ++index) {
        oversized_json.append(multibyte_code_point);
    }
    oversized_json.append("\"}");
    const auto aggregate_bytes = oversized_json.size() * entity_count * 2;
    const auto aggregate_characters =
        (oversized_json.size() - multibyte_count) * entity_count * 2;
    require(oversized_json.size() < 1024 * 1024,
            "each multibyte fixture object must remain within the per-object limit");
    require(aggregate_bytes > ProjectStore::maximum_encoded_json_bytes,
            "multibyte fixture must exceed the aggregate byte budget");
    require(aggregate_characters < ProjectStore::maximum_encoded_json_bytes,
            "multibyte fixture must remain below the old SQLite character-count check");
    constexpr std::array<Revision, 2> named_transition_states = {1, 2};
    insert_json_entities_in_revisions(bytes_file, entity_count, oversized_json,
                                      named_transition_states);
    rewrite_logical_digest(bytes_file);
    require_error_contains(
        [&] { (void)ProjectStore::load(bytes_file); }, StorageErrorCode::resource_limit,
        "encoded JSON bytes",
        "aggregate encoded UTF-8 byte limit+1 must reject before allocating a JSON document");

    const auto values_file = temp.path / "json-values.psketch";
    (void)ProjectStore::save(values_file, document.snapshot());
    nlohmann::json values = nlohmann::json::object();
    values["values"] = nlohmann::json::array();
    values["values"].get_ref<nlohmann::json::array_t&>().resize(90'000);
    insert_json_entities(values_file, 23, values.dump());
    require_error_contains(
        [&] { (void)ProjectStore::load(values_file); }, StorageErrorCode::resource_limit,
        "JSON value count",
        "aggregate JSON value limit+1 must reject before retaining an unbounded graph");
}

void test_validated_staging_handle_blocks_path_tampering_and_publishes_exact_bytes() {
#ifdef _WIN32
    TempDirectory temp;
    const auto file = temp.path / "validated-publication.psketch";
    auto document = populated_document();
    std::string validated_digest;
    const auto receipt = ProjectStore::save(
        file, document.snapshot(),
        SaveOptions{.after_validation_barrier =
                        [&](const std::filesystem::path& staging, const std::string& digest) {
                            validated_digest = digest;
                            const auto write_handle = CreateFileW(
                                staging.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                            require(write_handle == INVALID_HANDLE_VALUE &&
                                        GetLastError() == ERROR_SHARING_VIOLATION,
                                    "validated staging bytes must deny raw external writes");

                            const auto renamed = staging.parent_path() / "stolen-staging.psketch";
                            require(!MoveFileExW(staging.c_str(), renamed.c_str(), 0) &&
                                        GetLastError() == ERROR_SHARING_VIOLATION,
                                    "validated staging identity must deny external rename");
                            require(!DeleteFileW(staging.c_str()) &&
                                        GetLastError() == ERROR_SHARING_VIOLATION,
                                    "validated staging identity must deny external deletion");

                            const auto planted = CreateFileW(
                                staging.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            require(planted == INVALID_HANDLE_VALUE &&
                                        GetLastError() == ERROR_SHARING_VIOLATION,
                                    "validated staging path must reject a planted replacement");
                        }});
    require(!validated_digest.empty(), "save should expose its deterministic validation barrier");
    require(receipt.file_sha256 == validated_digest,
            "save receipt must identify the exact bytes validated under the publication lock");
    require(ProjectStore::file_sha256(file) == validated_digest,
            "handle publication must publish the exact validated staging bytes");
#endif
}

void test_destination_identity_is_rechecked_after_verified_backup() {
#ifdef _WIN32
    TempDirectory temp;
    const auto file = temp.path / "destination-race.psketch";
    const auto displaced = temp.path / "externally-moved-original.psketch";
    auto original = populated_document();
    const auto original_receipt = ProjectStore::save(file, original.snapshot());
    auto replacement = populated_document();
    replacement.apply(ApplyEntityChanges{
        .expected_revision = 2,
        .entity_changes = {EntityChange::upsert(
            entity("label-1", "label", {{"text", "replacement"}}))},
    });
    bool barrier_reached = false;
    require_error(
        [&] {
            (void)ProjectStore::save(
                file, replacement.snapshot(),
                SaveOptions{
                    .expected_destination_sha256 = original_receipt.file_sha256,
                    .before_publication_barrier =
                        [&](const std::filesystem::path& destination, const std::string& digest,
                            const std::optional<std::filesystem::path>& backup) {
                            barrier_reached = true;
                            require(backup.has_value(),
                                    "replacement barrier must identify its verified backup");
                            require(digest == original_receipt.file_sha256,
                                    "prepublication barrier must follow exact backup validation");
                            const auto writer = CreateFileW(
                                destination.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                            require(writer == INVALID_HANDLE_VALUE &&
                                        GetLastError() == ERROR_SHARING_VIOLATION,
                                    "destination guard must deny content modification after backup");
                            require(MoveFileExW(destination.c_str(), displaced.c_str(), 0),
                                    "fixture should exercise the remaining rename-only race");
                            const auto planted = CreateFileW(
                                destination.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                            require(planted != INVALID_HANDLE_VALUE,
                                    "fixture should plant a distinct replacement pathname");
                            static constexpr char external_bytes[] = "external replacement";
                            DWORD written = 0;
                            require(WriteFile(planted, external_bytes,
                                              static_cast<DWORD>(sizeof(external_bytes) - 1),
                                              &written, nullptr) &&
                                        written == sizeof(external_bytes) - 1 &&
                                        FlushFileBuffers(planted),
                                    "fixture should persist the external replacement bytes");
                            CloseHandle(planted);
                        }});
        },
        StorageErrorCode::external_change,
        "a destination renamed and replaced after backup validation must fail closed");
    require(barrier_reached, "destination race fixture must reach the prepublication barrier");
    require(ProjectStore::file_sha256(displaced) == original_receipt.file_sha256,
            "the moved expected destination must retain its original bytes");
    std::ifstream external(file, std::ios::binary);
    const std::string external_bytes((std::istreambuf_iterator<char>(external)),
                                     std::istreambuf_iterator<char>());
    require(external_bytes == "external replacement",
            "failed save must not overwrite the externally planted destination");
    for (const auto& entry : std::filesystem::directory_iterator(temp.path)) {
        require(entry.path().filename().wstring().find(L".bak.") == std::wstring::npos,
                "failed destination revalidation must clean its unpublished verified backup");
    }
#endif
}

void test_verified_backup_is_locked_through_publication() {
#ifdef _WIN32
    TempDirectory temp;
    const auto file = temp.path / "backup-lock.psketch";
    auto original = populated_document();
    const auto original_receipt = ProjectStore::save(file, original.snapshot());
    auto replacement = populated_document();
    replacement.apply(ApplyEntityChanges{
        .expected_revision = 2,
        .entity_changes = {EntityChange::upsert(
            entity("label-backup", "label", {{"text", "replacement"}}))},
    });
    std::optional<std::filesystem::path> observed_backup;
    const auto receipt = ProjectStore::save(
        file, replacement.snapshot(),
        SaveOptions{
            .expected_destination_sha256 = original_receipt.file_sha256,
            .before_publication_barrier =
                [&](const std::filesystem::path&, const std::string& expected_digest,
                    const std::optional<std::filesystem::path>& backup) {
                    require(expected_digest == original_receipt.file_sha256 && backup.has_value(),
                            "backup barrier must follow exact prior-digest validation");
                    observed_backup = backup;
                    const auto writer = CreateFileW(
                        backup->c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    require(writer == INVALID_HANDLE_VALUE &&
                                GetLastError() == ERROR_SHARING_VIOLATION,
                            "verified backup must deny external writes");
                    const auto renamed = temp.path / "stolen-backup.psketch";
                    require(!MoveFileExW(backup->c_str(), renamed.c_str(), 0) &&
                                GetLastError() == ERROR_SHARING_VIOLATION,
                            "verified backup must deny external rename");
                    require(!DeleteFileW(backup->c_str()) &&
                                GetLastError() == ERROR_SHARING_VIOLATION,
                            "verified backup must deny external deletion");
                    const auto attacker = temp.path / "attacker-backup.psketch";
                    const auto attacker_handle = CreateFileW(
                        attacker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    require(attacker_handle != INVALID_HANDLE_VALUE,
                            "test should create a backup replacement candidate");
                    CloseHandle(attacker_handle);
                    const auto replaced = MoveFileExW(attacker.c_str(), backup->c_str(),
                                                      MOVEFILE_REPLACE_EXISTING);
                    const auto replace_error = GetLastError();
                    require(!replaced &&
                                (replace_error == ERROR_SHARING_VIOLATION ||
                                 replace_error == ERROR_ACCESS_DENIED),
                            "verified backup must deny replacement");
                    require(DeleteFileW(attacker.c_str()),
                            "test should remove unused backup replacement candidate");
                }});
    require(observed_backup == receipt.backup_path,
            "receipt must identify the exact backup held through publication");
    require(ProjectStore::file_sha256(*receipt.backup_path) == original_receipt.file_sha256,
            "published receipt backup must retain the expected prior digest");
#endif
}

void test_failed_checked_cleanup_reports_every_locked_residual(bool typed_error) {
#ifdef _WIN32
    TempDirectory temp;
    const auto file = temp.path / "checked-cleanup.psketch";
    auto document = populated_document();
    const auto original = ProjectStore::save(file, document.snapshot());
    document.apply(ApplyEntityChanges{
        .expected_revision = 2,
        .entity_changes = {EntityChange::upsert(entity("label-cleanup", "label"))},
    });
    HANDLE wal_blocker = INVALID_HANDLE_VALUE;
    HANDLE backup_blocker = INVALID_HANDLE_VALUE;
    std::filesystem::path wal_residual;
    std::filesystem::path backup_residual;
    try {
        (void)ProjectStore::save(
            file, document.snapshot(),
            SaveOptions{
                .expected_destination_sha256 = original.file_sha256,
                .after_validation_barrier =
                    [&](const std::filesystem::path& staging, const std::string&) {
                        wal_residual = std::filesystem::path(staging.native() + L"-wal");
                        wal_blocker = CreateFileW(
                            wal_residual.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                        require(wal_blocker != INVALID_HANDLE_VALUE,
                                "test should create a deletion-denying staging sidecar");
                    },
                .before_publication_barrier =
                    [&](const std::filesystem::path&, const std::string&,
                        const std::optional<std::filesystem::path>& backup) {
                        require(backup.has_value(),
                                "replacement cleanup fixture must have a verified backup");
                        backup_residual = *backup;
                        backup_blocker = CreateFileW(
                            backup->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
                        require(backup_blocker != INVALID_HANDLE_VALUE,
                                "test should create a deletion-denying residual backup");
                        if (!typed_error)
                            throw std::runtime_error("barrier injected multi-residual failure");
                        throw StorageError(StorageErrorCode::injected_failure,
                                           "barrier injected multi-residual failure");
                    }});
        fail("locked cleanup residuals should report the original save failure");
    } catch (const StorageError& error) {
        require(error.code() == (typed_error ? StorageErrorCode::injected_failure :
                                               StorageErrorCode::io_error),
                "cleanup diagnostics must preserve the original error code");
        require(std::string_view(error.what()).find("barrier injected multi-residual failure") !=
                    std::string_view::npos &&
                    std::string_view(error.what()).find(wal_residual.string()) !=
                        std::string_view::npos &&
                    std::string_view(error.what()).find(backup_residual.string()) !=
                        std::string_view::npos,
                "cleanup diagnostic must retain the original failure and every residual path");
        require(error.residual_paths().size() == 2 &&
                    std::find(error.residual_paths().begin(), error.residual_paths().end(),
                              wal_residual) != error.residual_paths().end() &&
                    std::find(error.residual_paths().begin(), error.residual_paths().end(),
                              backup_residual) != error.residual_paths().end(),
                "cleanup error must expose every residual through its structured API");
    }
    require(wal_blocker != INVALID_HANDLE_VALUE && backup_blocker != INVALID_HANDLE_VALUE,
            "cleanup fixture should retain both deletion-denying handles");
    require(ProjectStore::file_sha256(file) == original.file_sha256,
            "multi-residual cleanup failure must preserve the original destination");
    CloseHandle(wal_blocker);
    CloseHandle(backup_blocker);
    std::error_code ignored;
    std::filesystem::remove(wal_residual, ignored);
    std::filesystem::remove(backup_residual, ignored);
    for (const auto& entry : std::filesystem::directory_iterator(temp.path)) {
        require(entry.path() == file,
                "failed multi-residual publication must not leave a staging database");
    }
#endif
}

void test_journal_stage_failure_cleans_only_exact_temporary_sidecars() {
    TempDirectory temp;
    const auto file = temp.path / "journal-failure.psketch";
    auto document = populated_document();
    const auto original = ProjectStore::save(file, document.snapshot());
    const auto unrelated = temp.path / "keep-this-journal";
    {
        std::ofstream marker(unrelated, std::ios::binary);
        marker << "user data";
    }
    require_error(
        [&] {
            (void)ProjectStore::save(
                file, document.snapshot(),
                SaveOptions{.expected_destination_sha256 = original.file_sha256,
                            .fault_stage = SaveFaultStage::after_journal_creation});
        },
        StorageErrorCode::injected_failure,
        "post-journal injected failure should escape through the save boundary");
    require(std::filesystem::exists(unrelated),
            "temporary cleanup must not remove a similarly named user file");
    require(ProjectStore::file_sha256(file) == original.file_sha256,
            "post-journal failure must preserve original project bytes");
    for (const auto& entry : std::filesystem::directory_iterator(temp.path)) {
        require(entry.path() == file || entry.path() == unrelated,
                "temporary database and exact SQLite sidecars must be removed");
    }
}

void test_save_uses_compact_staging_names_for_long_destination() {
#ifdef _WIN32
    TempDirectory temp;
    const auto& directory = temp.path;
    constexpr std::size_t target_length = 220;
    constexpr auto extension = std::string_view(".bldproj");
    require(directory.wstring().size() + 1 + extension.size() < target_length,
            "temporary root is too long for the bounded long-filename fixture");
    const auto filename_length = target_length - directory.wstring().size() - 1;
    const auto destination = directory /
        (std::string(filename_length - extension.size(), 'p') + std::string(extension));
    require(destination.wstring().size() == target_length,
            "long destination fixture must reproduce the installed-runtime path range");

    const auto document = populated_document();
    const auto receipt = ProjectStore::save(destination, document.snapshot());
    require(std::filesystem::is_regular_file(destination) &&
                ProjectStore::file_sha256(destination) == receipt.file_sha256,
            "long destination must publish the exact saved project");
    require(ProjectStore::load(destination).document.snapshot().entities() ==
                document.snapshot().entities(),
            "long destination must reopen with exact project entities");
#endif
}

void test_reopen_preserves_redo_navigation_and_named_abandoned_branch() {
    TempDirectory temp;
    const auto file = temp.path / "branch.psketch";
    auto document = populated_document();
    (void)document.undo(2);
    const auto undo_receipt = ProjectStore::save(file, document.snapshot());

    auto loaded = ProjectStore::load(file);
    require(loaded.document.can_redo(), "a saved undo state should retain redo after reopen");
    const auto redo_revision = loaded.document.redo(loaded.document.revision());
    const auto undo_again = loaded.document.undo(redo_revision);
    (void)loaded.document.apply(ApplyEntityChanges{
        .expected_revision = undo_again,
        .entity_changes = {EntityChange::upsert(
            entity("label-branch", "label", {{"text", "alternate"}}))},
    });
    const auto branch_receipt = ProjectStore::save(
        file, loaded.document.snapshot(),
        SaveOptions{.expected_destination_sha256 = undo_receipt.file_sha256});

    auto reopened_branch = ProjectStore::load(file);
    const auto branch_snapshot = reopened_branch.document.snapshot();
    require(branch_snapshot.named_revisions().at("surveyed") == 2,
            "named revision on an abandoned branch must survive reopen");
    require(branch_snapshot.history().at(2).name == "surveyed",
            "abandoned named revision record must remain in durable history");
    require(branch_receipt.revision == branch_snapshot.revision(),
            "branch replacement receipt should name the exact published revision");
}

void test_native_room_topology_is_validated_on_restore() {
    TempDirectory temp;
    const auto path = temp.path / "room.bldproj";
    const auto rectangle = [](double x, double y, double w, double h) {
        auto result = nlohmann::json::array();
        for (const auto& edge : sketch::Boundary{{{x,y},{x+w,y},0}, {{x+w,y},{x+w,y+h},0},
                 {{x+w,y+h},{x,y+h},0}, {{x,y+h},{x,y},0}})
            result.push_back({{"start", {edge.start.x, edge.start.y}},
                {"end", {edge.end.x, edge.end.y}}, {"sweep_radians", 0.0}});
        return result;
    };
    auto room = entity("persisted-room", "room", {{"boundary", rectangle(0,0,10,10)},
        {"holes", nlohmann::json::array({rectangle(1,1,2,2)})}, {"height_m", 3}, {"elevation_m", 0}});
    const auto document = Document::create({room});
    (void)ProjectStore::save(path, document.snapshot());
    require(ProjectStore::load(path).document.snapshot().entities() == document.snapshot().entities(),
        "valid room volume should reopen exactly");
    room.properties["holes"] = nlohmann::json::array({rectangle(11,1,2,2)});
    execute_sql(path, "UPDATE revision_entities SET properties_json='" + room.properties.dump() + "' WHERE id='persisted-room'");
    // A valid logical digest must not substitute for semantic validation.
    rewrite_logical_digest(path);
    require_error([&] { (void)ProjectStore::load(path); }, StorageErrorCode::integrity_failure,
        "persisted invalid room topology must fail at native project restore");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_native_room_topology_is_validated_on_restore();
        test_save_reopen_preserves_exact_revision_history_and_assets();
        test_existing_destination_requires_fingerprint_and_creates_backup();
        test_external_change_and_injected_failure_preserve_original();
        test_asset_hash_corruption_and_unsupported_format_are_rejected();
        test_saving_snapshot_r_does_not_mark_head_r_plus_one_clean();
        test_unknown_required_entity_reopens_read_only();
        test_competing_saves_with_one_fingerprint_publish_exactly_once();
        test_reopen_preserves_redo_navigation_and_named_abandoned_branch();
        test_impossible_history_is_rejected_after_digest_recomputation();
        test_translation_proof_storage_and_forgery_rejection();
        test_transform_proof_storage_and_forgery_rejection();
        test_boundary_authoring_receipt_after_v2_entity_requires_v3();
        test_unqualified_authoring_property_collisions_remain_v1_and_opaque();
        test_unknown_boundary_model_collision_requires_v2();
        test_v3_save_reopen_preserves_opaque_positive_authoring_envelope();
        test_downgraded_v3_receipt_markers_reject_before_receipt_acceptance();
        test_document_only_api_refuses_future_recovery_destinations_before_overwrite();
        test_schema_metadata_and_user_version_are_exact();
        test_load_resource_limits_precede_large_allocations();
        test_aggregate_json_byte_and_value_budgets_are_enforced();
        test_journal_stage_failure_cleans_only_exact_temporary_sidecars();
        test_save_uses_compact_staging_names_for_long_destination();
        test_validated_staging_handle_blocks_path_tampering_and_publishes_exact_bytes();
        test_destination_identity_is_rechecked_after_verified_backup();
        test_verified_backup_is_locked_through_publication();
        test_failed_checked_cleanup_reports_every_locked_residual(true);
        test_failed_checked_cleanup_reports_every_locked_residual(false);
    } catch (const std::exception& error) {
        std::cerr << "project_store_tests: unexpected exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "project_store_tests: unexpected non-standard exception\n";
        return 1;
    }
    return 0;
}
