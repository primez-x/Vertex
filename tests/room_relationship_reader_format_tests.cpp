#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/wall_split.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void rejects(Function function, const char* message) {
    try { function(); } catch (const DocumentError&) { return; }
    throw std::runtime_error(message);
}

Entity wall(std::string id, double start, double end) {
    return {std::move(id), "wall", {{"baseline", {{"start", {start, 0}}, {"end", {end, 0}},
            {"sweep_radians", 0}}}, {"thickness_m", 0.2}, {"height_m", 2.7}, {"elevation_m", 0}},
        false, Json::object()};
}
Json reference(std::string id, std::vector<std::string> members = {}) {
    Json result{{"id", std::move(id)}, {"kind", "architectural_wall"}};
    if (!members.empty()) result["wall_members"] = std::move(members);
    return result;
}
Entity relationships(Json references, unsigned version = 2, Json relations = Json::array(),
                     std::string id = "relationships") {
    return {std::move(id), "room_relationships", {{"model", {{"schema_version", version},
            {"references", std::move(references)}, {"relations", std::move(relations)}}}}, false,
        {{"vendor_note", "retain exact relationship metadata"}}};
}
Entity chain_record() {
    return relationships(Json::array({reference("wall-a", {"wall-a", "wall-b"})}));
}
std::vector<Entity> chain_entities() {
    return {wall("wall-a", 0, 2), wall("wall-b", 2, 4), chain_record()};
}
Entity area(std::string id, std::string type, double left) {
    Json segments = Json::array();
    const std::vector<std::pair<double, double>> points{{left, 0}, {left + 4, 0}, {left + 4, 3}, {left, 3}};
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto next = (index + 1) % points.size();
        segments.push_back({{"segment_id", id + "-edge-" + std::to_string(index)},
            {"start_vertex_id", id + "-vertex-" + std::to_string(index)},
            {"end_vertex_id", id + "-vertex-" + std::to_string(next)},
            {"start", {points[index].first, points[index].second}},
            {"end", {points[next].first, points[next].second}}, {"sweep_radians", 0}});
    }
    return {std::move(id), std::move(type), {{"boundary_model_version", 1}, {"segments", segments}}, false, Json::object()};
}
Json future_payload() {
    // A future model owns its entire shape. References are deliberately not
    // schema-one arrays or valid live IDs, so premature parsing is observable.
    return {{"schema_version", 99}, {"references", "opaque future references"},
        {"relations", {{"not_a_current_graph", true}}},
        {"future", {"exact", "payload", 1, Json{{"wall_members", {"missing-wall"}}}}}};
}

std::string manifest_digest(const DocumentSnapshot& snapshot, unsigned format,
                           const Json* historical_model = nullptr, const Json* changed_after = nullptr) {
    Json manifest{{"format_version", format}, {"document_id", snapshot.document_id()},
        {"head_revision", snapshot.revision()}, {"saved_revision", snapshot.revision()},
        {"named_revisions", snapshot.named_revisions()}, {"history", Json::array()}};
    for (const auto& revision : snapshot.history()) {
        require(!revision.boundary_geometry_edit && !revision.boundary_transform &&
                !revision.boundary_translation &&
                !revision.boundary_transforms && !revision.boundary_translations && revision.assets.empty(),
            "format fixture supports ordinary entity history and independent split proof only");
        Json row{{"revision", revision.revision}, {"parent_revision", revision.parent_revision},
            {"source_revision", revision.source_revision}, {"action", revision.action}, {"name", revision.name},
            {"undo_stack", revision.undo_stack}, {"redo_stack", revision.redo_stack},
            {"entities", Json::array()}, {"assets", Json::array()}};
        if (revision.boundary_constraint_changes) {
            require(revision.boundary_constraint_changes->wall_split.has_value(), "history fixture must contain only the intended split proof");
            row["boundary_constraint_changes"] = command_to_json(Command{*revision.boundary_constraint_changes});
        }
        for (const auto& [id, entity] : revision.entities) {
            auto properties = entity.properties;
            if (historical_model && id == "relationships")
                properties["model"] = changed_after && revision.revision > 0 ? *changed_after : *historical_model;
            row["entities"].push_back({{"id", id}, {"type", entity.type}, {"required", entity.required},
                {"properties", properties}, {"extensions", entity.extensions}});
        }
        manifest["history"].push_back(std::move(row));
    }
    const auto encoded = manifest.dump();
    return sha256_hex(std::as_bytes(std::span<const char>(encoded.data(), encoded.size())));
}

