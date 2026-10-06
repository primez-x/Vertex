#include "sketch/architectural_document_adapter.hpp"
#include "sketch/vertical_level_document_adapter.hpp"
#include "sketch/architecture.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/constraint_entity.hpp"
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
#include <string>

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

void test_circular_column_transform_persists_orientation_and_zero_reset() {
    using namespace sketch;
    auto circular = encode_building_entity(CircularColumn{"rotated-circle", {}, .3, 3});
    circular.properties["rotation_rad"] = .75;
    circular.properties["mark"] = "C-1";
    circular.extensions["opaque"] = {1, "retained"};
    auto unrelated = Entity::create("label", {{"text", "untouched"}});
    auto document = Document::create({circular, unrelated});
    const auto rotate = [&](double delta) {
        ArchitecturalOperation operation{ArchitecturalAction::transform, circular.id};
        operation.transform = ArchitecturalTransform{0, 0, 0, delta, 1};
        return ArchitecturalTransaction::create("rotate-circle", "r0", {circular.id},
            {operation}, "Rotate circular column");
    };
    const auto rotation = rotate(2 * std::numbers::pi + .5);
    const auto preview = preview_architectural_transaction(document.snapshot(), rotation);
    require(std::abs(preview.entities().at(circular.id).properties.at("rotation_rad").get<double>() - 1.25) < 1e-9,
            "circular column transform must accumulate and normalize orientation");
    require(document.snapshot().entities().at(circular.id) == circular,
            "circular orientation preview must not mutate the document");
    apply_architectural_transaction(document, rotation, document.revision());
    const auto rotated = document.snapshot().entities().at(circular.id);
    require(rotated == preview.entities().at(circular.id),
            "circular rotation commit must equal its detached preview");
    require(rotated.properties.at("mark") == "C-1" && rotated.extensions == circular.extensions &&
                document.snapshot().entities().at(unrelated.id) == unrelated,
            "circular rotation must preserve unrelated properties, metadata and entities");
    document.undo(document.revision());
    require(document.snapshot().entities().at(circular.id) == circular,
            "circular rotation undo must restore its previous orientation");
    document.redo(document.revision());
    require(document.snapshot().entities().at(circular.id) == rotated,
            "circular rotation redo must restore its committed orientation");

    apply_architectural_transaction(document, rotate(-1.25), document.revision());
    const auto reset = document.snapshot().entities().at(circular.id);
    require(std::abs(reset.properties.at("rotation_rad").get<double>()) < 1e-9,
            "reset circular orientation to zero must overwrite its old angle");
    const auto path = std::filesystem::temp_directory_path() / "vertex-circular-orientation.bldproj";
    std::filesystem::remove(path);
    (void)ProjectStore::save(path, document.snapshot());
    auto reopened = ProjectStore::load(path).document;
    require(reopened.snapshot().entities().at(circular.id) == reset,
            "zero circular orientation must survive save and reopen");
    reopened.undo(reopened.revision());
    require(reopened.snapshot().entities().at(circular.id) == rotated,
            "reopened history must restore the prior circular orientation");
    reopened.redo(reopened.revision());
    require(reopened.snapshot().entities().at(circular.id) == reset,
            "reopened redo must restore zero circular orientation");
    std::filesystem::remove(path);
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

void test_wall_group_transforms_preserve_exact_length_receipts() {
    using namespace sketch;
    const auto measured_wall = [](std::string id, nlohmann::json baseline,
                                  std::string expression, std::string unit, int exact_length) {
        auto entity = Entity::create("wall", {{"baseline",baseline},
            {"thickness_m",.2},{"height_m",3},{"elevation_m",0},{"vendor_property",{{"keep",true}}}});
        entity.id = std::move(id);
        entity.extensions["constraint_authoring"] = {{"version",1},{"future_section",{1,2}},
            {"last_length_entry",{{"version",1},{"original_expression",expression},{"entered_unit",unit},
                {"exact_metres",{{"numerator",exact_length},{"denominator",1},{"vendor_exact",true}}},
                {"baseline",baseline},{"vendor_receipt","preserve"}}}};
        entity.extensions["constraint_authoring"]["last_length_entry"]["baseline"]["vendor_baseline"] = 7;
        entity.extensions["vendor_extension"] = {"unchanged"};
        return entity;
    };
    const auto first = measured_wall("measured-a",segment_json(0,0,3,4),"500 cm","cm",5);
    const auto second = measured_wall("measured-b",segment_json(3,4,5,4),"2000 mm","mm",2);
    auto opening = Entity::create("opening",{{"wall_id",first.id},{"opening_kind","door"},
        {"offset_m",1},{"width_m",.8},{"sill_m",0},{"height_m",2},{"vendor_property","retain"}});
    opening.id = "measured-opening";
    opening.extensions["vendor_extension"] = true;
    const auto join = encode_constraint_entity(PersistentConstraint{"measured-join",ConstraintRelationKind::coincident,
        {{first.id,WallEndpointRole::end},{second.id,WallEndpointRole::start}}});
    auto unrelated = Entity::create("wall",{{"baseline",segment_json(20,20,25,20)},
        {"thickness_m",.2},{"height_m",3},{"elevation_m",0}});
    unrelated.id = "unrelated-wall";
    auto document = Document::create({first,second,opening,join,unrelated});
    const auto original = document.snapshot();
    const auto group = [&](ArchitecturalTransform transform, bool both = true) {
        ArchitecturalOperation a{ArchitecturalAction::transform,first.id}; a.transform = transform;
        ArchitecturalOperation b{ArchitecturalAction::transform,second.id}; b.transform = transform;
        return ArchitecturalTransaction::create("measured-group","source",{first.id,second.id},
            both ? std::vector<ArchitecturalOperation>{a,b} : std::vector<ArchitecturalOperation>{a},
            "Move measured wall group");
    };
    const auto transaction = group({10,-2,.5,std::numbers::pi/2,1});
    const auto preview = preview_architectural_transaction(original,transaction);
    for (const auto* source : {&first,&second}) {
        const auto& moved = preview.entities().at(source->id);
        auto expected_extensions = source->extensions;
        auto& expected_baseline = expected_extensions["constraint_authoring"]["last_length_entry"]["baseline"];
        for (const auto* field : {"start","end","sweep_radians"})
            expected_baseline[field] = moved.properties.at("baseline").at(field);
        require(moved.extensions == expected_extensions &&
                moved.properties.at("vendor_property") == source->properties.at("vendor_property"),
                "rigid group transform must rebase exact receipt coordinates without rewriting input or opaque fields");
    }
    const auto& moved_first = preview.entities().at(first.id).properties.at("baseline");
    require(std::abs(moved_first.at("start")[0].get<double>()-10) < 1e-9 &&
            std::abs(moved_first.at("start")[1].get<double>()+2) < 1e-9 &&
            std::abs(moved_first.at("end")[0].get<double>()-6) < 1e-9 &&
            std::abs(moved_first.at("end")[1].get<double>()-1) < 1e-9 &&
            moved_first.at("end") == preview.entities().at(second.id).properties.at("baseline").at("start"),
            "rigid group transform must rotate both owners while preserving their explicit shared endpoint");
    require(preview.entities().at(opening.id) == opening && preview.entities().at(join.id) == join &&
            preview.entities().at(unrelated.id) == unrelated && document.snapshot().entities() == original.entities(),
            "rigid preview must preserve opening stations, relationships, unrelated walls, and the source document");
    require(apply_architectural_transaction(document,transaction,original.revision()) == original.revision()+1,
            "measured group move must commit one revision");
    const auto moved = document.snapshot();
    require(moved.entities() == preview.entities(), "measured group command must commit its exact shown candidate");
    document.undo(document.revision());
    require(document.snapshot().entities() == original.entities(), "measured group move must undo all owners and receipts");
    document.redo(document.revision());
    require(document.snapshot().entities() == moved.entities(), "measured group move must redo exact retained receipt coordinates");
    const auto path = std::filesystem::temp_directory_path()/
        ("vertex-measured-wall-group-" + make_stable_id() + ".bldproj");
    (void)ProjectStore::save(path,document.snapshot());
    auto reopened = ProjectStore::load(path).document;
    require(reopened.snapshot().entities() == moved.entities(), "measured group state must survive save/reopen");
    reopened.undo(reopened.revision());
    require(reopened.snapshot().entities() == original.entities(), "reopened group history must restore original receipt inputs");
    reopened.redo(reopened.revision());
    require(reopened.snapshot().entities() == moved.entities(), "reopened group redo must restore exact transformed receipts");
    std::filesystem::remove(path);

    rejects([&] { (void)preview_architectural_transaction(original,group({1,0,0,0,1},false)); });
    rejects([&] { (void)apply_architectural_transaction(document,transaction,original.revision()); });
    for (double scale : {2.0,1.000000001}) {
        bool rejected = false;
        try { (void)architectural_transaction_command(original,group({0,0,0,0,scale}),original.revision()); }
        catch (const std::exception& error) {
            rejected = std::string(error.what()).find("length-preserving") != std::string::npos;
        }
        require(rejected,"scaling a measured wall must explicitly reject unsupported receipt rewriting");
    }
    const auto anchor = encode_constraint_entity(PersistentConstraint{"measured-anchor",ConstraintRelationKind::fixed_anchor,
        {{first.id,WallEndpointRole::start}},std::nullopt,Vec2{0,0}});
    auto anchored = Document::create({first,second,opening,join,anchor});
    const auto anchored_before = anchored.snapshot();
    rejects([&] { (void)apply_architectural_transaction(anchored,transaction,anchored.revision()); });
    require(anchored.snapshot().entities() == anchored_before.entities() &&
            anchored.revision() == anchored_before.revision(),
            "fixed-anchor conflict must reject the complete measured group atomically");

    for (bool unsupported : {false,true}) {
        auto malformed = first;
        if (unsupported) malformed.extensions["constraint_authoring"]["version"] = 99;
        else malformed.extensions["constraint_authoring"]["last_length_entry"]["baseline"]["end"] = {4,4};
        if (!unsupported) {
            rejects([&] { (void)Document::create({malformed,second,opening,join}); });
            auto valid = Document::create({first,second,opening,join});
            const auto before = valid.snapshot();
            rejects([&] { valid.apply(ApplyEntityChanges{valid.revision(), {EntityChange::upsert(malformed)}, {}, "Invalid receipt"}); });
            require(valid.snapshot().entities() == before.entities() && valid.revision() == before.revision(),
                "known stale length receipt must reject before publication without mutation");
            continue;
        }
        auto invalid = Document::create({malformed,second,opening,join});
        const auto before = invalid.snapshot();
        rejects([&] { (void)architectural_transaction_command(before,transaction,before.revision()); });
        require(invalid.snapshot().entities() == before.entities(),
                "malformed or unsupported measured receipts must never be silently repaired or removed");
    }
}

void test_rigid_curved_wall_transform_preserves_input_provenance() {
    using namespace sketch;
    for (int construction : {0,1,2}) {
        const auto baseline = construction == 1 ? arc_from_chord_arc_length({0,0},{4,0},5,false)
            : construction == 2 ? arc_from_chord_height({0,0},{4,0},.5) : Segment{{0,0},{4,0},.75};
        const int version = construction == 0 ? 1 : 2;
        const auto derived_sweep = angle_from_radians(baseline.sweep_radians);
        auto curved = Entity::create("wall",{{"baseline",segment_json(0,0,4,0,baseline.sweep_radians)},
            {"thickness_m",.2},{"height_m",3},{"elevation_m",0}});
        curved.id = "curve-provenance";
        curved.extensions["curve_input"] = {{"version",version},{"start",{0,0}},{"end",{4,0}},
            {"radians",baseline.sweep_radians},{"sweep",construction == 0 ? "0.75 rad" : derived_sweep.original_expression},
            {"normalized_sweep",derived_sweep.normalized_expression},{"vendor_field",{1,"keep"}}};
        if (version == 2) {
            auto& input = curved.extensions["curve_input"];
            input["construction"] = construction == 1 ? "arc_length" : "arc_height";
            input["measure"] = construction == 1 ? "5 m" : "1/2 m";
            input["normalized_measure"] = construction == 1 ? "5 m" : "0.5 m";
            input["measure_value"] = construction == 1 ? 5.0 : .5; input["clockwise"] = false;
        }
        auto document = Document::create({curved});
        const auto before = document.snapshot();
        ArchitecturalOperation operation{ArchitecturalAction::transform,curved.id};
        operation.transform = ArchitecturalTransform{2,3,.5,std::numbers::pi/2,1};
        const auto make_transaction = [&](ArchitecturalOperation op) {
            return ArchitecturalTransaction::create("curve-provenance-move","source",{curved.id},{op},"Move measured curve");
        };
        const auto transaction = make_transaction(operation);
        const auto preview = preview_architectural_transaction(before,transaction);
        const auto& moved = preview.entities().at(curved.id);
        auto expected = curved.extensions;
        expected["curve_input"]["start"] = moved.properties.at("baseline").at("start");
        expected["curve_input"]["end"] = moved.properties.at("baseline").at("end");
        const auto& archive=moved.extensions.at("curve_input_derivation");
        expected["curve_input_derivation"]=archive;
        require(archive.at("version")==2 && archive.at("source_input")==curved.extensions.at("curve_input") &&
                archive.at("source_baseline")==curved.properties.at("baseline") &&
                archive.at("operations").size()==1 && archive.at("operations").front().at("kind")=="rigid_transform",
                "rigid curved-wall transform must archive and independently replay the exact source construction");
        require(moved.extensions == expected && moved.properties.at("baseline").at("sweep_radians") == baseline.sweep_radians,
                "rigid curved-wall transform must move retained input coordinates and preserve every defining expression and value");
        apply_architectural_transaction(document,transaction,document.revision());
        document.undo(document.revision());
        require(document.snapshot().entities() == before.entities(),"curved provenance transform must undo exactly");
        document.redo(document.revision());
        require(document.snapshot().entities() == preview.entities(),"curved provenance transform must redo exactly");
        operation.transform->scale = 2;
        rejects([&] { (void)architectural_transaction_command(before,make_transaction(operation),before.revision()); });
        for (int failure = 0; failure < 3; ++failure) {
            auto invalid = curved;
            if (failure == 0) invalid.extensions["curve_input"]["version"] = 99;
            else if (failure == 1) invalid.extensions["curve_input"]["start"] = {1,0};
            else invalid.extensions["curve_input"] = "unknown provenance";
            auto invalid_document = Document::create({invalid});
            const auto source = invalid_document.snapshot();
            rejects([&] { (void)architectural_transaction_command(source,transaction,source.revision()); });
            require(invalid_document.snapshot().entities() == source.entities(),
                    "unsupported or stale curve input provenance must reject without lossy repair");
        }
    }
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

void test_multi_flight_stair_attachment_lifecycle() {
    using namespace sketch;
    auto property = Entity::create("property"); property.id = "stair-property";
    auto building = Entity::create("building", {{"property_id",property.id}}); building.id = "stair-building";
    auto floor = Entity::create("floor", {{"building_id",building.id}}); floor.id = "stair-floor";
    auto layer = Entity::create("layer", {{"floor_id",floor.id}}); layer.id = "stair-layer";
    auto stair = encode_building_entity(StairFlight{.id="lifecycle-stair", .base_position={2,3,1},
        .riser_count=8, .total_rise=2, .going=.3, .width=1,
        .flights={{"lower-flight",4},{"upper-flight",4}},
        .landings={{"turn-landing",1.2,.15,StairTurn::left_quarter,0}}});
    stair.properties["layer_id"] = layer.id;
    stair.properties["property_id"] = property.id;
    stair.properties["building_id"] = building.id;
    stair.properties["floor_id"] = floor.id;
    stair.properties["flights"][0]["opaque"] = {{"source_id","lower-flight"},{"note","keep"}};
    stair.properties["landings"][0]["opaque"] = {1,2,3};
    stair.properties["quantity_entries"] = {{"/going_m",{{"opaque","receipt"}}}};
    auto rail = encode_building_entity(Railing{.id="lifecycle-rail",.height=1,.thickness=.05,.post_spacing=.4,
        .host=StairRailingHost{stair.id,"upper-flight",StairRailingSide::left,0,1}});
    rail.properties["layer_id"] = layer.id;
    rail.properties["property_id"] = property.id;
    rail.properties["building_id"] = building.id;
    rail.properties["floor_id"] = floor.id;
    rail.properties["host"]["opaque"] = "upper-flight";
    rail.extensions["source_id"] = stair.id;
    auto future_host_metadata=Entity::create("railing",{{"version",99},{"form","stair_flight_railing"},
        {"host",{{"stair_id",stair.id},{"opaque","preserve"}}}});
    future_host_metadata.id="future-lifecycle-rail";
    auto document = Document::create({property,building,floor,layer,future_host_metadata});
    const auto transaction = [&](std::vector<ArchitecturalOperation> operations) {
        std::vector<std::string> ids;
        const auto current = document.snapshot();
        for (const auto& [id,entity] : current.entities()) { (void)entity; ids.push_back(id); }
        return ArchitecturalTransaction::create(make_stable_id(),"source",ids,std::move(operations),"Stair lifecycle");
    };
    const auto transform = [](const std::string& id, ArchitecturalTransform value) {
        ArchitecturalOperation operation{ArchitecturalAction::transform,id}; operation.transform=value; return operation;
    };
    const auto create = [](const Entity& entity) {
        ArchitecturalOperation operation{ArchitecturalAction::create,entity.id,entity.type};
        for (const auto& [key,value] : entity.properties.items()) operation.properties[key]=value.dump();
        return operation;
    };
    document.apply(architectural_transaction_command(document.snapshot(),transaction({create(stair),create(rail)}),document.revision()));
    // Transaction creation transports semantic fields; extension provenance is
    // already persisted on existing entities and must survive later cloning.
    auto authored=document.snapshot().entities().at(rail.id); authored.extensions=rail.extensions;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(authored)}, {},"Attach source metadata"});
    const auto before = document.snapshot();
    const auto edited=preview_architectural_transaction(before,transaction({
        {ArchitecturalAction::property_edit,stair.id,{},{},{{"going_m","0.35"}}}}));
    require(edited.entities().at(stair.id).properties["going_m"]==.35 &&
        edited.entities().at(rail.id)==before.entities().at(rail.id) &&
        edited.entities().at(stair.id).properties["flights"]==stair.properties["flights"] &&
        edited.entities().at(stair.id).properties["landings"]==stair.properties["landings"],
        "stair edit changed hosted rail authoring or nested metadata");
    rejects([&] { (void)preview_architectural_transaction(before,transaction({
        {ArchitecturalAction::property_edit,stair.id,{},{},{{"flights","[{\"id\":\"lower-flight\",\"riser_count\":8}]"},{"landings","[]"}}}})); });
    const auto moved_only=preview_architectural_transaction(before,transaction({transform(stair.id,{1,2,3,0,1})}));
    const auto moved_stair=decode_stair_properties(stair.id,moved_only.entities().at(stair.id).properties);
    const auto before_layout=derive_hosted_railing_layout(decode_railing_properties(rail.id,rail.properties),decode_stair_properties(stair.id,stair.properties));
    const auto moved_layout=derive_hosted_railing_layout(decode_railing_properties(rail.id,rail.properties),moved_stair);
    require(std::abs(moved_layout.rail_start.x-before_layout.rail_start.x-1)<1e-9 &&
        std::abs(moved_layout.rail_start.y-before_layout.rail_start.y-2)<1e-9 &&
        std::abs(moved_layout.rail_start.z-before_layout.rail_start.z-3)<1e-9 &&
        moved_only.entities().at(rail.id)==before.entities().at(rail.id),"host translation did not derive unchanged authored rail");
    rejects([&] { (void)preview_architectural_transaction(before,transaction({transform(rail.id,{1,0,0,0,1})})); });
    const ArchitecturalTransform move{5,-2,3,.5,2};
    const auto preview = preview_architectural_transaction(before,transaction({transform(rail.id,move),transform(stair.id,move)}));
    require(document.snapshot().entities()==before.entities(),"stair preview mutated source");
    const auto changed = decode_stair_properties(stair.id,preview.entities().at(stair.id).properties);
    require(changed.riser_count==8 && changed.flights[0].id=="lower-flight" &&
        std::abs(changed.total_rise-4)<1e-9 && std::abs(changed.landings[0].depth-2.4)<1e-9 &&
        std::abs(changed.landings[0].thickness-.3)<1e-9,"stair topology scaled incorrectly");
    require(std::abs(changed.base_position.x-(4*std::cos(.5)-6*std::sin(.5)+5))<1e-9 &&
        std::abs(changed.base_position.z-5)<1e-9,"whole stair placement transform incorrect");
    const auto& changed_rail=preview.entities().at(rail.id);
    require(changed_rail.properties["height_m"]==2 && changed_rail.properties["thickness_m"]==.1 &&
        changed_rail.properties["post_spacing_m"]==.8 && !changed_rail.properties.contains("base_position_m"),
        "hosted rail must scale authored dimensions once without independent placement");
    require(changed_rail.properties["host"]==rail.properties["host"] &&
        preview.entities().at(stair.id).properties["flights"][0]["opaque"]==stair.properties["flights"][0]["opaque"] &&
        preview.entities().at(stair.id).properties["landings"][0]["opaque"]==stair.properties["landings"][0]["opaque"],
        "nested authoring metadata lost during transform");
    require(!preview.entities().at(stair.id).properties["quantity_entries"].contains("/going_m"),
        "changed stair dimension retained stale receipt");
    rejects([&] { (void)preview_architectural_transaction(before,transaction({transform(stair.id,move),transform(rail.id,{1,0,0,0,1})})); });
    document.apply(architectural_transaction_command(before,transaction({transform(stair.id,move),transform(rail.id,move)}),before.revision()));
    const auto transformed = document.snapshot();
    require(transformed.entities().at(future_host_metadata.id)==future_host_metadata,
        "stair transform interpreted future railing host metadata");
    document.undo(document.revision()); require(document.snapshot().entities()==before.entities(),"stair transform undo");
    document.redo(document.revision()); require(document.snapshot().entities()==transformed.entities(),"stair transform redo");
    const auto clone = transaction({{ArchitecturalAction::duplicate,rail.id,{},"selected-rail-clone"},
        {ArchitecturalAction::duplicate,stair.id,{},"stair-clone"}});
    document.apply(architectural_transaction_command(document.snapshot(),clone,document.revision()));
    const auto cloned=document.snapshot();
    const auto& copy=cloned.entities().at("stair-clone");
    const auto copy_stair=decode_stair_properties(copy.id,copy.properties);
    require(copy_stair.flights[0].id!=changed.flights[0].id && copy_stair.flights[1].id!=changed.flights[1].id &&
        copy_stair.landings[0].id!=changed.landings[0].id,"cloned stair reused stable children");
    require(copy.properties["flights"][0]["opaque"]==stair.properties["flights"][0]["opaque"],"clone rewrote provenance");
    std::string clone_rail_id;
    for (const auto& [id,entity] : cloned.entities()) {
        if (entity.type=="railing" && entity.properties["host"]["stair_id"]==copy.id) {
            clone_rail_id=id;
            require(id!=rail.id && entity.properties["host"]["flight_id"]==copy_stair.flights[1].id &&
                entity.properties["host"]["opaque"]=="upper-flight" && entity.extensions==rail.extensions,
                "cloned rail must remap only authoritative host references");
        }
    }
    require(clone_rail_id=="selected-rail-clone","stair clone omitted or duplicated selected hosted rail");
    const auto erase=transaction({{ArchitecturalAction::erase,copy.id},{ArchitecturalAction::erase,clone_rail_id}});
    document.apply(architectural_transaction_command(cloned,erase,document.revision()));
    require(document.snapshot().entities()==transformed.entities(),"host deletion did not remove dependent rail atomically");
    document.undo(document.revision()); require(document.snapshot().entities()==cloned.entities(),"stair delete undo");
    document.redo(document.revision()); require(document.snapshot().entities()==transformed.entities(),"stair delete redo");
    const auto path=std::filesystem::temp_directory_path()/(make_stable_id()+".bldproj");
    (void)ProjectStore::save(path,document.snapshot());
    auto reopened=ProjectStore::load(path).document;
    require(reopened.snapshot().entities()==document.snapshot().entities(),"stair lifecycle reopen");
    reopened.undo(reopened.revision()); require(reopened.snapshot().entities()==cloned.entities(),"stair history reopen undo");
    std::filesystem::remove(path);
}

