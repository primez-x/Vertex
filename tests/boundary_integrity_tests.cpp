#include "sketch/document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>

#include <functional>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Entity rectangle(bool identified = true) {
    const std::vector<Json> points{{0.0, 0.0}, {4.0, 0.0}, {4.0, 3.0}, {0.0, 3.0}};
    Json segments = Json::array();
    for (std::size_t i = 0; i < points.size(); ++i) {
        Json segment{{"start", points[i]}, {"end", points[(i + 1) % points.size()]},
                     {"sweep_radians", 0.0}, {"vendor_note", "preserve"}};
        if (identified) {
            segment["segment_id"] = "edge-" + std::to_string(i);
            segment["start_vertex_id"] = "vertex-" + std::to_string(i);
            segment["end_vertex_id"] = "vertex-" + std::to_string((i + 1) % points.size());
        }
        segments.push_back(std::move(segment));
    }
    auto entity = Entity::create("boundary", {{"segments", segments}, {"vendor_data", {1, 2, 3}}});
    entity.id = "boundary-1";
    if (identified) entity.properties["boundary_model_version"] = 1;
    return entity;
}

void put(Document& document, Entity entity) {
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(std::move(entity))}, .message = "boundary edit"});
}

void require_invalid_snapshot_not_published(const DocumentSnapshot& snapshot) {
    const auto path = std::filesystem::temp_directory_path() /
        ("boundary-invalid-history-" + make_stable_id() + ".bldproj");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{path};
    bool rejected = false;
    try { (void)ProjectStore::save(path, snapshot); }
    catch (const StorageError& error) { rejected = error.code() == StorageErrorCode::invalid_snapshot; }
    require(rejected && !std::filesystem::exists(path),
            "invalid boundary history was published as a project");
}

Document reopen(const DocumentSnapshot& snapshot, int expected_format = 2) {
    const auto path = std::filesystem::temp_directory_path() /
        ("boundary-integrity-" + make_stable_id() + ".bldproj");
    const auto receipt = ProjectStore::save(path, snapshot);
    sqlite3* database = nullptr;
    require(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
            "cannot inspect boundary file format");
    sqlite3_stmt* version = nullptr;
    require(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &version, nullptr) == SQLITE_OK,
            "cannot inspect SQLite version marker");
    const bool sqlite_v2 = sqlite3_step(version) == SQLITE_ROW && sqlite3_column_int(version, 0) == expected_format;
    sqlite3_finalize(version);
    require(sqlite3_prepare_v2(database, "SELECT value FROM metadata WHERE key='format_version'", -1,
                               &version, nullptr) == SQLITE_OK,
            "cannot inspect format metadata");
    const bool metadata_v2 = sqlite3_step(version) == SQLITE_ROW &&
        std::string(reinterpret_cast<const char*>(sqlite3_column_text(version, 0))) == std::to_string(expected_format);
    sqlite3_finalize(version);
    sqlite3_close(database);
    require(sqlite_v2 && metadata_v2, "retained history has the wrong storage format");
    if (const auto* directory = _wgetenv(L"SKETCH_BOUNDARY_COMPAT_DIR"); directory && *directory) {
        const auto capture = std::filesystem::path(directory) /
            (snapshot.document_id() + "-r" + std::to_string(snapshot.revision()) + ".bldproj");
        require(std::filesystem::copy_file(path, capture), "cannot capture boundary compatibility fixture");
    }
    auto loaded = ProjectStore::load(path);
    require(loaded.file_sha256 == receipt.file_sha256, "saved boundary fixture changed");
    std::filesystem::remove(path);
    return std::move(loaded.document);
}

void require_rejected_unchanged(Document& document, Entity candidate, std::string_view message) {
    const auto before = document.snapshot();
    bool rejected = false;
    try { put(document, std::move(candidate)); }
    catch (const DocumentError& error) {
        rejected = error.code() == DocumentErrorCode::invalid_entity;
    }
    require(rejected, message);
    const auto after = document.snapshot();
    require(after.revision() == before.revision() && after.entities() == before.entities() &&
                after.history().size() == before.history().size() &&
                after.saved_revision() == before.saved_revision(),
            "rejected boundary edit changed the document or saved state");
}

