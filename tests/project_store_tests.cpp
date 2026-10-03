#include "sketch/project_store.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/redraw_angle_fixture.hpp"

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
#include <cmath>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
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

template <typename Function>
void require_document_error(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const sketch::DocumentError&) {
        return;
    }
    fail(message);
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("vertex-tests-" + sketch::make_stable_id());

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
    const auto saved = metadata_value(database, "saved_revision");
    nlohmann::json manifest = {
        {"format_version", format},
        {"document_id", metadata_value(database, "document_id")},
        {"head_revision", std::stoull(metadata_value(database, "head_revision"))},
        {"saved_revision", saved == "null" ? nlohmann::json(nullptr) : nlohmann::json(std::stoull(saved))},
        {"history", nlohmann::json::array()},
        {"named_revisions", nlohmann::json::object()},
    };

    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(
                database,
                format >= 18
                    ? "SELECT revision,parent_revision,source_revision,action,name,undo_stack_json,"
                      "redo_stack_json,boundary_translation_json,boundary_transform_json,boundary_edit_json,boundary_constraint_changes_json,boundary_translations_json,boundary_transforms_json FROM revisions ORDER BY revision"
                    : format >= 9
                    ? "SELECT revision,parent_revision,source_revision,action,name,undo_stack_json,"
                      "redo_stack_json,boundary_translation_json,boundary_transform_json,boundary_edit_json,boundary_constraint_changes_json,boundary_translations_json FROM revisions ORDER BY revision"
                    : format >= 8
                    ? "SELECT revision,parent_revision,source_revision,action,name,undo_stack_json,"
                      "redo_stack_json,boundary_translation_json,boundary_transform_json,boundary_edit_json,boundary_constraint_changes_json FROM revisions ORDER BY revision"
                    : format >= 7
                    ? "SELECT revision,parent_revision,source_revision,action,name,undo_stack_json,"
                      "redo_stack_json,boundary_translation_json,boundary_transform_json,boundary_edit_json FROM revisions ORDER BY revision"
                    : format >= 6
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
        if (format >= 7 && sqlite3_column_type(statement, 9) != SQLITE_NULL)
            manifest["history"].back()["boundary_geometry_edit"] =
                nlohmann::json::parse(sqlite_text(statement, 9));
        if (format >= 8 && sqlite3_column_type(statement, 10) != SQLITE_NULL)
            manifest["history"].back()["boundary_constraint_changes"] =
                nlohmann::json::parse(sqlite_text(statement, 10));
        if (format >= 9 && sqlite3_column_type(statement, 11) != SQLITE_NULL)
            manifest["history"].back()["boundary_translations"] =
                nlohmann::json::parse(sqlite_text(statement, 11));
        if (format >= 18 && sqlite3_column_type(statement,12)!=SQLITE_NULL)
            manifest["history"].back()["boundary_transforms"]=nlohmann::json::parse(sqlite_text(statement,12));
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

    require(sqlite3_prepare_v2(database,
        "SELECT count(*) FROM sqlite_schema WHERE name='project_recovery_records'",-1,&statement,nullptr)==SQLITE_OK,
        "test should detect archive recovery table");
    require(sqlite3_step(statement)==SQLITE_ROW,"test archive query should return a row");
    const bool archive=sqlite3_column_int(statement,0)!=0;
    sqlite3_finalize(statement);
    if(archive) {
        manifest["recovery_records"]=nlohmann::json::array();
        require(sqlite3_prepare_v2(database,
            "SELECT record_id,record_kind,envelope_json FROM project_recovery_records ORDER BY record_id",
            -1,&statement,nullptr)==SQLITE_OK,"test should read recovery records for digest");
        while(sqlite3_step(statement)==SQLITE_ROW)
            manifest["recovery_records"].push_back({{"record_id",sqlite_text(statement,0)},
                {"record_kind",sqlite_text(statement,1)},{"envelope",nlohmann::json::parse(sqlite_text(statement,2))}});
        sqlite3_finalize(statement);
    }
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

void test_typed_chord_nested_proofs_reject_recomputed_downgrade() {
    const auto legacy=translation_fixture();
    auto record=*sketch::decode_boundary_receipt_envelope(legacy.properties.at("boundary_authoring")).record;
    record.schema_version=sketch::boundary_receipt_schema_version_v4;
    auto& first=record.edges.front().receipt;
    first.kind=sketch::BoundaryConstructionKind::arc_chord_angle;
    first.chord_end.reset();
    first.chord_input=sketch::ChordInput{sketch::parse_quantity("2 m"),sketch::parse_angle("0 deg")};
    first.angle=sketch::parse_angle("90 deg");
    auto geometry=sketch::decode_identified_boundary_entity(legacy);
    const auto replay=sketch::replay_boundary_construction(record);
    for(std::size_t i=0;i<geometry.segments.size();++i) geometry.segments[i].segment=replay.edges[i].segment;
    auto typed=sketch::encode_identified_boundary_entity(geometry);
    typed.properties["boundary_authoring"]=sketch::encode_boundary_receipt_envelope(record);
    for(int proof=0;proof<3;++proof) {
        Entity owner=typed;
        if(proof==1) {
            auto edited=Document::create({typed});
            sketch::BoundaryGeometryEdit edit; edit.boundary_id=typed.id;
            edit.kind=sketch::BoundaryGeometryEditKind::move_vertex; edit.target_id="vertex-1"; edit.target_position={3,0};
            edited.apply(sketch::EditBoundaryGeometry{0,edit}); owner=edited.snapshot().entities().at(typed.id);
            require(!owner.properties.contains("boundary_authoring"),"downgrade fixture actually archives typed source");
        } else if(proof==2) {
            auto edited=Document::create({legacy});
            sketch::BoundaryGeometryEdit edit; edit.boundary_id=edit.target_id=typed.id;
            edit.kind=sketch::BoundaryGeometryEditKind::redefine_boundary;
            edit.replacement_segments=typed.properties.at("segments"); edit.replacement_authoring=typed.properties.at("boundary_authoring");
            edited.apply(sketch::EditBoundaryGeometry{0,edit}); owner=edited.snapshot().entities().at(typed.id);
            require(!owner.properties.contains("boundary_authoring") && owner.extensions.at("boundary_geometry_derivation").at("source_boundary_authoring").at("version")==2,"downgrade fixture has only nested typed replacement");
        }
        auto imported=Document::create({owner}); require(imported.snapshot().history().size()==1 && ProjectStore::required_format_version(imported.snapshot())==31,"fresh nested proof requires31 with no earlier typed history");
        TempDirectory temporary; const auto file=temporary.path/"typed-chord.bldproj";
        (void)ProjectStore::save(file,imported.snapshot());
        require(ProjectStore::load(file).document.snapshot().entities()==imported.snapshot().entities(),"known typed proof reopens before downgrade");
        execute_sql(file,"PRAGMA user_version=30; UPDATE metadata SET value='30' WHERE key='format_version'");
        rewrite_logical_digest(file);
        require_error([&] { (void)ProjectStore::load(file); },StorageErrorCode::unsupported_format,"recomputed digest cannot downgrade direct or nested typed chord proofs");
    }
}

void test_translation_group_storage_and_forgery_rejection() {
    TempDirectory temp;
    const auto file = temp.path / "translation-group-v9.psketch";
    auto first = translation_fixture();
    first.extensions["vendor"] = {{"retain", "first"}};
    auto second = first;
    auto geometry = sketch::decode_identified_boundary_entity(second);
    geometry.id = "second-boundary";
    second.id = geometry.id;
    second = sketch::encode_identified_boundary_entity(geometry, &second);
    auto receipt = sketch::decode_boundary_receipt_envelope(first.properties.at("boundary_authoring"));
    receipt.record->boundary_id = "second-boundary";
    second.properties["boundary_authoring"] = sketch::encode_boundary_receipt_envelope(*receipt.record);
    const auto dimension = sketch::encode_boundary_dimension_entity(
        sketch::BoundaryDimension{"dimension", first.id, "edge-0", {1, -0.5}});
    auto label = entity("group-label", "label", {{"text", "before"}}, false, {{"vendor", "retain"}});
    auto document = Document::create({first, second, dimension, label});
    const auto original = document.snapshot().entities();
    require(ProjectStore::required_format_version(document.snapshot()) == 3,
            "receipt history without batch must retain existing format");
    label.properties["text"] = "after";
    sketch::TranslateBoundaries command{0, {{first.id, {8, -4}}, {second.id, {8, -4}}},
        {sketch::EntityChange::upsert(label)}, "Move measured selection"};
    document.apply(command);
    const auto moved = document.snapshot().entities();
    document.undo(document.revision());
    require(ProjectStore::required_format_version(document.snapshot()) == 9,
            "undone group proof must require v9");
    (void)ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    const auto reopened = loaded.document.snapshot();
    require(reopened.entities() == original && loaded.document.can_redo() &&
            sketch::command_to_json(*reopened.history().at(1).boundary_translations) == sketch::command_to_json(command),
            "v9 storage must retain exact supplemental changes, proof and navigation");
    require(Document::fork_at_revision(reopened, 1).snapshot().entities() == moved,
            "v9 historical fork must replay exact measured selection");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == moved, "v9 redo must restore exact entities");
    loaded.document.undo(loaded.document.revision());
    command.expected_revision = loaded.document.revision();
    loaded.document.apply(command);
    const auto branch_file = temp.path / "batch-after-undo.psketch";
    (void)ProjectStore::save(branch_file, loaded.document.snapshot());
    require(ProjectStore::load(branch_file).document.snapshot().entities() == moved,
            "batch after undo must survive save and exact replay");
    const auto wire = sketch::command_to_json(*reopened.history().at(1).boundary_translations);
    for (int mutation = 0; mutation < 5; ++mutation) {
        const auto tampered = temp.path / ("batch-forged-" + std::to_string(mutation) + ".psketch");
        std::filesystem::copy_file(file, tampered);
        auto proof = wire;
        if (mutation == 1) proof["translations"][1]["offset"]["x"] = 9;
        if (mutation == 2) proof["entity_changes"] = nlohmann::json::array();
        if (mutation == 3) proof["expected_revision"] = 1;
        if (mutation == 4) proof["unexpected"] = true;
        execute_sql(tampered, "UPDATE revisions SET boundary_translations_json=" +
            (mutation == 0 ? std::string("NULL") : "'" + proof.dump() + "'") + " WHERE revision=1");
        rewrite_logical_digest(tampered);
        require_error([&] { (void)ProjectStore::load(tampered); }, StorageErrorCode::integrity_failure,
                      "recomputed digest must not admit missing, forged or malformed group proof");
    }
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

void test_boundary_geometry_edit_proof_storage_and_forgery_rejection() {
    TempDirectory temp;
    const auto file = temp.path / "boundary-edit-v7.psketch";
    auto document = Document::create({translation_fixture()});
    sketch::BoundaryGeometryEdit edit;
    edit.boundary_id = "translated-boundary";
    edit.kind = sketch::BoundaryGeometryEditKind::move_vertex;
    edit.target_id = "vertex-1";
    edit.target_position = {3.0, 0.25};
    document.apply(sketch::EditBoundaryGeometry{document.revision(), edit});
    const auto first_edited = document.snapshot().entities();

    auto stripped = first_edited.at("translated-boundary");
    stripped.extensions.erase("boundary_geometry_derivation");
    require_document_error([&] {
        (void)document.apply(ApplyEntityChanges{document.revision(),
            {EntityChange::upsert(stripped)}, {}, "strip geometry derivation"});
    }, "raw edits must not strip geometry derivation evidence");
    auto replaced = first_edited.at("translated-boundary");
    replaced.extensions.at("boundary_geometry_derivation").at("operations")[0]
        .at("value").at("position")[0] = 9.0;
    require_document_error([&] {
        (void)document.apply(ApplyEntityChanges{document.revision(),
            {EntityChange::upsert(replaced)}, {}, "replace geometry derivation"});
    }, "raw edits must not replace geometry derivation evidence");

    sketch::PlanarTransform transform;
    transform.pivot = {0.5, 0.5};
    transform.rotation_radians = 0.2;
    transform.offset = {2.0, -1.0};
    document.apply(sketch::TransformBoundary{document.revision(),
        {"translated-boundary", transform}});
    const auto transformed = document.snapshot().entities();
    const auto transformed_boundary = sketch::decode_identified_boundary_entity(
        transformed.at("translated-boundary"));
    auto second_edit = edit;
    second_edit.target_id = "vertex-2";
    second_edit.target_position = transformed_boundary.segments[2].segment.start;
    second_edit.target_position.x += 0.2;
    second_edit.target_position.y += 0.1;
    document.apply(sketch::EditBoundaryGeometry{document.revision(), second_edit});
    const auto final_entities = document.snapshot().entities();
    document.undo(document.revision());
    require(ProjectStore::required_format_version(document.snapshot()) == 7,
            "edit followed by transform with a retained redo must still require v7");

    (void)ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    const auto reopened = loaded.document.snapshot();
    require(reopened.entities() == transformed && loaded.document.can_redo() &&
                reopened.history().at(1).boundary_geometry_edit == edit,
            "v7 reopen must preserve mixed edit/transform history and its retained redo");
    const auto& entity = reopened.entities().at("translated-boundary");
    require(!entity.properties.contains("boundary_authoring") &&
                entity.extensions.at("boundary_geometry_derivation")
                    .at("source_boundary_authoring").is_object() &&
                entity.extensions.at("boundary_geometry_derivation")
                    .at("operations").size() == 2,
            "v7 direct editing and transforms must retain ordered derivation evidence");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == final_entities,
            "v7 redo must reproduce a geometry edit made after a transform");

    const auto missing = temp.path / "missing-boundary-edit-proof.psketch";
    std::filesystem::copy_file(file, missing);
    execute_sql(missing, "UPDATE revisions SET boundary_edit_json=NULL WHERE revision=1");
    rewrite_logical_digest(missing);
    require_error([&] { (void)ProjectStore::load(missing); },
                  StorageErrorCode::integrity_failure,
                  "a recomputed digest must not authorize missing boundary edit proof");

    const auto forged = temp.path / "forged-boundary-edit-proof.psketch";
    std::filesystem::copy_file(file, forged);
    execute_sql(forged,
        "UPDATE revisions SET boundary_edit_json='"
        "{\"version\":1,\"kind\":\"move_vertex\",\"boundary_id\":\"translated-boundary\","
        "\"vertex_id\":\"vertex-1\",\"position\":[4.0,0.25]}' WHERE revision=1");
    rewrite_logical_digest(forged);
    require_error([&] { (void)ProjectStore::load(forged); },
                  StorageErrorCode::integrity_failure,
                  "a recomputed digest must not authorize forged boundary edit intent");

    const auto downgraded = temp.path / "downgraded-boundary-edit.psketch";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "ALTER TABLE revisions DROP COLUMN boundary_edit_json; "
        "PRAGMA user_version=6; UPDATE metadata SET value='6' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); },
                  StorageErrorCode::unsupported_format,
                  "downgraded boundary edit history must fail its minimum format guard");
}

