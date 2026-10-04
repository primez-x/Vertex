#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> decltype(auto) contextual(const std::string& phase, Function function) {
    try { return function(); }
    catch (const std::exception& error) { throw std::runtime_error(phase + ": " + error.what()); }
}
template<class Function> void rejects(Function function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
Entity wall(const char* id, Vec2 start, Vec2 end, double sweep = 0) {
    Entity result{id, "wall", {{"baseline", {{"start", {start.x, start.y}},
        {"end", {end.x, end.y}}, {"sweep_radians", sweep}}}, {"thickness_m", 0.2},
        {"height_m", 3}, {"elevation_m", 0}, {"property_id", "property"},
        {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}}};
    if (sweep != 0) {
        const auto angle = angle_from_radians(sweep);
        result.extensions["curve_input"] = {{"version", 2}, {"construction", "angle"},
            {"measure", angle.original_expression}, {"measure_value", sweep}, {"radians", sweep},
            {"clockwise", sweep < 0}, {"start", {start.x, start.y}}, {"end", {end.x, end.y}},
            {"vendor", "retained original input"}};
    }
    return result;
}
Document fixture(bool curved = true) {
    std::vector<Entity> entities{
        {"property", "property", {{"calculation_workflow", "measurement"}}},
        {"building", "building", {{"property_id", "property"}}},
        {"floor", "floor", {{"building_id", "building"}}},
        {"layer", "layer", {{"floor_id", "floor"}}},
        wall("bottom", {0, 0}, {4, 0}, curved ? 0.6 : 0),
        wall("right", {4, 0}, {4, 3}), wall("top", {4, 3}, {0, 3}), wall("left", {0, 3}, {0, 0}),
        {"opening", "opening", {{"wall_id", "bottom"}, {"offset_m", 0.4}, {"width_m", 0.5},
            {"sill_m", 0.4}, {"height_m", 1}}}};
    const auto initial = Document::create(entities);
    const auto measured = contextual("fixture original exterior derivation", [&] {
        return derive_exterior_wall_measurement(initial.snapshot(), {"bottom", "right", "top", "left"}); });
    IdentifiedBoundary outline{"area", "measurement_boundary", {}};
    for (std::size_t index = 0; index < measured.boundary.size(); ++index)
        outline.segments.push_back({"edge-" + std::to_string(index), "vertex-" + std::to_string(index),
            "vertex-" + std::to_string((index + 1) % measured.boundary.size()), measured.boundary[index]});
    auto owner = encode_identified_boundary_entity(outline);
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
        owner.properties[key] = entities[4].properties.at(key);
    owner.properties["wall_measurement_source"] = measured.source;
    owner.extensions["vendor"] = "retained measured owner";
    entities.push_back(owner);
    auto consumer = owner; consumer.id = "consumer"; entities.push_back(consumer);
    BoundaryDimension dimension{"dimension", "area", outline.segments.front().segment_id, {2, -0.5}};
    dimension.placement = BoundaryDimensionPlacement::automatic;
    dimension.automatic_placement_version = 2;
    entities.push_back(encode_boundary_dimension_entity(dimension));
    return Document::create(entities);
}
ExteriorSegmentResizeIntent intent_for(const DocumentSnapshot& source, bool curved = true,
    BoundaryFixedEndpoint fixed = BoundaryFixedEndpoint::start, bool chain = true, bool connected = true) {
    const auto boundary = decode_identified_boundary_entity(source.entities().at("area"));
    const auto selected = std::find_if(boundary.segments.begin(), boundary.segments.end(),
        [&](const auto& edge) { return !curved || edge.segment.sweep_radians != 0; });
    require(selected != boundary.segments.end(), "fixture requires selected measured edge");
    return {"area", selected->segment_id, parse_quantity("16 ft 4 in"), fixed, chain, connected};
}
ApplyBoundaryConstraintChanges command_for(const DocumentSnapshot& source, const ExteriorSegmentResizeIntent& intent) {
    ApplyBoundaryConstraintChanges result;
    result.expected_revision = source.revision();
    result.message = "Resize measured exterior edge";
    result.exterior_segment_resize = intent;
    result.exterior_source_completion = true;
    const auto case_name = intent.boundary_id + "/" + intent.segment_id +
        " anchor=" + (intent.fixed_endpoint == BoundaryFixedEndpoint::start ? "start" : "end") +
        " chain=" + (intent.move_boundary_chain ? "true" : "false") +
        " connected=" + (intent.move_connected_objects ? "true" : "false") +
        " entry=" + intent.exact_length.original_expression;
    const auto physical = contextual("physical inverse " + case_name, [&] {
        return exterior_segment_resize_physical_entities(source.entities(), intent); });
    result.exterior_source_edits = contextual("consumer redraw derivation " + case_name, [&] {
        return exterior_wall_measurement_source_updates(source.entities(), physical); });
    return result;
}
Json extracted(const std::filesystem::path& root, const DocumentSnapshot& snapshot) {
    const auto destination = root / make_stable_id();
    extract_project(snapshot, destination);
    std::ifstream input(destination / "project.json");
    return Json::parse(input);
}
std::string fixture_digest(const DocumentSnapshot& snapshot, unsigned format, const Json* altered_proof = nullptr) {
    Json manifest{{"format_version", format}, {"document_id", snapshot.document_id()},
        {"head_revision", snapshot.revision()}, {"saved_revision", snapshot.revision()},
        {"named_revisions", snapshot.named_revisions()}, {"history", Json::array()}};
    for (const auto& revision : snapshot.history()) {
        require(revision.assets.empty() && !revision.boundary_geometry_edit && !revision.boundary_translation &&
            !revision.boundary_transform && !revision.boundary_translations && !revision.boundary_transforms,
            "digest fixture contains ordinary states and the intended resize proof only");
        Json row{{"revision", revision.revision}, {"parent_revision", revision.parent_revision},
            {"source_revision", revision.source_revision}, {"action", revision.action}, {"name", revision.name},
            {"undo_stack", revision.undo_stack}, {"redo_stack", revision.redo_stack},
            {"entities", Json::array()}, {"assets", Json::array()}};
        if (revision.boundary_constraint_changes)
            row["boundary_constraint_changes"] = altered_proof ? *altered_proof :
                command_to_json(Command{*revision.boundary_constraint_changes});
        for (const auto& [id, entity] : revision.entities)
            row["entities"].push_back({{"id", id}, {"type", entity.type}, {"required", entity.required},
                {"properties", entity.properties}, {"extensions", entity.extensions}});
        manifest["history"].push_back(std::move(row));
    }
    const auto encoded = manifest.dump();
    return sha256_hex(std::as_bytes(std::span<const char>(encoded.data(), encoded.size())));
}
void rewrite_fixture(const std::filesystem::path& path, const DocumentSnapshot& snapshot,
    unsigned format, const Json* altered_proof = nullptr) {
    sqlite3* database{};
    require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK, "open test-owned resize proof database");
    if (altered_proof) {
        sqlite3_stmt* statement{};
        require(sqlite3_prepare_v2(database,
            "UPDATE revisions SET boundary_constraint_changes_json=? WHERE boundary_constraint_changes_json IS NOT NULL",
            -1, &statement, nullptr) == SQLITE_OK, "prepare independently valid tampered proof");
        const auto encoded = altered_proof->dump();
        sqlite3_bind_text(statement, 1, encoded.c_str(), -1, SQLITE_TRANSIENT);
        const auto result = sqlite3_step(statement); sqlite3_finalize(statement);
        require(result == SQLITE_DONE, "replace retained resize proof");
    }
    const auto sql = "PRAGMA user_version=" + std::to_string(format) +
        "; UPDATE metadata SET value='" + std::to_string(format) + "' WHERE key='format_version'; " +
        "UPDATE metadata SET value='" + fixture_digest(snapshot, format, altered_proof) + "' WHERE key='logical_digest';";
    const auto result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(database);
    require(result == SQLITE_OK, "recompute valid logical digest for proof fixture");
}
void exact_history(const DocumentSnapshot& left, const DocumentSnapshot& right) {
    require(left.entities() == right.entities() && left.history().size() == right.history().size(),
        "save/reopen retains current entities and complete history");
    for (std::size_t index = 0; index < left.history().size(); ++index) {
        const auto& a = left.history()[index]; const auto& b = right.history()[index];
        require(a.entities == b.entities && a.assets == b.assets && a.action == b.action &&
            a.undo_stack == b.undo_stack && a.redo_stack == b.redo_stack &&
            a.parent_revision == b.parent_revision && a.source_revision == b.source_revision &&
            a.boundary_constraint_changes.has_value() == b.boundary_constraint_changes.has_value(),
            "retained history and Undo/Redo authority must remain exact");
        if (a.boundary_constraint_changes)
            require(command_to_json(Command{*a.boundary_constraint_changes}).dump() ==
                    command_to_json(Command{*b.boundary_constraint_changes}).dump(),
                "entered quantity and complete envelope13 proof must roundtrip exactly");
    }
}
void roundtrip(const std::filesystem::path& root, const DocumentSnapshot& snapshot) {
    require(ProjectStore::required_format_version(snapshot) == 39, "retained resize proof requires native39");
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, snapshot);
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable(), "supported resize proof must reopen editable");
    exact_history(snapshot, loaded);
    require(extracted(root, loaded).at("exchange_version") == 37, "retained resize proof requires exchange37");
    rewrite_fixture(path, snapshot, 38);
    const auto fingerprint = ProjectStore::file_sha256(path);
    bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::unsupported_format;
    }
    require(refused && ProjectStore::file_sha256(path) == fingerprint,
        "valid recomputed digest cannot downgrade a retained resize proof or mutate source bytes");
}
void proof_and_history(const std::filesystem::path& root) {
    auto document = contextual("proof fixture", [&] { return fixture(); }); const auto before = document.snapshot();
    const auto intent = intent_for(before);
    const auto command = command_for(before, intent);
    const auto encoded = command_to_json(Command{command});
    require(encoded.at("version") == 13 && encoded.at("exterior_segment_resize") == encode_exterior_segment_resize(intent),
        "resize must retain exact typed intent in envelope13");
    require(command_to_json(command_from_json(encoded)).dump() == encoded.dump(), "envelope13 codec must roundtrip exactly");
    const auto preview = contextual("proof preview", [&] { return Document::preview_command(before, command); });
    require(document.snapshot().entities() == before.entities(), "proof preview must not mutate source");
    validate_exterior_segment_resize_result(before.entities(), preview.entities(), intent);
    require(preview.entities().at("opening") == before.entities().at("opening") &&
        wall_measurement_source_current(preview, preview.entities().at("consumer")),
        "resize replay must preserve hosted opening and derive every current consumer");
    contextual("proof Apply", [&] { document.apply(command_from_json(encoded)); });
    const auto after = document.snapshot();
    require(after.revision() == before.revision() + 1 && after.entities() == preview.entities(), "one typed apply must match verified preview");
    contextual("active proof native/exchange roundtrip", [&] { roundtrip(root, after); });
    const auto forged_path = root / "forged-resize-proof.sketch";
    (void)ProjectStore::save(forged_path, after);
    auto forged = encoded; forged["exterior_segment_resize"]["fixed_endpoint"] = "end";
    rewrite_fixture(forged_path, after, 39, &forged);
    const auto forged_fingerprint = ProjectStore::file_sha256(forged_path);
    bool refused{};
    try { (void)ProjectStore::load(forged_path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::integrity_failure;
    }
    require(refused && ProjectStore::file_sha256(forged_path) == forged_fingerprint,
        "valid stored checksum cannot authorize altered retained anchor proof against original consumer geometry");
    contextual("proof Undo", [&] { document.undo(document.revision()); });
    require(document.snapshot().entities() == before.entities(), "Undo must restore exact physical walls, consumers and dimensions");
    contextual("undone proof native/exchange roundtrip", [&] { roundtrip(root, document.snapshot()); });
    contextual("proof Redo", [&] { document.redo(document.revision()); });
    require(document.snapshot().entities() == after.entities(), "Redo must restore exact resize result");
    contextual("redone proof native/exchange roundtrip", [&] { roundtrip(root, document.snapshot()); });
    const auto imported_history = contextual("retained proof fork", [&] { return Document::fork(document.snapshot()).snapshot(); });
    contextual("forked proof native/exchange roundtrip", [&] { roundtrip(root, imported_history); });

    std::vector<Entity> materialized;
    for (const auto& [id, entity] : after.entities()) { (void)id; materialized.push_back(entity); }
    const auto entity_only = contextual("entity-only materialized import", [&] { return Document::create(materialized).snapshot(); });
    require(entity_only.is_editable() && ProjectStore::required_format_version(entity_only) < 39 &&
            extracted(root, entity_only).at("exchange_version").get<unsigned>() < 37 &&
            !entity_only.history().front().boundary_constraint_changes,
        "entity-only materialization retains existing geometry semantics without claiming an absent resize receipt");

    std::vector<EntityChange> removals;
    for (const auto& id : {"area", "consumer", "dimension"}) removals.push_back(EntityChange::erase(id));
    contextual("remove measured consumers", [&] { document.apply(ApplyEntityChanges{document.revision(), removals, {}, "Remove measured consumers"}); });
    contextual("deleted consumers retained proof roundtrip", [&] { roundtrip(root, document.snapshot()); });

    auto changed = command; changed.exterior_segment_resize->exact_length = parse_quantity("17 ft");
    rejects([&] { (void)Document::preview_command(before, changed); }, "altered length intent cannot lend authority to old consumers");
    changed = command; changed.exterior_segment_resize->fixed_endpoint = BoundaryFixedEndpoint::end;
    rejects([&] { (void)Document::preview_command(before, changed); }, "altered anchor intent cannot lend authority to old consumers");
    changed = command; changed.exterior_segment_resize->segment_id = "missing-edge";
    rejects([&] { (void)Document::preview_command(before, changed); }, "missing selected identity must refuse");
    changed = command; changed.exterior_source_edits.front().replacement_segments[0]["start"][0] = 99;
    rejects([&] { (void)Document::preview_command(before, changed); }, "raw altered consumer geometry cannot supply source authority");
    changed = command; changed.physical_entity_changes.push_back(EntityChange::upsert(preview.entities().at("bottom")));
    rejects([&] { (void)Document::preview_command(before, changed); }, "raw physical output cannot borrow resize authority");
    changed = command; changed.exterior_corner_move = ExteriorCornerMoveIntent{"area", "vertex-0", {0, 0}, true};
    rejects([&] { (void)command_to_json(Command{changed}); }, "resize cannot mix corner intent");
    changed = command; changed.wall_split = WallSplitIntent{"bottom", "new-wall", 0.5, "new-seam", {}};
    rejects([&] { (void)command_to_json(Command{changed}); }, "resize cannot mix split intent");
    auto bad = encoded; bad["exterior_segment_resize"]["exact_length"]["exact_metres"]["numerator"] = 1;
    rejects([&] { (void)command_from_json(bad); }, "altered exact rational must fail receipt decoding");
    bad = encoded; bad["unrecognized_authority"] = true;
    rejects([&] { (void)command_from_json(bad); }, "envelope13 must reject unknown fields");
    bad = encoded; bad["version"] = 8;
    rejects([&] { (void)command_from_json(bad); }, "old dialect must not accept new resize authority");
    changed = command; changed.measured_source_completion = true;
    const auto mixed = command_to_json(Command{changed});
    require(mixed.at("version") == 13 && command_to_json(command_from_json(mixed)) == mixed,
        "late measured completion must retain envelope13 and the exact resize intent");
    require(contextual("resize with measured completion replay", [&] { return Document::preview_command(before, changed); }).entities() == preview.entities(),
        "empty measured completion must not change source resize replay");
}
void anchors_and_modes() {
    for (const bool curved : {false, true}) for (const bool chain : {false, true}) {
        const auto case_name = std::string(curved ? "curved" : "straight") +
            (chain ? " start chain=true connected=false" : " end chain=false connected=true");
        contextual("anchor/movement matrix " + case_name, [&] {
        const auto document = fixture(curved); const auto before = document.snapshot();
        const auto intent = intent_for(before, curved, chain ? BoundaryFixedEndpoint::start : BoundaryFixedEndpoint::end,
            chain, !chain);
        const auto preview = Document::preview_command(before, command_for(before, intent));
        validate_exterior_segment_resize_result(before.entities(), preview.entities(), intent);
        require(wall_measurement_source_current(preview, preview.entities().at("area")),
            "straight/curved start/end and distinct movement choices must derive current measured geometry");
        });
    }
}
void frozen_related_proof(const std::filesystem::path& root) {
    const auto base = fixture().snapshot();
    const auto selected = intent_for(base, true, BoundaryFixedEndpoint::start, false, true);
    const auto boundary = decode_identified_boundary_entity(base.entities().at("area"));
    const auto curve = std::find_if(boundary.segments.begin(), boundary.segments.end(),
        [&](const auto& edge) { return edge.segment_id == selected.segment_id; });
    MeasurementLinework stroke; stroke.stroke_id = "dependent-stroke"; stroke.anchor = curve->segment.end;
    ConstructionReceipt receipt; receipt.segment_id = "stroke-edge"; receipt.kind = BoundaryConstructionKind::line_to_point;
    receipt.start = stroke.anchor; receipt.chord_end = Vec2{stroke.anchor.x, stroke.anchor.y + 1};
    stroke.edges.push_back({receipt.segment_id, "stroke-start", "stroke-end", receipt});
    PersistentConstraint join; join.id = "measured-join"; join.relation = ConstraintRelationKind::coincident;
    join.bindings = {{"area", WallEndpointRole::end, curve->segment_id, curve->end_vertex_id},
        {stroke.stroke_id, WallEndpointRole::start, receipt.segment_id, "stroke-start"}};
    std::vector<Entity> entities;
    for (const auto& [id, entity] : base.entities()) { (void)id; entities.push_back(entity); }
    entities.push_back({stroke.stroke_id, "measurement_linework", {{"model", encode_measurement_linework_model(stroke)},
        {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}}, true});
    entities.push_back(encode_constraint_entity(join));
    auto document = Document::create(entities); const auto before = document.snapshot();
    ConstraintAuthoringIntent intent; intent.exterior_segment_resize = selected;
    const auto preview = preview_constraint_authoring(before, intent);
    require(preview.accepted(), "explicit-only measured dependent must follow a related=true resize");
    const auto candidate = preview_constraint_authoring_snapshot(before, preview);
    const auto proof = *candidate.history().back().boundary_constraint_changes;
    require(!proof.measured_stroke_edits.empty() && candidate.entities().at(stroke.stroke_id) != before.entities().at(stroke.stroke_id),
        "true resize proof must retain actual explicit dependent movement");
    auto frozen = proof; frozen.exterior_segment_resize->move_connected_objects = false;
    rejects([&] { (void)command_to_json(Command{frozen}); }, "false related choice must refuse retained dependent edits at serialization");
    rejects([&] { (void)Document::preview_command(before, frozen); }, "false related choice must refuse typed dependent edits during actual replay");
    auto forged = command_to_json(Command{proof});
    forged["exterior_segment_resize"]["move_connected_objects"] = false;
    rejects([&] { (void)command_from_json(forged); }, "flipping only related choice must reject strict persisted proof admission");
    document.apply(proof);
    const auto after = document.snapshot();
    const auto path = root / "forged-frozen-related-proof.sketch";
    (void)ProjectStore::save(path, after);
    rewrite_fixture(path, after, 39, &forged);
    const auto fingerprint = ProjectStore::file_sha256(path);
    bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::integrity_failure;
    }
    require(refused && ProjectStore::file_sha256(path) == fingerprint && document.snapshot().entities() == after.entities(),
        "valid checksum cannot authorize moving frozen explicitly related geometry or mutate the stored proof");
}
} // namespace
int main() {
    testing::noninteractive_errors();
    const auto root = std::filesystem::temp_directory_path() / ("vertex-exterior-resize-proof-" + make_stable_id());
    std::filesystem::create_directory(root);
    try {
        contextual("proof and retained history", [&] { proof_and_history(root); });
        contextual("anchor and movement cases", [&] { anchors_and_modes(); });
        contextual("frozen explicitly related proof", [&] { frozen_related_proof(root); });
        std::filesystem::remove_all(root);
        std::cout << "Exterior segment resize proof tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "exterior_segment_resize_proof_tests: " << error.what() << "\nEvidence: " << root << '\n';
        return 1;
    }
}