void test_raw_commands_cannot_bypass_identity_validation() {
    using Mutation = std::function<void(Entity&)>;
    const std::vector<std::pair<const char*, Mutation>> cases{
        {"duplicate segment IDs accepted", [](Entity& e) {
            e.properties["segments"][1]["segment_id"] = "edge-0";
        }},
        {"disconnected vertex identities accepted", [](Entity& e) {
            e.properties["segments"][0]["end_vertex_id"] = "unconnected-vertex";
        }},
        {"inconsistent repeated vertex coordinates accepted", [](Entity& e) {
            e.properties["segments"][1]["start"][0] = 4.00000001;
        }},
        {"malformed segment silently skipped", [](Entity& e) {
            e.properties["segments"].push_back({{"start", "invalid"}});
        }},
        {"missing sweep accepted", [](Entity& e) {
            e.properties["segments"][2].erase("sweep_radians");
        }},
        {"invalid identifier accepted", [](Entity& e) {
            e.properties["segments"][0]["segment_id"] = "edge with spaces";
        }},
    };
    for (const auto& [message, mutate] : cases) {
        auto document = Document::create();
        auto entity = rectangle();
        put(document, entity);
        document.mark_saved(document.revision());
        mutate(entity);
        require_rejected_unchanged(document, entity, message);
    }
}

void test_downgrade_is_rejected_but_upgrade_undo_is_valid() {
    auto document = Document::create();
    const auto legacy = rectangle(false);
    put(document, legacy);
    const auto identified = rectangle();
    put(document, identified);
    require_rejected_unchanged(document, legacy, "raw command stripped boundary identities");
    document.undo(document.revision());
    require(document.snapshot().entities().at(legacy.id) == legacy,
            "undo of identity upgrade did not restore the exact legacy entity");
    document = reopen(document.snapshot());
    require(document.snapshot().entities().at(legacy.id) == legacy,
            "reopening an undone upgrade did not preserve the anonymous head");
    document.redo(document.revision());
    require(document.snapshot().entities().at(identified.id) == identified,
            "redo of identity upgrade changed identities or metadata");
    auto reopened = reopen(document.snapshot());
    require(reopened.snapshot().entities() == document.snapshot().entities() && reopened.is_editable(),
            "validated undo/redo history cannot be reopened");
}

void test_future_boundary_version_is_preserved_read_only() {
    auto document = Document::create();
    auto future = rectangle();
    future.properties["boundary_model_version"] = 999;
    future.properties["segments"] = Json{{"future_geometry", {1, 2, 3}}};
    put(document, future);
    require(!document.is_editable() && !document.read_only_reason().empty(),
            "future boundary model version remained editable");
    require(document.snapshot().entities().at(future.id) == future,
            "future boundary payload was changed");
    auto reopened = reopen(document.snapshot());
    require(!reopened.is_editable() && reopened.snapshot().entities().at(future.id) == future,
            "restore lost future boundary read-only preservation");
}

Entity dimension() {
    return {"dimension-1", "dimension",
        {{"dimension_version", 1}, {"dimension_kind", "segment_length"},
         {"target", {{"entity_id", "boundary-1"}, {"segment_id", "edge-0"}}},
         {"text_position", {2, -0.5}}, {"placement_origin", "manual"}},
        false, Json::object()};
}

