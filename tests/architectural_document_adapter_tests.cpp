#include "sketch/architectural_document_adapter.hpp"
#include "sketch/architecture.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_store.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/vertical_levels.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template <typename F>
void rejects(F&& function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid architectural adapter operation accepted");
}

nlohmann::json segment_json(double x0, double y0, double x1, double y1,
                            double sweep = 0.0) {
    return { {"start", {x0, y0}}, {"end", {x1, y1}}, {"sweep_radians", sweep} };
}

nlohmann::json rectangle_json(double width, double height) {
    return nlohmann::json::array({
        segment_json(0.0, 0.0, width, 0.0),
        segment_json(width, 0.0, width, height),
        segment_json(width, height, 0.0, height),
        segment_json(0.0, height, 0.0, 0.0),
    });
}
}

void test_material_assignments() {
    using namespace sketch;
    auto catalog = Entity::create("assembly_model", {{"version", 1},
        {"model", AssemblyModel::create({{"wood", "Wood"}}, {}, {}).to_json()}});
    catalog.id = "material-catalog";
    auto wall = Entity::create("wall", {{"height_m", 3.0}, {"material_assignment",
        {{"version", 1}, {"catalog_id", catalog.id}, {"material_id", "wood"}}}});
    wall.id = "material-wall";
    auto document = Document::create({catalog, wall});
    const auto original = document.snapshot();
    rejects([&] { document.apply(ApplyEntityChanges{document.revision(), {EntityChange::erase(catalog.id)}, {}, "remove catalog"}); });
    auto empty = catalog;
    empty.properties["model"] = AssemblyModel::create({}, {}, {}).to_json();
    rejects([&] { document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(empty)}, {}, "remove referenced material"}); });
    auto invalid = wall;
    invalid.properties["material_assignment"]["version"] = 2;
    rejects([&] { document.apply(ApplyEntityChanges{document.revision(), {EntityChange::upsert(invalid)}, {}, "unknown assignment version"}); });
    require(document.snapshot().entities() == original.entities() && document.revision() == original.revision(),
        "invalid material changes must be atomic");
    auto detached = wall;
    detached.properties.erase("material_assignment");
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(detached), EntityChange::erase(catalog.id)}, {}, "detach and remove catalog"});
    document.undo(document.revision());
    require(document.snapshot().entities() == original.entities(), "material references restore with undo");
    const auto path = std::filesystem::temp_directory_path() / "sketch-material-reference.bldproj";
    (void)ProjectStore::save(path, document.snapshot());
    const auto reopened = ProjectStore::load(path).document;
    require(reopened.snapshot().entities() == original.entities(), "material references save and reopen");
    std::filesystem::remove(path);
}

void test_building_transform_updates_canonical_geometry() {
    using namespace sketch;
    auto beam = encode_building_entity(Beam{
        .id = "beam-transform",
        .start = {1.0, 0.0, 0.0},
        .end = {5.0, 0.0, 0.0},
        .up = {0.0, 0.0, 1.0},
        .width = 0.2,
        .depth = 0.3,
    });
    beam.properties["mark"] = "B-1";
    beam.properties["material_name"] = "steel";
    Document document = Document::create({beam});
    ArchitecturalOperation operation{ArchitecturalAction::transform, beam.id};
    operation.transform = ArchitecturalTransform{1.0, 2.0, 3.0,
                                                  std::numbers::pi / 2.0, 2.0};
    const auto transaction = ArchitecturalTransaction::create(
        "transform-beam", "r0", {beam.id}, {operation}, "Transform beam");

    const auto preview = preview_architectural_transaction(document.snapshot(), transaction);
    const auto preview_beam = std::get<Beam>(decode_building_entity(
        preview.entities().at(beam.id)));
    require(std::abs(preview_beam.start.x - 1.0) < 1e-9 &&
                std::abs(preview_beam.start.y - 4.0) < 1e-9 &&
                std::abs(preview_beam.start.z - 3.0) < 1e-9 &&
                std::abs(preview_beam.end.x - 1.0) < 1e-9 &&
                std::abs(preview_beam.end.y - 12.0) < 1e-9 &&
                std::abs(preview_beam.end.z - 3.0) < 1e-9 &&
                std::abs(preview_beam.width - 0.4) < 1e-9 &&
                std::abs(preview_beam.depth - 0.6) < 1e-9,
            "architectural transform must update the beam's canonical geometry");
    require(preview.entities().at(beam.id).properties.at("mark") == "B-1" &&
                preview.entities().at(beam.id).properties.at("material_name") == "steel" &&
                !preview.entities().at(beam.id).properties.contains("transform"),
            "architectural transform must preserve unrelated metadata without a stale marker");
    require(document.snapshot().entities().at(beam.id) == beam,
            "architectural transform preview must not mutate the source");

    apply_architectural_transaction(document, transaction, document.revision());
    const auto applied = std::get<Beam>(decode_building_entity(document.snapshot().entities().at(beam.id)));
    require(std::abs(applied.end.y - 12.0) < 1e-9,
            "architectural transform command must commit canonical geometry");
    document.undo(document.revision());
    require(std::get<Beam>(decode_building_entity(document.snapshot().entities().at(beam.id))).end.x == 5.0,
            "architectural transform must be undoable");
    document.redo(document.revision());
    const auto redone = std::get<Beam>(decode_building_entity(document.snapshot().entities().at(beam.id)));
    require(std::abs(redone.end.y - 12.0) < 1e-9,
            "architectural transform redo must restore canonical geometry");
}