void test_multi_flight_level_placement_lifecycle() {
    using namespace sketch;
    const VerticalLevelGraph graph({{"lower",10},{"upper",12}},{{"storey","lower","upper"}});
    auto levels=Entity::create("vertical_levels",{{"model",nlohmann::json::parse(graph.serialize())}}); levels.id="v2-levels";
    auto property=Entity::create("property"); property.id="v2-property";
    auto building=Entity::create("building",{{"property_id",property.id}}); building.id="v2-building";
    auto floor=Entity::create("floor",{{"building_id",building.id},
        {"vertical_level_binding",{{"version",1},{"graph_id",levels.id},{"level_id","lower"}}}}); floor.id="v2-floor";
    auto layer=Entity::create("layer",{{"floor_id",floor.id}}); layer.id="v2-layer";
    auto stair=encode_building_entity(StairFlight{.id="level-v2-stair",.base_position={2,3,.25},
        .riser_count=8,.total_rise=2,.going=.3,.width=1,
        .level_connection=StairLevelConnection{levels.id,"storey","lower","upper"},
        .flights={{"level-flight-a",4},{"level-flight-b",4}},
        .landings={{"level-landing",1.2,.15,StairTurn::left_quarter,0}}});
    stair.properties["layer_id"]=layer.id;
    stair.properties["property_id"]=property.id;
    stair.properties["building_id"]=building.id;
    stair.properties["floor_id"]=floor.id;
    stair.properties["vertical_placement"]={{"version",1},{"mode","level"},{"offset_m",.5}};
    stair.properties["flights"][0]["opaque"]="keep";
    stair.properties["quantity_entries"]={{"/total_rise_m",{{"opaque","old"}}},{"/going_m",{{"opaque","keep"}}}};
    auto legacy=stair; legacy.id="absolute-v2-upgrade";
    legacy.properties.erase("vertical_placement"); legacy.properties["base_position_m"]={8,3,4};
    legacy.properties["flights"][0]["id"]="absolute-flight-a";
    legacy.properties["flights"][1]["id"]="absolute-flight-b";
    legacy.properties["landings"][0]["id"]="absolute-landing";
    auto rail=encode_building_entity(Railing{.id="level-v2-rail",.height=1,.thickness=.05,.post_spacing=.4,
        .host=StairRailingHost{stair.id,"level-flight-a",StairRailingSide::right,0,1}});
    rail.properties["layer_id"]=layer.id;
    rail.properties["property_id"]=property.id;
    rail.properties["building_id"]=building.id;
    rail.properties["floor_id"]=floor.id;
    auto future_rail=Entity::create("railing",{{"version",99},{"form","stair_flight_railing"},
        {"host",{{"future_payload",{1,2,3}},{"stair_id",stair.id}}}});
    future_rail.id="future-level-rail";
    auto legacy_rail=Entity::create("railing",{{"description","Legacy railing"},{"host",{{"opaque",true}}}});
    legacy_rail.id="legacy-level-rail";
    auto document=Document::create({levels,property,building,floor,layer,stair,legacy,rail,future_rail,legacy_rail});
    const auto before=document.snapshot();
    const auto candidate=prepare_vertical_level_edit(before,levels.id,graph.with_elevation("upper",13));
    require(document.snapshot().entities()==before.entities(),"level preview mutated source");
    const auto receipt=apply_vertical_level_edit(document,candidate);
    require(receipt.affected_stairs.size()==2,"v2 rise propagation omitted connected stairs");
    const auto raised=document.snapshot();
    require(raised.entities().at(future_rail.id)==future_rail && raised.entities().at(legacy_rail.id)==legacy_rail,
        "level edit decoded or changed unrelated opaque railing hosts");
    require(raised.entities().at(stair.id).properties["total_rise_m"]==3 &&
        raised.entities().at(stair.id).properties["flights"]==stair.properties["flights"] &&
        raised.entities().at(stair.id).properties["base_position_m"]==stair.properties["base_position_m"] &&
        raised.entities().at(rail.id)==rail,"level rise edit changed child identities or authored placement");
    require(!raised.entities().at(stair.id).properties["quantity_entries"].contains("/total_rise_m") &&
        raised.entities().at(stair.id).properties["quantity_entries"].contains("/going_m"),"level edit receipt invalidation");
    const auto effective=decode_stair_properties(stair.id,resolve_vertical_placement(raised,raised.entities().at(stair.id)).properties);
    require(std::abs(effective.base_position.z-10.75)<1e-9,"level placement origin not resolved once");
    const auto before_rail=derive_hosted_railing_layout(decode_railing_properties(rail.id,rail.properties),effective);
    const auto raised_graph=graph.with_elevation("upper",13);
    const auto translated_graph=raised_graph.with_elevation("upper",15).with_elevation("lower",12);
    const auto shifted=prepare_vertical_level_edit(raised,levels.id,translated_graph);
    require(shifted.affected_stairs().empty(),"same-rise level shift should change only graph");
    (void)apply_vertical_level_edit(document,shifted);
    const auto after=document.snapshot();
    require(after.entities().at(stair.id)==raised.entities().at(stair.id) &&
        after.entities().at(legacy.id)==raised.entities().at(legacy.id),"level shift rewrote authored base coordinates");
    const auto moved=decode_stair_properties(stair.id,resolve_vertical_placement(after,after.entities().at(stair.id)).properties);
    const auto after_rail=derive_hosted_railing_layout(decode_railing_properties(rail.id,rail.properties),moved);
    require(std::abs(moved.base_position.z-12.75)<1e-9 &&
        std::abs(after_rail.rail_start.z-before_rail.rail_start.z-2)<1e-9,"same-rise level shift did not move stair and rail exactly once");
    require(resolve_vertical_placement(after,after.entities().at(legacy.id)).properties["base_position_m"][2]==4,
        "absolute v2 upgrade unexpectedly follows lower-level elevation");
    document.undo(document.revision()); require(document.snapshot().entities()==raised.entities(),"level shift undo");
    document.undo(document.revision()); require(document.snapshot().entities()==before.entities(),"v2 rise undo");
    document.redo(document.revision()); document.redo(document.revision());
    require(document.snapshot().entities()==after.entities(),"v2 level redo");
}