void test_dimension_references_are_atomic_and_survive_history() {
    auto document = Document::create({rectangle(), dimension()});
    const auto original = document.snapshot();
    auto invalid_dimension = dimension();
    invalid_dimension.properties["target"]["segment_id"] = "missing";
    require_rejected_unchanged(document, invalid_dimension,
                              "dangling dimension segment reference was accepted");
    invalid_dimension = dimension();
    invalid_dimension.properties["target"]["entity_id"] = "missing";
    require_rejected_unchanged(document, invalid_dimension,
                              "dangling dimension owner was accepted");
    auto renamed = rectangle();
    renamed.properties["segments"][0]["segment_id"] = "replacement-edge";
    require_rejected_unchanged(document, renamed,
                              "referenced segment was retired without updating its dimension");
    const auto require_atomic_rejection = [&](std::vector<EntityChange> changes) {
        bool rejected = false;
        try {
            document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
                .entity_changes = std::move(changes), .message = "invalid reference change"});
        } catch (const DocumentError&) { rejected = true; }
        require(rejected && document.snapshot().entities() == original.entities() &&
                    document.revision() == original.revision(),
                "reference violation changed document state");
    };
    require_atomic_rejection({EntityChange::erase("boundary-1")});
    auto wrong_owner = rectangle();
    wrong_owner.type = "label";
    require_atomic_rejection({EntityChange::upsert(wrong_owner)});

    auto rebound = dimension();
    rebound.properties["target"]["segment_id"] = "replacement-edge";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(renamed), EntityChange::upsert(rebound)},
        .message = "replace edge and retarget dimension"});
    document = reopen(document.snapshot());
    require(document.is_editable() && document.snapshot().entities().at("dimension-1") == rebound,
            "dimension reference changed during reopen");
    document.undo(document.revision());
    require(document.snapshot().entities() == original.entities(),
            "undo did not restore the dimension and source edge together");
    document.redo(document.revision());
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase("boundary-1"), EntityChange::erase("dimension-1")},
        .message = "delete boundary and dimension together"});
    require(document.snapshot().entities().empty(), "atomic boundary and dimension deletion failed");
    document = reopen(document.snapshot());
    document.undo(document.revision());
    require(document.snapshot().entities().at("dimension-1") == rebound,
            "deleted dimension retained history could not be restored");
}

void test_unknown_dimensions_preserve_read_only_without_hiding_invalid_known_data() {
    for (const bool future_kind : {false, true}) {
        auto unknown = dimension();
        if (future_kind) unknown.properties["dimension_kind"] = "future_kind";
        else unknown.properties["dimension_version"] = 999;
        unknown.properties["target"] = {{"opaque_future_target", {1, 2, 3}}};
        auto document = Document::create({rectangle(), unknown});
        require(!document.is_editable(), "unknown dimension semantics remained editable");
        auto loaded = reopen(document.snapshot());
        require(!loaded.is_editable() && loaded.snapshot().entities().at(unknown.id) == unknown,
                "unknown dimension did not survive read-only reopen");
        auto invalid_known = dimension();
        invalid_known.id = "dimension-2";
        invalid_known.properties["target"]["segment_id"] = "missing";
        bool rejected = false;
        try { (void)Document::create({rectangle(), unknown, invalid_known}); }
        catch (const DocumentError&) { rejected = true; }
        require(rejected, "unknown dimension hid invalid supported dimension");
    }
    auto future_boundary = rectangle();
    future_boundary.properties["boundary_model_version"] = 999;
    future_boundary.properties["segments"] = {{"opaque_future_segments", true}};
    auto future = Document::create({future_boundary, dimension()});
    require(!future.is_editable(), "dimension on future boundary did not preserve read-only data");
    auto loaded = reopen(future.snapshot());
    require(loaded.snapshot().entities() == future.snapshot().entities(),
            "dimension targeting future boundary changed on reopen");
}

