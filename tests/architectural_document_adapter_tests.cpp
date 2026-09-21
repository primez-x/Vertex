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
#include <limits>
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

void test_typed_join_commands() {
    using namespace sketch;
    auto a = Entity::create("wall", {{"baseline", segment_json(0, 0, 4, 0)},
        {"height_m", 3.0}, {"thickness_m", 0.2}, {"elevation_m", 0.0}});
    a.id = "join-wall-a";
    auto b = a;
    b.id = "join-wall-b";
    b.properties["baseline"] = segment_json(4, 0, 4, 3);
    auto far = a;
    far.id = "join-wall-far";
    far.properties["baseline"] = segment_json(20, 0, 24, 0);
    auto label = Entity::create("label");
    label.id = "join-label";
    auto opening = Entity::create("opening", {{"wall_id", a.id}, {"opening_kind", "door"},
        {"offset_m", 1.0}, {"width_m", 0.9}, {"sill_m", 0.0}, {"height_m", 2.0}});
    opening.id = "join-door";
    auto roof_a = encode_building_entity(SlopedRoofPanel{.id = "join-roof-a",
        .run = 4, .span = 3, .rise = 1, .pitch_radians = std::atan(0.25), .thickness = 0.2});
    auto roof_b = encode_building_entity(SlopedRoofPanel{.id = "join-roof-b",
        .base_position = {0, 2, 0}, .run = 4, .span = 3, .rise = 1,
        .pitch_radians = std::atan(0.25), .thickness = 0.2});
    auto document = Document::create({a, b, far, label, opening, roof_a, roof_b});
    const auto before = document.snapshot();
    const auto create = [&](std::vector<std::string> ids, ArchitecturalJoinKind kind = ArchitecturalJoinKind::wall) {
        return architectural_join_create_command(document.snapshot(), "typed-join", ids, kind, document.revision());
    };
    rejects([&] { (void)create({}); });
    rejects([&] { (void)create({a.id}); });
    rejects([&] { (void)create({a.id, a.id}); });
    rejects([&] { (void)create({a.id, label.id}); });
    rejects([&] { (void)create({a.id, roof_a.id}); });
    rejects([&] { (void)create({a.id, "missing"}); });
    rejects([&] { (void)create({a.id, far.id}); });
    rejects([&] { (void)create({a.id, b.id}, static_cast<ArchitecturalJoinKind>(99)); });
    rejects([&] { (void)architectural_join_create_command(before, "typed-join", {a.id, b.id}, ArchitecturalJoinKind::wall, 99); });
    rejects([&] { (void)architectural_join_create_command(before, a.id, {a.id, b.id}, ArchitecturalJoinKind::wall, before.revision()); });
    const auto command = create({a.id, b.id});
    require(command.entity_changes.size() == 1 && command.expected_revision == before.revision(), "join creation must be one fenced change");
    require(document.snapshot().entities() == before.entities(), "join admission changed source");
    document.apply(command);
    require(document.revision() == before.revision() + 1, "join must create one revision");
    for (const auto& [id, entity] : before.entities())
        require(document.snapshot().entities().at(id) == entity, "join changed a source object");
    rejects([&] { (void)architectural_join_create_command(document.snapshot(), "other-join", {a.id, b.id}, ArchitecturalJoinKind::wall, document.revision()); });
    rejects([&] { document.apply(command); });
    const auto joined = document.snapshot();
    document.undo(document.revision());
    require(document.snapshot().entities() == before.entities(), "join undo failed");
    document.redo(document.revision());
    require(document.snapshot().entities() == joined.entities(), "join redo failed");
    const auto remove = [&](std::vector<std::string> ids) {
        return architectural_join_remove_command(document.snapshot(), ids, ArchitecturalJoinKind::wall, document.revision());
    };
    rejects([&] { (void)remove({}); });
    rejects([&] { (void)remove({far.id}); });
    rejects([&] { (void)remove({a.id, label.id}); });
    rejects([&] { (void)remove({a.id, roof_a.id}); });
    rejects([&] { (void)architectural_join_remove_command(document.snapshot(), {a.id}, ArchitecturalJoinKind::wall, 99); });
    require(remove({a.id}).entity_changes.size() == 1, "member selection must remove join");
    require(remove({"typed-join"}).entity_changes.size() == 1, "join selection must remove join");
    const auto removal = remove({a.id, b.id, "typed-join", a.id});
    require(removal.entity_changes.size() == 1, "removal must deduplicate joins");
    document.apply(removal);
    require(document.snapshot().entities() == before.entities(), "removal must preserve every source");
    document.undo(document.revision());
    require(document.snapshot().entities() == joined.entities(), "removal undo failed");
    document.redo(document.revision());
    require(document.snapshot().entities() == before.entities(), "removal redo failed");
    document.apply(create({roof_a.id, roof_b.id}, ArchitecturalJoinKind::roof));
    require(document.snapshot().entities().at("typed-join").type == "roof_join", "roof command must create roof join");
    document.apply(architectural_join_remove_command(document.snapshot(), {roof_b.id, "typed-join"}, ArchitecturalJoinKind::roof, document.revision()));
    require(document.snapshot().entities() == before.entities(), "roof removal changed sources");

    // A malformed hosted cut must fail admission even though its host alone
    // can be fused. The generic Document permits incomplete opening geometry.
    auto invalid_opening = opening;
    invalid_opening.properties["width_m"] = -1;
    const auto invalid_host = Document::create({a, b, invalid_opening});
    rejects([&] { (void)architectural_join_create_command(invalid_host.snapshot(), "bad-cut",
        {a.id, b.id}, ArchitecturalJoinKind::wall, invalid_host.revision()); });

    auto distant_roof = roof_b;
    distant_roof.properties["base_position_m"] = {0, 20, 0};
    const auto disconnected_roofs = Document::create({roof_a, distant_roof});
    rejects([&] { (void)architectural_join_create_command(disconnected_roofs.snapshot(), "bad-roofs",
        {roof_a.id, roof_b.id}, ArchitecturalJoinKind::roof, disconnected_roofs.revision()); });

    // Raw sources touch, but valid organization places the second pair on an
    // upper level. Prove the resolved separation itself, not an invalid-tree
    // rejection, and retain same-level acceptance coverage.
    const VerticalLevelGraph graph({{"ground", 0}, {"upper", 10}}, {{"storey", "ground", "upper"}});
    auto levels = Entity::create("vertical_levels", {{"model", nlohmann::json::parse(graph.serialize())}});
    levels.id = "join-levels";
    auto property = Entity::create("property"); property.id = "join-property";
    auto building = Entity::create("building", {{"property_id", property.id}}); building.id = "join-building";
    const auto floor = [&](std::string id, std::string level_id) {
        auto entity = Entity::create("floor", {{"building_id", building.id},
            {"vertical_level_binding", {{"version", 1}, {"graph_id", levels.id}, {"level_id", std::move(level_id)}}}});
        entity.id = std::move(id);
        return entity;
    };
    const auto ground_floor = floor("join-ground-floor", "ground");
    const auto upper_floor = floor("join-upper-floor", "upper");
    auto ground_layer = Entity::create("layer", {{"floor_id", ground_floor.id}}); ground_layer.id = "join-ground-layer";
    auto upper_layer = Entity::create("layer", {{"floor_id", upper_floor.id}}); upper_layer.id = "join-upper-layer";
    const auto place = [&](Entity entity, const std::string& layer_id) {
        entity.properties["layer_id"] = layer_id;
        entity.properties["vertical_placement"] = {{"version", 1}, {"mode", "level"}, {"offset_m", 0.0}};
        return entity;
    };
    const auto hierarchy = std::vector<Entity>{property, building, ground_floor, upper_floor,
        ground_layer, upper_layer, levels};
    auto placed_entities = hierarchy;
    placed_entities.insert(placed_entities.end(), {
        place(a, ground_layer.id), place(b, upper_layer.id),
        place(roof_a, ground_layer.id), place(roof_b, upper_layer.id)});
    const auto placed = Document::create(std::move(placed_entities));
    const auto placed_snapshot = placed.snapshot();
    const auto resolved_upper_wall = resolve_vertical_placement(
        placed_snapshot, placed_snapshot.entities().at(b.id));
    const auto resolved_upper_roof = resolve_vertical_placement(
        placed_snapshot, placed_snapshot.entities().at(roof_b.id));
    require(std::abs(resolved_upper_wall.properties.at("elevation_m").get<double>() - 10.0) < 1e-9,
        "upper wall fixture must resolve to the bound level");
    require(std::abs(resolved_upper_roof.properties.at("base_position_m").at(2).get<double>() - 10.0) < 1e-9,
        "upper roof fixture must resolve to the bound level");
    rejects([&] { (void)architectural_join_create_command(placed_snapshot, "bad-level-walls",
        {a.id, b.id}, ArchitecturalJoinKind::wall, placed.revision()); });
    rejects([&] { (void)architectural_join_create_command(placed_snapshot, "bad-level-roofs",
        {roof_a.id, roof_b.id}, ArchitecturalJoinKind::roof, placed.revision()); });

    auto same_level_entities = hierarchy;
    same_level_entities.insert(same_level_entities.end(), {
        place(a, ground_layer.id), place(b, ground_layer.id),
        place(roof_a, ground_layer.id), place(roof_b, ground_layer.id)});
    const auto same_level = Document::create(std::move(same_level_entities));
    require(architectural_join_create_command(same_level.snapshot(), "same-level-walls",
                {a.id, b.id}, ArchitecturalJoinKind::wall, same_level.revision()).entity_changes.size() == 1,
        "same-level connected walls must remain joinable after placement resolution");
    require(architectural_join_create_command(same_level.snapshot(), "same-level-roofs",
                {roof_a.id, roof_b.id}, ArchitecturalJoinKind::roof, same_level.revision()).entity_changes.size() == 1,
        "same-level touching roofs must remain joinable after placement resolution");

    auto c = a;
    c.id = "join-wall-c";
    c.properties["baseline"] = segment_json(20, 0, 20, 4);
    auto multiple = Document::create({a, b, far, c});
    multiple.apply(architectural_join_create_command(multiple.snapshot(), "join-one",
        {a.id, b.id}, ArchitecturalJoinKind::wall, multiple.revision()));
    multiple.apply(architectural_join_create_command(multiple.snapshot(), "join-two",
        {far.id, c.id}, ArchitecturalJoinKind::wall, multiple.revision()));
    const auto multiple_revision = multiple.revision();
    const auto remove_multiple = architectural_join_remove_command(multiple.snapshot(),
        {a.id, "join-one", "join-two", c.id}, ArchitecturalJoinKind::wall, multiple_revision);
    require(remove_multiple.entity_changes.size() == 2, "selection must remove both joins once");
    multiple.apply(remove_multiple);
    require(multiple.revision() == multiple_revision + 1 && multiple.snapshot().entities().size() == 4,
        "multiple join removal must be atomic and retain all members");
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

void test_hosted_assembly_scales_with_wall() {
    using namespace sketch;
    for (const auto kind : {OpeningAssemblyKind::door, OpeningAssemblyKind::window}) {
        for (const double scale : {0.5, 1.0, 2.0}) {
            auto wall = Entity::create("wall", {
                {"baseline", segment_json(0.0, 0.0, 4.0, 0.0)},
                {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.5}});
            wall.id = "assembly-host";
            auto assembly = default_opening_assembly(kind);
            assembly.inset_m = -0.02;
            auto opening = Entity::create("opening", {
                {"wall_id", wall.id}, {"opening_kind", opening_assembly_kind_name(kind)},
                {"offset_m", 1.0}, {"width_m", 0.9}, {"sill_m", 0.1}, {"height_m", 2.0},
                {"opening_assembly", opening_assembly_json(assembly)}, {"mark", "preserved"}});
            opening.id = "assembly-opening";
            auto document = Document::create({wall, opening});
            const auto original = document.snapshot();
            ArchitecturalOperation excessive{ArchitecturalAction::transform, wall.id};
            excessive.transform = ArchitecturalTransform{0.0, 0.0, 0.0, 0.0, 200.0};
            const auto invalid_transaction = ArchitecturalTransaction::create(
                "excessive-assembly", "r0", {wall.id, opening.id}, {excessive}, "Invalid assembly scale");
            rejects([&] { (void)preview_architectural_transaction(original, invalid_transaction); });
            rejects([&] { apply_architectural_transaction(document, invalid_transaction, document.revision()); });
            require(document.revision() == original.revision() &&
                        document.snapshot().entities() == original.entities(),
                    "an out-of-range assembly scale must reject without partial host or opening edits");
            Wall before;
            std::string error;
            require(read_document_wall(wall, {&opening}, before, error), "assembly host decode");
            const auto before_volume = solid_volume(make_opening_assembly(
                before, before.openings.front(), assembly));
            ArchitecturalOperation transform{ArchitecturalAction::transform, wall.id};
            transform.transform = ArchitecturalTransform{1.0, 2.0, 0.5, 0.25, scale};
            const auto transaction = ArchitecturalTransaction::create(
                "scale-assembly", "r0", {wall.id, opening.id}, {transform}, "Scale assembly host");
            const auto preview = preview_architectural_transaction(original, transaction);
            const auto& result = preview.entities().at(opening.id);
            const auto scaled = parse_opening_assembly(result.properties.at("opening_assembly"));
            require(scaled.kind == kind &&
                        std::abs(scaled.frame_width_m - assembly.frame_width_m * scale) < 1e-10 &&
                        std::abs(scaled.frame_depth_m - assembly.frame_depth_m * scale) < 1e-10 &&
                        std::abs(scaled.panel_thickness_m - assembly.panel_thickness_m * scale) < 1e-10 &&
                        std::abs(scaled.glazing_thickness_m - assembly.glazing_thickness_m * scale) < 1e-10 &&
                        std::abs(scaled.inset_m - assembly.inset_m * scale) < 1e-10,
                    "hosted assembly dimensions must scale with the wall");
            Wall after;
            require(read_document_wall(preview.entities().at(wall.id), {&result}, after, error),
                    "scaled assembly host decode");
            require(std::abs(solid_volume(make_opening_assembly(after, after.openings.front(), scaled)) -
                             before_volume * scale * scale * scale) < 1e-8,
                    "hosted assembly solid volume must scale cubically");
            require(result.properties.at("mark") == "preserved" &&
                        document.snapshot().entities() == original.entities(),
                    "assembly preview preserves metadata and the source");
            apply_architectural_transaction(document, transaction, document.revision());
            require(document.snapshot().entities() == preview.entities(), "assembly scale commits preview");
            document.undo(document.revision());
            require(document.snapshot().entities() == original.entities(), "assembly scale undo");
            document.redo(document.revision());
            require(document.snapshot().entities() == preview.entities(), "assembly scale redo");
            const auto path = std::filesystem::temp_directory_path() / "sketch-scaled-opening.bldproj";
            (void)ProjectStore::save(path, document.snapshot());
            require(ProjectStore::load(path).document.snapshot().entities() == preview.entities(),
                    "scaled opening assembly survives save/reopen");
            std::filesystem::remove(path);
        }
    }
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

void test_room_volume_dimensions() {
    using namespace sketch;
    for (const double winding : {1.0, -1.0}) {
        for (const auto anchor : {RoomFootprintAnchor::first_corner,
                                  RoomFootprintAnchor::center,
                                  RoomFootprintAnchor::opposite_corner}) {
            auto boundary = rectangle_json(4, 2 * winding);
            // Rotate away from world axes and translate to exercise local dimensions.
            for (auto& edge : boundary) {
                for (const auto* key : {"start", "end"}) {
                    const double x = edge[key][0], y = edge[key][1];
                    edge[key] = {10 + 0.6*x - 0.8*y, -7 + 0.8*x + 0.6*y};
                }
                edge["survey_note"] = "retain";
            }
            auto room = Entity::create("room", {{"boundary", boundary}, {"segments", boundary},
                {"height_m", 3}, {"height", 3}, {"elevation_m", 0}, {"elevation", 0}, {"note", "keep"}});
            room.extensions["vendor"] = {{"opaque", true}};
            auto doc = Document::create({room});
            const auto before = doc.snapshot();
            const RoomDimensionEdit dimensions{8, 6, 5, -2, anchor};
            const auto preview = resized_room_volume_entity(room, dimensions);
            RoomVolume decoded;
            std::string error;
            require(read_document_room(preview, decoded, error), "dimension preview must decode");
            require(std::abs(segment_length(decoded.boundary[0]) - 8) < 1e-9 &&
                    std::abs(segment_length(decoded.boundary[1]) - 6) < 1e-9,
                    "dimensions must follow the first two local edges");
            const double fraction = anchor == RoomFootprintAnchor::first_corner ? 0 :
                anchor == RoomFootprintAnchor::center ? 0.5 : 1;
            const Vec2 fixed{10 + fraction*(0.6*4 - 0.8*2*winding),
                             -7 + fraction*(0.8*4 + 0.6*2*winding)};
            const auto p = decoded.boundary[0].start;
            const auto q = decoded.boundary[2].start;
            require(std::abs(p.x + fraction*(q.x-p.x) - fixed.x) < 1e-9 &&
                    std::abs(p.y + fraction*(q.y-p.y) - fixed.y) < 1e-9, "anchor must remain fixed");
            require(signed_area(decoded.boundary)*winding > 0 &&
                    std::abs(solid_volume(make_room_volume(decoded)) - 240) < 1e-7,
                    "dimensions must preserve winding and produce requested volume");
            require(preview.id == room.id && preview.extensions == room.extensions &&
                    preview.properties["note"] == "keep" &&
                    preview.properties["boundary"][0]["survey_note"] == "retain" &&
                    preview.properties["boundary"] == preview.properties["segments"] &&
                    preview.properties["height"] == 5 && preview.properties["elevation"] == -2,
                    "dimension edit must preserve metadata and synchronize aliases");
            require(resized_room_volume_entity(preview, dimensions) == preview,
                    "reapplying exact dimensions must not perturb rotated rectangle coordinates or metadata");
            const auto command = room_dimension_update_command(before, room.id, dimensions, before.revision());
            require(doc.snapshot().entities() == before.entities(), "preview must not mutate source");
            doc.apply(command);
            require(doc.snapshot().entities().at(room.id) == preview, "command must equal preview");
            const auto applied = doc.snapshot();
            rejects([&] { doc.apply(command); });
            rejects([&] { (void)room_dimension_update_command(doc.snapshot(), room.id, dimensions, before.revision()); });
            require(doc.revision() == applied.revision() && doc.snapshot().entities() == applied.entities(),
                    "stale room edits must not change state or history");
            doc.undo(doc.revision());
            require(doc.snapshot().entities() == before.entities(), "dimension undo must restore exact metadata");
            doc.redo(doc.revision());
            require(doc.snapshot().entities().at(room.id) == preview, "dimension redo must restore preview");
        }
    }
    auto room = Entity::create("room", {{"boundary", rectangle_json(4, 2)}, {"height_m", 3}, {"elevation_m", 0}});
    const auto original = room;
    for (const auto dimensions : {RoomDimensionEdit{0,2,3,0}, {-1,2,3,0}, {4,2,0,0},
            {4,2,3,std::numeric_limits<double>::infinity()},
            {std::numeric_limits<double>::quiet_NaN(),2,3,0}, {4,1e-15,3,0}}) {
        rejects([&] { (void)resized_room_volume_entity(room, dimensions); });
    }
    rejects([&] { (void)resized_room_volume_entity(room, {4,2,3,0, static_cast<RoomFootprintAnchor>(99)}); });
    rejects([&] { (void)resized_room_volume_entity(room, {4,std::nullopt,3,0}); });
    rejects([&] { (void)resized_room_volume_entity(room, {std::nullopt,2,3,0}); });
    auto wrong_role = room;
    wrong_role.type = "label";
    auto doc = Document::create({room, Entity::create("label")});
    const auto before_invalid = doc.snapshot();
    rejects([&] { (void)resized_room_volume_entity(wrong_role, {4,2,3,0}); });
    rejects([&] { (void)room_dimension_update_command(doc.snapshot(), "missing", {4,2,3,0}, doc.revision()); });
    rejects([&] { doc.apply(room_dimension_update_command(doc.snapshot(), room.id, {0,2,3,0}, doc.revision())); });
    require(doc.revision() == before_invalid.revision() && doc.snapshot().entities() == before_invalid.entities(),
            "invalid room command must not mutate state or history");
    auto skew = room;
    skew.properties["boundary"][1]["end"] = {5,2};
    skew.properties["boundary"][2]["start"] = {5,2};
    rejects([&] { (void)resized_room_volume_entity(skew, {8,6,5,0}); });
    const auto skew_vertical = resized_room_volume_entity(skew, {std::nullopt,std::nullopt,5,-3});
    require(skew_vertical.properties["boundary"] == skew.properties["boundary"],
            "height-only edit must accept a nonrectangular valid room");
    auto curved = room;
    curved.properties["boundary"][0]["sweep_radians"] = 0.1;
    rejects([&] { (void)resized_room_volume_entity(curved, {8,6,5,0}); });
    const auto curved_vertical = resized_room_volume_entity(curved, {std::nullopt,std::nullopt,5,2});
    require(curved_vertical.properties["boundary"] == curved.properties["boundary"],
            "height-only edit must preserve analytical curved boundary");
    auto hole = rectangle_json(0.5, 0.5);
    for (auto& edge : hole) {
        for (const auto* key : {"start", "end"}) {
            edge[key][0] = edge[key][0].get<double>() + 0.5;
            edge[key][1] = edge[key][1].get<double>() + 0.5;
        }
        edge["opaque"] = {1,2,3};
    }
    room.properties["holes"] = nlohmann::json::array({hole});
    rejects([&] { (void)resized_room_volume_entity(room, {8,6,5,0}); });
    const auto vertical = resized_room_volume_entity(room, {std::nullopt,std::nullopt,5,1});
    require(vertical.properties["holes"] == room.properties["holes"] &&
            vertical.properties["boundary"] == room.properties["boundary"], "vertical edit must retain exact footprint JSON");
    RoomVolume decoded;
    std::string error;
    require(read_document_room(vertical, decoded, error) &&
            std::abs(solid_volume(make_room_volume(decoded)) - 38.75) < 1e-7, "vertical edit volume subtracts holes");
    require(original.properties["boundary"] == room.properties["boundary"], "failed previews must not mutate input");
    auto legacy = original;
    legacy.properties["segments"] = legacy.properties["boundary"];
    legacy.properties.erase("boundary");
    legacy.properties["height"] = 3;
    legacy.properties.erase("height_m");
    legacy.properties["elevation"] = 0;
    legacy.properties.erase("elevation_m");
    const auto legacy_preview = resized_room_volume_entity(legacy, {8,6,5,1});
    require(!legacy_preview.properties.contains("boundary") &&
            legacy_preview.properties["height"] == 5 && legacy_preview.properties["elevation"] == 1 &&
            read_document_room(legacy_preview, decoded, error) &&
            std::abs(solid_volume(make_room_volume(decoded))-240) < 1e-7,
            "legacy room aliases must resize and remain readable");
}

int main() {
    try {
        test_typed_join_commands();
        test_room_volume_dimensions();
        test_material_assignments();
        test_building_transform_updates_canonical_geometry();
        test_railing_transform_updates_canonical_geometry();
        test_shared_solid_transforms_update_canonical_geometry();
        test_hosted_assembly_scales_with_wall();
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
        const auto path = std::filesystem::temp_directory_path() / "vertex-architectural-adapter.bldproj";
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