void test_boundary_constraint_proof_storage() {
    TempDirectory temp;
    const auto file = temp.path / "boundary-constraint-v8.psketch";
    sketch::PersistentConstraint relation;
    relation.id = "length";
    relation.relation = sketch::ConstraintRelationKind::fixed_length;
    relation.bindings = {{"translated-boundary", sketch::WallEndpointRole::start, "edge-0", "vertex-0"},
                         {"translated-boundary", sketch::WallEndpointRole::end, "edge-0", "vertex-1"}};
    relation.length = sketch::parse_quantity("2 m");
    auto document = Document::create({translation_fixture(), sketch::encode_constraint_entity(relation)});
    const auto initial = document.snapshot().entities();
    relation.length = sketch::parse_quantity("3 m");
    sketch::ApplyBoundaryConstraintChanges command{0,
        {{"translated-boundary", sketch::BoundaryGeometryEditKind::move_vertex,
          "vertex-0", {5.0, 0.0}},
         {"translated-boundary", sketch::BoundaryGeometryEditKind::move_vertex,
          "vertex-1", {8.0, 0.0}},
         {"translated-boundary", sketch::BoundaryGeometryEditKind::move_vertex,
          "vertex-2", {8.0, 1.0}},
         {"translated-boundary", sketch::BoundaryGeometryEditKind::move_vertex,
          "vertex-3", {5.0, 1.0}}},
        {EntityChange::upsert(sketch::encode_constraint_entity(relation))}, "constraint transaction"};
    document.apply(command);
    const auto edited = document.snapshot().entities();
    document.undo(document.revision());
    require(ProjectStore::required_format_version(document.snapshot()) == 8,
            "undone constraint proof must require v8");
    (void)ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    const auto proof = loaded.document.snapshot().history().at(1).boundary_constraint_changes;
    require(proof && sketch::command_to_json(*proof) == sketch::command_to_json(command),
            "constraint proof must survive reopen exactly");
    require(loaded.document.snapshot().entities() == initial && loaded.document.can_redo(),
            "reopen must preserve undone transaction");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == edited, "redo after reopen must replay transaction");
    loaded.document.undo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == initial, "undo after reopen must restore source");
    const auto copy = temp.path / "boundary-constraint-copy.psketch";
    (void)ProjectStore::save(copy, loaded.document.snapshot());
    require(ProjectStore::load(copy).document.can_redo(), "resaved navigation must survive reopen");

    sketch::ProjectWorkspace workspace(document.snapshot());
    const auto workspace_snapshot = workspace.capture();
    const auto history = sketch::capture_workspace_history_record(workspace_snapshot);
    sketch::RecoveryLedger ledger{{"history", "workspace_history",
        sketch::encode_workspace_history_record(workspace_snapshot.document(), history, std::nullopt)}};
    const auto archive_file = temp.path / "constraint-archive.psketch";
    (void)ProjectStore::save_archive(archive_file,
        {workspace_snapshot.document(), ledger, sketch::ArchiveRole::ordinary});
    const auto archive = ProjectStore::load_archive(archive_file, sketch::ArchiveRole::ordinary);
    require(archive.supported() && archive.archive->document().history().at(1).boundary_constraint_changes &&
                sketch::command_to_json(*archive.archive->document().history().at(1).boundary_constraint_changes) ==
                    sketch::command_to_json(command),
            "recovery archive must preserve the proof and its history binding");
    require_error([&] { (void)ProjectStore::load(archive_file); }, StorageErrorCode::unsupported_format,
                  "document-only load must not discard the v8 recovery ledger");

    const auto missing = temp.path / "missing-constraint.psketch";
    std::filesystem::copy_file(file, missing);
    execute_sql(missing, "UPDATE revisions SET boundary_constraint_changes_json=NULL WHERE revision=1");
    rewrite_logical_digest(missing);
    require_error([&] { (void)ProjectStore::load(missing); }, StorageErrorCode::integrity_failure,
                  "recomputed digest cannot authorize missing constraint proof");
    const auto forged = temp.path / "forged-constraint.psketch";
    std::filesystem::copy_file(file, forged);
    execute_sql(forged, "UPDATE revisions SET boundary_constraint_changes_json="
        "json_set(boundary_constraint_changes_json,'$.boundary_edits[0].position[0]',4.0) WHERE revision=1");
    rewrite_logical_digest(forged);
    require_error([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
                  "recomputed digest cannot authorize forged constraint proof");
    const auto wrong_kind = temp.path / "wrong-constraint-kind.psketch";
    std::filesystem::copy_file(file, wrong_kind);
    execute_sql(wrong_kind, "UPDATE revisions SET boundary_constraint_changes_json="
        "json_set(boundary_constraint_changes_json,'$.kind','apply_entity_changes') WHERE revision=1");
    require_error([&] { (void)ProjectStore::load(wrong_kind); }, StorageErrorCode::integrity_failure,
                  "constraint column must reject other command kinds");
}

Entity curved_constraint_wall() {
    return entity("curve-wall", "wall", {
        {"baseline", {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}}, {"sweep_radians", 0.4}}},
        {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0}});
}

sketch::PersistentConstraint horizontal_curve_constraint() {
    sketch::PersistentConstraint relation;
    relation.id = "curve-horizontal";
    relation.relation = sketch::ConstraintRelationKind::horizontal;
    relation.bindings = {{"curve-wall", sketch::WallEndpointRole::start},
                         {"curve-wall", sketch::WallEndpointRole::end}};
    return relation;
}

void test_physical_arc_length_history_requires_v12() {
    TempDirectory temp;
    auto wall = curved_constraint_wall();
    const auto pi = std::numbers::pi;
    wall.properties["baseline"] = {{"start", {0.0, 0.0}},
        {"end", {8.0 / pi, 0.0}}, {"sweep_radians", pi}};
    sketch::PersistentConstraint relation;
    relation.id = "physical-curve-length";
    relation.relation = sketch::ConstraintRelationKind::fixed_arc_length;
    relation.bindings = {{wall.id, sketch::WallEndpointRole::start},
                         {wall.id, sketch::WallEndpointRole::end}};
    relation.length = sketch::parse_quantity("4 m");
    const auto encoded = sketch::encode_constraint_entity(relation);
    require(encoded.properties.at("version") == 3, "physical arc length must use entity version three");
    auto document = Document::create({wall, encoded});
    const auto initial = document.snapshot().entities();
    require(ProjectStore::required_format_version(document.snapshot()) == 12,
            "physical arc-length state must require v12 without an edit proof");
    relation.length = sketch::parse_quantity("6 m");
    sketch::ApplyBoundaryConstraintChanges command{0, {},
        {EntityChange::upsert(sketch::encode_constraint_entity(relation))}, "Edit curve length"};
    command.wall_edits.push_back({wall.id, {{0, 0}, {12.0 / pi, 0}, pi}, std::nullopt, 2});
    document.apply(command);
    const auto edited = document.snapshot().entities();
    document.undo(document.revision());
    const auto file = temp.path / "physical-curve-length-v12.bldproj";
    (void)ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    require(loaded.document.snapshot().entities() == initial && loaded.document.can_redo(),
            "physical arc-length undo and redo navigation must reopen exactly");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == edited,
            "reopened physical arc-length command must replay exact geometry and quantity");
    const auto decoded = sketch::decode_constraint_entity(loaded.document.snapshot().entities().at(relation.id));
    require(decoded.version == 3 && decoded.constraint->length->original_expression == "6 m",
            "physical target and its exact entry must survive reopening");
    loaded.document.apply(ApplyEntityChanges{loaded.document.revision(),
        {EntityChange::erase(relation.id), EntityChange::erase(wall.id)}, {}, "Delete curve"});
    // A later straight-only command must not lower the retained v12 reader floor.
    auto straight = curved_constraint_wall();
    straight.id = "later-straight";
    straight.properties["baseline"]["sweep_radians"] = 0;
    loaded.document.apply(ApplyEntityChanges{loaded.document.revision(),
        {EntityChange::upsert(straight)}, {}, "Add straight wall"});
    sketch::ApplyBoundaryConstraintChanges later{loaded.document.revision(), {}, {}, "Edit straight wall"};
    later.wall_edits.push_back({straight.id, {{0, 0}, {5, 0}, 0}, std::nullopt});
    loaded.document.apply(later);
    require(ProjectStore::required_format_version(loaded.document.snapshot()) == 12,
            "deleted arc constraint and later straight proof must retain v12");
    const auto deleted_file = temp.path / "deleted-physical-curve-length.bldproj";
    (void)ProjectStore::save(deleted_file, loaded.document.snapshot());
    require(ProjectStore::load(deleted_file).document.snapshot().entities() == loaded.document.snapshot().entities(),
            "retained physical curve history must remain readable after deleting its owner");
    const auto source_hash = ProjectStore::file_sha256(file);
    sketch::ProjectWorkspace workspace(document.snapshot());
    const auto workspace_snapshot = workspace.capture();
    const auto history = sketch::capture_workspace_history_record(workspace_snapshot);
    sketch::RecoveryLedger ledger{{"history", "workspace_history",
        sketch::encode_workspace_history_record(workspace_snapshot.document(), history, std::nullopt)}};
    const auto archive_file = temp.path / "physical-curve-workspace-archive.bldproj";
    (void)ProjectStore::save_archive(archive_file,
        {workspace_snapshot.document(), ledger, sketch::ArchiveRole::ordinary});
    const auto archive = ProjectStore::load_archive(archive_file, sketch::ArchiveRole::ordinary);
    require(archive.supported() && archive.archive->document().entities() == initial &&
        ProjectStore::required_format_version(archive.archive->document()) == 12,
        "workspace archive must preserve physical curve state, reader floor and recovery data");
    require_error([&] { (void)ProjectStore::load(archive_file); }, StorageErrorCode::unsupported_format,
        "document-only load must not discard v12 workspace recovery data");
    if (const auto* capture = std::getenv("VERTEX_PHYSICAL_CURVE_CAPTURE"))
        std::filesystem::copy_file(archive_file, std::filesystem::path(capture));
    const auto downgraded = temp.path / "downgraded-physical-curve-length.bldproj";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "PRAGMA user_version=11; UPDATE metadata SET value='11' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    const auto downgraded_hash = ProjectStore::file_sha256(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::unsupported_format,
                  "recomputed digest cannot downgrade physical arc-length semantics to v11");
    require(ProjectStore::file_sha256(file) == source_hash && ProjectStore::file_sha256(downgraded) == downgraded_hash,
            "failed physical curve migration must preserve both original files");
    const auto forged = temp.path / "chord-masquerading-as-physical-length.bldproj";
    std::filesystem::copy_file(file, forged);
    // The receipt still says six metres of arc. A six-metre chord does not
    // satisfy it, even when the saved wall and typed proof agree exactly.
    execute_sql(forged, "UPDATE revisions SET boundary_constraint_changes_json="
        "json_set(boundary_constraint_changes_json,'$.wall_edits[0].baseline.end[0]',6.0) WHERE revision=1; "
        "UPDATE revision_entities SET properties_json=json_set(properties_json,'$.baseline.end[0]',6.0) "
        "WHERE revision=1 AND id='curve-wall'");
    rewrite_logical_digest(forged);
    const auto forged_hash = ProjectStore::file_sha256(forged);
    require_error([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
                  "matching proof, geometry and recomputed digest cannot substitute chord distance for arc length");
    require(ProjectStore::file_sha256(forged) == forged_hash && ProjectStore::file_sha256(file) == source_hash,
            "refusing forged physical measurement must preserve saved artifacts");
}