void rewrite_storage(const std::filesystem::path& path, const DocumentSnapshot& snapshot,
                     unsigned format, const Json* historical_model = nullptr, const Json* changed_after = nullptr) {
    sqlite3* database{};
    require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK, "open test-owned format fixture");
    if (historical_model) {
        std::optional<Json> original_properties;
        for (const auto& revision : snapshot.history()) {
            const auto found = revision.entities.find("relationships");
            if (found != revision.entities.end()) { original_properties = found->second.properties; break; }
        }
        require(original_properties.has_value(), "historical fixture needs a retained relationship owner");
        auto properties = *original_properties;
        properties["model"] = *historical_model;
        const auto encoded = properties.dump();
        sqlite3_stmt* statement{};
        require(sqlite3_prepare_v2(database,
            "UPDATE revision_entities SET properties_json=? WHERE id='relationships'", -1,
            &statement, nullptr) == SQLITE_OK, "prepare exact historical future payload");
        sqlite3_bind_text(statement, 1, encoded.c_str(), -1, SQLITE_TRANSIENT);
        const auto result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        require(result == SQLITE_DONE, "write exact historical future payload");
        if (changed_after) {
            properties["model"] = *changed_after;
            const auto changed_encoded = properties.dump();
            require(sqlite3_prepare_v2(database,
                "UPDATE revision_entities SET properties_json=? WHERE revision>0 AND id='relationships'", -1,
                &statement, nullptr) == SQLITE_OK, "prepare independent future-state tamper");
            sqlite3_bind_text(statement, 1, changed_encoded.c_str(), -1, SQLITE_TRANSIENT);
            const auto changed_result = sqlite3_step(statement);
            sqlite3_finalize(statement);
            require(changed_result == SQLITE_DONE, "write future-state tamper");
        }
    }
    const auto sql = "PRAGMA user_version=" + std::to_string(format) +
        "; UPDATE metadata SET value='" + std::to_string(format) + "' WHERE key='format_version'; " +
        "UPDATE metadata SET value='" + manifest_digest(snapshot, format, historical_model, changed_after) + "' WHERE key='logical_digest';";
    const auto result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(database);
    require(result == SQLITE_OK, "rewrite reader marker with independently valid logical digest");
}

Json exchange(const std::filesystem::path& root, const DocumentSnapshot& snapshot) {
    const auto directory = root / make_stable_id();
    extract_project(snapshot, directory);
    std::ifstream input(directory / "project.json");
    return Json::parse(input);
}
void require_exact_history(const DocumentSnapshot& source, const DocumentSnapshot& loaded) {
    require(source.entities() == loaded.entities() && source.history().size() == loaded.history().size(),
        "native reopen must retain current entities and complete history");
    for (std::size_t index = 0; index < source.history().size(); ++index) {
        const auto& left = source.history()[index];
        const auto& right = loaded.history()[index];
        require(left.entities == right.entities && left.assets == right.assets && left.action == right.action &&
                left.parent_revision == right.parent_revision && left.source_revision == right.source_revision &&
                left.undo_stack == right.undo_stack && left.redo_stack == right.redo_stack,
            "native reopen must retain exact ordinary entities, ancestry and Undo/Redo authority");
    }
}
void roundtrip(const std::filesystem::path& root, const DocumentSnapshot& snapshot) {
    require(snapshot.is_editable() && ProjectStore::required_format_version(snapshot) == 38,
        "known logical wall-chain relationships require native reader38 across retained history");
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, snapshot);
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable(), "known wall-chain relationship history must reopen editable");
    require_exact_history(snapshot, loaded);
    require(exchange(root, snapshot).at("exchange_version") == 36,
        "logical wall-chain relationship history requires exchange36");
    rewrite_storage(path, snapshot, 37);
    const auto fingerprint = ProjectStore::file_sha256(path);
    bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::unsupported_format;
    }
    require(refused && ProjectStore::file_sha256(path) == fingerprint,
        "valid recomputed digest cannot downgrade logical relationship history to reader37 or mutate source bytes");
}