void test_railing_transform_updates_canonical_geometry() {
    using namespace sketch;
    auto railing = encode_building_entity(Railing{
        .id = "railing-transform",
        .base_position = {1.0, 2.0, 0.5},
        .orientation_radians = 0.25,
        .length = 4.0,
        .height = 1.1,
        .thickness = 0.08,
        .post_spacing = 0.9,
    });
    Document document = Document::create({railing});
    ArchitecturalOperation operation{ArchitecturalAction::transform, railing.id};
    operation.transform = ArchitecturalTransform{2.0, -1.0, 0.5,
                                                  std::numbers::pi / 2.0, 2.0};
    const auto transaction = ArchitecturalTransaction::create(
        "transform-railing", "r0", {railing.id}, {operation}, "Transform railing");
    const auto preview = preview_architectural_transaction(document.snapshot(), transaction);
    const auto transformed = std::get<Railing>(decode_building_entity(
        preview.entities().at(railing.id)));
    require(std::abs(transformed.base_position.x - (-2.0)) < 1e-9 &&
                std::abs(transformed.base_position.y - 1.0) < 1e-9 &&
                std::abs(transformed.base_position.z - 1.5) < 1e-9 &&
                std::abs(transformed.orientation_radians -
                         (0.25 + std::numbers::pi / 2.0)) < 1e-9 &&
                std::abs(transformed.length - 8.0) < 1e-9 &&
                std::abs(transformed.height - 2.2) < 1e-9 &&
                std::abs(transformed.thickness - 0.16) < 1e-9 &&
                std::abs(transformed.post_spacing - 1.8) < 1e-9,
            "architectural railing transform must update canonical geometry");
    require(document.snapshot().entities().at(railing.id) == railing,
            "architectural railing transform preview must not mutate the source");
}