void test_direct_curve_length_history_requires_v13() {
    TempDirectory temp;
    const auto pi = std::numbers::pi;
    auto wall = curved_constraint_wall();
    wall.properties["baseline"] = {{"start", {0,0}}, {"end", {8/pi,0}}, {"sweep_radians", pi}};
    auto document = Document::create({wall});
    const auto initial = document.snapshot().entities();
    sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "Direct curve length"};
    command.wall_edits.push_back({wall.id, {{0,0},{12/pi,0},pi}, sketch::parse_quantity("6 m"), 3});
    document.apply(command);
    const auto edited = document.snapshot().entities();
    require(ProjectStore::required_format_version(document.snapshot()) == 13,
        "direct physical arc length must require v13");
    document.undo(document.revision());
    const auto file = temp.path / "direct-curve-v13.bldproj";
    (void)ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    require(loaded.document.snapshot().entities() == initial && loaded.document.can_redo(),
        "undone direct arc input must retain exact redo navigation");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == edited,
        "direct physical input must reopen and redo exactly");
    const auto imported = Document::create({edited.at(wall.id)});
    require(ProjectStore::required_format_version(imported.snapshot()) == 13,
        "known physical input receipt must require v13 without original command history");
    const auto imported_file = temp.path / "imported-direct-curve.bldproj";
    (void)ProjectStore::save(imported_file, imported.snapshot());
    require(ProjectStore::load(imported_file).document.snapshot().entities() == imported.snapshot().entities(),
        "imported physical input must reopen exactly");
    auto opaque = edited.at(wall.id); opaque.type = "vendor_wall";
    const auto vendor = Document::create({opaque});
    require(ProjectStore::required_format_version(vendor.snapshot()) == 1,
        "generic vendor collision must not acquire physical input semantics");
    auto future = edited.at(wall.id);
    future.extensions["constraint_authoring"]["last_length_entry"]["version"] = 99;
    require(ProjectStore::required_format_version(Document::create({future}).snapshot()) == 1,
        "unknown optional receipt must not be claimed as understood physical input");
    loaded.document.apply(ApplyEntityChanges{loaded.document.revision(),
        {EntityChange::erase(wall.id)}, {}, "Delete curve"});
    auto straight = curved_constraint_wall(); straight.id = "later-straight";
    straight.properties["baseline"]["sweep_radians"] = 0;
    loaded.document.apply(ApplyEntityChanges{loaded.document.revision(), {EntityChange::upsert(straight)}, {}, "Add straight"});
    sketch::ApplyBoundaryConstraintChanges later{loaded.document.revision(), {}, {}, "Later straight edit"};
    later.wall_edits.push_back({straight.id, {{0,0},{5,0},0}, std::nullopt});
    loaded.document.apply(later);
    require(ProjectStore::required_format_version(loaded.document.snapshot()) == 13,
        "deleted, undone, and mixed later proofs must not lower physical input reader floor");
    const auto mixed_file = temp.path / "retained-direct-curve.bldproj";
    (void)ProjectStore::save(mixed_file, loaded.document.snapshot());
    require(ProjectStore::load(mixed_file).document.snapshot().entities() == loaded.document.snapshot().entities(),
        "retained physical input and later straight history must reopen");
    sketch::ProjectWorkspace workspace(imported.snapshot());
    const auto capture = workspace.capture();
    const auto history = sketch::capture_workspace_history_record(capture);
    sketch::RecoveryLedger ledger{{"history", "workspace_history",
        sketch::encode_workspace_history_record(capture.document(), history, std::nullopt)}};
    const auto archive_file = temp.path / "direct-curve-workspace.bldproj";
    (void)ProjectStore::save_archive(archive_file, {capture.document(), ledger, sketch::ArchiveRole::ordinary});
    const auto archive = ProjectStore::load_archive(archive_file, sketch::ArchiveRole::ordinary);
    require(archive.supported() && archive.archive->document().entities() == imported.snapshot().entities() &&
        ProjectStore::required_format_version(archive.archive->document()) == 13,
        "workspace archive must retain direct curve input, v13 floor and recovery ledger");
    require_error([&] { (void)ProjectStore::load(archive_file); }, StorageErrorCode::unsupported_format,
        "document-only load must not discard a v13 recovery ledger");
    if (const auto* path = std::getenv("VERTEX_DIRECT_CURVE_CAPTURE"))
        std::filesystem::copy_file(archive_file, std::filesystem::path(path));
    const auto source_hash = ProjectStore::file_sha256(file);
    const auto downgraded = temp.path / "direct-curve-downgraded.bldproj";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "PRAGMA user_version=12; UPDATE metadata SET value='12' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    const auto downgrade_hash = ProjectStore::file_sha256(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::unsupported_format,
        "recomputed digest must not downgrade retained physical input to v12");
    require(ProjectStore::file_sha256(downgraded) == downgrade_hash && ProjectStore::file_sha256(file) == source_hash,
        "refused physical input downgrade must preserve both files");
    const auto forged = temp.path / "direct-chord-forgery.bldproj";
    std::filesystem::copy_file(file, forged);
    execute_sql(forged, "UPDATE revisions SET boundary_constraint_changes_json="
        "json_set(boundary_constraint_changes_json,'$.wall_edits[0].baseline.end[0]',6.0) WHERE revision=1; "
        "UPDATE revision_entities SET properties_json=json_set(properties_json,'$.baseline.end[0]',6.0), "
        "extensions_json=json_set(extensions_json,'$.constraint_authoring.last_length_entry.baseline.end[0]',6.0) "
        "WHERE revision=1 AND id='curve-wall'");
    rewrite_logical_digest(forged);
    const auto forged_hash = ProjectStore::file_sha256(forged);
    require_error([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
        "matching proof, wall and receipt cannot substitute chord distance for physical arc length");
    require(ProjectStore::file_sha256(forged) == forged_hash, "refused physical input forgery must preserve saved bytes");
}

void test_curved_constraint_state_requires_v10_without_geometry_proof() {
    TempDirectory temp;
    const auto wall = curved_constraint_wall();
    const auto relation = sketch::encode_constraint_entity(horizontal_curve_constraint());
    auto document = Document::create({wall, relation});
    require(ProjectStore::required_format_version(document.snapshot()) == 10,
            "already satisfied curved wall relation without an edit proof must require v10");
    const auto head_file = temp.path / "curved-state.psketch";
    (void)ProjectStore::save(head_file, document.snapshot());
    require(ProjectStore::load(head_file).document.snapshot().entities() == document.snapshot().entities(),
            "curved relation state must reopen exactly without a geometry proof");

    auto retained = Document::create({wall});
    retained.apply(ApplyEntityChanges{0, {EntityChange::upsert(relation)}, {}, "already satisfied curve relation"});
    retained.undo(retained.revision());
    require(!retained.snapshot().entities().contains(relation.id) &&
                ProjectStore::required_format_version(retained.snapshot()) == 10,
            "undone curved relation state must retain the v10 reader floor");
    const auto file = temp.path / "curved-state-undone.psketch";
    (void)ProjectStore::save(file, retained.snapshot());
    auto loaded = ProjectStore::load(file);
    require(loaded.document.can_redo(), "undone curved relation must preserve redo on reopen");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities().at(relation.id) == relation,
            "reopened already satisfied curved relation must redo exactly");
    const auto downgraded = temp.path / "curved-state-downgraded.psketch";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "PRAGMA user_version=9; UPDATE metadata SET value='9' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::unsupported_format,
                  "recomputed digest cannot downgrade an undone curved relation state");
}

void test_curved_constraint_floor_uses_actual_bound_segment() {
    TempDirectory temp;
    auto owner = translation_fixture();
    owner.properties.erase("boundary_authoring");
    auto geometry = sketch::decode_identified_boundary_entity(owner);
    geometry.segments.front().segment.sweep_radians = 0.2;
    owner = sketch::encode_identified_boundary_entity(geometry, &owner);
    sketch::PersistentConstraint relation;
    relation.id = "edge-relation";
    relation.relation = sketch::ConstraintRelationKind::vertical;
    relation.bindings = {{owner.id, sketch::WallEndpointRole::start, "edge-1", "vertex-1"},
                         {owner.id, sketch::WallEndpointRole::end, "edge-1", "vertex-2"}};
    auto straight = Document::create({owner, sketch::encode_constraint_entity(relation)});
    require(ProjectStore::required_format_version(straight.snapshot()) == 2,
            "straight bound edge on a curved boundary must retain its historical floor");
    const auto legacy_file = temp.path / "straight-edge-curved-owner.psketch";
    (void)ProjectStore::save(legacy_file, straight.snapshot());
    require(ProjectStore::load(legacy_file).document.snapshot().entities() == straight.snapshot().entities(),
            "legacy straight edge relation on curved owner must still reopen");
    relation.relation = sketch::ConstraintRelationKind::horizontal;
    relation.bindings = {{owner.id, sketch::WallEndpointRole::start, "edge-0", "vertex-0"},
                         {owner.id, sketch::WallEndpointRole::end, "edge-0", "vertex-1"}};
    auto curved = Document::create({owner, sketch::encode_constraint_entity(relation)});
    require(ProjectStore::required_format_version(curved.snapshot()) == 10,
            "an already satisfied relation on the curved bound edge must require v10");
    const auto curve_file = temp.path / "curved-edge-relation.psketch";
    (void)ProjectStore::save(curve_file, curved.snapshot());
    require(ProjectStore::load(curve_file).document.snapshot().entities() == curved.snapshot().entities(),
            "curved boundary edge relation must reopen exactly");
}

void test_curved_wall_proof_storage_and_recovery_overwrite_guard() {
    TempDirectory temp;
    auto document = Document::create({curved_constraint_wall()});
    const auto initial = document.snapshot().entities();
    sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "curve chord edit"};
    command.wall_edits.push_back({"curve-wall", {{0, 0}, {5, 0}, 0.4}, std::nullopt, 2});
    document.apply(command);
    const auto edited = document.snapshot().entities();
    require(ProjectStore::required_format_version(document.snapshot()) == 10,
            "curved wall proof at head must require v10 even without relations");
    document.undo(document.revision());
    require(ProjectStore::required_format_version(document.snapshot()) == 10,
            "undone curved wall proof must retain v10");
    const auto file = temp.path / "curve-proof.psketch";
    (void)ProjectStore::save(file, document.snapshot());
    auto loaded = ProjectStore::load(file);
    const auto proof = sketch::command_to_json(*loaded.document.snapshot().history()[1].boundary_constraint_changes);
    require(proof.at("version") == 3 && proof.at("wall_edits")[0].at("version") == 2,
            "reopen must retain the explicit curved wall proof and command versions");
    require(loaded.document.snapshot().entities() == initial && loaded.document.can_redo(),
            "undone curved wall geometry must reopen with redo");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == edited,
            "reopened curved wall proof must redo exact geometry and receipts");
    const auto downgraded = temp.path / "curve-proof-downgraded.psketch";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "PRAGMA user_version=9; UPDATE metadata SET value='9' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::unsupported_format,
                  "recomputed digest cannot downgrade retained curved wall proof history");

    sketch::ProjectWorkspace workspace(document.snapshot());
    const auto workspace_snapshot = workspace.capture();
    const auto history = sketch::capture_workspace_history_record(workspace_snapshot);
    sketch::RecoveryLedger ledger{{"history", "workspace_history",
        sketch::encode_workspace_history_record(workspace_snapshot.document(), history, std::nullopt)}};
    const auto archive_file = temp.path / "curve-archive.psketch";
    const auto receipt = ProjectStore::save_archive(archive_file,
        {workspace_snapshot.document(), ledger, sketch::ArchiveRole::ordinary});
    require(ProjectStore::load_archive(archive_file, sketch::ArchiveRole::ordinary).supported(),
            "v10 recovery archive must reopen through recovery-aware API");
    require_error([&] { (void)ProjectStore::load(archive_file); }, StorageErrorCode::unsupported_format,
                  "document-only load must preserve the v10 recovery boundary");
    const auto before = ProjectStore::file_sha256(archive_file);
    require_error([&] { (void)ProjectStore::save(archive_file, document.snapshot(),
                      SaveOptions{receipt.file_sha256}); }, StorageErrorCode::unsupported_format,
                  "document-only save with matching fingerprint must not replace a v10 recovery archive");
    require(ProjectStore::file_sha256(archive_file) == before,
            "rejected document-only overwrite must preserve v10 archive bytes");
}

