#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("vertex-linework-storage-" + make_stable_id());
    TempDirectory() { std::filesystem::create_directory(path); }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

std::vector<Entity> context() {
    return {{"p", "property", Json::object(), false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}}, false},
        {"l", "layer", {{"floor_id", "f"}}, false}};
}

MeasurementLinework local_stroke() {
    MeasurementLinework model;
    model.stroke_id = "stroke";
    model.anchor = {2, 3};
    ConstructionReceipt receipt;
    receipt.segment_id = "stroke:e0";
    receipt.kind = BoundaryConstructionKind::line_heading;
    receipt.start = model.anchor;
    receipt.distance = parse_quantity("6 ft 6 3/4 in");
    receipt.heading = parse_angle("0 deg");
    model.edges.push_back({receipt.segment_id, "stroke:v0", "stroke:v1", receipt});
    model.extensions = {{"vendor", {{"expression", "keep me"}}}};
    return model;
}

Entity stroke(bool transformed) {
    auto model = local_stroke();
    if (transformed)
        model = transformed_measurement_linework(model, {{}, 0, false, false, {4, -6}});
    return {"stroke", "measurement_linework",
        {{"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"},
         {"layer_id", "l"}, {"model", encode_measurement_linework_model(model)}},
        true, {{"vendor", "entity extension"}}};
}

Entity lineage_area() {
    Entity area{"area", "measurement_boundary",
        {{"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"},
         {"layer_id", "l"}, {"classification", "living"},
         {"segments", Json::array({
             {{"start", {0, 0}}, {"end", {2, 0}}, {"sweep_radians", 0}},
             {{"start", {2, 0}}, {"end", {2, 2}}, {"sweep_radians", 0}},
             {{"start", {2, 2}}, {"end", {0, 2}}, {"sweep_radians", 0}},
             {{"start", {0, 2}}, {"end", {0, 0}}, {"sweep_radians", 0}}})}}, false,
        {{"measurement_linework_sources", Json::array({Json::array({
            {{"owner_id", "stroke"}, {"segment_id", "stroke:e0"},
             {"parameter_start", 0}, {"parameter_end", 1}, {"reversed", false}}})})},
         {"vendor", "lineage remains exact even when stale"}}};
    return upgrade_legacy_boundary_entity(area);
}

Document fixture(Entity value) {
    auto values = context();
    values.push_back(std::move(value));
    return Document::create(std::move(values));
}

Document lineage_fixture() {
    auto values = context();
    values.push_back(stroke(false));
    values.push_back(lineage_area());
    return Document::create(std::move(values));
}

struct Database {
    sqlite3* handle = nullptr;
    explicit Database(const std::filesystem::path& path, int flags = SQLITE_OPEN_READONLY) {
        const auto encoded = path.u8string();
        const std::string utf8(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        require(sqlite3_open_v2(utf8.c_str(), &handle, flags, nullptr) == SQLITE_OK,
                "test must open the controlled native project");
    }
    ~Database() { sqlite3_close(handle); }
    void execute(const std::string& sql) {
        require(sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
                "controlled native mutation must succeed");
    }
    std::string scalar(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK,
                "test query must prepare");
        require(sqlite3_step(statement) == SQLITE_ROW, "test query must return a row");
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        std::string result(text, static_cast<std::size_t>(sqlite3_column_bytes(statement, 0)));
        sqlite3_finalize(statement);
        return result;
    }
};

// The fixtures use ordinary entity transactions only. Encode their independent
// legacy manifest explicitly so a valid digest cannot mask a format-floor bug.
std::string legacy_digest(const DocumentSnapshot& snapshot, unsigned format) {
    Json manifest{{"format_version", format}, {"document_id", snapshot.document_id()},
        {"head_revision", snapshot.revision()}, {"saved_revision", snapshot.revision()},
        {"history", Json::array()}, {"named_revisions", snapshot.named_revisions()}};
    for (const auto& revision : snapshot.history()) {
        require(revision.assets.empty() && !revision.boundary_translation &&
            !revision.boundary_transform && !revision.boundary_geometry_edit &&
            !revision.boundary_constraint_changes && !revision.boundary_translations &&
            !revision.boundary_transforms, "legacy fixture must have no typed command proofs or assets");
        Json row{{"revision", revision.revision}, {"parent_revision", revision.parent_revision},
            {"source_revision", revision.source_revision}, {"action", revision.action},
            {"name", revision.name}, {"undo_stack", revision.undo_stack},
            {"redo_stack", revision.redo_stack}, {"entities", Json::array()}, {"assets", Json::array()}};
        for (const auto& [id, value] : revision.entities)
            row["entities"].push_back({{"id", id}, {"type", value.type}, {"required", value.required},
                {"properties", value.properties}, {"extensions", value.extensions}});
        manifest["history"].push_back(std::move(row));
    }
    const auto encoded = manifest.dump();
    return sha256_hex(std::as_bytes(std::span<const char>(encoded.data(), encoded.size())));
}