void test_known_chain_history(const std::filesystem::path& root) {
    roundtrip(root, Document::create(chain_entities()).snapshot()); // entity-only imported state
    auto document = Document::create({wall("wall-a", 0, 2), wall("wall-b", 2, 4)});
    document.apply(ApplyEntityChanges{0, {EntityChange::upsert(chain_record())}, {}, "Add logical wall chain"});
    roundtrip(root, document.snapshot());
    document.undo(document.revision());
    roundtrip(root, document.snapshot()); // chain retained only in undone history
    document.redo(document.revision());
    document.apply(ApplyEntityChanges{document.revision(), {EntityChange::erase("relationships")}, {}, "Delete chain record"});
    roundtrip(root, document.snapshot()); // chain retained only in deleted history
    const auto sticky = relationships(Json::array({reference("wall-a")}));
    roundtrip(root, Document::create({wall("wall-a", 0, 2), sticky}).snapshot());
}

void test_future_models(const std::filesystem::path& root) {
    auto future = chain_record();
    future.properties["model"] = future_payload();
    const auto document = Document::create({future});
    const auto snapshot = document.snapshot();
    require(!snapshot.is_editable() && ProjectStore::required_format_version(snapshot) == 38 &&
            snapshot.entities().at(future.id) == future,
        "positive unknown relationship models must remain exact, opaque and read-only without parsing references");
    rejects([&] { auto copy = Document::fork(snapshot); copy.apply(ApplyEntityChanges{0,
        {EntityChange::erase(future.id)}, {}, "Try to erase future relationship"}); },
        "future relationship payload must not authorize edits through a fork");
    const auto path = root / "future.sketch";
    (void)ProjectStore::save(path, snapshot);
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(!loaded.is_editable(), "future relationship native reopen must retain read-only protection");
    require_exact_history(snapshot, loaded);
    const auto exported = exchange(root, loaded);
    require(exported.at("exchange_version") == 36 && !exported.at("document").at("editable").get<bool>() &&
            exported.at("revisions")[0].at("entities")[0].at("properties").at("model") == future_payload(),
        "exchange36 must preserve an unknown model exactly and retain read-only authority");

    auto historical = Document::create(chain_entities());
    historical.apply(ApplyEntityChanges{0, {EntityChange::erase("relationships")}, {}, "Delete relationship"});
    const auto deleted = historical.snapshot();
    const auto historical_path = root / "future-history.sketch";
    (void)ProjectStore::save(historical_path, deleted);
    const auto payload = future_payload();
    rewrite_storage(historical_path, deleted, 38, &payload);
    const auto historical_loaded = ProjectStore::load(historical_path).document.snapshot();
    require(!historical_loaded.is_editable() && !historical_loaded.entities().contains("relationships") &&
            historical_loaded.history()[0].entities.at("relationships").properties.at("model") == payload &&
            ProjectStore::required_format_version(historical_loaded) == 38,
        "future relationship retained only in deleted history must prevent editing and keep reader38");
    const auto copied_path = root / "future-history-copy.sketch";
    (void)ProjectStore::save(copied_path, historical_loaded);
    const auto historical_copy = ProjectStore::load(copied_path).document.snapshot();
    require(!historical_copy.is_editable(), "future history must remain read-only after another save/reopen");
    require_exact_history(historical_loaded, historical_copy);
    require(exchange(root, historical_copy).at("exchange_version") == 36,
        "future relationship history remains exchange36 even with no active relationship entity");
    rewrite_storage(historical_path, deleted, 37, &payload);
    bool refused{};
    try { (void)ProjectStore::load(historical_path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::unsupported_format;
    }
    require(refused, "unknown future relationship history must reject a downgraded native marker");

    auto undone = Document::create({wall("wall-a", 0, 2), wall("wall-b", 2, 4)});
    undone.apply(ApplyEntityChanges{0, {EntityChange::upsert(chain_record())}, {}, "Add relationship"});
    undone.undo(undone.revision());
    const auto undo_snapshot = undone.snapshot();
    const auto undo_path = root / "future-undo-history.sketch";
    (void)ProjectStore::save(undo_path, undo_snapshot);
    rewrite_storage(undo_path, undo_snapshot, 38, &payload);
    const auto undo_loaded = ProjectStore::load(undo_path).document.snapshot();
    require(!undo_loaded.is_editable() && !undo_loaded.entities().contains("relationships") &&
            undo_loaded.history()[1].entities.at("relationships").properties.at("model") == payload &&
            ProjectStore::required_format_version(undo_loaded) == 38,
        "future relationship retained only in undone history must remain exact and read-only");
    require(exchange(root, undo_loaded).at("exchange_version") == 36,
        "future undone relationship history must keep exchange36");
}

void test_future_before_unrelated_split(const std::filesystem::path& root) {
    auto source = Document::create({wall("wall-a", 0, 2), wall("split-wall", 8, 14),
        relationships(Json::array({reference("wall-a")}), 1)});
    source.apply(make_wall_split_command(source.snapshot(), {"split-wall", "split-second", 0.5, "split-seam", {}}));
    const auto known = source.snapshot();
    require(ProjectStore::required_format_version(known) == 37 && exchange(root, known).at("exchange_version") == 35,
        "unrelated known split with ordinary schema1 keeps native37/exchange35");
    const auto path = root / "future-before-unrelated-split.sketch";
    (void)ProjectStore::save(path, known);
    const auto opaque = future_payload();
    rewrite_storage(path, known, 38, &opaque);
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(!loaded.is_editable() && loaded.entities().at("relationships").properties.at("model") == opaque &&
            loaded.entities().at("split-wall") == known.entities().at("split-wall") &&
            loaded.entities().at("split-second") == known.entities().at("split-second") &&
            loaded.entities().at("split-seam") == known.entities().at("split-seam") &&
            loaded.history().back().boundary_constraint_changes.has_value(),
        "restoration must preserve an unrelated opaque model while independently replaying every known split proof");
    const auto copy_path = root / "future-split-copy.sketch";
    (void)ProjectStore::save(copy_path, loaded);
    require_exact_history(loaded, ProjectStore::load(copy_path).document.snapshot());
    require(exchange(root, loaded).at("exchange_version") == 36,
        "retained future model plus known split must use exchange36");
    auto tampered = opaque; tampered["future_mutation"] = true;
    rewrite_storage(path, known, 38, &opaque, &tampered);
    const auto fingerprint = ProjectStore::file_sha256(path);
    bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::integrity_failure;
    }
    require(refused && ProjectStore::file_sha256(path) == fingerprint,
        "opaque preservation must not allow a known split proof to mutate a future relationship payload");
}