void test_unrelated_lifecycle_preserves_opaque_stairs_and_rails() {
    using namespace sketch;
    std::vector<Entity> opaque;
    for (const auto* type : {"stair","railing"}) {
        for (const auto& properties : {
            nlohmann::json{{"description","legacy"},{"host",{{"opaque",true}}}},
            nlohmann::json{{"version",2},{"form","unknown_future_form"},{"host","opaque"}},
            nlohmann::json{{"version",99},{"form",std::string_view(type)=="stair"?"multi_flight_stair":"stair_flight_railing"},
                {"host",{{"opaque",{1,2,3}}}}}}) {
            auto entity=Entity::create(type,properties); entity.id=make_stable_id();
            entity.extensions["source"]={{"untouched","provenance"}}; opaque.push_back(std::move(entity));
        }
    }
    auto column=encode_building_entity(RectangularColumn{.id="opaque-neighbor-column",.width=.3,.depth=.4,.height=3});
    auto entities=opaque; entities.push_back(column);
    auto document=Document::create(std::move(entities));
    const auto apply=[&](std::vector<ArchitecturalOperation> operations) {
        const auto source=document.snapshot();
        std::vector<std::string> ids;
        for (const auto& [id,entity] : source.entities()) { (void)entity; ids.push_back(id); }
        const auto transaction=ArchitecturalTransaction::create(make_stable_id(),"source",ids,std::move(operations),"Edit neighbor");
        document.apply(architectural_transaction_command(source,transaction,source.revision()));
        const auto after=document.snapshot();
        for (const auto& preserved : opaque)
            require(after.entities().at(preserved.id)==preserved,"unrelated lifecycle changed opaque stair or railing");
    };
    ArchitecturalOperation transform{ArchitecturalAction::transform,column.id};
    transform.transform=ArchitecturalTransform{1,2,3,.5,1};
    apply({transform});
    apply({{ArchitecturalAction::duplicate,column.id,{},"neighbor-column-clone"}});
    apply({{ArchitecturalAction::erase,column.id},{ArchitecturalAction::erase,"neighbor-column-clone"}});
}

int main() {
    try {
        test_typed_join_commands();
        test_multi_flight_stair_attachment_lifecycle();
        test_multi_flight_level_placement_lifecycle();
        test_unrelated_lifecycle_preserves_opaque_stairs_and_rails();
        test_room_volume_dimensions();
        test_material_assignments();
        test_building_transform_updates_canonical_geometry();
        test_circular_column_transform_persists_orientation_and_zero_reset();
        test_railing_transform_updates_canonical_geometry();
        test_wall_group_transforms_preserve_exact_length_receipts();
        test_rigid_curved_wall_transform_preserves_input_provenance();
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