void set_stored_format(const std::filesystem::path& path, const DocumentSnapshot& snapshot,
                       unsigned format) {
    Database database(path, SQLITE_OPEN_READWRITE);
    const auto number = std::to_string(format);
    database.execute("PRAGMA user_version=" + number +
        "; UPDATE metadata SET value='" + number + "' WHERE key='format_version';"
        " UPDATE metadata SET value='" + legacy_digest(snapshot, format) + "' WHERE key='logical_digest'");
}

void require_format(const std::filesystem::path& path, unsigned format) {
    Database database(path);
    require(database.scalar("PRAGMA user_version") == std::to_string(format) &&
        database.scalar("SELECT value FROM metadata WHERE key='format_version'") == std::to_string(format),
        "both native format markers must advertise the required reader");
}

Json extracted(const DocumentSnapshot& snapshot, const std::filesystem::path& destination) {
    extract_project(snapshot, destination);
    std::ifstream input(destination / "project.json");
    require(input.good(), "extraction must publish its interchange manifest");
    return Json::parse(input);
}

void require_history_equal(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    require(document_authoring_source_digest_v1(before) == document_authoring_source_digest_v1(after),
        "native reopen must preserve identity, exact retained entities, receipts, navigation, and names");
}

// Removing the v2 floor, checking just the head, or downgrading extraction breaks these cases.
void v2_current_undo_and_deleted_history() {
    TempDirectory temporary;
    auto document = fixture(stroke(false));
    require(ProjectStore::required_format_version(document.snapshot()) == 1,
            "independent schema-one strokes keep the legacy floor");
    document.apply(NameRevision{document.revision(), "Local stroke"});
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(stroke(true))}, .message = "Move measured stroke"});
    const auto current = document.snapshot();
    require(ProjectStore::required_format_version(current) == 28, "current schema-two stroke requires native 28");
    document.undo(document.revision());
    const auto undone = document.snapshot();
    require(undone.entities().at("stroke").properties.at("model").at("version") == 1,
            "undo fixture must actually restore the local schema-one model");
    require(ProjectStore::required_format_version(undone) == 28, "redo-only schema-two stroke still requires native 28");
    document.redo(document.revision());
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase("stroke")}, .message = "Delete measured stroke"});
    const auto deleted = document.snapshot();
    require(!deleted.entities().contains("stroke") && ProjectStore::required_format_version(deleted) == 28,
            "deleted schema-two stroke still requires native 28 throughout history");
    const std::vector<DocumentSnapshot> snapshots{current, undone, deleted};
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto native = temporary.path / ("stroke-" + std::to_string(i) + ".vertex");
        (void)ProjectStore::save(native, snapshots[i]);
        require_format(native, 28);
        const auto loaded = ProjectStore::load(native);
        require_history_equal(snapshots[i], loaded.document.snapshot());
        const auto exchange = extracted(loaded.document.snapshot(), temporary.path / ("extract-" + std::to_string(i)));
        require(exchange.at("exchange_version") == 26, "retained schema-two strokes require extraction 26");
        require(exchange.at("revisions").size() == snapshots[i].history().size(),
                "extraction must keep all stroke history including deleted models");
    }
    const auto native = temporary.path / "false-27.vertex";
    (void)ProjectStore::save(native, deleted);
    set_stored_format(native, deleted, 27);
    const auto fingerprint = ProjectStore::file_sha256(native);
    try {
        (void)ProjectStore::load(native);
        throw std::runtime_error("schema-two stroke disguised as native 27 must be rejected");
    } catch (const StorageError& error) {
        require(error.code() == StorageErrorCode::unsupported_format, "v2 downgrade must fail at the reader floor");
    }
    require(ProjectStore::file_sha256(native) == fingerprint, "downgrade rejection must preserve source bytes");
}

// Removing the lineage floor or limiting it to live owners breaks this case.
void lineage_current_undo_and_deleted_history() {
    TempDirectory temporary;
    auto document = fixture(stroke(false));
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(lineage_area())}, .message = "Derive measured area"});
    const auto current = document.snapshot();
    document.undo(document.revision());
    const auto undone = document.snapshot();
    document.redo(document.revision());
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase("area")}, .message = "Delete measured area"});
    const std::vector<DocumentSnapshot> snapshots{current, undone, document.snapshot()};
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        require(ProjectStore::required_format_version(snapshots[i]) == 28,
                "current, undone, and deleted measured lineage needs native 28");
        const auto native = temporary.path / ("lineage-" + std::to_string(i) + ".vertex");
        (void)ProjectStore::save(native, snapshots[i]);
        require_format(native, 28);
        const auto loaded = ProjectStore::load(native);
        require_history_equal(snapshots[i], loaded.document.snapshot());
        require(extracted(loaded.document.snapshot(), temporary.path / ("lineage-extract-" + std::to_string(i)))
                    .at("exchange_version") == 26, "retained lineage extraction needs version 26");
    }
}