void test_admission_and_union() {
    const auto a = wall("wall-a", 0, 2);
    const auto b = wall("wall-b", 2, 4);
    const auto chain = chain_record();
    const auto invalid_model = [&](Json model) {
        auto entity = chain; entity.properties["model"] = std::move(model);
        rejects([&] { (void)Document::create({a, b, entity}); }, "malformed known relationship model must reject admission");
    };
    for (const Json version : {Json(0), Json(-1), Json(2.0), Json("2"), Json(true), Json(nullptr)}) {
        auto model = chain.properties.at("model"); model["schema_version"] = version; invalid_model(model);
    }
    auto missing_version = chain.properties.at("model"); missing_version.erase("schema_version"); invalid_model(missing_version);
    for (const Json members : {Json("wall-a"), Json::array(), Json::array({"wall-a"}),
                              Json::array({"wall-b", "wall-a"}), Json::array({"wall-a", "wall-a"})}) {
        auto model = chain.properties.at("model"); model["references"][0]["wall_members"] = members; invalid_model(model);
    }
    auto non_wall = chain.properties.at("model"); non_wall["references"][0]["kind"] = "room_boundary"; invalid_model(non_wall);
    rejects([&] { (void)Document::create({a, chain}); }, "every effective wall member must physically exist");
    auto wrong_type = b; wrong_type.type = "generic";
    rejects([&] { (void)Document::create({a, wrong_type, chain}); }, "every effective member must have architectural wall role");
    auto disconnected = b; disconnected.properties["baseline"]["start"] = {3, 0};
    rejects([&] { (void)Document::create({a, disconnected, chain}); }, "logical wall chain must be analytically connected in declared native order");
    auto degenerate = b; degenerate.properties["baseline"]["end"] = {2, 0};
    rejects([&] { (void)Document::create({a, degenerate, chain}); }, "logical wall chain must not admit a degenerate physical member");
    const Entity opening{"opening", "opening", {{"wall_id", "wall-b"}, {"offset_m", 1.5},
        {"width_m", 1}, {"sill_m", 0}, {"height_m", 2}}, false, Json::object()};
    rejects([&] { (void)Document::create({a, b, opening, chain}); },
        "every chain member's physical semantics must validate hosted openings before admission");

    auto identical = chain; identical.id = "same-definition";
    require(Document::create({a, b, chain, identical}).is_editable(),
        "identical logical definitions in separate records must be compatible");
    const auto conflicting_definition = relationships(Json::array({reference("wall-a")}), 1, Json::array(), "conflicting-definition");
    rejects([&] { (void)Document::create({a, b, chain, conflicting_definition}); },
        "same logical identity must not name different physical membership across records");
    const auto alias = relationships(Json::array({reference("wall-b")}), 1, Json::array(), "physical-alias");
    rejects([&] { (void)Document::create({a, b, chain, alias}); },
        "one physical wall must not belong to different logical references across records");

    const auto room = area("room", "room_boundary", 8);
    const auto measurement = area("measurement", "measurement_boundary", 16);
    const auto other_room = area("other-room", "room_boundary", 24);
    const auto refs = Json::array({Json{{"id", "room"}, {"kind", "room_boundary"}},
                                  Json{{"id", "measurement"}, {"kind", "appraisal_measurement_boundary"}},
                                  Json{{"id", "other-room"}, {"kind", "room_boundary"}}});
    const auto forward = relationships(refs, 1,
        Json::array({Json{{"source_id", "room"}, {"target_id", "measurement"}, {"kind", "follows"}}}), "forward");
    const auto reverse = relationships(refs, 1,
        Json::array({Json{{"source_id", "measurement"}, {"target_id", "room"}, {"kind", "follows"}}}), "reverse");
    rejects([&] { (void)Document::create({room, measurement, other_room, forward, reverse}); },
        "cross-record dependency cycle must reject whole document admission");
    auto duplicate = forward; duplicate.id = "duplicate-relation";
    require(Document::create({room, measurement, other_room, forward, duplicate}).is_editable(),
        "identical references and relations must deduplicate across records without false ambiguity");
    const auto second_driver = relationships(refs, 1,
        Json::array({Json{{"source_id", "room"}, {"target_id", "other-room"}, {"kind", "follows"}}}), "second-driver");
    rejects([&] { (void)Document::create({room, measurement, other_room, forward, second_driver}); },
        "cross-record follows drivers must not become ambiguous");
    const auto independent = relationships(refs, 1,
        Json::array({Json{{"source_id", "room"}, {"target_id", "measurement"}, {"kind", "independent"}}}), "independent");
    rejects([&] { (void)Document::create({room, measurement, other_room, forward, independent}); },
        "cross-record independence must not conflict with a dependency declaration");
}

