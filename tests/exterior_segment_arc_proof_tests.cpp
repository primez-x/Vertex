#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/wall_split.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>

// Exercise the completed-proof validator separately from admission and codec.
namespace sketch {
void validate_completed_constraint_change(const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    const ApplyBoundaryConstraintChanges& command, bool retained_replay = false);
}

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
            "digest fixture contains ordinary states and the intended arc proof only");
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
    require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK, "open test-owned arc proof database");
    if (altered_proof) {
        sqlite3_stmt* statement{};
        require(sqlite3_prepare_v2(database,
            "UPDATE revisions SET boundary_constraint_changes_json=? WHERE boundary_constraint_changes_json IS NOT NULL",
            -1, &statement, nullptr) == SQLITE_OK, "prepare independently valid tampered proof");
        const auto encoded = altered_proof->dump();
        sqlite3_bind_text(statement, 1, encoded.c_str(), -1, SQLITE_TRANSIENT);
        const auto result = sqlite3_step(statement); sqlite3_finalize(statement);
        require(result == SQLITE_DONE, "replace retained arc proof");
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
                "entered quantity and complete envelope14 proof must roundtrip exactly");
    }
}

ExteriorSegmentArcIntent intent_for(const DocumentSnapshot& source,
    BoundaryConstructionKind kind = BoundaryConstructionKind::arc_chord_angle, bool connected = true) {
    const auto boundary = decode_identified_boundary_entity(source.entities().at("area"));
    const auto& selected = boundary.segments.front();
    ConstructionReceipt receipt;
    receipt.segment_id = selected.segment_id; receipt.kind = kind;
    receipt.start = selected.segment.start; receipt.chord_end = selected.segment.end;
    if (kind == BoundaryConstructionKind::arc_chord_angle) receipt.angle = parse_angle("45.0 deg");
    if (kind == BoundaryConstructionKind::arc_chord_height) receipt.height = parse_quantity("10 in");
    if (kind == BoundaryConstructionKind::arc_chord_length) receipt.arc_length = parse_quantity("15 ft");
    return {"area", selected.segment_id, receipt, connected};
}
ApplyBoundaryConstraintChanges command_for(const DocumentSnapshot& source, const ExteriorSegmentArcIntent& intent) {
    ApplyBoundaryConstraintChanges result;
    result.expected_revision = source.revision(); result.message = "Reconstruct measured exterior arc";
    result.exterior_segment_arc = intent; result.exterior_source_completion = true;
    const auto physical = exterior_segment_arc_physical_entities(source.entities(), intent);
    result.exterior_source_edits = exterior_wall_measurement_source_updates(source.entities(), physical);
    return result;
}
void roundtrip(const std::filesystem::path& root, const DocumentSnapshot& snapshot) {
    require(ProjectStore::required_format_version(snapshot) == 40, "retained arc proof requires native40");
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, snapshot);
    const auto loaded = ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable(), "supported arc proof must reopen editable");
    exact_history(snapshot, loaded);
    require(extracted(root, loaded).at("exchange_version") == 38, "retained arc proof requires exchange38");
    rewrite_fixture(path, snapshot, 39);
    const auto fingerprint = ProjectStore::file_sha256(path);
    bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::unsupported_format;
    }
    require(refused && ProjectStore::file_sha256(path) == fingerprint,
        "valid recomputed digest cannot downgrade retained arc proof or mutate source bytes");
}
void stored_tamper(const std::filesystem::path& root, const DocumentSnapshot& snapshot, const Json& proof) {
    const auto path = root / (make_stable_id() + ".sketch");
    (void)ProjectStore::save(path, snapshot); rewrite_fixture(path, snapshot, 40, &proof);
    const auto fingerprint = ProjectStore::file_sha256(path);
    bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) {
        refused = error.code() == StorageErrorCode::integrity_failure;
    }
    require(refused && ProjectStore::file_sha256(path) == fingerprint,
        "valid recomputed digest cannot authorize altered arc proof or mutate source bytes");
}
void proof_and_history(const std::filesystem::path& root) {
    auto document = fixture(); const auto before = document.snapshot();
    const auto intent = intent_for(before); const auto command = command_for(before, intent);
    const auto encoded = command_to_json(Command{command});
    require(encoded.at("version") == 14 && encoded.at("exterior_segment_arc") == encode_exterior_segment_arc(intent),
        "arc must retain exact measured construction receipt in envelope14");
    require(command_to_json(command_from_json(encoded)).dump() == encoded.dump(), "envelope14 must roundtrip exactly");
    const auto preview = Document::preview_command(before, command);
    require(document.snapshot().entities() == before.entities(), "proof preview must not mutate source");
    validate_exterior_segment_arc_result(before.entities(), preview.entities(), intent);
    require(preview.entities().at("opening") == before.entities().at("opening") &&
        wall_measurement_source_current(preview, preview.entities().at("consumer")),
        "arc replay must preserve hosted opening and derive every current consumer");
    auto incorrect = preview.entities();
    incorrect.at("bottom") = before.entities().at("bottom");
    rejects([&] { validate_completed_constraint_change(before.entities(), incorrect, command); },
        "completed proof must independently reject stale physical source");
    incorrect = preview.entities(); incorrect.at("area") = before.entities().at("area");
    rejects([&] { validate_exterior_segment_arc_result(before.entities(), incorrect, intent); },
        "entire requested outline verification must reject a stale selected owner");
    incorrect = preview.entities();
    incorrect.at("top").properties["baseline"]["start"][1] = 3.2;
    incorrect.at("top").properties["baseline"]["end"][1] = 3.2;
    incorrect.at("right").properties["baseline"]["end"][1] = 3.2;
    incorrect.at("left").properties["baseline"]["start"][1] = 3.2;
    const auto redraws = exterior_wall_measurement_source_updates(preview.entities(), incorrect);
    incorrect = edited_boundary_entities_batch(incorrect, redraws);
    require(wall_measurement_source_current(incorrect, incorrect.at("area")),
        "independent rejection fixture must retain current physical source geometry");
    rejects([&] { validate_exterior_segment_arc_result(before.entities(), incorrect, intent); },
        "current forward geometry with altered unselected edges cannot satisfy entire requested outline");
    document.apply(command_from_json(encoded)); const auto after = document.snapshot();
    require(after.revision() == before.revision() + 1 && after.entities() == preview.entities(),
        "one typed Apply must match verified Preview");
    roundtrip(root, after);
    auto changed = command; changed.exterior_segment_arc->arc_construction.angle = parse_angle("55 deg");
    const auto forged = command_to_json(Command{changed});
    stored_tamper(root, after, forged);
    rejects([&] { (void)Document::preview_command(before, changed); },
        "altered curvature cannot lend authority to old consumer redraws");
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(), "Undo restores exact source, consumers and dimensions");
    roundtrip(root, document.snapshot());
    document.redo(document.revision());
    require(document.snapshot().entities() == after.entities(), "Redo restores exact arc result");
    roundtrip(root, document.snapshot()); roundtrip(root, Document::fork(document.snapshot()).snapshot());
    std::vector<EntityChange> removals;
    for (const auto* id : {"area", "consumer", "dimension"}) removals.push_back(EntityChange::erase(id));
    document.apply(ApplyEntityChanges{document.revision(), removals, {}, "Remove measured consumers"});
    roundtrip(root, document.snapshot());

    changed = command; changed.exterior_segment_arc->segment_id = "missing-edge";
    rejects([&] { (void)Document::preview_command(before, changed); }, "missing selected edge must reject atomically");
    changed = command; changed.exterior_source_edits.front().replacement_segments[0]["start"][0] = 99;
    rejects([&] { (void)Document::preview_command(before, changed); }, "raw altered consumer geometry cannot supply arc authority");
    changed = command; changed.physical_entity_changes.push_back(EntityChange::upsert(preview.entities().at("bottom")));
    rejects([&] { (void)Document::preview_command(before, changed); }, "raw physical output cannot borrow arc authority");
    changed = command; changed.exterior_corner_move = ExteriorCornerMoveIntent{"area", "vertex-0", {0, 0}, true};
    rejects([&] { (void)command_to_json(Command{changed}); }, "arc cannot mix corner intent");
    changed = command; changed.exterior_segment_resize = ExteriorSegmentResizeIntent{
        "area", intent.segment_id, parse_quantity("16 ft"), BoundaryFixedEndpoint::start, true, true};
    rejects([&] { (void)command_to_json(Command{changed}); }, "arc cannot mix resize intent");
    changed = command; changed.wall_split = WallSplitIntent{"bottom", "new-wall", 0.5, "new-seam", {}};
    rejects([&] { (void)command_to_json(Command{changed}); }, "arc cannot mix split intent");
    changed = command; changed.rigid_wall_transform_completion = true;
    rejects([&] { (void)command_to_json(Command{changed}); }, "arc cannot borrow rigid authority");
    changed = command; changed.supplemental_source_completion = true;
    rejects([&] { (void)command_to_json(Command{changed}); }, "arc cannot borrow supplemental authority");
    for (const auto* flag : {"source_completion", "supplemental_source_completion",
        "supplemental_asset_reference_completion", "rigid_wall_transform_completion", "measured_source_completion"}) {
        auto bad = encoded; bad[flag] = 1;
        rejects([&] { (void)command_from_json(bad); }, "arc mode must reject nonboolean flags");
    }
    for (const auto* flag : {"supplemental_source_completion", "supplemental_asset_reference_completion", "rigid_wall_transform_completion"}) {
        auto bad = encoded; bad[flag] = true;
        rejects([&] { (void)command_from_json(bad); }, "arc mode must reject competing completion flags");
    }
    auto bad = encoded; bad["source_completion"] = false;
    rejects([&] { (void)command_from_json(bad); }, "arc source mode must remain true");
    bad = encoded; bad["unrecognized_authority"] = true;
    rejects([&] { (void)command_from_json(bad); }, "envelope14 rejects unknown fields");
    for (const auto version : {8, 11, 12, 13}) {
        bad = encoded; bad["version"] = version;
        rejects([&] { (void)command_from_json(bad); }, "old dialect cannot accept new arc authority");
    }
    bad = encode_exterior_segment_arc(intent); bad["unknown"] = true;
    rejects([&] { (void)decode_exterior_segment_arc(bad); }, "arc intent rejects unknown fields");
    changed = command; changed.exterior_segment_arc->arc_construction.angle->radians += 0.1;
    rejects([&] { (void)command_to_json(Command{changed}); }, "inconsistent exact angle receipt must reject");
    changed = command; changed.measured_source_completion = true;
    const auto measured = command_to_json(Command{changed});
    require(measured.at("version") == 14 && command_to_json(command_from_json(measured)) == measured,
        "late measured completion must retain envelope14 and exact arc intent");
    require(Document::preview_command(before, changed).entities() == preview.entities(),
        "empty measured completion must not alter arc replay");
}
void construction_and_provenance(const std::filesystem::path& root) {
    for (const bool curved : {false, true}) for (const auto kind : {
        BoundaryConstructionKind::arc_chord_angle, BoundaryConstructionKind::arc_chord_height,
        BoundaryConstructionKind::arc_chord_length}) {
        const auto document = fixture(curved); const auto before = document.snapshot();
        const auto intent = intent_for(before, kind);
        const auto preview = Document::preview_command(before, command_for(before, intent));
        validate_exterior_segment_arc_result(before.entities(), preview.entities(), intent);
        require(wall_measurement_source_current(preview, preview.entities().at("area")),
            "all three exact constructions must derive current measured exterior geometry");
        std::vector<Entity> entities;
        for (const auto& [id, entity] : preview.entities()) { (void)id; entities.push_back(entity); }
        const auto materialized = Document::create(entities).snapshot();
        const auto& derivation = preview.entities().at("bottom").extensions.at("curve_input_derivation");
        if (!curved) {
            require(derivation.at("version") == 3 && derivation.at("source_input").is_null() &&
                derivation.at("source_baseline") == before.entities().at("bottom").properties.at("baseline"),
                "line-origin provenance must preserve true original line without fictitious curve input");
            require(derivation.at("operations").front().at("input").at("construction") == "angle",
                "line-origin first operation must archive derived physical angle");
            require(ProjectStore::required_format_version(materialized) == 40 &&
                extracted(root, materialized).at("exchange_version") == 38,
                "entity-only line-origin provenance independently owns native40 and exchange38");
            roundtrip(root, materialized);
        } else {
            require(derivation.at("version") != 3 && ProjectStore::required_format_version(materialized) < 40 &&
                extracted(root, materialized).at("exchange_version").get<unsigned>() < 38,
                "existing curved provenance retains earlier entity-only floor");
        }
    }
}
void frozen_related_proof(const std::filesystem::path& root) {
    auto document = fixture(); const auto before = document.snapshot();
    const auto intent = intent_for(before); const auto command = command_for(before, intent);
    const auto preview = Document::preview_command(before, command);
    document.apply(command); const auto after = document.snapshot();
    // The geometry lanes are forbidden even when their replay would be a no-op.
    const auto boundary = decode_identified_boundary_entity(before.entities().at("consumer"));
    BoundaryGeometryEdit dependent; dependent.boundary_id = "consumer";
    dependent.kind = BoundaryGeometryEditKind::move_vertex;
    dependent.target_id = boundary.segments.front().start_vertex_id;
    dependent.target_position = boundary.segments.front().segment.start;
    auto mixed = command; mixed.boundary_edits.push_back(dependent);
    // Build a correctly typed persisted lane before changing only the freeze flag.
    auto forged = command_to_json(Command{mixed});
    forged["exterior_segment_arc"]["move_connected_objects"] = false;
    rejects([&] { (void)command_from_json(forged); }, "frozen arc persisted proof rejects dependent geometry lane");
    stored_tamper(root, after, forged);
    mixed.exterior_segment_arc->move_connected_objects = false;
    rejects([&] { (void)command_to_json(Command{mixed}); }, "frozen arc serialization rejects boundary geometry lane");
    rejects([&] { (void)Document::preview_command(before, mixed); }, "frozen arc admission rejects boundary geometry lane");
    rejects([&] { validate_completed_constraint_change(before.entities(), preview.entities(), mixed); },
        "frozen arc completed proof replay rejects dependent geometry lane");
    mixed = command; mixed.exterior_segment_arc->move_connected_objects = false;
    ConstraintWallGeometryEdit wall_edit; wall_edit.wall_id = "right";
    mixed.wall_edits.push_back(wall_edit);
    rejects([&] { (void)command_to_json(Command{mixed}); }, "frozen arc rejects dependent wall lane");
    rejects([&] { (void)Document::preview_command(before, mixed); }, "frozen arc admission rejects wall lane");
    rejects([&] { validate_completed_constraint_change(before.entities(), preview.entities(), mixed); },
        "frozen arc completed replay rejects wall lane");
    mixed = command; mixed.exterior_segment_arc->move_connected_objects = false;
    ApplyBoundaryConstraintChanges::MeasuredStrokeEdit stroke; stroke.stroke_id = "dependent-stroke";
    mixed.measured_stroke_edits.push_back(stroke);
    rejects([&] { (void)command_to_json(Command{mixed}); }, "frozen arc rejects dependent measured stroke lane");
    rejects([&] { (void)Document::preview_command(before, mixed); }, "frozen arc admission rejects stroke lane");
    rejects([&] { validate_completed_constraint_change(before.entities(), preview.entities(), mixed); },
        "frozen arc completed replay rejects measured stroke lane");
}
void complete_v3_rigid_and_split_history(const std::filesystem::path& root) {
    auto document=fixture(false);
    document.apply(command_for(document.snapshot(),intent_for(document.snapshot())));
    const auto authored=document.snapshot();
    const PlanarTransform transform{{0,0},0.25,false,false,{2,1}};
    ConstraintAuthoringIntent intent;intent.wall_geometry_move=WallGeometryMoveIntent{};
    for(const auto* id:{"bottom","right","top","left"}) {
        const auto& encoded=authored.entities().at(id).properties.at("baseline");
        const Segment old{{encoded.at("start")[0].get<double>(),encoded.at("start")[1].get<double>()},
            {encoded.at("end")[0].get<double>(),encoded.at("end")[1].get<double>()},encoded.at("sweep_radians").get<double>()};
        const auto next=transform_segment(old,transform);
        // Version-four rigid authority belongs to curved walls. Straight
        // members use their ordinary endpoint move within the same command.
        intent.wall_geometry_move->targets.push_back({id,next.start,next.end,
            old.sweep_radians==0 ? std::nullopt : std::optional<PlanarTransform>{transform}});
    }
    const auto preview=preview_constraint_authoring(authored,intent);
    if(!preview.accepted())for(const auto& diagnostic:preview.diagnostics())std::cerr<<diagnostic<<'\n';
    require(preview.accepted(),"Complete v3 physical perimeter rigid transform must be admitted");
    (void)apply_constraint_authoring(document,preview);
    require(wall_measurement_source_current(document.snapshot(),document.snapshot().entities().at("area")),
        "Complete v3 rigid command must redraw the measured exterior");
    roundtrip(root,document.snapshot());
    document.undo(document.revision());require(document.snapshot().entities()==authored.entities(),"V3 rigid Undo must restore original source archive");
    WallSplitIntent split{"bottom","split-bottom",0.4,"split-seam",{{"area","area-seam","area-second","area-second-dimension"},
        {"consumer","consumer-seam","consumer-second",{}}}};
    document.apply(make_wall_split_command(document.snapshot(),split));
    const auto divided=document.snapshot();
    require(divided.entities().at("bottom").extensions.at("curve_input_derivation").at("version")==3 &&
        divided.entities().at("split-bottom").extensions.at("curve_input_derivation").at("version")==3,
        "Complete wall split command must retain both line-origin archives");
    require(wall_measurement_source_current(divided,divided.entities().at("area")),"V3 split must retain current measured sources");
    roundtrip(root,divided);
    document.undo(document.revision());require(document.snapshot().entities()==authored.entities(),"V3 split Undo must restore exact original source geometry");
    document.redo(document.revision());require(document.snapshot().entities()==divided.entities(),"V3 split Redo must independently replay exact archives");
}
void older_floors(const std::filesystem::path& root) {
    auto document = fixture();
    ExteriorSegmentResizeIntent resize{"area", "edge-0", parse_quantity("16 ft"),
        BoundaryFixedEndpoint::start, true, true};
    const auto physical = exterior_segment_resize_physical_entities(document.snapshot().entities(), resize);
    ApplyBoundaryConstraintChanges command; command.expected_revision = document.revision();
    command.exterior_segment_resize = resize; command.exterior_source_completion = true;
    command.exterior_source_edits = exterior_wall_measurement_source_updates(document.snapshot().entities(), physical);
    require(command_to_json(Command{command}).at("version") == 13, "existing resize dialect remains thirteen");
    document.apply(command);
    require(ProjectStore::required_format_version(document.snapshot()) == 39, "existing resize proof retains native39");
    require(extracted(root, document.snapshot()).at("exchange_version") == 37, "existing resize proof retains exchange37");
    const auto relationship = Entity{"relationships", "room_relationships", {{"model", {
        {"schema_version", 2}, {"references", Json::array({{{"id", "split-wall"}, {"kind", "architectural_wall"}}})},
        {"relations", Json::array()}}}}};
    std::vector<Entity> split_entities{wall("split-wall", {0, 0}, {4, 0}), relationship};
    for (const auto* id : {"property", "building", "floor", "layer"})
        split_entities.push_back(document.snapshot().entities().at(id));
    auto split = Document::create(split_entities);
    const auto split_command = make_wall_split_command(split.snapshot(), {"split-wall", "split-second", 0.5, "split-seam", {}});
    require(command_to_json(split_command).at("version") == 12, "existing wall split dialect remains twelve");
    split.apply(split_command);
    require(ProjectStore::required_format_version(split.snapshot()) == 38 &&
        extracted(root, split.snapshot()).at("exchange_version") == 36,
        "existing split with logical relationship semantics retains native38/exchange36");
}
} // namespace
int main() {
    testing::noninteractive_errors();
    const auto root = std::filesystem::temp_directory_path() / ("vertex-exterior-arc-proof-" + make_stable_id());
    std::filesystem::create_directory(root);
    try {
        contextual("proof and retained history", [&] { proof_and_history(root); });
        contextual("construction and provenance", [&] { construction_and_provenance(root); });
        contextual("frozen dependent lanes", [&] { frozen_related_proof(root); });
        contextual("v3 rigid and split history", [&] { complete_v3_rigid_and_split_history(root); });
        contextual("older proof floors", [&] { older_floors(root); });
        std::filesystem::remove_all(root);
        std::cout << "Exterior segment arc proof tests passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "exterior_segment_arc_proof_tests: " << error.what() << "\nEvidence: " << root << '\n';
        return 1;
    }
}