void test_shared_solid_transforms_update_canonical_geometry() {
    using namespace sketch;

    auto wall = Entity::create("wall", {
        {"baseline", segment_json(0.0, 0.0, 4.0, 0.0)},
        {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.5},
    });
    wall.id = "wall-solid-transform";
    auto opening = Entity::create("opening", {
        {"wall_id", wall.id}, {"opening_kind", "door"},
        {"offset_m", 1.0}, {"width_m", 0.9}, {"sill_m", 0.1}, {"height_m", 2.0},
    });
    opening.id = "opening-solid-transform";
    Document wall_document = Document::create({wall, opening});
    ArchitecturalOperation wall_move{ArchitecturalAction::transform, wall.id};
    wall_move.transform = ArchitecturalTransform{1.0, 2.0, 0.5,
                                                 std::numbers::pi / 2.0, 2.0};
    const auto wall_transaction = ArchitecturalTransaction::create(
        "transform-wall-solid", "r0", {wall.id, opening.id}, {wall_move},
        "Transform wall solid");
    const auto wall_preview = preview_architectural_transaction(
        wall_document.snapshot(), wall_transaction);
    const auto& transformed_wall = wall_preview.entities().at(wall.id);
    const auto& transformed_opening = wall_preview.entities().at(opening.id);
    require(std::abs(transformed_wall.properties.at("baseline").at("start").at(0).get<double>() - 1.0) < 1e-9 &&
                std::abs(transformed_wall.properties.at("baseline").at("start").at(1).get<double>() - 2.0) < 1e-9 &&
                std::abs(transformed_wall.properties.at("baseline").at("end").at(0).get<double>() - 1.0) < 1e-9 &&
                std::abs(transformed_wall.properties.at("baseline").at("end").at(1).get<double>() - 10.0) < 1e-9 &&
                std::abs(transformed_wall.properties.at("thickness_m").get<double>() - 0.4) < 1e-9 &&
                std::abs(transformed_wall.properties.at("height_m").get<double>() - 6.0) < 1e-9 &&
                std::abs(transformed_wall.properties.at("elevation_m").get<double>() - 1.5) < 1e-9,
            "wall transform must update its canonical baseline and dimensions");
    require(std::abs(transformed_opening.properties.at("offset_m").get<double>() - 2.0) < 1e-9 &&
                std::abs(transformed_opening.properties.at("width_m").get<double>() - 1.8) < 1e-9 &&
                std::abs(transformed_opening.properties.at("sill_m").get<double>() - 0.2) < 1e-9 &&
                std::abs(transformed_opening.properties.at("height_m").get<double>() - 4.0) < 1e-9,
            "wall transform must scale hosted opening dimensions");
    Wall decoded_wall;
    std::string error;
    require(read_document_wall(transformed_wall, {&transformed_opening}, decoded_wall, error),
            "transformed wall must remain decodable");
    require(std::abs(solid_volume(make_wall(decoded_wall)) - 16.32) < 1e-8,
            "transformed wall solid must preserve exact hosted-opening volume");
    require(wall_document.snapshot().entities().at(wall.id) == wall &&
                wall_document.snapshot().entities().at(opening.id) == opening,
            "wall transform preview must not mutate its source");
    apply_architectural_transaction(wall_document, wall_transaction, wall_document.revision());
    wall_document.undo(wall_document.revision());
    require(wall_document.snapshot().entities().at(wall.id) == wall &&
                wall_document.snapshot().entities().at(opening.id) == opening,
            "wall transform undo must restore the host and its opening");
    wall_document.redo(wall_document.revision());
    require(std::abs(wall_document.snapshot().entities().at(wall.id).properties
                         .at("baseline").at("end").at(1).get<double>() - 10.0) < 1e-9 &&
                std::abs(wall_document.snapshot().entities().at(opening.id).properties
                         .at("width_m").get<double>() - 1.8) < 1e-9,
            "wall transform redo must restore canonical host and opening geometry");

    auto slab = Entity::create("slab", {
        {"boundary", rectangle_json(4.0, 3.0)},
        {"holes", nlohmann::json::array()}, {"thickness_m", 0.25}, {"elevation_m", -0.25},
    });
    slab.id = "slab-solid-transform";
    Document slab_document = Document::create({slab});
    ArchitecturalOperation slab_move{ArchitecturalAction::transform, slab.id};
    slab_move.transform = ArchitecturalTransform{-1.0, 2.0, 1.0, 0.0, 2.0};
    const auto slab_transaction = ArchitecturalTransaction::create(
        "transform-slab-solid", "r0", {slab.id}, {slab_move}, "Transform slab solid");
    const auto slab_preview = preview_architectural_transaction(
        slab_document.snapshot(), slab_transaction);
    Slab decoded_slab;
    require(read_document_slab(slab_preview.entities().at(slab.id), decoded_slab, error),
            "transformed slab must remain decodable");
    require(std::abs(decoded_slab.boundary.front().start.x + 1.0) < 1e-9 &&
                std::abs(decoded_slab.boundary.front().start.y - 2.0) < 1e-9 &&
                std::abs(decoded_slab.boundary.front().end.x - 7.0) < 1e-9 &&
                std::abs(decoded_slab.boundary.front().end.y - 2.0) < 1e-9 &&
                std::abs(decoded_slab.thickness - 0.5) < 1e-9 &&
                std::abs(decoded_slab.elevation - 0.5) < 1e-9 &&
                std::abs(solid_volume(make_slab(decoded_slab)) - 24.0) < 1e-8,
            "slab transform must update footprint, thickness, elevation, and solid volume");

    auto room = Entity::create("room", {
        {"boundary", rectangle_json(4.0, 3.0)},
        {"holes", nlohmann::json::array()}, {"height_m", 2.4}, {"elevation_m", 0.0},
    });
    room.id = "room-solid-transform";
    Document room_document = Document::create({room});
    ArchitecturalOperation room_move{ArchitecturalAction::transform, room.id};
    room_move.transform = ArchitecturalTransform{2.0, 3.0, 0.5,
                                                 std::numbers::pi / 2.0, 1.5};
    const auto room_transaction = ArchitecturalTransaction::create(
        "transform-room-solid", "r0", {room.id}, {room_move}, "Transform room solid");
    const auto room_preview = preview_architectural_transaction(
        room_document.snapshot(), room_transaction);
    RoomVolume decoded_room;
    require(read_document_room(room_preview.entities().at(room.id), decoded_room, error),
            "transformed room must remain decodable");
    require(std::abs(decoded_room.boundary.front().start.x - 2.0) < 1e-9 &&
                std::abs(decoded_room.boundary.front().start.y - 3.0) < 1e-9 &&
                std::abs(decoded_room.boundary.front().end.x - 2.0) < 1e-9 &&
                std::abs(decoded_room.boundary.front().end.y - 9.0) < 1e-9 &&
                std::abs(decoded_room.height - 3.6) < 1e-9 &&
                std::abs(decoded_room.elevation - 0.5) < 1e-9 &&
                std::abs(solid_volume(make_room_volume(decoded_room)) - 97.2) < 1e-8,
            "room transform must update footprint, height, elevation, and solid volume");
}