void test_legacy_wall_aliases(const std::filesystem::path& root) {
    auto legacy_wall = wall("wall-a", 0, 2);
    for (const auto* key : {"thickness", "height", "elevation"}) {
        const auto canonical = std::string(key) + "_m";
        legacy_wall.properties[key] = legacy_wall.properties.at(canonical);
        legacy_wall.properties.erase(canonical);
    }
    const Entity legacy_opening{"legacy-opening", "opening", {{"wall_id", "wall-a"},
        {"offset", 0.25}, {"width", 0.5}, {"sill", 0.2}, {"height", 1.5}}, false,
        {{"vendor_note", "retain legacy opening properties"}}};
    const auto ordinary = relationships(Json::array({reference("wall-a")}), 1);
    auto document = Document::create({legacy_wall, legacy_opening, ordinary});
    const auto note = Entity::create("generic", {{"note", "independent history"}});
    document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(note)}, {}, "Add note"});
    document.undo(document.revision());
    document.redo(document.revision());
    const auto snapshot = document.snapshot();
    require(snapshot.is_editable() && ProjectStore::required_format_version(snapshot) == 1,
        "schema1 wall and hosted opening aliases must remain editable with native floor1");
    const auto path = root / "legacy-wall-aliases.sketch";
    (void)ProjectStore::save(path, snapshot);
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable() && ProjectStore::required_format_version(loaded) == 1,
        "legacy wall aliases must reopen editable without raising the native reader floor");
    require_exact_history(snapshot, loaded);
    for (const auto& revision : loaded.history()) {
        require(revision.entities.at(legacy_wall.id) == legacy_wall &&
                revision.entities.at(legacy_opening.id) == legacy_opening &&
                revision.entities.at(ordinary.id) == ordinary,
            "legacy wall, opening and schema1 relationship JSON must remain exact in every retained state");
    }
    require(exchange(root, loaded).at("exchange_version") == 1,
        "legacy alias schema1 history must retain exchange floor1");
    const auto invalid_wall = [&](Entity candidate) {
        rejects([&] { (void)Document::create({candidate, legacy_opening, ordinary}); },
            "legacy property names must not conceal malformed physical wall semantics");
    };
    auto candidate_wall = legacy_wall; candidate_wall.properties["thickness"] = -0.2; invalid_wall(candidate_wall);
    candidate_wall = legacy_wall; candidate_wall.properties["height"] = "2.7"; invalid_wall(candidate_wall);
    candidate_wall = legacy_wall; candidate_wall.properties.erase("elevation"); invalid_wall(candidate_wall);
    candidate_wall = legacy_wall; candidate_wall.properties["baseline"]["end"] = {0, 0}; invalid_wall(candidate_wall);
    const auto invalid_opening = [&](Entity candidate) {
        rejects([&] { (void)Document::create({legacy_wall, candidate, ordinary}); },
            "legacy property names must not conceal malformed hosted opening semantics");
    };
    auto candidate_opening = legacy_opening; candidate_opening.properties["width"] = 3; invalid_opening(candidate_opening);
    candidate_opening = legacy_opening; candidate_opening.properties["sill"] = 2; invalid_opening(candidate_opening);
    candidate_opening = legacy_opening; candidate_opening.properties["offset"] = "0.25"; invalid_opening(candidate_opening);
    candidate_opening = legacy_opening; candidate_opening.properties.erase("height"); invalid_opening(candidate_opening);
}