void test_v1_upgrade_preserves_original_file_and_reversible_history() {
    auto document = Document::create({rectangle(false)});
    const auto original = document.snapshot();
    const auto path = std::filesystem::temp_directory_path() /
        ("boundary-upgrade-" + make_stable_id() + ".bldproj");
    struct Cleanup {
        std::vector<std::filesystem::path> files;
        ~Cleanup() {
            for (const auto& file : files) {
                std::error_code ignored;
                std::filesystem::remove(file, ignored);
            }
        }
    } cleanup{{path}};
    const auto old_receipt = ProjectStore::save(path, original);
    require(ProjectStore::required_format_version(original) == 1,
            "legacy-only project unexpectedly requires new storage format");
    put(document, rectangle());
    bool injected = false;
    try {
        (void)ProjectStore::save(path, document.snapshot(),
            SaveOptions{.expected_destination_sha256 = old_receipt.file_sha256,
                        .fault_stage = SaveFaultStage::after_validation});
    } catch (const StorageError& error) {
        injected = error.code() == StorageErrorCode::injected_failure;
    }
    require(injected && ProjectStore::file_sha256(path) == old_receipt.file_sha256,
            "failed upgrade changed the original v1 file");
    const auto upgraded = ProjectStore::save(path, document.snapshot(),
        SaveOptions{.expected_destination_sha256 = old_receipt.file_sha256});
    require(upgraded.backup_path.has_value(), "upgrade did not preserve the original file");
    cleanup.files.push_back(*upgraded.backup_path);
    require(ProjectStore::file_sha256(*upgraded.backup_path) == old_receipt.file_sha256,
            "upgrade backup is not byte-identical to original v1 project");
    const auto backup = ProjectStore::load(*upgraded.backup_path);
    require(backup.document.snapshot().entities() == original.entities() &&
                ProjectStore::required_format_version(backup.document.snapshot()) == 1,
            "preserved v1 backup was rewritten during upgrade");
    auto loaded = ProjectStore::load(path);
    require(ProjectStore::required_format_version(loaded.document.snapshot()) == 2,
            "upgraded project lost its storage guard");
    loaded.document.undo(loaded.document.revision());
    require(loaded.document.snapshot().entities() == original.entities(),
            "upgraded project cannot undo to original geometry");
}

void test_abandoned_unknown_history_and_forged_navigation() {
    auto document = Document::create();
    put(document, rectangle());
    document.undo(document.revision());
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(Entity{"note", "label", {{"text", "new branch"}},
                                                      false, Json::object()})},
        .message = "branch after undo"});
    auto future_history = document.snapshot();
    auto& abandoned = const_cast<std::vector<RevisionRecord>&>(future_history.history()).at(1);
    abandoned.entities.at("boundary-1").properties["boundary_model_version"] = 999;
    abandoned.entities.at("boundary-1").properties["segments"] = {{"future_payload", true}};
    auto loaded = reopen(future_history);
    require(!loaded.is_editable() && loaded.snapshot().entities() == document.snapshot().entities(),
            "unknown semantics in abandoned history did not preserve the current head read-only");
    require(loaded.snapshot().history().at(1).entities == abandoned.entities,
            "abandoned unknown history payload changed during storage");

    auto malformed_history = document.snapshot();
    auto& malformed = const_cast<std::vector<RevisionRecord>&>(malformed_history.history()).at(1);
    malformed.entities.at("boundary-1").properties["segments"][1]["segment_id"] = "edge-0";
    require_invalid_snapshot_not_published(malformed_history);

    auto upgraded = Document::create({rectangle(false)});
    put(upgraded, rectangle());
    upgraded.undo(upgraded.revision());
    auto fake_apply = upgraded.snapshot();
    auto& fake_record = const_cast<std::vector<RevisionRecord>&>(fake_apply.history()).back();
    fake_record.action = "apply";
    fake_record.source_revision.reset();
    fake_record.undo_stack = {0, 1};
    fake_record.redo_stack.clear();
    require_invalid_snapshot_not_published(fake_apply);

    auto fake_navigation = upgraded.snapshot();
    auto& undo = const_cast<std::vector<RevisionRecord>&>(fake_navigation.history()).back();
    undo.entities.at("boundary-1").properties["vendor_data"] = "not the recorded source";
    require_invalid_snapshot_not_published(fake_navigation);
}