void test_connected_stair_transform_preserves_links_and_rejects_scale() {
    using namespace sketch;
    auto graph = Entity::create("vertical_levels", {
        {"model", nlohmann::json::parse(
            VerticalLevelGraph({{"ground", 0.0}, {"first", 3.0}},
                               {{"ground-first", "ground", "first"}})
                .serialize())},
    });
    graph.id = "levels-1";
    auto stair = encode_building_entity(StairFlight{
        "stair-connected", {1.0, 2.0, 0.0}, 0.0, 6, 3.0, 0.25, 1.1,
        std::nullopt,
        StairLevelConnection{"levels-1", "ground-first", "ground", "first"}});
    Document document = Document::create({graph, stair});

    ArchitecturalOperation scale{ArchitecturalAction::transform, stair.id};
    scale.transform = ArchitecturalTransform{0.0, 0.0, 0.0, 0.0, 2.0};
    const auto scale_transaction = ArchitecturalTransaction::create(
        "scale-connected-stair", "r0", {stair.id}, {scale}, "Scale connected stair");
    rejects([&] { (void)preview_architectural_transaction(document.snapshot(), scale_transaction); });
    require(document.revision() == 0,
            "a rejected connected-stair scale must not mutate the document");

    ArchitecturalOperation move{ArchitecturalAction::transform, stair.id};
    move.transform = ArchitecturalTransform{2.0, -1.0, 0.5, 0.25, 1.0};
    const auto move_transaction = ArchitecturalTransaction::create(
        "move-connected-stair", "r0", {stair.id}, {move}, "Move connected stair");
    const auto preview = preview_architectural_transaction(document.snapshot(), move_transaction);
    const auto moved = std::get<StairFlight>(decode_building_entity(
        preview.entities().at(stair.id)));
    require(moved.level_connection.has_value() &&
                *moved.level_connection == StairLevelConnection{
                    "levels-1", "ground-first", "ground", "first"} &&
                std::abs(moved.base_position.z - 0.5) < 1e-9,
            "translation and rotation must preserve a connected stair link");
}

