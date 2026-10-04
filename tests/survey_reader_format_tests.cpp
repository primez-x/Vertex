#include "sketch/boundary_entity.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/survey_report.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json calls(bool curves) {
    return build_survey_report({curves ? "NE,0,10 m\nCURVE,NE,90,20 m,-180\nSE,0,10 m\nSW,90,20 m" :
        "NE,0,10 m\nNE,90,20 m\nSE,0,10 m\nSW,90,20 m", "Reader fixture", "0.01 m", Unit::metre});
}
Entity owner(bool curves = true) {
    const auto report = rebuild_survey_report(calls(curves));
    const auto geometry = make_survey_boundary(report, SurveyClosureMode::retain_measured_calls);
    IdentifiedBoundary boundary{"parcel", "measurement_boundary", {}};
    for (std::size_t i = 0; i < geometry.boundary.size(); ++i)
        boundary.segments.push_back({"s" + std::to_string(i), "v" + std::to_string(i),
            "v" + std::to_string((i + 1) % geometry.boundary.size()), geometry.boundary[i]});
    auto result = encode_identified_boundary_entity(boundary);
    result.extensions["survey_source"] = {{"version", curves ? 2 : 1}, {"report", report.report()},
        {"added_closing_segment", geometry.added_closing_segment}, {"adjusted_final_endpoint", false},
        {"endpoint_adjustment_m", nullptr}};
    return result;
}
// These fixtures contain ordinary entity transactions only. Recompute the
// actual persisted logical manifest so downgrade refusal tests reader policy,
// rather than merely detecting a stale checksum.
std::string digest(const DocumentSnapshot& snapshot, unsigned format, bool future_history = false) {
    Json manifest{{"format_version", format}, {"document_id", snapshot.document_id()},
        {"head_revision", snapshot.revision()}, {"saved_revision", snapshot.revision()},
        {"named_revisions", snapshot.named_revisions()}, {"history", Json::array()}};
    for (const auto& revision : snapshot.history()) {
        require(!revision.boundary_geometry_edit && !revision.boundary_transform &&
            !revision.boundary_translation && !revision.boundary_constraint_changes &&
            !revision.boundary_transforms && !revision.boundary_translations && revision.assets.empty(),
            "downgrade fixture must contain only ordinary entity changes");
        Json row{{"revision", revision.revision}, {"parent_revision", revision.parent_revision},
            {"source_revision", revision.source_revision}, {"action", revision.action}, {"name", revision.name},
            {"undo_stack", revision.undo_stack}, {"redo_stack", revision.redo_stack},
            {"entities", Json::array()}, {"assets", Json::array()}};
        for (const auto& [id, entity] : revision.entities)
            row["entities"].push_back({{"id", id}, {"type", entity.type}, {"required", entity.required},
                {"properties", entity.properties}, {"extensions", entity.extensions}});
        if (future_history && revision.revision == 0)
            row["entities"][0]["extensions"]["survey_source"]["version"] = 99;
        manifest["history"].push_back(std::move(row));
    }
    const auto encoded = manifest.dump();
    return sha256_hex(std::as_bytes(std::span<const char>(encoded.data(), encoded.size())));
}
void downgrade(const std::filesystem::path& path, const DocumentSnapshot& snapshot) {
    sqlite3* db{};
    require(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK, "open downgrade fixture");
    const auto sql = "PRAGMA user_version=33; UPDATE metadata SET value='33' WHERE key='format_version'; "
        "UPDATE metadata SET value='" + digest(snapshot, 33) + "' WHERE key='logical_digest';";
    const auto code = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(code == SQLITE_OK, "rewrite version and valid logical digest");
}
void roundtrip(const std::filesystem::path& root, const DocumentSnapshot& snapshot) {
    require(snapshot.is_editable() && ProjectStore::required_format_version(snapshot) == 34,
        "understood curved survey history requires native reader34");
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, snapshot);
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable() && loaded.entities() == snapshot.entities() &&
        loaded.history().size() == snapshot.history().size(), "native34 reopens exact geometry/source/history");
    const auto extracted = root / make_stable_id();
    extract_project(snapshot, extracted);
    std::ifstream input(extracted / "project.json"); const auto exchange = Json::parse(input);
    require(exchange.at("exchange_version") == 32, "curved retained history requires exchange32");
    downgrade(path, snapshot);
    const auto hash = ProjectStore::file_sha256(path);
    bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::unsupported_format;
    }
    require(refused && ProjectStore::file_sha256(path) == hash,
        "valid recomputed digest cannot downgrade curved history or mutate source bytes");
}
void readonly_roundtrip(const std::filesystem::path& root, Entity entity) {
    const auto document = Document::create({entity});
    require(!document.snapshot().is_editable() && ProjectStore::required_format_version(document.snapshot()) == 34,
        "future or downgraded typed survey metadata preserves payload with read-only protection");
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, document.snapshot());
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(!loaded.is_editable() && loaded.entities().at(entity.id) == entity,
        "unknown typed survey metadata reopens without loss or edit authority");
}
void test_reader_floor(const std::filesystem::path& root) {
    const auto parcel = owner();
    roundtrip(root, Document::create({parcel}).snapshot()); // entity-only import
    auto document = Document::create();
    document.apply(ApplyEntityChanges{0, {EntityChange::upsert(parcel)}, {}, "Add curve calls"});
    roundtrip(root, document.snapshot());
    document.undo(document.revision()); roundtrip(root, document.snapshot());
    document.redo(document.revision());
    document.apply(ApplyEntityChanges{document.revision(), {EntityChange::erase(parcel.id)}, {}, "Delete parcel"});
    roundtrip(root, document.snapshot());
    auto straight = owner(false);
    straight.extensions["survey_source"]["version"] = 2;
    straight.extensions["survey_source"]["original_report"] = calls(true);
    roundtrip(root, Document::create({straight}).snapshot());
    straight.extensions["survey_source"]["original_report"] = calls(false);
    roundtrip(root, Document::create({straight}).snapshot()); // v2 remains sticky with two straight reports.
    auto legacy = Document::create({owner(false)});
    require(legacy.snapshot().is_editable() && ProjectStore::required_format_version(legacy.snapshot()) < 34,
        "ordinary straight version1 retains its historical reader floor");
    auto translated = Document::create({parcel});
    auto moved = decode_identified_boundary_entity(parcel);
    for (auto& edge : moved.segments) {
        edge.segment.start.x += 30; edge.segment.start.y += 40;
        edge.segment.end.x += 30; edge.segment.end.y += 40;
    }
    translated.apply(ApplyEntityChanges{0,
        {EntityChange::upsert(encode_identified_boundary_entity(moved, &parcel))}, {}, "Move archival survey geometry"});
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, translated.snapshot());
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable() && loaded.entities() == translated.snapshot().entities() &&
        loaded.entities().at(parcel.id).extensions.at("survey_source") == parcel.extensions.at("survey_source"),
        "legitimate live geometry transforms retain archival calls without falsely withholding editability");
}
void test_future_history(const std::filesystem::path& root) {
    auto document = Document::create({owner()});
    document.apply(ApplyEntityChanges{0, {EntityChange::erase("parcel")}, {}, "Delete owner"});
    const auto snapshot = document.snapshot();
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, snapshot);
    auto extension = snapshot.history()[0].entities.at("parcel").extensions;
    extension["survey_source"]["version"] = 99;
    sqlite3* db{};
    require(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK, "open historical future fixture");
    sqlite3_stmt* statement{};
    require(sqlite3_prepare_v2(db, "UPDATE revision_entities SET extensions_json=? WHERE revision=0 AND id='parcel'", -1,
        &statement, nullptr) == SQLITE_OK, "prepare future history payload");
    const auto payload = extension.dump();
    sqlite3_bind_text(statement, 1, payload.c_str(), -1, SQLITE_TRANSIENT);
    require(sqlite3_step(statement) == SQLITE_DONE, "write exact future source payload");
    sqlite3_finalize(statement);
    const auto sql = "UPDATE metadata SET value='" + digest(snapshot, 34, true) + "' WHERE key='logical_digest';";
    const auto code = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr); sqlite3_close(db);
    require(code == SQLITE_OK, "recompute historical future fixture digest");
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(!loaded.is_editable() && loaded.entities().empty() &&
        loaded.history()[0].entities.at("parcel").extensions == extension &&
        ProjectStore::required_format_version(loaded) == 34,
        "future survey retained only in deleted history remains exact and prevents editing");
}
void test_unknown_and_collisions(const std::filesystem::path& root) {
    auto future = owner(); future.extensions["survey_source"]["version"] = 99;
    readonly_roundtrip(root, future);
    future = owner(); future.extensions["survey_source"]["report"]["version"] = 99;
    readonly_roundtrip(root, future);
    future = owner(); future.extensions["survey_source"]["report"]["input_provenance"]["version"] = 99;
    readonly_roundtrip(root, future);
    future = owner(); future.extensions["survey_source"]["original_report"] = calls(true);
    future.extensions["survey_source"]["original_report"]["version"] = 99;
    readonly_roundtrip(root, future);
    future = owner(); future.extensions["survey_source"]["original_report"] = calls(false);
    future.extensions["survey_source"]["original_report"]["input_provenance"]["version"] = 99;
    readonly_roundtrip(root, future);
    auto stripped = owner(); stripped.extensions["survey_source"]["version"] = 1;
    readonly_roundtrip(root, stripped);
    stripped = owner(false); stripped.extensions["survey_source"]["original_report"] = calls(true);
    readonly_roundtrip(root, stripped);
    auto malformed = owner(); malformed.extensions["survey_source"]["report"]["version"] = "2";
    readonly_roundtrip(root, malformed);
    malformed = owner(); malformed.extensions["survey_source"]["report"]["input_provenance"]["curves"][0]["version"] = 99;
    readonly_roundtrip(root, malformed);
    malformed = owner(); malformed.extensions["survey_source"]["adjusted_final_endpoint"] = "false";
    readonly_roundtrip(root, malformed);
    auto vendor = Entity::create("generic", Json::object(), false, {{"survey_source", future.extensions.at("survey_source")}});
    const auto opaque = Document::create({vendor}).snapshot();
    require(opaque.is_editable() && ProjectStore::required_format_version(opaque) == 1,
        "unrelated vendor survey metadata remains opaque and does not acquire a typed reader floor");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    const auto root = std::filesystem::temp_directory_path() / ("vertex-survey-reader-" + sketch::make_stable_id());
    std::filesystem::create_directory(root);
    try {
        test_reader_floor(root); test_unknown_and_collisions(root); test_future_history(root);
        std::filesystem::remove_all(root);
        std::cout << "Survey reader format tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "survey_reader_format: " << error.what() << "\nEvidence: " << root << '\n'; return 1;
    }
}