void test_consistent_curved_wall_history_forgery_rejects_new_crossing() {
    TempDirectory temp;
    const auto source = curved_constraint_wall();
    auto obstacle = source;
    obstacle.id = "crossing-obstacle";
    obstacle.properties["baseline"] = {{"start", {6.0, -10.0}}, {"end", {6.0, 10.0}},
                                        {"sweep_radians", 0.0}};
    auto straight_source = source;
    straight_source.properties["baseline"]["sweep_radians"] = 0.0;
    auto straight = Document::create({straight_source, obstacle, translation_fixture()});
    const auto straight_initial = straight.snapshot().entities();
    sketch::ApplyBoundaryConstraintChanges historical{0,
        {{"translated-boundary", sketch::BoundaryGeometryEditKind::move_vertex, "vertex-1", {2.5, 0}}},
        {}, "safe historical straight wall edit"};
    historical.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0}, std::nullopt});
    straight.apply(historical);
    const auto straight_edited = straight.snapshot().entities();
    require(sketch::command_to_json(historical).at("version") == 2,
            "historical straight fixture must retain its command version");
    const auto straight_file = temp.path / "safe-straight-wall-history.psketch";
    (void)ProjectStore::save(straight_file, straight.snapshot());
    auto straight_loaded = ProjectStore::load(straight_file);
    require(straight_loaded.document.snapshot().entities() == straight_edited,
            "safe historical straight wall command must reopen after topology admission");
    straight_loaded.document.undo(straight_loaded.document.revision());
    require(straight_loaded.document.snapshot().entities() == straight_initial,
            "historical straight wall command must undo exactly");
    straight_loaded.document.redo(straight_loaded.document.revision());
    require(straight_loaded.document.snapshot().entities() == straight_edited,
            "historical straight wall command must redo exactly");
    auto document = Document::create({source, obstacle});
    const auto initial = document.snapshot().entities();
    sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "safe curved endpoint edit"};
    command.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0.4}, std::nullopt, 2});
    document.apply(command);
    const auto edited = document.snapshot().entities();
    const auto file = temp.path / "safe-curved-wall-history.psketch";
    (void)ProjectStore::save(file, document.snapshot());
    auto valid = ProjectStore::load(file);
    require(valid.document.snapshot().entities() == edited,
            "valid historical curved typed command must still reopen before forgery");
    valid.document.undo(valid.document.revision());
    require(valid.document.snapshot().entities() == initial,
            "valid historical curved typed command must undo exactly");
    valid.document.redo(valid.document.revision());
    require(valid.document.snapshot().entities() == edited,
            "valid historical curved typed command must redo exactly");

    auto forged_wall = edited.at(source.id);
    forged_wall.properties["baseline"]["end"][0] = 7.0;
    const sketch::ConstraintWallGeometryEdit forged_edit{source.id, {{0, 0}, {7, 0}, 0.4}, std::nullopt, 2};
    require(sketch::replay_constraint_wall_edit(source, forged_edit) == forged_wall,
            "forged result must exactly match the independently replayed typed wall proof");
    require(sketch::segment_intersection(forged_edit.baseline, {{6, -10}, {6, 10}, 0}).kind ==
                sketch::SegmentIntersectionKind::proper,
            "forgery fixture must introduce an analytical proper crossing");
    // Both stored command and result agree; the logical digest is recomputed.
    // The sole invalid behavior is a new same-plane wall crossing on replay.
    const auto forged = temp.path / "consistent-crossing-wall-history.psketch";
    std::filesystem::copy_file(file, forged);
    execute_sql(forged, "UPDATE revisions SET boundary_constraint_changes_json="
        "json_set(boundary_constraint_changes_json,'$.wall_edits[0].baseline.end[0]',7.0) WHERE revision=1; "
        "UPDATE revision_entities SET properties_json=json_set(properties_json,'$.baseline.end[0]',7.0) "
        "WHERE revision=1 AND id='curve-wall'");
    rewrite_logical_digest(forged);
    const auto valid_hash = ProjectStore::file_sha256(file);
    const auto forged_hash = ProjectStore::file_sha256(forged);
    require_error_contains([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
                           "topology", "consistent proof/result and recomputed digest must not bless new wall crossing");
    require(ProjectStore::file_sha256(file) == valid_hash && ProjectStore::file_sha256(forged) == forged_hash,
            "refusing forged topology must preserve both source and rejected history bytes");
}

void test_straight_wall_only_typed_history_requires_v11_and_rejects_consistent_forgery() {
    TempDirectory temp;
    auto source = curved_constraint_wall();
    source.id = "straight-wall";
    source.properties["baseline"]["sweep_radians"] = 0.0;
    auto obstacle = source;
    obstacle.id = "crossing-obstacle";
    obstacle.properties["baseline"] = {{"start", {6.0, -10.0}}, {"end", {6.0, 10.0}}, {"sweep_radians", 0.0}};
    auto authored = Document::create({source, obstacle});
    const auto initial = authored.snapshot().entities();
    // Authoring coverage independently proves the service emits this intent.
    // Storage admission itself must remain solver-independent.
    sketch::ApplyBoundaryConstraintChanges intent{authored.revision(), {}, {}, "Resize straight wall"};
    intent.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0}, sketch::parse_quantity("5 m")});
    authored.apply(intent);
    const auto head = authored.snapshot();
    require(head.history().back().boundary_constraint_changes && ProjectStore::required_format_version(head) == 11,
            "straight wall-only typed intent must require v11");
    const auto authored_file = temp.path / "authored-straight-v11.psketch";
    (void)ProjectStore::save(authored_file, head);
    auto reopened = ProjectStore::load(authored_file);
    require(reopened.document.snapshot().entities() == head.entities(), "v11 straight authoring must reopen exactly");
    reopened.document.undo(reopened.document.revision());
    require(reopened.document.snapshot().entities() == initial && ProjectStore::required_format_version(reopened.document.snapshot()) == 11,
            "undone straight authoring must retain v11 and exact pre-authoring state");
    const auto undone_file = temp.path / "authored-straight-v11-undone.psketch";
    (void)ProjectStore::save(undone_file, reopened.document.snapshot());
    auto undone = ProjectStore::load(undone_file);
    require(undone.document.can_redo(), "v11 undone straight proof must retain redo on reopen");
    undone.document.redo(undone.document.revision());
    require(undone.document.snapshot().entities() == head.entities(), "v11 redo must reproduce exact straight wall length receipt");
    undone.document.apply(ApplyEntityChanges{undone.document.revision(), {EntityChange::erase(source.id)}, {}, "delete wall"});
    require(ProjectStore::required_format_version(undone.document.snapshot()) == 11,
            "deleting an authored wall must not lower its retained straight proof reader floor");
    auto later_curve = source;
    later_curve.id = "later-curve";
    later_curve.properties["baseline"]["sweep_radians"] = 0.4;
    undone.document.apply(ApplyEntityChanges{undone.document.revision(),
        {EntityChange::upsert(later_curve)}, {}, "Add later curve"});
    sketch::ApplyBoundaryConstraintChanges curve_edit{undone.document.revision(), {}, {}, "Edit later curve"};
    curve_edit.wall_edits.push_back({later_curve.id, {{0, 0}, {5, 0}, 0.4}, std::nullopt, 2});
    undone.document.apply(curve_edit);
    require(ProjectStore::required_format_version(undone.document.snapshot()) == 11,
            "later curved proof must not lower a retained straight wall-only reader floor");
    const auto mixed_file = temp.path / "mixed-straight-curved-v11.psketch";
    (void)ProjectStore::save(mixed_file, undone.document.snapshot());
    require(ProjectStore::load(mixed_file).document.snapshot().entities() == undone.document.snapshot().entities(),
            "mixed straight and later curved proofs must reopen exactly");
    const auto downgraded = temp.path / "authored-straight-downgraded.psketch";
    std::filesystem::copy_file(undone_file, downgraded);
    execute_sql(downgraded, "PRAGMA user_version=10; UPDATE metadata SET value='10' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::unsupported_format,
                  "recomputed checksum cannot downgrade retained straight wall-only proof to v10");
    const auto wrong_envelope = temp.path / "straight-wall-only-wrong-envelope.psketch";
    std::filesystem::copy_file(authored_file, wrong_envelope);
    execute_sql(wrong_envelope, "UPDATE revisions SET boundary_constraint_changes_json="
        "json_set(boundary_constraint_changes_json,'$.version',2) WHERE revision=1");
    rewrite_logical_digest(wrong_envelope);
    require_error([&] { (void)ProjectStore::load(wrong_envelope); }, StorageErrorCode::integrity_failure,
                  "historical mixed v2 envelope cannot be substituted for v4 wall-only proof");
    const auto future = temp.path / "straight-future-format.psketch";
    std::filesystem::copy_file(authored_file, future);
    const auto future_version = std::to_string(ProjectStore::format_version + 1);
    execute_sql(future, "PRAGMA user_version=" + future_version + "; UPDATE metadata SET value='" + future_version + "' WHERE key='format_version'");
    require_error([&] { (void)ProjectStore::load(future); }, StorageErrorCode::unsupported_format,
                  "storage versions newer than the supported format must reject before semantic admission");

    // No length receipt is needed for a connected endpoint movement. Matching
    // proof/result forgery therefore isolates topology from exact-entry checks.
    auto direct = Document::create({source, obstacle});
    sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "safe straight endpoint edit"};
    command.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0}, std::nullopt});
    direct.apply(command);
    const auto file = temp.path / "direct-straight-v4.psketch";
    (void)ProjectStore::save(file, direct.snapshot());
    auto forged_wall = direct.snapshot().entities().at(source.id);
    forged_wall.properties["baseline"]["end"][0] = 7.0;
    require(sketch::replay_constraint_wall_edit(source, {source.id, {{0, 0}, {7, 0}, 0}, std::nullopt}) == forged_wall,
            "straight wall-only forged result must exactly match its endpoint proof");
    const auto forged = temp.path / "consistent-straight-crossing.psketch";
    std::filesystem::copy_file(file, forged);
    execute_sql(forged, "UPDATE revisions SET boundary_constraint_changes_json="
        "json_set(boundary_constraint_changes_json,'$.wall_edits[0].baseline.end[0]',7.0) WHERE revision=1; "
        "UPDATE revision_entities SET properties_json=json_set(properties_json,'$.baseline.end[0]',7.0) "
        "WHERE revision=1 AND id='straight-wall'");
    rewrite_logical_digest(forged);
    const auto original_hash = ProjectStore::file_sha256(file);
    const auto forged_hash = ProjectStore::file_sha256(forged);
    require_error_contains([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
                           "topology", "matching straight v4 proof/result and recomputed checksum cannot bless crossing");
    require(ProjectStore::file_sha256(file) == original_hash && ProjectStore::file_sha256(forged) == forged_hash,
            "straight topology refusal must preserve both source and rejected file bytes");
}