void test_ordinary_and_vendor_collisions(const std::filesystem::path& root) {
    const auto ordinary = relationships(Json::array({reference("wall-a")}), 1);
    const auto document = Document::create({wall("wall-a", 0, 2), ordinary});
    require(document.is_editable() && ProjectStore::required_format_version(document.snapshot()) == 1,
        "ordinary singleton schema1 relationship must retain its existing native floor");
    require(exchange(root, document.snapshot()).at("exchange_version") == 1,
        "ordinary schema1 must retain its historical exchange floor");
    auto vendor = Entity::create("generic", {{"model", future_payload()}, {"room_relationships", chain_record().properties.at("model")}});
    vendor.extensions["room_relationships"] = future_payload();
    const auto collision = Document::create({vendor}).snapshot();
    require(collision.is_editable() && ProjectStore::required_format_version(collision) == 1 &&
            exchange(root, collision).at("exchange_version") == 1,
        "unrelated vendor property collisions must remain opaque without claiming relationship authority or reader38");
}
} // namespace

int main(int argc, char** argv) {
    testing::noninteractive_errors();
    const auto root = std::filesystem::temp_directory_path() / ("vertex-room-reader-" + make_stable_id());
    std::filesystem::create_directory(root);
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode.empty() || mode == "--history-only") test_known_chain_history(root);
        if (mode.empty() || mode == "--future-only") test_future_models(root);
        if (mode.empty() || mode == "--future-split-only") test_future_before_unrelated_split(root);
        if (mode.empty() || mode == "--admission-only") test_admission_and_union();
        if (mode.empty() || mode == "--legacy-only") test_legacy_wall_aliases(root);
        if (mode.empty() || mode == "--legacy-only") test_ordinary_and_vendor_collisions(root);
        std::filesystem::remove_all(root);
        std::cout << "Room relationship reader format tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "room_relationship_reader_format: " << error.what() << "\nEvidence: " << root << '\n';
        return 1;
    }
}