void test_wall_duplicate_and_delete_manage_hosted_openings() {
    using namespace sketch;
    auto wall = Entity::create("wall", {{"height_m", 3.0}, {"thickness_m", 0.2}});
    wall.id = "wall-host";
    auto opening = Entity::create("opening", {{"wall_id", wall.id},
                                               {"opening_kind", "door"},
                                               {"width_m", 0.9}, {"height_m", 2.0},
                                               {"offset_m", 1.0}, {"sill_m", 0.0}});
    opening.id = "door-host";
    Document document = Document::create({wall, opening});
    ArchitecturalOperation duplicate{ArchitecturalAction::duplicate, wall.id};
    duplicate.duplicate_id = "wall-copy";
    const auto copy_transaction = ArchitecturalTransaction::create(
        "duplicate-wall", "r0", {wall.id, opening.id}, {duplicate}, "Duplicate wall");
    const auto preview = preview_architectural_transaction(document.snapshot(), copy_transaction);
    require(preview.entities().contains("wall-copy"), "wall duplicate must create the target wall");
    require(preview.entities().contains("wall-copy:door-host") &&
                preview.entities().at("wall-copy:door-host").properties.at("wall_id") == "wall-copy" &&
                preview.entities().at("door-host").properties.at("wall_id") == wall.id,
            "wall duplicate must clone hosted openings and remap only the clone owner");
    apply_architectural_transaction(document, copy_transaction, document.revision());
    require(document.snapshot().entities().contains("wall-copy:door-host"),
            "accepted wall duplicate must persist its hosted opening");

    ArchitecturalTransaction erase_transaction = ArchitecturalTransaction::create(
        "erase-wall", "r1",
        {wall.id, opening.id, "wall-copy", "wall-copy:door-host"},
        {{ArchitecturalAction::erase, "wall-copy"}}, "Delete wall");
    apply_architectural_transaction(document, erase_transaction, document.revision());
    require(!document.snapshot().entities().contains("wall-copy") &&
                !document.snapshot().entities().contains("wall-copy:door-host") &&
                document.snapshot().entities().contains(wall.id) &&
                document.snapshot().entities().contains(opening.id),
            "wall deletion must remove only its owned opening graph");
}