void test_rigid_curve_archive_requires_v14_and_rejects_forgery() {
    TempDirectory temp;
    auto source = curved_constraint_wall();
    const auto curve = sketch::arc_from_chord_arc_length({0, 0}, {4, 0}, 5.0, false);
    source.properties["baseline"]["sweep_radians"] = curve.sweep_radians;
    source.properties["baseline"]["vendor"] = "original geometry";
    const auto angle = sketch::angle_from_radians(curve.sweep_radians);
    source.extensions["curve_input"] = {{"version", 2}, {"construction", "arc_length"},
        {"measure", "5 m"}, {"normalized_measure", "5 m"}, {"measure_value", 5.0},
        {"clockwise", false}, {"start", {0, 0}}, {"end", {4, 0}},
        {"sweep", angle.original_expression}, {"normalized_sweep", angle.normalized_expression},
        {"radians", curve.sweep_radians}, {"vendor", {{"preserve", "exact input"}}}};
    auto reflected = source;
    const sketch::PlanarTransform transform{{2, 0}, 0, true, false, {}};
    sketch::transform_wall_curve_input(reflected, transform);
    require(reflected.properties == source.properties, "curve helper must leave geometry publication to its caller");
    const auto changed_curve = sketch::transform_segment(curve, transform);
    reflected.properties["baseline"]["start"] = {changed_curve.start.x, changed_curve.start.y};
    reflected.properties["baseline"]["end"] = {changed_curve.end.x, changed_curve.end.y};
    reflected.properties["baseline"]["sweep_radians"] = changed_curve.sweep_radians;
    const auto& archive = reflected.extensions.at("curve_input_derivation");
    require(archive.at("version") == 2 && archive.at("source_input") == source.extensions.at("curve_input") &&
            archive.at("source_baseline") == source.properties.at("baseline"),
        "fresh reflection must retain the exact original measured input and baseline");
    auto document = Document::create({source});
    document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(reflected)}, {}, "Flip measured curve"});
    require(ProjectStore::required_format_version(document.snapshot()) == 14, "rigid archive must require v14 at head");
    const auto head_file = temp.path / "rigid-curve-head.bldproj";
    (void)ProjectStore::save(head_file, document.snapshot());
    require(ProjectStore::load(head_file).document.snapshot().entities() == document.snapshot().entities(),
        "rigid archive head must reopen exactly");
    document.undo(document.revision());
    require(ProjectStore::required_format_version(document.snapshot()) == 14,
        "undone rigid archive must retain the v14 floor");
    const auto undone_file = temp.path / "rigid-curve-undone.bldproj";
    (void)ProjectStore::save(undone_file, document.snapshot());
    auto loaded = ProjectStore::load(undone_file);
    require(loaded.document.snapshot().entities().at(source.id) == source && loaded.document.can_redo(),
        "undone rigid transform must reopen its source and redo navigation");
    loaded.document.redo(loaded.document.revision());
    require(loaded.document.snapshot().entities().at(source.id) == reflected, "reopened rigid transform must redo exact archive");
    loaded.document.apply(ApplyEntityChanges{loaded.document.revision(), {EntityChange::erase(source.id)}, {}, "Delete curve"});
    require(ProjectStore::required_format_version(loaded.document.snapshot()) == 14,
        "deleted rigid archive must retain the v14 floor");
    const auto deleted_file = temp.path / "rigid-curve-deleted.bldproj";
    (void)ProjectStore::save(deleted_file, loaded.document.snapshot());
    require(ProjectStore::load(deleted_file).document.snapshot().entities().empty(), "deleted rigid history must reopen");

    const auto imported = Document::create({reflected});
    require(imported.snapshot().history().size() == 1 && ProjectStore::required_format_version(imported.snapshot()) == 14,
        "imported rigid archive must require v14 without its originating transform");
    const auto imported_file = temp.path / "rigid-curve-imported.bldproj";
    (void)ProjectStore::save(imported_file, imported.snapshot());
    require(ProjectStore::load(imported_file).document.snapshot().entities() == imported.snapshot().entities(),
        "imported rigid archive must reopen exact source evidence");
    auto floating_version = reflected;
    floating_version.extensions["curve_input_derivation"]["version"] = 2.0;
    require_document_error([&] { (void)Document::create({floating_version}); },
        "floating archive version must not bypass the known rigid archive reader floor");
    sketch::ProjectWorkspace workspace(imported.snapshot());
    const auto capture = workspace.capture();
    const auto history = sketch::capture_workspace_history_record(capture);
    sketch::RecoveryLedger ledger{{"history", "workspace_history",
        sketch::encode_workspace_history_record(capture.document(), history, std::nullopt)}};
    const auto workspace_file = temp.path / "rigid-curve-workspace.bldproj";
    (void)ProjectStore::save_archive(workspace_file, {capture.document(), ledger, sketch::ArchiveRole::ordinary});
    const auto recovered = ProjectStore::load_archive(workspace_file, sketch::ArchiveRole::ordinary);
    require(recovered.supported() && recovered.archive->document().entities() == imported.snapshot().entities() &&
            ProjectStore::required_format_version(recovered.archive->document()) == 14,
        "recovery archive must retain exact rigid evidence and format14");
    const auto source_hash = ProjectStore::file_sha256(imported_file);
    const auto downgrade = temp.path / "rigid-curve-downgraded.bldproj";
    std::filesystem::copy_file(imported_file, downgrade);
    execute_sql(downgrade, "PRAGMA user_version=13; UPDATE metadata SET value='13' WHERE key='format_version'");
    rewrite_logical_digest(downgrade);
    const auto downgrade_hash = ProjectStore::file_sha256(downgrade);
    require_error([&] { (void)ProjectStore::load(downgrade); }, StorageErrorCode::unsupported_format,
        "recomputed digest must not authorize a v13 rigid archive downgrade");
    require(ProjectStore::file_sha256(downgrade) == downgrade_hash, "refused downgrade must preserve its exact bytes");
    for (const bool parity : {true, false}) {
        const auto forged = temp.path / (parity ? "rigid-parity-forged.bldproj" : "rigid-baseline-forged.bldproj");
        std::filesystem::copy_file(imported_file, forged);
        if (parity) execute_sql(forged, "UPDATE revision_entities SET extensions_json=json_set(extensions_json,"
            "'$.curve_input_derivation.operations[0].transform.flip_horizontal',json('false')) WHERE revision=0");
        else execute_sql(forged, "UPDATE revision_entities SET properties_json=json_set(properties_json,'$.baseline.end[0]',-0.5),"
            "extensions_json=json_set(extensions_json,'$.curve_input.end[0]',-0.5,"
            "'$.curve_input_derivation.operations[0].baseline.end[0]',-0.5) WHERE revision=0");
        rewrite_logical_digest(forged);
        const auto forged_hash = ProjectStore::file_sha256(forged);
        require_error_contains([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
            "rigid operation does not reproduce", "imported rigid archive must independently replay despite a recomputed digest");
        require(ProjectStore::file_sha256(forged) == forged_hash, "refused rigid forgery must preserve its exact bytes");
    }
    require(ProjectStore::file_sha256(imported_file) == source_hash, "all refusals must preserve the original valid archive");
    auto vendor = reflected; vendor.type = "vendor_wall";
    require(ProjectStore::required_format_version(Document::create({vendor}).snapshot()) == 1,
        "generic vendor archive collision must remain opaque");
}