void test_history_navigation_preserves_json_number_representation() {
    for (const bool signed_zero : {false, true}) {
        auto entity = rectangle();
        entity.properties["opaque_number"] = signed_zero ? Json(0.0) : Json(1);
        auto document = Document::create({entity});
        entity.properties["opaque_number"] = 2;
        put(document, entity);
        document.undo(document.revision());
        auto modified = document.snapshot();
        auto& head = const_cast<std::vector<RevisionRecord>&>(modified.history()).back();
        head.entities.at("boundary-1").properties["opaque_number"] = signed_zero ? Json(-0.0) : Json(1.0);
        require_invalid_snapshot_not_published(modified);
    }
}

void test_deleted_identity_cannot_be_recreated_with_stripped_semantics() {
    auto document = Document::create({rectangle()});
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase("boundary-1")}, .message = "delete identified boundary"});
    document = reopen(document.snapshot());
    require_rejected_unchanged(document, rectangle(false),
                              "deleted identified boundary was recreated as anonymous under its old ID");
    require_rejected_unchanged(document, rectangle(), "byte-identical retired boundary was resurrected");
    document.undo(document.revision());
    auto edited = rectangle();
    edited.properties["vendor_data"] = "edit after exact undo";
    put(document, edited);
    document = reopen(document.snapshot());
    require(document.snapshot().entities().at(edited.id) == edited,
            "surviving identity could not be edited after exact undo");
    auto dimension_document = Document::create({rectangle(), dimension()});
    dimension_document.apply(ApplyEntityChanges{.expected_revision = dimension_document.revision(),
        .entity_changes = {EntityChange::erase("dimension-1")}, .message = "delete dimension"});
    dimension_document = reopen(dimension_document.snapshot());
    require_rejected_unchanged(dimension_document,
        Entity{"dimension-1", "label", {{"text", "different kind"}}, false, Json::object()},
        "deleted dimension ID was recreated with another semantic type");
    require_rejected_unchanged(dimension_document, dimension(), "byte-identical retired dimension was resurrected");
}

void test_child_identity_binding_and_exact_reversal() {
    auto document = Document::create({rectangle(), dimension()});
    auto swapped = rectangle();
    std::swap(swapped.properties["segments"][0]["segment_id"],
              swapped.properties["segments"][2]["segment_id"]);
    require_rejected_unchanged(document, swapped, "square edge IDs silently transferred dimension ownership");

    auto lens = rectangle();
    lens.properties["segments"] = Json::array({
        {{"segment_id", "edge-0"}, {"start_vertex_id", "vertex-0"}, {"end_vertex_id", "vertex-1"},
         {"start", {0.0, 0.0}}, {"end", {4.0, 0.0}}, {"sweep_radians", std::numbers::pi}},
        {{"segment_id", "edge-1"}, {"start_vertex_id", "vertex-1"}, {"end_vertex_id", "vertex-0"},
         {"start", {4.0, 0.0}}, {"end", {0.0, 0.0}}, {"sweep_radians", std::numbers::pi}}});
    auto lens_document = Document::create({lens, dimension()});
    auto lens_swap = lens;
    std::swap(lens_swap.properties["segments"][0]["segment_id"],
              lens_swap.properties["segments"][1]["segment_id"]);
    require_rejected_unchanged(lens_document, lens_swap,
                              "two-edge lens swap bypassed ordered endpoint ownership");

    auto moved = rectangle();
    for (auto& edge : moved.properties["segments"])
        for (const auto* key : {"start", "end"}) edge[key][0] = edge[key][0].get<double>() + 10.0;
    put(document, moved);
    auto reversed = reverse_identified_boundary_entity(moved);
    put(document, reversed);
    reversed.properties["vendor_data"] = "edit after reversal";
    put(document, reversed);
    document = reopen(document.snapshot());
    require(document.snapshot().entities().at("boundary-1") == reversed,
            "valid move, reversal, or subsequent edit failed to survive storage");
    put(lens_document, reverse_identified_boundary_entity(lens));
    lens_document = reopen(lens_document.snapshot());
    require(lens_document.is_editable(), "typed lens reversal was rejected");
}