int main() {
    try {
        test_material_assignments();
        test_building_transform_updates_canonical_geometry();
        test_railing_transform_updates_canonical_geometry();
        test_shared_solid_transforms_update_canonical_geometry();
        test_connected_stair_transform_preserves_links_and_rejects_scale();
        test_wall_duplicate_and_delete_manage_hosted_openings();
        using namespace sketch;
        auto wall = Entity::create("wall", {{"height_m", 3.0}});
        wall.id = "wall-a";
        Document document = Document::create({wall});
        const auto before = document.snapshot();
        ArchitecturalOperation edit{ArchitecturalAction::property_edit, "wall-a", {}, {},
                                    {{"height_m", "4.0"}, {"opaque_note", "4m"}}};
        ArchitecturalOperation transform{ArchitecturalAction::transform, "wall-a"};
        transform.transform = ArchitecturalTransform{1, 2, 0, 0.25, 1};
        const auto transaction = ArchitecturalTransaction::create(
            "tx-1", "model-r0", {"wall-a"}, {edit, transform}, "Update wall");
        const auto preview = preview_architectural_transaction(before, transaction);
        require(preview.entities().at("wall-a").properties.at("height_m") == 4.0,
                "preview numeric property edit lost its type");
        require(preview.entities().at("wall-a").properties.at("opaque_note") == "4m",
                "preview opaque property edit lost its string value");
        require(preview.entities().at("wall-a").properties.contains("transform"),
                "preview transform missing");
        require(document_snapshot_digest(document.snapshot()) == document_snapshot_digest(before),
                "preview mutated source document");
        const auto revision = apply_architectural_transaction(document, transaction, document.revision());
        require(revision == 1 && document.snapshot().entities().at("wall-a").properties.at("height_m") == 4.0,
                "architectural transaction did not apply");
        const auto path = std::filesystem::temp_directory_path() / "property-studio-architectural-adapter.bldproj";
        std::filesystem::remove(path);
        (void)ProjectStore::save(path, document.snapshot());
        auto reopened = ProjectStore::load(path).document;
        require(reopened.snapshot().entities().at("wall-a").properties.at("height_m") == 4.0 &&
                    reopened.snapshot().entities().at("wall-a").properties.contains("transform"),
                "architectural transaction did not survive save/reopen");
        std::filesystem::remove(path);
        document.undo(document.revision());
        require(document.snapshot().entities().at("wall-a").properties.at("height_m") == 3.0,
                "architectural transaction did not undo");
        rejects([&] { (void)apply_architectural_transaction(document, transaction, 99); });
        auto stale = ArchitecturalTransaction::create("tx-2", "r", {"missing"},
            {{ArchitecturalAction::select, "missing"}}, "stale");
        rejects([&] { (void)preview_architectural_transaction(document.snapshot(), stale); });
        const auto select = ArchitecturalTransaction::create("tx-3", "r", {"wall-a"},
            {{ArchitecturalAction::select, "wall-a"}}, "Select");
        require(preview_architectural_transaction(document.snapshot(), select).revision() == document.revision(),
                "select-only preview should not create a revision");
        rejects([&] { (void)apply_architectural_transaction(document, select, 99); });

        auto label = Entity::create("label");
        label.id = "label-a";
        auto invalid_phases = Entity::create("model_phases", {{"model",
            ModelPhases::create({label.id}, {label.id}, {}).to_json()}});
        rejects([&] { (void)Document::create({label, invalid_phases}); });

        AssemblyType type{"type-a", "Wall", {{"finish", "paint"}}, {}, {}};
        const auto assemblies = AssemblyModel::create({}, {type},
            {{"instance-a", "type-a", {}, {}, {}}});
        auto assembly_entity = Entity::create("assembly_model", {{"model", assemblies.to_json()}, {"note", "keep"}});
        assembly_entity.id = "assemblies";
        const auto phases = ModelPhases::create({wall.id}, {wall.id}, {{"option-a", "Remove wall", {wall.id}, {}}});
        auto phase_entity = Entity::create("model_phases", {{"model", phases.to_json()}});
        phase_entity.id = "phases";
        auto semantic = Document::create({wall, assembly_entity, phase_entity});
        type.properties["finish"] = "tile";
        semantic.apply(assembly_type_update_command(semantic.snapshot(), "assemblies", type, semantic.revision()));
        semantic.apply(model_phase_selection_command(semantic.snapshot(), "phases", "option-a", semantic.revision()));
        require(semantic.snapshot().entities().at("assemblies").properties.at("note") == "keep", "typed edit lost unrelated properties");
        const auto digest = document_snapshot_digest(semantic.snapshot());
        rejects([&] { semantic.apply(model_phase_selection_command(semantic.snapshot(), "phases", "missing", semantic.revision())); });
        rejects([&] { semantic.apply(assembly_type_update_command(semantic.snapshot(), "phases", type, semantic.revision())); });
        rejects([&] { semantic.apply(ApplyEntityChanges{semantic.revision(), {EntityChange::erase(wall.id)}, {}, "Remove referenced wall"}); });
        auto future = phase_entity;
        future.properties["model"]["version"] = 2;
        rejects([&] { semantic.apply(ApplyEntityChanges{semantic.revision(), {EntityChange::upsert(future)}, {}, "Unknown version"}); });
        require(document_snapshot_digest(semantic.snapshot()) == digest, "rejected semantic edits changed history");
        (void)ProjectStore::save(path, semantic.snapshot());
        auto semantic_reopened = ProjectStore::load(path).document;
        require(ModelPhases::from_json(semantic_reopened.snapshot().entities().at("phases").properties.at("model")).active_alternative() == "option-a", "phase selection did not reopen");
        semantic_reopened.undo(semantic_reopened.revision());
        require(!ModelPhases::from_json(semantic_reopened.snapshot().entities().at("phases").properties.at("model")).active_alternative(), "phase selection did not undo after reopening");
        semantic_reopened.undo(semantic_reopened.revision());
        require(AssemblyModel::from_json(semantic_reopened.snapshot().entities().at("assemblies").properties.at("model")).resolve("instance-a").properties.at("finish") == "paint", "assembly edit did not undo");
        semantic_reopened.redo(semantic_reopened.revision());
        semantic_reopened.redo(semantic_reopened.revision());
        require(semantic_reopened.snapshot().entities() == semantic.snapshot().entities(), "semantic redo did not restore edited models");
        std::filesystem::remove(path);
        std::cout << "architectural document adapter tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