void test_imported_curve_derivation_requires_v10_without_command_history() {
    TempDirectory temp;
    auto source = curved_constraint_wall();
    const auto baseline = sketch::arc_from_chord_arc_length({0, 0}, {4, 0}, 5.0, false);
    source.properties["baseline"]["sweep_radians"] = baseline.sweep_radians;
    const auto angle = sketch::angle_from_radians(baseline.sweep_radians);
    source.extensions["curve_input"] = {{"version", 2}, {"construction", "arc_length"},
        {"measure", "5 m"}, {"normalized_measure", "5 m"}, {"measure_value", 5.0},
        {"clockwise", false}, {"start", {0, 0}}, {"end", {4, 0}},
        {"sweep", angle.original_expression}, {"normalized_sweep", angle.normalized_expression},
        {"radians", baseline.sweep_radians}, {"vendor", "preserve"}};
    const auto derived = sketch::replay_constraint_wall_edit(source,
        {source.id, {{0, 0}, {4.5, 0}, baseline.sweep_radians}, std::nullopt, 2});
    auto document = Document::create({derived});
    require(document.snapshot().history().size() == 1 &&
                !document.snapshot().history().front().boundary_constraint_changes &&
                ProjectStore::required_format_version(document.snapshot()) == 10,
            "imported curve derivation must require v10 without relations or originating command");
    const auto file = temp.path / "imported-curve-derivation.psketch";
    (void)ProjectStore::save(file, document.snapshot());
    require(ProjectStore::load(file).document.snapshot().entities().at(derived.id) == derived,
            "imported curve derivation must reopen with exact source input and derived geometry");
    const auto downgraded = temp.path / "imported-curve-derivation-downgraded.psketch";
    std::filesystem::copy_file(file, downgraded);
    execute_sql(downgraded, "PRAGMA user_version=9; UPDATE metadata SET value='9' WHERE key='format_version'");
    rewrite_logical_digest(downgraded);
    require_error([&] { (void)ProjectStore::load(downgraded); }, StorageErrorCode::unsupported_format,
                  "recomputed digest cannot downgrade a recognized imported curve derivation");
    document.apply(ApplyEntityChanges{0, {EntityChange::erase(derived.id)}, {}, "remove derived wall"});
    require(ProjectStore::required_format_version(document.snapshot()) == 10,
            "retained removed curve derivation must preserve the v10 reader floor");

    const auto collision = entity("vendor-label", "label", {{"text", "opaque"}}, false,
        {{"curve_input_derivation", derived.extensions.at("curve_input_derivation")}});
    const auto vendor = Document::create({collision});
    require(ProjectStore::required_format_version(vendor.snapshot()) == 1,
            "generic curve derivation extension collision must remain opaque at its old floor");
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

void test_rigid_group_storage_and_history_floors() {
    TempDirectory temp;
    auto document=Document::create({identified_boundary("rigid-group-area")});const auto before=document.snapshot();
    const sketch::TransformBoundaries command{document.revision(),{{"rigid-group-area",{{.5,.5},.37,true,false,{7,-3}}}},{},"Rotate retained area"};
    document.apply(command);const auto transformed=document.snapshot();
    auto deleted=Document::fork(transformed);deleted.apply(ApplyEntityChanges{deleted.revision(),{EntityChange::erase("rigid-group-area")},{},"Delete transformed owner later"});
    document.undo(document.revision());
    for(const auto& snapshot:{transformed,document.snapshot(),deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==18,"current, undone and deleted grouped transform history requires native eighteen");
        const auto path=temp.path/("rigid-group-"+sketch::make_stable_id()+".bldproj");(void)ProjectStore::save(path,snapshot);
        auto loaded=ProjectStore::load(path);require(loaded.document.snapshot().entities()==snapshot.entities() && loaded.document.snapshot().history().size()==snapshot.history().size(),"native eighteen must reopen exact grouped transform history and navigation");
        if(snapshot.entities()==before.entities()) {loaded.document.redo(loaded.document.revision());require(loaded.document.snapshot().entities()==transformed.entities(),"undone grouped transform reopens with exact redo state");}
        const auto downgrade=temp.path/("rigid-underfloor-"+sketch::make_stable_id()+".bldproj");std::filesystem::copy_file(path,downgrade);
        execute_sql(downgrade,"PRAGMA user_version=17; UPDATE metadata SET value='17' WHERE key='format_version'");rewrite_logical_digest(downgrade);
        const auto digest=ProjectStore::file_sha256(downgrade);
        require_error([&]{(void)ProjectStore::load(downgrade);},StorageErrorCode::integrity_failure,"recomputed digest cannot downgrade the grouped transform proof column to native seventeen");
        require(ProjectStore::file_sha256(downgrade)==digest,"refused grouped-transform downgrade preserves exact file bytes");
    }
    const auto original=temp.path/"rigid-original.bldproj";(void)ProjectStore::save(original,transformed);
    for(const auto* mutation:{"UPDATE revisions SET boundary_transforms_json=NULL WHERE revision=1",
        "UPDATE revisions SET boundary_transforms_json=json_set(boundary_transforms_json,'$.transformations[0].transform.rotation_radians',0.5) WHERE revision=1",
        "UPDATE revisions SET boundary_transforms_json=json_set(boundary_transforms_json,'$.unknown',true) WHERE revision=1"}) {
        const auto forged=temp.path/("rigid-forged-"+sketch::make_stable_id()+".bldproj");std::filesystem::copy_file(original,forged);
        execute_sql(forged,mutation);rewrite_logical_digest(forged);
        require_error([&]{(void)ProjectStore::load(forged);},StorageErrorCode::integrity_failure,"removed, forged or unknown grouped transform proof must reject despite a recomputed digest");
    }
}

void test_selected_rigid_curve_storage_v27(bool compact = false) {
    TempDirectory temp;
    auto source = curved_constraint_wall();
    const auto curve = sketch::arc_from_chord_arc_length({0,0}, {4,0}, 5.0, false);
    const auto angle = sketch::angle_from_radians(curve.sweep_radians);
    source.properties["baseline"]["sweep_radians"] = curve.sweep_radians;
    source.properties["baseline"]["vendor"] = "exact original baseline";
    source.extensions["curve_input"] = {{"version",2},{"construction","arc_length"},{"measure","5 m"},
        {"normalized_measure","5 m"},{"measure_value",5.0},{"clockwise",false},{"start",{0,0}},{"end",{4,0}},
        {"sweep",angle.original_expression},{"normalized_sweep",angle.normalized_expression},
        {"radians",curve.sweep_radians},{"vendor",{{"retain","exact original input"}}}};
    auto neighbor = curved_constraint_wall(); neighbor.id = "rigid-neighbor";
    neighbor.properties["baseline"] = {{"start",{4,0}},{"end",{8,0}},{"sweep_radians",0.0}};
    const sketch::PersistentConstraint join{"rigid-join",sketch::ConstraintRelationKind::coincident,
        {{source.id,sketch::WallEndpointRole::end},{neighbor.id,sketch::WallEndpointRole::start}}};
    auto opening = entity("rigid-opening","opening",{{"wall_id",source.id},{"offset_m",1.0},
        {"width_m",0.5},{"sill_m",0.0},{"height_m",2.0}},false,{{"vendor","exact opening"}});
    std::vector<Entity> values{source,neighbor,opening,sketch::encode_constraint_entity(join)};
    std::vector<Asset> assets;
    if (compact) {
        values.push_back(entity("rigid-label","label",{{"text","Before"}}));
        values.push_back(entity("rigid-object","object",{{"asset_id","rigid-image"}}));
        assets.push_back(Asset::create("rigid-image","application/octet-stream",{std::byte{1}},{{"caption","Before"}}));
    }
    auto document = Document::create(values,assets);
    const auto before = document.snapshot();
    const sketch::PlanarTransform transform{{2,0},0.2,false,false,{1,1}};
    const auto target = sketch::transform_segment(curve,transform);
    // Storage admission is independent of the authoring solver. Prove the
    // selected rigid motion and an ordinary connected endpoint adjustment.
    sketch::ApplyBoundaryConstraintChanges typed;
    typed.expected_revision=before.revision();
    typed.wall_edits={{source.id,target,std::nullopt,4,transform},
        {neighbor.id,{target.end,{8,0},0},std::nullopt,1}};
    typed.rigid_wall_transform_completion=true;
    sketch::Command command=typed;
    if (compact) {
        auto& proof = std::get<sketch::ApplyBoundaryConstraintChanges>(command);
        auto label = before.entities().at("rigid-label"); label.properties["text"] = "After";
        proof.supplemental_entity_changes.push_back(EntityChange::upsert(label));
        proof.supplemental_asset_changes.push_back(AssetChange::upsert(Asset::create("rigid-image","application/octet-stream",
            std::vector<std::byte>(530*1024,std::byte{42}),{{"caption","After"},{"vendor",{{"exact",true}}}})));
        proof.supplemental_source_completion = true;
        proof.supplemental_asset_reference_completion = true;
    }
    const auto wire = sketch::command_to_json(command);
    require(wire.at("version")==10 && wire.at("wall_edits")[0].at("version")==4 &&
        !wire.at("wall_edits")[1].contains("version") && wire.at("source_completion")==false,
        "selected rigid proof must retain outer ten and only qualify its selected wall");
    if (compact) require(wire.dump().size()<1024*1024 &&
        !wire.at("supplemental_asset_changes")[0].at("asset").contains("bytes_hex"),
        "rigid ten mixed proof must use bounded compact references without exterior authority");
    document.apply(command);
    const auto changed = document.snapshot();
    const auto& archive = changed.entities().at(source.id).extensions.at("curve_input_derivation");
    require(archive.at("source_input")==source.extensions.at("curve_input") &&
        archive.at("source_baseline")==source.properties.at("baseline") && changed.entities().at(opening.id)==opening &&
        changed.entities().at(neighbor.id)!=neighbor,"rigid native fixture must preserve exact source and host while propagating its join");
    auto emptied = changed;
    auto& marker = *const_cast<std::vector<sketch::RevisionRecord>&>(emptied.history())[1].boundary_constraint_changes;
    marker.wall_edits.clear(); marker.supplemental_entity_changes.clear(); marker.supplemental_asset_changes.clear();
    require(ProjectStore::required_format_version(emptied)==27,"empty retained rigid ten marker must retain native27");
    auto deleted = Document::fork(changed);
    deleted.apply(ApplyEntityChanges{deleted.revision(),{EntityChange::erase(source.id),EntityChange::erase(opening.id),
        EntityChange::erase(join.id)}, {}, "Delete selected rigid curve later"});
    document.undo(document.revision());
    for (const auto& snapshot : {changed,document.snapshot(),deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==27,"head, Undo and deleted rigid history require native27");
        const auto path = temp.path/("selected-rigid-"+sketch::make_stable_id()+".bldproj");
        (void)ProjectStore::save(path,snapshot);
        auto loaded = ProjectStore::load(path);
        const auto restored = loaded.document.snapshot();
        require(restored.entities()==snapshot.entities() && restored.assets()==snapshot.assets() &&
            sketch::command_to_json(*restored.history()[1].boundary_constraint_changes)==wire &&
            sketch::document_authoring_source_digest_v1(restored)==sketch::document_authoring_source_digest_v1(snapshot),
            "native27 must hydrate exact selected rigid proof, assets and history navigation");
        if (snapshot.entities()==before.entities()) {
            loaded.document.redo(loaded.document.revision());
            require(loaded.document.snapshot().entities()==changed.entities() && loaded.document.snapshot().assets()==changed.assets(),
                "reopened rigid Undo must redo geometry and supplemental assets together");
        }
        execute_sql(path,"PRAGMA user_version=26; UPDATE metadata SET value='26' WHERE key='format_version'");
        rewrite_logical_digest(path); const auto hash = ProjectStore::file_sha256(path);
        require_error([&]{(void)ProjectStore::load(path);},StorageErrorCode::unsupported_format,
            "recomputed digest must not admit retained rigid ten history below native27");
        require(ProjectStore::file_sha256(path)==hash,"rigid floor refusal must preserve source bytes");
    }
    sketch::ProjectWorkspace workspace(document.snapshot()); const auto capture = workspace.capture();
    const auto history = sketch::capture_workspace_history_record(capture);
    sketch::RecoveryLedger ledger{{"rigid-history","workspace_history",
        sketch::encode_workspace_history_record(capture.document(),history,std::nullopt)}};
    const auto recovery = temp.path/"selected-rigid-recovery.bldproj";
    (void)ProjectStore::save_archive(recovery,{capture.document(),ledger,sketch::ArchiveRole::ordinary});
    const auto recovered = ProjectStore::load_archive(recovery,sketch::ArchiveRole::ordinary);
    require(recovered.supported() && recovered.archive->document().entities()==document.snapshot().entities() &&
        recovered.archive->document().assets()==document.snapshot().assets() &&
        sketch::command_to_json(*recovered.archive->document().history()[1].boundary_constraint_changes)==wire &&
        ProjectStore::required_format_version(recovered.archive->document())==27,
        "recovery archive must hydrate selected rigid ten history even while its asset update is undone");
    const auto downgraded_recovery = temp.path/"selected-rigid-recovery-underfloor.bldproj";
    std::filesystem::copy_file(recovery,downgraded_recovery);
    rewrite_logical_digest(downgraded_recovery);
    require(ProjectStore::load_archive(downgraded_recovery,sketch::ArchiveRole::ordinary).supported(),
        "test digest recomputation must preserve an unchanged recovery archive");
    execute_sql(downgraded_recovery,"PRAGMA user_version=26; UPDATE metadata SET value='26' WHERE key='format_version'");
    rewrite_logical_digest(downgraded_recovery); const auto recovery_hash = ProjectStore::file_sha256(downgraded_recovery);
    require_error([&]{(void)ProjectStore::load_archive(downgraded_recovery,sketch::ArchiveRole::ordinary);},
        StorageErrorCode::unsupported_format,"recovery loading must retain native27 admission for undone rigid ten history");
    require(ProjectStore::file_sha256(downgraded_recovery)==recovery_hash,"rigid recovery floor refusal preserves archive bytes");
    const auto original = temp.path/"selected-rigid-original.bldproj";
    (void)ProjectStore::save(original,changed); const auto original_hash = ProjectStore::file_sha256(original);
    for (const auto* mutation : {
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.wall_edits[0].rigid_transform.offset[0]',99) WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.wall_edits[0].baseline.end[0]',99) WHERE revision=1"}) {
        const auto forged = temp.path/("selected-rigid-forged-"+sketch::make_stable_id()+".bldproj");
        std::filesystem::copy_file(original,forged); execute_sql(forged,mutation); rewrite_logical_digest(forged);
        const auto hash = ProjectStore::file_sha256(forged);
        require_error([&]{(void)ProjectStore::load(forged);},StorageErrorCode::integrity_failure,
            "recomputed digest cannot bless a mismatched selected rigid transform or baseline");
        require(ProjectStore::file_sha256(forged)==hash,"rigid forgery refusal preserves source bytes");
    }
    const auto empty_floor = temp.path/"selected-rigid-empty-underfloor.bldproj";
    std::filesystem::copy_file(original,empty_floor);
    execute_sql(empty_floor,"PRAGMA user_version=26; UPDATE metadata SET value='26' WHERE key='format_version'; UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.wall_edits',json('[]')) WHERE revision=1");
    rewrite_logical_digest(empty_floor);
    const auto empty_hash = ProjectStore::file_sha256(empty_floor);
    require_error([&]{(void)ProjectStore::load(empty_floor);},StorageErrorCode::unsupported_format,
        "empty rigid ten marker must reject native26 before history replay");
    require(ProjectStore::file_sha256(empty_floor)==empty_hash,"empty rigid marker refusal preserves source bytes");
    if (compact) for (const auto* mutation : {
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.supplemental_asset_changes[0].asset.metadata_sha256','0000000000000000000000000000000000000000000000000000000000000000') WHERE revision=1",
        "DELETE FROM revision_assets WHERE revision=1 AND asset_id='rigid-image'"}) {
        const auto forged = temp.path/("rigid-compact-forged-"+sketch::make_stable_id()+".bldproj");
        std::filesystem::copy_file(original,forged); execute_sql(forged,mutation); rewrite_logical_digest(forged);
        const auto hash = ProjectStore::file_sha256(forged);
        require_error([&]{(void)ProjectStore::load(forged);},StorageErrorCode::integrity_failure,
            "rigid compact ten hydration must reject missing or metadata-mismatched revision assets");
        require(ProjectStore::file_sha256(forged)==hash,"rigid hydration refusal preserves exact source bytes");
    }
    require(ProjectStore::file_sha256(original)==original_hash,"rigid admission refusals must preserve the valid archive");
}

void test_live_exterior_source_storage_and_history_floor(bool mixed = false, bool compact = false) {
    TempDirectory temp;
    std::vector<Entity> values{
        entity("live-property", "property"),
        entity("live-building", "building", {{"property_id", "live-property"}}),
        entity("live-floor", "floor", {{"building_id", "live-building"}}),
        entity("live-layer", "layer", {{"floor_id", "live-floor"}})};
    const sketch::Vec2 corners[]{{0,0},{4,0},{4,3},{0,3}};
    std::vector<std::string> wall_ids;
    for (std::size_t i = 0; i < 4; ++i) {
        wall_ids.push_back("live-wall-" + std::to_string(i));
        const auto a = corners[i], b = corners[(i + 1) % 4];
        values.push_back(entity(wall_ids.back(), "wall", {{"baseline", {{"start", {a.x,a.y}},
            {"end", {b.x,b.y}}, {"sweep_radians", 0.0}}}, {"thickness_m", 0.2}, {"height_m", 3.0},
            {"elevation_m", 0.0}, {"floor_id", "live-floor"}, {"layer_id", "live-layer"}}));
    }
    const auto measured = sketch::derive_exterior_wall_measurement(Document::create(values).snapshot(), wall_ids);
    auto geometry = nlohmann::json::array();
    for (const auto& edge : measured.boundary)
        geometry.push_back({{"start", {edge.start.x,edge.start.y}}, {"end", {edge.end.x,edge.end.y}},
            {"sweep_radians", edge.sweep_radians}});
    values.push_back(sketch::upgrade_legacy_boundary_entity(entity("live-area", "measurement_boundary",
        {{"boundary", geometry}, {"floor_id", "live-floor"}, {"layer_id", "live-layer"},
         {"wall_measurement_source", measured.source}, {"name", "Retained source area"}})));
    std::vector<Asset> assets;
    if (mixed) {
        values.push_back(entity("live-label", "label", {{"text", "Before"}}));
        values.push_back(entity("live-object", "object", {{"asset_id", "live-image"}, {"name", "Before"}}));
        assets.push_back(Asset::create("live-image", "application/octet-stream", {std::byte{1}}, {{"caption", "Before"}}));
    }
    auto document = Document::create(values, assets);
    const auto before = document.snapshot();
    auto changed_wall = before.entities().at(wall_ids.front());
    changed_wall.properties["thickness_m"] = 0.4;
    ApplyEntityChanges ordinary{before.revision(), {EntityChange::upsert(changed_wall)}, {}, "Widen live source wall"};
    if (mixed) {
        auto label = before.entities().at("live-label");
        label.properties["text"] = "After";
        auto object = before.entities().at("live-object");
        object.properties["name"] = "After";
        ordinary.entity_changes.push_back(EntityChange::upsert(label));
        ordinary.entity_changes.push_back(EntityChange::upsert(object));
        ordinary.asset_changes.push_back(AssetChange::upsert(Asset::create("live-image", "application/octet-stream",
            compact ? std::vector<std::byte>(530*1024,std::byte{42}) : std::vector<std::byte>{std::byte{2},std::byte{3}}, {{"caption", "After"}})));
    }
    auto command = sketch::complete_exterior_wall_measurement_command(before, ordinary);
    if(mixed && !compact)std::get<sketch::ApplyBoundaryConstraintChanges>(command).supplemental_asset_reference_completion=false;
    const auto wire = sketch::command_to_json(command);
    const auto native_floor=compact ? 26U : mixed ? 20U : 19U;
    require(wire.at("version") == (compact ? 9 : mixed ? 7 : 6) && wire.at("physical_entity_changes").size() == 1 &&
        wire.at("exterior_source_edits").size() == 1, "fixture must join physical and measured source intent");
    if (mixed) require(wire.at("supplemental_entity_changes").size() == 2 &&
        wire.at("supplemental_asset_changes").size() == 1, "v7 fixture must retain label, object and asset intents");
    document.apply(command);
    const auto changed = document.snapshot();
    if(compact) {
        require(wire.dump().size()<1024*1024 && !wire.at("supplemental_asset_changes")[0].at("asset").contains("bytes_hex"),"compact asset proof preserves bounded JSON without duplicate bytes");
        sketch::ProjectWorkspace workspace(changed);const auto capture=workspace.capture();
        const auto history=sketch::capture_workspace_history_record(capture);
        sketch::RecoveryLedger ledger{{"compact-history","workspace_history",sketch::encode_workspace_history_record(capture.document(),history,std::nullopt)}};
        const auto archive_path=temp.path/"compact-archive.bldproj";
        (void)ProjectStore::save_archive(archive_path,{capture.document(),ledger,sketch::ArchiveRole::ordinary});
        const auto archive=ProjectStore::load_archive(archive_path,sketch::ArchiveRole::ordinary);
        require(archive.supported() && archive.archive->document().assets()==changed.assets() &&
            sketch::command_to_json(*archive.archive->document().history()[1].boundary_constraint_changes)==wire &&
            ProjectStore::required_format_version(archive.archive->document())==26,"recovery archive retains exact compact proof and independently stored asset");
    }
    std::vector<Entity> imported_values;
    for (const auto& [id, value] : changed.entities()) { (void)id; imported_values.push_back(value); }
    std::vector<Asset> imported_assets;
    for (const auto& [id, asset] : changed.assets()) { (void)id; imported_assets.push_back(asset); }
    const auto imported = Document::create(imported_values, imported_assets);
    require(ProjectStore::required_format_version(imported.snapshot()) == 16,
        "source entities without retained v6 command history must keep their existing native floor");
    auto emptied_proof = changed;
    auto& retained_proof = *const_cast<std::vector<sketch::RevisionRecord>&>(emptied_proof.history())[1].boundary_constraint_changes;
    retained_proof.physical_entity_changes.clear();
    retained_proof.exterior_source_edits.clear();
    if (mixed) {
        retained_proof.supplemental_entity_changes.clear();
        retained_proof.supplemental_asset_changes.clear();
    }
    require(ProjectStore::required_format_version(emptied_proof) == native_floor,
        "retained source discriminator must keep the native floor even when command lanes are removed");
    auto deleted = Document::fork(changed);
    deleted.apply(ApplyEntityChanges{deleted.revision(), {EntityChange::erase("live-area")}, {}, "Delete source owner later"});
    document.undo(document.revision());
    for (const auto& snapshot : {changed, document.snapshot(), deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot) == native_floor,
            "current, undone and deleted source history requires its native format floor");
        const auto path = temp.path / ("live-source-" + sketch::make_stable_id() + ".bldproj");
        (void)ProjectStore::save(path, snapshot);
        auto loaded = ProjectStore::load(path);
        const auto restored = loaded.document.snapshot();
        require(restored.entities() == snapshot.entities() && restored.assets() == snapshot.assets() &&
            restored.history().size() == snapshot.history().size() &&
            sketch::command_to_json(*restored.history()[1].boundary_constraint_changes) == wire &&
            sketch::document_authoring_source_digest_v1(restored) == sketch::document_authoring_source_digest_v1(snapshot),
            "native storage must reopen exact source proof, entities, assets, history and navigation");
        if (snapshot.entities() == before.entities()) {
            loaded.document.redo(loaded.document.revision());
            require(loaded.document.snapshot().entities() == changed.entities() && loaded.document.snapshot().assets() == changed.assets(),
                "reopened Undo must retain one Redo for geometry, supplemental entities and assets");
        }
        execute_sql(path, compact ? "PRAGMA user_version=25; UPDATE metadata SET value='25' WHERE key='format_version'" : mixed ? "PRAGMA user_version=19; UPDATE metadata SET value='19' WHERE key='format_version'" :
            "PRAGMA user_version=18; UPDATE metadata SET value='18' WHERE key='format_version'");
        rewrite_logical_digest(path);
        const auto hash = ProjectStore::file_sha256(path);
        require_error([&] { (void)ProjectStore::load(path); }, StorageErrorCode::unsupported_format,
            "a recomputed digest must not admit source history below its native format floor");
        require(ProjectStore::file_sha256(path) == hash, "refused source downgrade preserves file bytes");
    }
    const auto original = temp.path / "live-original.bldproj";
    (void)ProjectStore::save(original, changed);
    if(compact)for(const auto* mutation:{
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.supplemental_asset_changes[0].asset.byte_size',1) WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.supplemental_asset_changes[0].asset.media_type','image/png') WHERE revision=1",
        "UPDATE revision_assets SET metadata_json=json_set(metadata_json,'$.caption','Forged') WHERE revision=1 AND asset_id='live-image'",
        "DELETE FROM revision_assets WHERE revision=1 AND asset_id='live-image'"}) {
        const auto forged=temp.path/("compact-forged-"+sketch::make_stable_id()+".bldproj");std::filesystem::copy_file(original,forged);
        execute_sql(forged,mutation);rewrite_logical_digest(forged);const auto hash=ProjectStore::file_sha256(forged);
        require_error([&]{(void)ProjectStore::load(forged);},StorageErrorCode::integrity_failure,"compact hydration refuses missing or mismatched exact result assets despite recomputed digest");
        require(ProjectStore::file_sha256(forged)==hash,"refused compact hydration preserves source bytes");
    }
    for (const auto* mutation : {
        "UPDATE revisions SET boundary_constraint_changes_json=NULL WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.physical_entity_changes[0].entity.properties.thickness_m',0.6) WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.exterior_source_edits[0].replacement_segments[0].start[0]',999) WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.exterior_source_edits',json('[]')) WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.version',5) WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.unknown',true) WHERE revision=1"}) {
        const auto forged = temp.path / ("live-forged-" + sketch::make_stable_id() + ".bldproj");
        std::filesystem::copy_file(original, forged);
        execute_sql(forged, mutation);
        rewrite_logical_digest(forged);
        require_error([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
            "missing, forged or mislabeled source proof must fail independently of a recomputed digest");
    }
    if (mixed) for (const auto* mutation : {
        "UPDATE revisions SET boundary_constraint_changes_json=json_remove(boundary_constraint_changes_json,'$.supplemental_entity_changes') WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_remove(boundary_constraint_changes_json,'$.supplemental_asset_changes') WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.supplemental_entity_changes[0].entity.properties.text','Forged') WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.supplemental_asset_changes[0].asset.metadata.caption','Forged') WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.supplemental_asset_changes',json('[]')) WHERE revision=1",
        "UPDATE revisions SET boundary_constraint_changes_json=json_set(boundary_constraint_changes_json,'$.version',6) WHERE revision=1"}) {
        const auto forged = temp.path / ("mixed-forged-" + sketch::make_stable_id() + ".bldproj");
        std::filesystem::copy_file(original, forged);
        execute_sql(forged, mutation);
        rewrite_logical_digest(forged);
        require_error([&] { (void)ProjectStore::load(forged); }, StorageErrorCode::integrity_failure,
            "supplemental intent tampering must reject independently of a recomputed logical digest");
    }
}