void test_retired_children_and_abandoned_upgrade() {
    auto document = Document::create({rectangle(false)});
    put(document, rectangle());
    document.undo(document.revision());
    put(document, Entity{"note", "label", {{"text", "unrelated branch"}}, false, Json::object()});
    auto anonymous_edit = rectangle(false);
    anonymous_edit.properties["vendor_data"] = "changed";
    require_rejected_unchanged(document, anonymous_edit, "edited anonymous state bypassed abandoned upgrade");
    require_rejected_unchanged(document, rectangle(), "abandoned upgrade child identities were reused");
    auto fresh = upgrade_legacy_boundary_entity(rectangle(false));
    put(document, fresh);
    document = reopen(document.snapshot());
    require(document.is_editable(), "fresh upgrade after abandoned upgrade did not survive storage");

    auto child_document = Document::create({rectangle()});
    auto replaced = rectangle();
    replaced.properties["segments"][0]["segment_id"] = "new-edge-0";
    replaced.properties["segments"][3]["segment_id"] = "new-edge-3";
    replaced.properties["segments"][0]["start_vertex_id"] = "new-vertex-0";
    replaced.properties["segments"][3]["end_vertex_id"] = "new-vertex-0";
    put(child_document, replaced);
    child_document = reopen(child_document.snapshot());
    auto reuse_edge = replaced;
    reuse_edge.properties["segments"][0]["segment_id"] = "edge-0";
    require_rejected_unchanged(child_document, reuse_edge, "retired edge ID was reused after reopen");
    auto reuse_vertex = replaced;
    reuse_vertex.properties["segments"][0]["segment_id"] = "third-edge-0";
    reuse_vertex.properties["segments"][3]["segment_id"] = "third-edge-3";
    reuse_vertex.properties["segments"][0]["start_vertex_id"] = "vertex-0";
    reuse_vertex.properties["segments"][3]["end_vertex_id"] = "vertex-0";
    require_rejected_unchanged(child_document, reuse_vertex, "retired vertex ID was reused with fresh edges");
    child_document.undo(child_document.revision());
    require_rejected_unchanged(child_document, replaced, "abandoned replacement children were reused");
    child_document.redo(child_document.revision());
    require(child_document.snapshot().entities().at("boundary-1") == replaced,
            "exact redo did not restore retired child identities");
}

void test_split_retirement_and_forged_resurrection() {
    auto document = Document::create({rectangle(), dimension()});
    auto split = rectangle();
    auto edges = split.properties["segments"];
    auto first = edges[0];
    auto second = edges[0];
    first["segment_id"] = "split-first";
    first["end"] = {2.0, 0.0};
    first["end_vertex_id"] = "split-vertex";
    second["segment_id"] = "split-second";
    second["start"] = {2.0, 0.0};
    second["start_vertex_id"] = "split-vertex";
    split.properties["segments"] = Json::array({first, second, edges[1], edges[2], edges[3]});
    auto remapped = dimension();
    remapped.properties["target"]["segment_id"] = "split-first";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(split), EntityChange::upsert(remapped)},
        .message = "split and explicitly remap dimension"});
    document = reopen(document.snapshot());
    require(document.snapshot().entities().at("boundary-1") == split, "valid split failed to survive storage");
    document.undo(document.revision());
    document.redo(document.revision());

    auto deleted = Document::create({rectangle()});
    deleted.apply(ApplyEntityChanges{.expected_revision = deleted.revision(),
        .entity_changes = {EntityChange::erase("boundary-1")}, .message = "delete"});
    put(deleted, Entity{"note", "label", {{"text", "branch"}}, false, Json::object()});
    auto forged = deleted.snapshot();
    const_cast<std::vector<RevisionRecord>&>(forged.history()).back().entities.emplace("boundary-1", rectangle());
    require_invalid_snapshot_not_published(forged);
}