// Applying the new lineage floor to legacy reads strands already published files.
void legacy_v1_lineage_opens_and_upgrade_preserves_original() {
    TempDirectory temporary;
    auto document = lineage_fixture();
    const auto original = document.snapshot();
    const auto native = temporary.path / "published-27.vertex";
    (void)ProjectStore::save(native, original);
    set_stored_format(native, original, 27);
    const auto original_hash = ProjectStore::file_sha256(native);
    auto loaded = ProjectStore::load(native);
    require_format(native, 27);
    require(ProjectStore::file_sha256(native) == original_hash,
            "opening legacy lineage must preserve its original native bytes");
    require_history_equal(original, loaded.document.snapshot());
    require(ProjectStore::required_format_version(loaded.document.snapshot()) == 28,
            "legacy read exception must not lower the next-save floor");
    SaveOptions options;
    options.expected_destination_sha256 = loaded.file_sha256;
    const auto saved = ProjectStore::save(native, loaded.document.snapshot(), options);
    require_format(native, 28);
    require(saved.backup_path && ProjectStore::file_sha256(*saved.backup_path) == original_hash,
            "upgrading legacy lineage must retain the exact published original as backup");
    require_history_equal(original, ProjectStore::load(native).document.snapshot());
    require_history_equal(original, ProjectStore::load(*saved.backup_path).document.snapshot());
}

// Reserved properties on vendor owners and future models must stay opaque.
void owner_and_model_qualification_preserves_existing_floors() {
    TempDirectory temporary;
    const auto v1 = fixture(stroke(false));
    const auto native = temporary.path / "v1.vertex";
    (void)ProjectStore::save(native, v1.snapshot());
    require_format(native, 1);
    require(extracted(v1.snapshot(), temporary.path / "v1-extract").at("exchange_version") == 1,
            "independent v1 strokes keep extraction one");
    auto vendor = stroke(true);
    vendor.type = "vendor_stroke";
    vendor.required = false;
    vendor.extensions["measurement_linework_sources"] = lineage_area().extensions.at("measurement_linework_sources");
    require(ProjectStore::required_format_version(fixture(vendor).snapshot()) == 1,
            "vendor collisions do not become typed stroke or lineage semantics");
    // A legacy boundary fixture is assembled directly to avoid identified topology fields.
    Entity anonymous{"legacy", "measurement_boundary", {{"segments", Json::array({
        {{"start", {0, 0}}, {"end", {1, 0}}, {"sweep_radians", 0}},
        {{"start", {1, 0}}, {"end", {0, 1}}, {"sweep_radians", 0}},
        {{"start", {0, 1}}, {"end", {0, 0}}, {"sweep_radians", 0}}})}}, false,
        {{"measurement_linework_sources", Json::array()}}};
    require(ProjectStore::required_format_version(Document::create({anonymous}).snapshot()) == 1,
            "anonymous legacy boundary lineage collision keeps its old floor");
    auto future_boundary = lineage_area();
    future_boundary.properties["boundary_model_version"] = 999;
    require(ProjectStore::required_format_version(fixture(future_boundary).snapshot()) == 2,
            "future identified boundary keeps its existing opaque floor");
    auto other_boundary = lineage_area();
    other_boundary.type = "room_boundary";
    require(ProjectStore::required_format_version(fixture(other_boundary).snapshot()) == 2,
            "lineage collision on another known boundary type does not raise the floor");
    for (const auto pair : std::vector<std::pair<int, int>>{{999, 2}, {2, 999}, {1, 2}}) {
        auto future = stroke(true);
        future.properties["model"]["version"] = pair.first;
        future.properties["model"]["replay_version"] = pair.second;
        const auto opaque = fixture(future);
        require(!opaque.is_editable() && ProjectStore::required_format_version(opaque.snapshot()) == 1,
                "future schema/replay models keep their original opaque policy and floor");
        const auto path = temporary.path / ("opaque-" + std::to_string(pair.first) + "-" + std::to_string(pair.second) + ".vertex");
        (void)ProjectStore::save(path, opaque.snapshot());
        require_history_equal(opaque.snapshot(), ProjectStore::load(path).document.snapshot());
    }
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        v2_current_undo_and_deleted_history();
        lineage_current_undo_and_deleted_history();
        legacy_v1_lineage_opens_and_upgrade_preserves_original();
        owner_and_model_qualification_preserves_existing_floors();
        std::cout << "measurement linework storage checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