void test_reviewed_exterior_source_reader_floor() {
  for (const bool fresh : {false,true}) {
    TempDirectory temp;
    std::vector<Entity> entities{
        entity("source-property", "property"),
        entity("source-building", "building", {{"property_id", "source-property"}}),
        entity("source-floor", "floor", {{"building_id", "source-building"}}),
        entity("source-layer", "layer", {{"floor_id", "source-floor"}})};
    const sketch::Vec2 points[]{{0,0},{4,0},{4,3},{0,3}};
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < 4; ++i) {
        ids.push_back("new-source-" + std::to_string(i));
        const auto a = points[i], b = points[(i + 1) % 4];
        entities.push_back(entity(ids.back(), "wall", {{"baseline", {{"start", {a.x,a.y}},
            {"end", {b.x,b.y}}, {"sweep_radians", 0.0}}}, {"thickness_m", 0.2}, {"height_m", 3.0},
            {"floor_id", "source-floor"}, {"layer_id", "source-layer"}}));
    }
    auto source_document = Document::create(entities);
    const auto measured = sketch::derive_exterior_wall_measurement(source_document.snapshot(), ids);
    auto geometry = nlohmann::json::array();
    for (const auto& segment : measured.boundary) geometry.push_back({{"start", {segment.start.x,segment.start.y}},
        {"end", {segment.end.x,segment.end.y}}, {"sweep_radians", segment.sweep_radians}});
    auto old_source = measured.source;
    for (auto& wall : old_source["walls"]) wall["id"] = "old-" + wall["id"].get<std::string>();
    auto owner = sketch::upgrade_legacy_boundary_entity(entity("source-area", "measurement_boundary",
        {{"boundary", geometry}, {"floor_id", "source-floor"}, {"layer_id", "source-layer"},
         {"wall_measurement_source", old_source}}));
    entities.push_back(owner);
    auto document = Document::create(entities);
    sketch::BoundaryGeometryEdit edit;
    edit.boundary_id = owner.id; edit.target_id = owner.id;
    edit.kind = sketch::BoundaryGeometryEditKind::redefine_boundary;
    edit.replacement_segments = owner.properties.at("segments");
    edit.replacement_wall_source_ids = ids;
    if (fresh) {
        for (std::size_t i = 0; i < edit.replacement_segments.size(); ++i) {
            edit.replacement_segments[i]["segment_id"] = "fresh-storage-edge-" + std::to_string(i);
            edit.replacement_segments[i]["start_vertex_id"] = "fresh-storage-corner-" + std::to_string(i);
            edit.replacement_segments[i]["end_vertex_id"] = "fresh-storage-corner-" + std::to_string((i + 1) % edit.replacement_segments.size());
        }
        auto wire = sketch::encode_boundary_geometry_edit(edit);
        wire["version"] = 4; wire["fresh_topology"] = true;
        edit = sketch::decode_boundary_geometry_edit(wire);
    }
    document.apply(sketch::EditBoundaryGeometry{document.revision(), edit});
    const auto repaired = document.snapshot();
    std::vector<Entity> imported_values;
    for (const auto& [id, value] : repaired.entities()) { (void)id; imported_values.push_back(value); }
    auto imported = Document::create(imported_values);
    auto deleted = Document::fork(repaired);
    deleted.apply(ApplyEntityChanges{deleted.revision(), {EntityChange::erase(owner.id)}, {}, "Delete measured owner later"});
    document.undo(document.revision());
    for (const auto& snapshot : {repaired, document.snapshot(), imported.snapshot(), deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot) == (fresh ? 17U : 16U),
            "current, undone and imported reviewed source intent must retain its required reader floor");
        const auto path = temp.path / ("source-" + sketch::make_stable_id() + ".bldproj");
        (void)ProjectStore::save(path, snapshot);
        require(ProjectStore::load(path).document.snapshot().entities() == snapshot.entities(),
            "reviewed source reader sixteen must reopen all retained states");
        const auto downgraded_version = fresh ? "16" : "15";
        execute_sql(path, std::string("PRAGMA user_version=") + downgraded_version + "; UPDATE metadata SET value='" + downgraded_version + "' WHERE key='format_version'");
        rewrite_logical_digest(path);
        const auto original_hash = ProjectStore::file_sha256(path);
        require_error([&] { (void)ProjectStore::load(path); }, StorageErrorCode::unsupported_format,
            "recomputed digest cannot downgrade current, undone or imported source intent");
        require(ProjectStore::file_sha256(path) == original_hash, "refused source downgrade must preserve the file");
    }
  }
}

void test_automatic_angle_redraw_reader_floor() {
    TempDirectory temp;
    auto document = sketch::testing::document_with_removed_automatic_angle();
    const auto changed = document.snapshot();
    std::vector<Entity> entities;
    for (const auto& [id, value] : changed.entities()) { (void)id; entities.push_back(value); }
    const auto imported = Document::create(std::move(entities));
    const auto imported_snapshot = imported.snapshot();
    const auto& imported_intent = imported_snapshot.entities().at("angle-area")
        .extensions.at("boundary_geometry_derivation").at("operations").at(0).at("value");
    require(imported_intent.at("version") == 5 &&
        imported_intent.at("allow_automatic_angle_removal") == true &&
        imported_intent.at("replacement_removed_reference_ids") == nlohmann::json::array({"automatic-angle"}),
        "imported geometry must retain the exact version-five automatic-angle decision");
    auto deleted = Document::fork(changed);
    deleted.apply(ApplyEntityChanges{deleted.revision(), {EntityChange::erase("angle-area")}, {}, "Delete later"});
    document.undo(document.revision());
    for (const auto& snapshot : {changed, document.snapshot(), imported_snapshot, deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot) == 23,
            "current, undone, imported and deleted automatic-angle redraw requires reader23");
        const auto path = temp.path / ("angle-redraw-" + sketch::make_stable_id() + ".bldproj");
        (void)ProjectStore::save(path, snapshot);
        const auto reopened = ProjectStore::load(path).document.snapshot();
        require(reopened.entities() == snapshot.entities() && reopened.history().size() == snapshot.history().size(),
            "reader23 must reopen exact state and retained redraw history");
        execute_sql(path, "PRAGMA user_version=22; UPDATE metadata SET value='22' WHERE key='format_version'");
        rewrite_logical_digest(path);
        const auto original_hash = ProjectStore::file_sha256(path);
        require_error([&] { (void)ProjectStore::load(path); }, StorageErrorCode::unsupported_format,
            "recomputed digest cannot downgrade automatic-angle redraw intent");
        require(ProjectStore::file_sha256(path) == original_hash, "refused downgrade must preserve original bytes");
    }
}