void test_strict_metadata_equality_for_all_navigation() {
    for (const auto* field : {"properties", "extensions", "asset"}) {
        for (const auto* navigation : {"undo", "redo", "name"}) {
            for (const bool signed_zero : {false, true}) {
                auto entity = rectangle();
                auto asset = Asset::create("asset-1", "application/octet-stream", {std::byte{1}});
                const auto set_value = [&](Entity& e, Asset& a, Json value) {
                    if (std::string_view(field) == "properties") e.properties["opaque_number"] = value;
                    else if (std::string_view(field) == "extensions") e.extensions["opaque_number"] = value;
                    else a.metadata["opaque_number"] = value;
                };
                set_value(entity, asset, signed_zero ? Json(0.0) : Json(1));
                auto document = Document::create({entity}, {asset});
                // A committed no-op still has real undo/redo navigation and
                // provides the same numeric representation to all three paths.
                put(document, entity);
                if (std::string_view(navigation) == "name")
                    document.apply(NameRevision{document.revision(), "numeric revision"});
                else {
                    document.undo(document.revision());
                    if (std::string_view(navigation) == "redo") document.redo(document.revision());
                }
                auto genuine = reopen(document.snapshot());
                require(genuine.is_editable(), "genuine numeric metadata history failed to reopen");
                auto forged = document.snapshot();
                auto& record = const_cast<std::vector<RevisionRecord>&>(forged.history()).back();
                set_value(record.entities.at("boundary-1"), record.assets.at("asset-1"),
                          signed_zero ? Json(-0.0) : Json(1.0));
                require_invalid_snapshot_not_published(forged);
            }
        }
    }
}

void test_raw_upgrade_preserves_legacy_geometry_and_opaque_fields() {
    auto colliding = rectangle();
    colliding.properties.erase("boundary_model_version");
    auto document = Document::create({colliding});
    require_rejected_unchanged(document, rectangle(), "marker flip interpreted colliding vendor IDs as geometry identity");
    auto partial = rectangle(false);
    partial.properties["segments"][0]["segment_id"] = "opaque-vendor-token";
    auto partial_document = Document::create({partial});
    require_rejected_unchanged(partial_document, rectangle(), "raw upgrade overwrote partial vendor identity metadata");
    for (const bool geometry_change : {false, true}) {
        auto legacy = Document::create({rectangle(false)});
        auto candidate = rectangle();
        if (geometry_change) {
            for (auto& edge : candidate.properties["segments"])
                for (const auto* key : {"start", "end"}) edge[key][0] = edge[key][0].get<double>() + 1.0;
        } else candidate.properties["vendor_data"] = "lost metadata";
        require_rejected_unchanged(legacy, candidate, "raw upgrade changed geometry or opaque metadata");
        put(legacy, upgrade_legacy_boundary_entity(rectangle(false)));
        legacy = reopen(legacy.snapshot());
        require(legacy.is_editable(), "exact metadata-preserving upgrade failed");
    }
}

void erase_boundary(Document& document) {
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase("boundary-1")}, .message = "delete legacy boundary"});
}