void test_ansi_appraisal_reader_floor_retains_history() {
    TempDirectory temp;
    auto property = entity("ansi-property", "property", {{"name", "Appraisal"}});
    auto document = Document::create({property});
    require(ProjectStore::required_format_version(document.snapshot()) == 1,
        "ordinary legacy property must not require the new appraisal reader");
    property.properties["appraisal_policy"] = {{"policy_kind", "ansi_z765_2021"}, {"version", 1},
        {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"},
        {"ansi", {{"interior_inspected", true}, {"direct_measurement", true},
            {"acquisition_increment", "inch"}, {"limitations_statement", ""}}}};
    document.apply(ApplyEntityChanges{0, {EntityChange::upsert(property)}, {}, "Declare measurement rules"});
    auto changed = document.snapshot();
    auto deleted = Document::fork(changed);
    deleted.apply(ApplyEntityChanges{deleted.revision(), {EntityChange::erase(property.id)}, {}, "Remove property"});
    document.undo(document.revision());
    for (const auto& snapshot : {changed, document.snapshot(), deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot) == 21,
            "current, undone and deleted ANSI evidence must require reader21");
        const auto path = temp.path / ("ansi-" + sketch::make_stable_id() + ".bldproj");
        (void)ProjectStore::save(path, snapshot);
        const auto reopened = ProjectStore::load(path).document.snapshot();
        require(reopened.entities() == snapshot.entities() && reopened.history().size() == snapshot.history().size(),
            "native appraisal evidence and retained history must reopen exactly");
        execute_sql(path, "PRAGMA user_version=20; UPDATE metadata SET value='20' WHERE key='format_version'");
        rewrite_logical_digest(path);
        const auto hash = ProjectStore::file_sha256(path);
        require_error([&] { (void)ProjectStore::load(path); }, StorageErrorCode::unsupported_format,
            "recomputed logical digest must not downgrade retained appraisal evidence");
        require(ProjectStore::file_sha256(path) == hash, "rejected appraisal downgrade preserves source bytes");
    }
    for (const auto& evidence : {
        entity("ansi-floor", "floor", {{"appraisal_facts", {{"ansi", {{"any_part_below_grade", true}}}}}}),
        entity("ansi-area", "measurement_boundary", {{"boundary", nlohmann::json::array({
            {{"start", {0,0}}, {"end", {2,0}}, {"sweep_radians", 0}},
            {{"start", {2,0}}, {"end", {2,2}}, {"sweep_radians", 0}},
            {{"start", {2,2}}, {"end", {0,2}}, {"sweep_radians", 0}},
            {{"start", {0,2}}, {"end", {0,0}}, {"sweep_radians", 0}}})},
            {"appraisal_facts", {{"ansi", {{"ceiling", {{"kind", "flat"}}}}}}}})})
        require(ProjectStore::required_format_version(Document::create({evidence}).snapshot()) == 21,
            "floor and area evidence independently require the new reader");
    auto opaque = entity("vendor", "generic", {{"appraisal_facts", {{"ansi", {}}}},
        {"appraisal_policy", {{"policy_kind", "ansi_z765_2021"}}}});
    require(ProjectStore::required_format_version(Document::create({opaque}).snapshot()) == 1,
        "unrelated vendor field collision stays opaque in its original format");
}

void test_view_appearance_reader_floor_and_source_integrity() {
    TempDirectory temp;
    const auto rejects_source = [](auto operation, std::string_view message) {
        try { operation(); }
        catch (const sketch::DocumentError& error) {
            require(error.code() == sketch::DocumentErrorCode::dangling_reference, message);
            return;
        }
        throw std::runtime_error(std::string(message));
    };
    sketch::CoordinatedView plan{"plan", "Plan"};
    sketch::DrawingSheet sheet;
    sheet.id = "sheet"; sheet.number = "A101";
    sheet.viewports = {{"viewport", "plan", {10,10,180,120}, 50}};
    auto graph = sketch::make_sheet_view_entity("views", sketch::SheetViewModel::create({plan}, {sheet}));
    auto wall = entity("wall", "wall");
    auto document = Document::create({graph, wall});
    require(ProjectStore::required_format_version(document.snapshot()) == 1,
        "inherited view appearance retains its original native reader floor");
    graph.properties["model"]["version"] = 7;
    require(ProjectStore::required_format_version(Document::create({graph, wall}).snapshot()) == 24,
        "a preserved version7 payload requires its reader even without overrides");
    graph.properties["model"]["views"][0]["presentation"]["appearance"] = {
        {"visible", true}, {"style", nullptr}, {"objects", nlohmann::json::array({
            {{"object_id", "wall"}, {"style", nullptr}, {"visible", false}}})}};
    document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(graph)}, {}, "Hide wall in plan"});
    const auto styled = document.snapshot();
    sketch::ProjectWorkspace workspace(styled);
    const auto capture = workspace.capture();
    const auto history = sketch::capture_workspace_history_record(capture);
    sketch::RecoveryLedger ledger{{"view-history", "workspace_history",
        sketch::encode_workspace_history_record(capture.document(), history, std::nullopt)}};
    const auto archive_path = temp.path / "view-appearance-archive.bldproj";
    (void)ProjectStore::save_archive(archive_path, {capture.document(), ledger, sketch::ArchiveRole::ordinary});
    const auto archive = ProjectStore::load_archive(archive_path, sketch::ArchiveRole::ordinary);
    require(archive.supported() && archive.archive->document().entities() == styled.entities() &&
        ProjectStore::required_format_version(archive.archive->document()) == 24,
        "recovery-aware native archive retains exact view intent and reader floor");
    rejects_source([&] {
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::erase(wall.id)}, {}, "Delete referenced wall"});
    },
        "view appearance cannot retain a missing source in an unrestricted view");
    auto deleted = Document::fork(styled);
    deleted.apply(ApplyEntityChanges{deleted.revision(), {EntityChange::erase(graph.id)}, {}, "Delete view graph"});
    document.undo(document.revision());
    for (const auto& snapshot : {styled, document.snapshot(), deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot) == 24,
            "current, undone and deleted view appearance retains native reader24");
        const auto path = temp.path / ("appearance-" + sketch::make_stable_id() + ".bldproj");
        (void)ProjectStore::save(path, snapshot);
        const auto reopened = ProjectStore::load(path).document.snapshot();
        require(reopened.entities() == snapshot.entities() && reopened.history().size() == snapshot.history().size(),
            "view appearance and all retained history reopen exactly");
        execute_sql(path, "PRAGMA user_version=23; UPDATE metadata SET value='23' WHERE key='format_version'");
        rewrite_logical_digest(path);
        const auto hash = ProjectStore::file_sha256(path);
        require_error([&] { (void)ProjectStore::load(path); }, StorageErrorCode::unsupported_format,
            "a recomputed digest cannot downgrade retained view presentation");
        require(ProjectStore::file_sha256(path) == hash, "refused appearance downgrade preserves original bytes");
    }
    auto vendor = entity("vendor", "generic", graph.properties);
    require(ProjectStore::required_format_version(Document::create({vendor}).snapshot()) == 1,
        "an unrelated vendor appearance field remains opaque");
    auto missing = graph;
    missing.properties["model"]["views"][0]["presentation"]["appearance"]["objects"][0]["object_id"] = "missing";
    rejects_source([&] { (void)Document::create({missing, wall}); },
        "missing view appearance source is rejected on admission");
}

void test_svg_palette_reader_floor() {
    TempDirectory temp;const auto catalog=sketch::default_symbol_catalog();
    const auto definition=std::find_if(catalog.begin(),catalog.end(),[](const auto& value){return value.svg_asset.has_value();});
    require(definition!=catalog.end(),"palette storage fixture has SVG source");
    sketch::SymbolInstance symbol{"palette-symbol",definition->id};symbol.definition=*definition;
    const auto artwork=std::filesystem::path(__FILE__).parent_path().parent_path()/"assets"/definition->svg_asset->relative_path;
    std::ifstream input(artwork,std::ios::binary);require(input.good(),"palette fixture reads actual catalog artwork");
    symbol.pinned_svg=std::string(std::istreambuf_iterator<char>(input),{});
    sketch::AnnotationState state;state.symbols.push_back(symbol);
    auto owner=sketch::make_annotation_entity("palette-owner",state);
    auto document=Document::create({owner});
    require(ProjectStore::required_format_version(document.snapshot())==1,"absent SVG palette retains legacy native floor");
    auto preserved=owner;preserved.properties["state"]["version"]=7;
    require(ProjectStore::required_format_version(Document::create({preserved}).snapshot())==25,"raw annotation7 retains reader25 even when palette absent");
    state.symbols[0].svg_palette=sketch::SymbolSvgPalette{};
    owner=sketch::make_annotation_entity(owner.id,state);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(owner)},{},"Author explicit library-valued colors"});
    const auto head=document.snapshot();auto deleted=Document::fork(head);
    sketch::ProjectWorkspace palette_workspace(head);
    const auto palette_capture=palette_workspace.capture();
    const auto palette_history=sketch::capture_workspace_history_record(palette_capture);
    sketch::RecoveryLedger palette_ledger{{"palette-history","workspace_history",
        sketch::encode_workspace_history_record(palette_capture.document(),palette_history,std::nullopt)}};
    const auto palette_archive_path=temp.path/"palette-archive.bldproj";
    (void)ProjectStore::save_archive(palette_archive_path,{palette_capture.document(),palette_ledger,sketch::ArchiveRole::ordinary});
    const auto palette_archive=ProjectStore::load_archive(palette_archive_path,sketch::ArchiveRole::ordinary);
    require(palette_archive.supported() && palette_archive.archive->document().entities()==head.entities() &&
        palette_archive.archive->recovery().size()==1 &&
        palette_archive.archive->recovery().front().record_id==palette_ledger.front().record_id &&
        palette_archive.archive->recovery().front().record_kind==palette_ledger.front().record_kind &&
        palette_archive.archive->recovery().front().envelope==palette_ledger.front().envelope &&
        ProjectStore::required_format_version(palette_archive.archive->document())==25,
        "recovery archive preserves palette intent, exact source artwork, history ledger and reader25");
    deleted.apply(ApplyEntityChanges{deleted.revision(),{EntityChange::erase(owner.id)},{},"Delete palette owner"});
    document.undo(document.revision());
    for(const auto& snapshot:{head,document.snapshot(),deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==25,"active, undone and deleted palette history needs native25");
        const auto path=temp.path/("palette-"+sketch::make_stable_id()+".bldproj");
        (void)ProjectStore::save(path,snapshot);const auto restored=ProjectStore::load(path).document.snapshot();
        require(restored.entities()==snapshot.entities() && restored.history().size()==snapshot.history().size(),"palette and exact artwork reopen with retained history");
        execute_sql(path,"PRAGMA user_version=24; UPDATE metadata SET value='24' WHERE key='format_version'");
        rewrite_logical_digest(path);const auto hash=ProjectStore::file_sha256(path);
        require_error([&]{(void)ProjectStore::load(path);},StorageErrorCode::unsupported_format,"a recomputed digest cannot downgrade palette intent");
        require(ProjectStore::file_sha256(path)==hash,"refused palette downgrade preserves source bytes");
    }
    auto vendor=entity("palette-vendor","generic",owner.properties);
    require(ProjectStore::required_format_version(Document::create({vendor}).snapshot())==1,"vendor palette collision stays opaque");
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
        test_view_appearance_reader_floor_and_source_integrity();
        test_svg_palette_reader_floor();
        test_ansi_appraisal_reader_floor_retains_history();
        test_automatic_angle_redraw_reader_floor();
        test_rigid_group_storage_and_history_floors();
        test_live_exterior_source_storage_and_history_floor();
        test_live_exterior_source_storage_and_history_floor(true);
        test_live_exterior_source_storage_and_history_floor(true,true);
        test_selected_rigid_curve_storage_v27();
        test_selected_rigid_curve_storage_v27(true);
        test_reviewed_exterior_source_reader_floor();
        test_physical_arc_length_history_requires_v12();
        test_direct_curve_length_history_requires_v13();
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
        test_typed_chord_nested_proofs_reject_recomputed_downgrade();
        test_translation_group_storage_and_forgery_rejection();
        test_transform_proof_storage_and_forgery_rejection();
        test_boundary_geometry_edit_proof_storage_and_forgery_rejection();
        test_boundary_constraint_proof_storage();
        test_curved_constraint_state_requires_v10_without_geometry_proof();
        test_curved_constraint_floor_uses_actual_bound_segment();
        test_curved_wall_proof_storage_and_recovery_overwrite_guard();
        test_consistent_curved_wall_history_forgery_rejects_new_crossing();
        test_straight_wall_only_typed_history_requires_v11_and_rejects_consistent_forgery();
        test_imported_curve_derivation_requires_v10_without_command_history();
        test_rigid_curve_archive_requires_v14_and_rejects_forgery();
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