void test_legacy_lineage_preserves_v1_without_laundering_identity() {
    auto document = Document::create({rectangle(false)});
    erase_boundary(document);
    require_rejected_unchanged(document, rectangle(), "deleted legacy ID acquired arbitrary identified geometry");
    auto future = rectangle(false);
    future.properties["boundary_model_version"] = 999;
    future.properties["segments"] = {{"future_payload", true}};
    require_rejected_unchanged(document, future, "deleted legacy ID bypassed lineage with a future version");
    put(document, rectangle(false));
    document = reopen(document.snapshot(), 1);
    require(document.is_editable() && ProjectStore::required_format_version(document.snapshot()) == 1,
            "legacy-only reuse lost v1 compatibility");
    const auto upgraded = upgrade_legacy_boundary_entity(rectangle(false));
    require_rejected_unchanged(document, upgraded, "legacy recreation laundered a same-ID upgrade");
    require_rejected_unchanged(document, future, "legacy recreation laundered opaque future identification");

    document.undo(document.revision()); // Undo ordinary recreation.
    document.undo(document.revision()); // Undo the deletion, restoring the original.
    require_rejected_unchanged(document, upgraded, "abandoned ordinary reuse did not retain lineage taint");
    auto rekeyed = upgraded;
    rekeyed.id = "fresh-boundary-id";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::erase("boundary-1"), EntityChange::upsert(rekeyed)},
        .message = "explicitly rekey legacy boundary before identification"});
    document = reopen(document.snapshot());
    require(document.snapshot().entities().at(rekeyed.id) == rekeyed,
            "fresh-ID rekey did not survive save/reopen");
    document.undo(document.revision());
    document.redo(document.revision());
    require(document.snapshot().entities().at(rekeyed.id) == rekeyed,
            "rekey failed exact history navigation");

    auto continuous = Document::create({rectangle(false)});
    erase_boundary(continuous);
    continuous.undo(continuous.revision());
    put(continuous, upgraded);
    continuous = reopen(continuous.snapshot());
    require(continuous.is_editable(), "deletion followed by exact undo incorrectly tainted legacy identity");

    for (const auto* replacement_type : {"label", "room_boundary"}) {
        auto retyped = Document::create({rectangle(false)});
        auto replacement = rectangle(false);
        replacement.type = replacement_type;
        put(retyped, replacement);
        require_rejected_unchanged(retyped, upgraded, "intervening entity type bypassed legacy lineage");
        retyped.undo(retyped.revision());
        require_rejected_unchanged(retyped, upgraded, "abandoned retyping cleared legacy lineage taint");
        retyped = reopen(retyped.snapshot(), 1);
        require(retyped.is_editable(), "legacy-only retyping history was rejected");
    }

    auto unknown = Document::create({rectangle(false)});
    put(unknown, future);
    unknown = reopen(unknown.snapshot());
    require(!unknown.is_editable() && unknown.snapshot().entities().at(future.id) == future,
            "continuous legacy-to-future boundary was not retained read-only");

    auto forged_document = Document::create({rectangle(false)});
    erase_boundary(forged_document);
    put(forged_document, rectangle(false));
    put(forged_document, Entity{"note", "label", {{"text", "ordinary edit"}}, false, Json::object()});
    auto forged = forged_document.snapshot();
    const_cast<std::vector<RevisionRecord>&>(forged.history()).back().entities.at("boundary-1") = upgraded;
    require_invalid_snapshot_not_published(forged);
}
} // namespace

int main() {
    try {
        test_raw_commands_cannot_bypass_identity_validation();
        test_downgrade_is_rejected_but_upgrade_undo_is_valid();
        test_future_boundary_version_is_preserved_read_only();
        test_dimension_references_are_atomic_and_survive_history();
        test_unknown_dimensions_preserve_read_only_without_hiding_invalid_known_data();
        test_v1_upgrade_preserves_original_file_and_reversible_history();
        test_abandoned_unknown_history_and_forged_navigation();
        test_history_navigation_preserves_json_number_representation();
        test_deleted_identity_cannot_be_recreated_with_stripped_semantics();
        test_child_identity_binding_and_exact_reversal();
        test_retired_children_and_abandoned_upgrade();
        test_split_retirement_and_forged_resurrection();
        test_strict_metadata_equality_for_all_navigation();
        test_raw_upgrade_preserves_legacy_geometry_and_opaque_fields();
        test_legacy_lineage_preserves_v1_without_laundering_identity();
        std::cout << "Boundary integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_integrity_tests: " << error.what() << '\n';
        return 1;
    }
}
