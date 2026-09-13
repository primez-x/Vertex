#include "sketch/document.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/reference_grid.hpp"
#include "sketch/terrain_surface.hpp"
#include "support/noninteractive_errors.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using sketch::ApplyEntityChanges;
using sketch::AssemblyModel;
using sketch::Asset;
using sketch::AssetChange;
using sketch::Document;
using sketch::DocumentError;
using sketch::DocumentErrorCode;
using sketch::Entity;
using sketch::EntityChange;
using sketch::NameRevision;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "document_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void require_error(Function&& function, DocumentErrorCode code, std::string_view message) {
    try {
        function();
    } catch (const DocumentError& error) {
        if (error.code() == code) {
            return;
        }
        std::cerr << "document_tests: " << message << ": wrong error code\n";
        std::exit(1);
    }
    fail(message);
}

Entity entity(std::string id, std::string type, nlohmann::json properties = nlohmann::json::object(),
              bool required = false, nlohmann::json extensions = nlohmann::json::object()) {
    return Entity{std::move(id), std::move(type), std::move(properties), required,
                  std::move(extensions)};
}

void test_compound_change_is_atomic_and_references_are_checked() {
    auto document = Document::create();
    const auto r1 = document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("property-1", "property"))},
        .message = "property",
    });
    require(r1 == 1, "first command should create revision one");

    const auto before = document.snapshot();
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 1,
                .entity_changes = {
                    EntityChange::erase("property-1"),
                    EntityChange::upsert(entity(
                        "wall-1", "wall", {{"floor_id", "missing-floor"}, {"height_m", 2.4}})),
                },
                .message = "invalid compound command",
            });
        },
        DocumentErrorCode::dangling_reference,
        "a dangling reference should reject the complete command");

    const auto after = document.snapshot();
    require(after.revision() == before.revision(), "a rejected command must not advance revision");
    require(after.entities() == before.entities(), "a rejected command must not partially delete");
}

void test_stale_revision_is_rejected() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("property-1", "property"))},
    });
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 0,
                .entity_changes = {EntityChange::upsert(entity("building-1", "building"))},
            });
        },
        DocumentErrorCode::stale_revision,
        "a command based on stale state should fail");
    require(document.revision() == 1, "stale command must not advance revision");
}

void test_undo_redo_and_branch_history_are_preserved() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("property-1", "property"))},
    });
    document.apply(ApplyEntityChanges{
        .expected_revision = 1,
        .entity_changes = {EntityChange::upsert(
            entity("building-old", "building", {{"property_id", "property-1"}}))},
    });
    document.apply(NameRevision{.expected_revision = 2, .name = "old branch"});
    const auto named_old_revision = document.revision();

    const auto undo_revision = document.undo(named_old_revision);
    require(undo_revision > named_old_revision, "undo must create a monotonic revision");
    require(document.can_redo(), "undo should make redo available");
    const auto redo_revision = document.redo(undo_revision);
    require(redo_revision > undo_revision, "redo must create a monotonic revision");
    require(document.snapshot().entities().contains("building-old"),
            "redo should restore the exact prior state");

    const auto second_undo = document.undo(redo_revision);
    document.apply(ApplyEntityChanges{
        .expected_revision = second_undo,
        .entity_changes = {EntityChange::upsert(
            entity("building-new", "building", {{"property_id", "property-1"}}))},
        .message = "new branch",
    });

    const auto snapshot = document.snapshot();
    require(!document.can_redo(), "editing after undo should clear the navigation redo stack");
    require(snapshot.named_revisions().at("old branch") == named_old_revision,
            "a named revision on the abandoned branch must remain addressable");
    bool found_old_revision = false;
    for (const auto& revision : snapshot.history()) {
        found_old_revision = found_old_revision || revision.revision == named_old_revision;
    }
    require(found_old_revision, "branching must not discard old history records");
}

void test_snapshots_are_immutable_copies_and_saved_revision_can_lag_head() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("label-1", "label", {{"text", "before"}}))},
    });
    const auto snapshot_at_one = document.snapshot();
    document.apply(ApplyEntityChanges{
        .expected_revision = 1,
        .entity_changes = {EntityChange::upsert(entity("label-1", "label", {{"text", "after"}}))},
    });
    document.mark_saved(1);

    require(snapshot_at_one.revision() == 1, "captured snapshot revision should not move");
    require(snapshot_at_one.entities().at("label-1").properties.at("text") == "before",
            "captured snapshot state should not be mutated by later edits");
    require(document.revision() == 2 && document.dirty(),
            "saving an older snapshot must leave a newer head dirty");
    require(document.saved_revision() == 1, "saved revision should identify the persisted snapshot");
}

void test_unknown_optional_data_is_preserved_and_unknown_required_is_read_only() {
    const auto optional = entity(
        "future-1", "future_widget",
        {{"opaque", {1, "two", true}}, {"vendor_blob", {{"n", 1844674407370955161ULL}}}}, false,
        {{"vendor_extension", {{"ordered", nlohmann::json::array({"a", "b"})}}}});
    auto document = Document::create({optional});
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("label-1", "label", {{"text", "known"}}))},
    });
    const auto edited_snapshot = document.snapshot();
    const auto& preserved = edited_snapshot.entities().at("future-1");
    require(preserved == optional, "unknown optional entity and fields must survive unrelated edits");

    auto required_document = Document::create(
        {entity("future-required", "future_widget", nlohmann::json::object(), true)});
    require(!required_document.is_editable(), "an unknown required entity should force read-only mode");
    require(!required_document.read_only_reason().empty(), "read-only mode should explain the blocker");
    require_error(
        [&] {
            required_document.apply(ApplyEntityChanges{
                .expected_revision = 0,
                .entity_changes = {EntityChange::upsert(entity("label-1", "label"))},
            });
        },
        DocumentErrorCode::read_only,
        "unsafe editing must be blocked for unknown required data");
}

void test_duplicate_changes_fail_without_state_mutation() {
    auto document = Document::create();
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 0,
                .entity_changes = {
                    EntityChange::upsert(entity("wall-1", "wall")),
                    EntityChange::erase("wall-1"),
                },
            });
        },
        DocumentErrorCode::duplicate_change,
        "two operations for one entity in a command should be rejected");
    require(document.revision() == 0 && document.snapshot().entities().empty(),
            "duplicate change rejection should be atomic");
}

void test_non_finite_json_number_rolls_back_compound_command() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("property-1", "property"))},
    });
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 1,
                .entity_changes = {
                    EntityChange::erase("property-1"),
                    EntityChange::upsert(entity(
                        "label-1", "label",
                        {{"position_m", {std::numeric_limits<double>::quiet_NaN(), 2.0}}}))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "NaN must be rejected before JSON encoding silently converts it to null");
    require(document.revision() == 1 && document.snapshot().entities().contains("property-1"),
            "non-finite JSON rejection should roll back the complete command");
}

void test_canonical_reference_fields_enforce_target_type() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("property-1", "property"))},
    });
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 1,
                .entity_changes = {EntityChange::upsert(
                    entity("wall-1", "wall", {{"floor_id", "property-1"}}))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "floor_id should reject an existing entity of the wrong semantic type");
    require(document.revision() == 1, "reference type failure must not advance revision");
}

void test_asset_id_is_not_misclassified_as_an_entity_reference() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(
            entity("label-1", "label", {{"asset_id", "photo-1"}}))},
        .asset_changes = {AssetChange::upsert(
            Asset::create("photo-1", "image/png", {std::byte{0x01}}))},
    });
    require(document.snapshot().assets().contains("photo-1"),
            "asset reference should be accepted when the asset exists in the same command");
    document.apply(ApplyEntityChanges{
        .expected_revision = 1,
        .entity_changes = {EntityChange::upsert(
            entity("reference-1", "reference_asset",
                   {{"asset_id", "photo-1"}, {"render_asset_id", "preview-1"}}))},
        .asset_changes = {AssetChange::upsert(
            Asset::create("preview-1", "image/png", {std::byte{0x02}}))},
    });
    require(document.snapshot().entities().at("reference-1").properties.at("render_asset_id") ==
                "preview-1",
            "reference render_asset_id should be validated as an asset reference");
}

void test_asset_references_are_structurally_validated_atomically() {
    auto document = Document::create();
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 0,
                .entity_changes = {EntityChange::upsert(
                    entity("label-1", "label", {{"asset_id", "missing-photo"}}))},
            });
        },
        DocumentErrorCode::dangling_reference,
        "a missing asset_id should reject the command");

    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {EntityChange::upsert(
            entity("label-1", "label", {{"asset_ids", {"photo-1"}}}))},
        .asset_changes = {AssetChange::upsert(
            Asset::create("photo-1", "image/png", {std::byte{0x01}}))},
    });
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 1,
                .asset_changes = {AssetChange::erase("photo-1")},
            });
        },
        DocumentErrorCode::dangling_reference,
        "deleting an asset still referenced by an entity should fail");
    require(document.revision() == 1 && document.snapshot().assets().contains("photo-1"),
            "rejected asset deletion should preserve revision and bytes");
}

void test_persisted_string_bounds_are_enforced_before_mutation() {
    auto document = Document::create();
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 0,
                .message = std::string(1025, 'm'),
            });
        },
        DocumentErrorCode::invalid_entity,
        "an action message too large to reopen must be rejected before commit");
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 0,
                .message = std::string("bad\0message", 11),
            });
        },
        DocumentErrorCode::invalid_entity,
        "an embedded NUL action message must be rejected before commit");
    require_error(
        [&] {
            document.apply(NameRevision{
                .expected_revision = 0,
                .name = std::string("bad\xC3\x28", 5),
            });
        },
        DocumentErrorCode::duplicate_revision_name,
        "invalid UTF-8 revision names must be rejected before commit");
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 0,
                .asset_changes = {AssetChange::upsert(
                    Asset::create("asset-1", std::string("image/\xC3\x28", 8),
                                  {std::byte{0x01}}))},
            });
        },
        DocumentErrorCode::invalid_asset,
        "invalid UTF-8 media types must be rejected before commit");
    require(document.revision() == 0, "invalid persisted strings must not mutate history");
}

void test_current_architecture_types_and_boundary_links_are_structurally_validated() {
    for (const auto type : {"measurement_boundary", "room_boundary", "column", "beam", "railing", "slab"}) {
        require(sketch::is_known_entity_type(type),
                "current architecture entities must be recognized semantic types");
    }

    auto document = Document::create();
    document.apply(ApplyEntityChanges{
        .expected_revision = 0,
        .entity_changes = {
            EntityChange::upsert(entity("floor-1", "floor")),
            EntityChange::upsert(entity("layer-1", "layer")),
            EntityChange::upsert(entity(
                "measure-1", "measurement_boundary",
                {{"floor_id", "floor-1"}, {"layer_id", "layer-1"}})),
            EntityChange::upsert(entity(
                "room-edge-1", "room_boundary",
                {{"floor_id", "floor-1"}, {"layer_id", "layer-1"}})),
        },
    });
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 1,
                .entity_changes = {EntityChange::erase("floor-1")},
            });
        },
        DocumentErrorCode::dangling_reference,
        "measurement and room boundaries must prevent deletion of their floor");
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 1,
                .entity_changes = {EntityChange::erase("layer-1")},
            });
        },
        DocumentErrorCode::dangling_reference,
        "measurement and room boundaries must prevent deletion of their layer");

    document.apply(ApplyEntityChanges{
        .expected_revision = 1,
        .entity_changes = {
            EntityChange::upsert(entity("column-1", "column", {{"floor_id", "floor-1"}})),
            EntityChange::upsert(entity(
                "beam-1", "beam",
                {{"floor_id", "floor-1"}, {"column_id", "column-1"}})),
            EntityChange::upsert(entity(
                "railing-1", "railing",
                {{"floor_id", "floor-1"}})),
        },
    });
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 2,
                .entity_changes = {EntityChange::upsert(
                    entity("label-typed", "label", {{"beam_id", "column-1"}}))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "beam_id must reject an existing column target");
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = 2,
                .entity_changes = {EntityChange::upsert(
                    entity("label-railing-typed", "label", {{"railing_id", "column-1"}}))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "railing_id must reject an existing column target");
    require(document.revision() == 2,
            "architecture reference failures must not advance the revision");

    auto surface_document = Document::create({
        entity("floor-surface", "floor"),
        entity("layer-surface", "layer"),
        entity("surface-floor", "slab", {{"floor_id", "floor-surface"},
                                             {"layer_id", "layer-surface"},
                                             {"element_kind", "floor"}}),
    });
    require(surface_document.snapshot().entities().at("surface-floor").properties
                    .at("element_kind") == "floor",
            "semantic floor surfaces should retain their element kind");
    const auto surface_revision = surface_document.revision();
    auto invalid_surface = surface_document.snapshot().entities().at("surface-floor");
    invalid_surface.properties.at("element_kind") = "roof";
    require_error(
        [&] {
            surface_document.apply(ApplyEntityChanges{
                .expected_revision = surface_revision,
                .entity_changes = {EntityChange::upsert(std::move(invalid_surface))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "slab element kinds must be restricted to the supported horizontal assemblies");
    require(surface_document.revision() == surface_revision,
            "an invalid slab element kind must not advance the revision");
}

void test_composite_wall_layers_are_validated_at_document_boundary() {
    const auto layers = nlohmann::json::array({
        nlohmann::json{{"id", "outer"}, {"thickness_m", 0.02}},
        nlohmann::json{{"id", "core"}, {"thickness_m", 0.16},
                       {"material_assignment", {{"version", 1},
                                                   {"catalog_id", "catalog"},
                                                   {"material_id", "brick"}}}},
        nlohmann::json{{"id", "inner"}, {"thickness_m", 0.02}},
    });
    const auto catalog = entity(
        "catalog", "assembly_model",
        {{"version", 1}, {"model", AssemblyModel::create({{"brick", "Brick"}}, {}, {}).to_json()}});
    auto document = Document::create({catalog, entity(
        "wall-assembly", "wall", {{"thickness_m", 0.20}, {"layers", layers}})});
    require(document.snapshot().entities().contains("wall-assembly"),
            "a valid composite wall layer stack should be admitted by the document");
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = document.revision(),
                .entity_changes = {EntityChange::erase("catalog")},
            });
        },
        DocumentErrorCode::dangling_reference,
        "a wall layer material must retain its catalog reference");

    auto invalid = document.snapshot().entities().at("wall-assembly");
    invalid.properties["layers"][1]["thickness_m"] = 0.15;
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = document.revision(),
                .entity_changes = {EntityChange::upsert(std::move(invalid))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "a composite wall whose layers do not sum to thickness must be rejected");
    require(document.revision() == 0,
            "invalid composite wall data must not advance the document revision");

    auto missing_thickness = entity("wall-no-thickness", "wall", {{"layers", layers}});
    require_error(
        [&] {
            (void)Document::create({std::move(missing_thickness)});
        },
        DocumentErrorCode::invalid_entity,
        "a composite wall must declare thickness_m for layer validation");

    auto unknown_material = document.snapshot().entities().at("wall-assembly");
    unknown_material.properties["layers"][1]["material_assignment"]["material_id"] = "missing";
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = document.revision(),
                .entity_changes = {EntityChange::upsert(std::move(unknown_material))},
            });
        },
        DocumentErrorCode::dangling_reference,
        "a wall layer material must exist in its catalog");
}

void test_embedded_architectural_models_are_validated_at_document_boundary() {
    const auto phases = sketch::ModelPhases::create({"wall-1", "railing-1"},
        {"wall-1", "railing-1"}, {}).to_json();
    auto document = Document::create({
        entity("wall-1", "wall"),
        entity("railing-1", "railing"),
        entity("phases-1", "model_phases", {{"model", phases}}),
    });
    require(document.snapshot().entities().contains("phases-1"),
            "a valid model phases entity should be admitted by the document");

    const auto revision = document.revision();
    const auto missing_entity_phases = sketch::ModelPhases::create({"wall-1", "missing-wall"},
        {"wall-1", "missing-wall"}, {}).to_json();
    require_error(
        [&] {
            document.apply(ApplyEntityChanges{
                .expected_revision = revision,
                .entity_changes = {EntityChange::upsert(
                    entity("phases-1", "model_phases", {{"model", missing_entity_phases}}))},
            });
        },
        DocumentErrorCode::dangling_reference,
        "model phases must reject entity IDs absent from the document");
    require(document.revision() == revision,
            "a rejected model phases reference must not advance the revision");

    require_error(
        [&] {
            (void)Document::create({entity("assemblies-1", "assembly_model")});
        },
        DocumentErrorCode::invalid_entity,
        "assembly model entities must carry a validated embedded model");

    const auto levels = sketch::VerticalLevelGraph({{"ground", 0}, {"first", 3}},
        {{"ground-first", "ground", "first"}});
    auto levels_document = Document::create({
        entity("levels-1", "vertical_levels", {{"model", nlohmann::json::parse(levels.serialize())}}),
    });
    require(levels_document.snapshot().entities().contains("levels-1"),
            "vertical level graphs should be admitted by the document boundary");
    const auto levels_revision = levels_document.revision();
    auto invalid_levels = nlohmann::json::parse(levels.serialize());
    invalid_levels.at("links")[0].at("height_m") = -1;
    require_error(
        [&] {
            levels_document.apply(ApplyEntityChanges{
                .expected_revision = levels_revision,
                .entity_changes = {EntityChange::upsert(entity(
                    "levels-1", "vertical_levels", {{"model", invalid_levels}}))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "vertical level entities must reject invalid serialized links");
    require(levels_document.revision() == levels_revision,
            "a rejected vertical level graph must not advance the revision");

    const auto bound_graph = sketch::VerticalLevelGraph({{"ground", 0}, {"upper", 3}});
    auto bound_document = Document::create({
        entity("property-1", "property"),
        entity("building-1", "building", {{"property_id", "property-1"}}),
        entity("floor-1", "floor", {{"building_id", "building-1"},
                                       {"vertical_level_binding", {
                                           {"version", 1}, {"graph_id", "levels-1"},
                                           {"level_id", "ground"}}}}),
        entity("levels-1", "vertical_levels",
               {{"model", nlohmann::json::parse(bound_graph.serialize())}}),
    });
    require(bound_document.snapshot().entities().at("floor-1").properties
                    .at("vertical_level_binding").at("level_id") == "ground",
            "floors should retain a validated vertical level binding");
    const auto bound_revision = bound_document.revision();
    auto missing_level_binding = bound_document.snapshot().entities().at("floor-1");
    missing_level_binding.properties.at("vertical_level_binding").at("level_id") = "missing";
    require_error(
        [&] {
            bound_document.apply(ApplyEntityChanges{
                .expected_revision = bound_revision,
                .entity_changes = {EntityChange::upsert(std::move(missing_level_binding))},
            });
        },
        DocumentErrorCode::dangling_reference,
        "a floor binding must reject a level absent from its graph");
    require(bound_document.revision() == bound_revision,
            "a rejected floor level binding must not advance the revision");
    require_error(
        [&] {
            (void)Document::create({
                entity("property-1", "property"),
                entity("label-1", "label", {{"vertical_level_binding", {
                    {"version", 1}, {"graph_id", "levels-1"}, {"level_id", "ground"}}}}),
                entity("levels-1", "vertical_levels",
                       {{"model", nlohmann::json::parse(bound_graph.serialize())}}),
            });
        },
        DocumentErrorCode::invalid_entity,
        "only floor entities may carry a vertical level binding");

    auto grid_document = Document::create({
        entity("grid-1", "reference_grid",
               {{"model", sketch::ReferenceGridModel{}.to_json()}}),
    });
    require(grid_document.snapshot().entities().contains("grid-1"),
            "reference grids should be admitted by the document boundary");
    const auto grid_revision = grid_document.revision();
    auto invalid_grid = grid_document.snapshot().entities().at("grid-1");
    invalid_grid.properties.at("model").at("spacing_x_m") = 0.0;
    require_error(
        [&] {
            grid_document.apply(ApplyEntityChanges{
                .expected_revision = grid_revision,
                .entity_changes = {EntityChange::upsert(std::move(invalid_grid))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "reference grid entities must reject invalid model geometry");
    require(grid_document.revision() == grid_revision,
            "a rejected reference grid must not advance the revision");

    const auto terrain_model = sketch::TerrainSurface(
        "document fixture",
        {sketch::TerrainPoint{"p0", 0.0, 0.0, 100.0},
         sketch::TerrainPoint{"p1", 4.0, 0.0, 101.0},
         sketch::TerrainPoint{"p2", 0.0, 4.0, 102.0}},
        {sketch::TerrainTriangle{{0, 1, 2}}})
                                      .to_json();
    auto terrain_document = Document::create({
        entity("terrain-1", "terrain_surface", {{"model", terrain_model}}),
    });
    require(terrain_document.snapshot().entities().contains("terrain-1"),
            "terrain surfaces should be admitted by the document boundary");
    const auto terrain_revision = terrain_document.revision();
    auto invalid_terrain = terrain_document.snapshot().entities().at("terrain-1");
    invalid_terrain.properties.at("model").at("contour_interval_m") = 0.0;
    require_error(
        [&] {
            terrain_document.apply(ApplyEntityChanges{
                .expected_revision = terrain_revision,
                .entity_changes = {EntityChange::upsert(std::move(invalid_terrain))},
            });
        },
        DocumentErrorCode::invalid_entity,
        "terrain surfaces must reject invalid embedded models");
    require(terrain_document.revision() == terrain_revision,
            "a rejected terrain surface must not advance the revision");

    auto linked_terrain = entity("terrain-linked", "terrain_surface",
                                 {{"source_entity_id", "room-source"},
                                  {"model", terrain_model}});
    auto linked_document = Document::create({
        entity("room-source", "room_boundary"), std::move(linked_terrain),
    });
    const auto linked_revision = linked_document.revision();
    require_error(
        [&] {
            linked_document.apply(ApplyEntityChanges{
                .expected_revision = linked_revision,
                .entity_changes = {EntityChange::erase("room-source")},
            });
        },
        DocumentErrorCode::dangling_reference,
        "terrain source references must protect their source boundary");

    const auto relationships = sketch::RoomRelationshipSnapshot::create(
        {{"room-edge-1", sketch::RoomReferenceKind::room_boundary}}, {}).to_json();
    auto relationships_document = Document::create({
        entity("room-edge-1", "room_boundary"),
        entity("relationships-1", "room_relationships", {{"model", relationships}}),
    });
    require(relationships_document.snapshot().entities().contains("relationships-1"),
            "room relationship snapshots should be admitted when target roles match");

    const auto wrong_role = sketch::RoomRelationshipSnapshot::create(
        {{"wall-1", sketch::RoomReferenceKind::room_boundary}}, {}).to_json();
    require_error(
        [&] {
            (void)Document::create({
                entity("wall-1", "wall"),
                entity("relationships-1", "room_relationships", {{"model", wrong_role}}),
            });
        },
        DocumentErrorCode::invalid_entity,
        "room relationship references must match their declared semantic role");
}

void test_command_preview_replays_history_without_mutating_source() {
    auto document = Document::create({entity("site", "property")});
    document.apply(ApplyEntityChanges{.expected_revision = 0,
        .entity_changes = {EntityChange::upsert(entity("note", "label", {{"text", "original"}}))}});
    document.apply(NameRevision{.expected_revision = 1, .name = "named branch"});
    document.undo(document.revision());
    document.mark_saved(document.revision());
    const auto source = document.snapshot();
    const auto source_digest = sketch::document_snapshot_digest(source);
    const sketch::Command command = ApplyEntityChanges{.expected_revision = source.revision(),
        .entity_changes = {EntityChange::upsert(entity("new-note", "label", {{"text", "candidate"}}))},
        .message = "previewed compound command"};
    const auto preview = Document::preview_command(source, command);
    require(preview.entities().contains("new-note") && preview.revision() == source.revision() + 1,
            "preview must apply the real command to a private validated history");
    require(preview.history().size() == source.history().size() + 1 &&
                preview.named_revisions() == source.named_revisions(),
            "preview must retain abandoned and named history");
    require(sketch::document_snapshot_digest(document.snapshot()) == source_digest &&
                sketch::document_snapshot_digest(source) == source_digest,
            "preview must not modify source history, navigation, saved state or entities");
    document.apply(command);
    require(sketch::document_snapshot_digest(document.snapshot()) ==
                sketch::document_snapshot_digest(preview),
            "actual apply must reproduce the entire preview snapshot");
}

void test_command_preview_rejects_invalid_stale_and_read_only_sources() {
    auto document = Document::create({entity("site", "property")});
    const auto source = document.snapshot();
    const auto digest = sketch::document_snapshot_digest(source);
    require_error([&] { (void)Document::preview_command(source,
        ApplyEntityChanges{.expected_revision = 0, .entity_changes = {
            EntityChange::erase("site"),
            EntityChange::upsert(entity("building", "building", {{"property_id", "missing"}}))}}); },
        DocumentErrorCode::dangling_reference, "preview must run atomic reference validation");
    require_error([&] { (void)Document::preview_command(source,
        NameRevision{.expected_revision = 1, .name = "stale"}); },
        DocumentErrorCode::stale_revision, "preview must reject stale commands");
    require(sketch::document_snapshot_digest(source) == digest &&
                sketch::document_snapshot_digest(document.snapshot()) == digest,
            "failed preview must leave both input and live document unchanged");

    const auto read_only = Document::create({entity("future", "future_required_type", nlohmann::json::object(), true)});
    require_error([&] { (void)Document::preview_command(read_only.snapshot(),
        NameRevision{.expected_revision = 0, .name = "forbidden"}); },
        DocumentErrorCode::read_only, "preview must enforce retained read-only semantics");

    auto forged = source;
    auto& record = const_cast<sketch::RevisionRecord&>(forged.history().front());
    record.action = "apply";
    require_error([&] { (void)Document::preview_command(forged,
        NameRevision{.expected_revision = 0, .name = "forged"}); },
        DocumentErrorCode::invalid_history, "preview must validate source history before applying");
}

void test_validated_document_fork_preserves_authority_and_isolation() {
    auto document = Document::create({entity("site", "property")});
    auto never_saved = Document::fork(document.snapshot());
    require(!never_saved.saved_revision_optional() &&
                sketch::document_snapshot_digest(never_saved.snapshot()) ==
                    sketch::document_snapshot_digest(document.snapshot()),
            "validated fork must preserve the never-saved marker and full identity");
    document.apply(ApplyEntityChanges{.expected_revision = 0, .entity_changes = {
        EntityChange::upsert(entity("note", "label", {{"text", "original"}}))}});
    document.apply(NameRevision{.expected_revision = 1, .name = "retained name"});
    document.mark_saved(document.revision());
    document.undo(document.revision());
    const auto source = document.snapshot();
    const auto source_digest = sketch::document_snapshot_digest(source);
    auto candidate = Document::fork(source);
    require(sketch::document_snapshot_digest(candidate.snapshot()) == source_digest &&
                candidate.can_undo() == document.can_undo() &&
                candidate.can_redo() && candidate.dirty(),
            "fork must preserve both navigation stacks, names and lagging saved marker");
    candidate.redo(candidate.revision());
    candidate.apply(ApplyEntityChanges{.expected_revision = candidate.revision(),
        .entity_changes = {EntityChange::upsert(entity("private-note", "label"))}});
    require(candidate.snapshot().entities().contains("private-note") &&
                sketch::document_snapshot_digest(document.snapshot()) == source_digest &&
                sketch::document_snapshot_digest(source) == source_digest,
            "editing a private candidate must leave its source and live document intact");
    auto malformed = source;
    const_cast<sketch::RevisionRecord&>(malformed.history().front()).action = "apply";
    require_error([&] { (void)Document::fork(malformed); }, DocumentErrorCode::invalid_history,
                  "fork must validate history before accepting a candidate");
    auto read_only = Document::create({entity("future", "future_required_type",
                                             nlohmann::json::object(), true)});
    auto protected_copy = Document::fork(read_only.snapshot());
    require(!protected_copy.is_editable() &&
                sketch::document_snapshot_digest(protected_copy.snapshot()) ==
                    sketch::document_snapshot_digest(read_only.snapshot()),
            "fork must preserve opaque data and its read-only protection");
    require_error([&] { protected_copy.apply(NameRevision{.expected_revision = 0, .name = "blocked"}); },
                  DocumentErrorCode::read_only, "fork must not bypass read-only edit rejection");
}

void test_session_read_only_latch_survives_navigation_and_fork() {
    auto document = Document::create({entity("site", "property")});
    document.apply(NameRevision{.expected_revision = 0, .name = "first"});
    document.apply(NameRevision{.expected_revision = 1, .name = "second"});
    const auto undo_revision = document.undo(document.revision());
    require(document.can_undo() && document.can_redo(),
            "navigation stacks should exist before applying the session read-only latch");

    document.mark_read_only("external project change");
    require(!document.is_editable() && document.read_only_reason() == "external project change",
            "session read-only state should expose its durable reason");
    const auto snapshot = document.snapshot();
    require(!snapshot.is_editable() && snapshot.read_only_reason() == "external project change",
            "session read-only state should be captured in snapshots");
    require_error([&] { (void)document.undo(undo_revision); }, DocumentErrorCode::read_only,
                  "undo must not bypass a session read-only latch");
    require_error([&] { (void)document.redo(undo_revision); }, DocumentErrorCode::read_only,
                  "redo must not bypass a session read-only latch");

    auto fork = Document::fork(snapshot);
    require(!fork.is_editable() && fork.read_only_reason() == "external project change",
            "validated forks must preserve the session read-only latch");
    require_error([&] { fork.apply(NameRevision{.expected_revision = undo_revision, .name = "blocked"}); },
                  DocumentErrorCode::read_only,
                  "a fork must not clear the session read-only latch");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_compound_change_is_atomic_and_references_are_checked();
        test_stale_revision_is_rejected();
        test_undo_redo_and_branch_history_are_preserved();
        test_snapshots_are_immutable_copies_and_saved_revision_can_lag_head();
        test_unknown_optional_data_is_preserved_and_unknown_required_is_read_only();
        test_duplicate_changes_fail_without_state_mutation();
        test_non_finite_json_number_rolls_back_compound_command();
        test_canonical_reference_fields_enforce_target_type();
        test_asset_id_is_not_misclassified_as_an_entity_reference();
        test_asset_references_are_structurally_validated_atomically();
        test_persisted_string_bounds_are_enforced_before_mutation();
        test_current_architecture_types_and_boundary_links_are_structurally_validated();
        test_composite_wall_layers_are_validated_at_document_boundary();
        test_embedded_architectural_models_are_validated_at_document_boundary();
        test_command_preview_replays_history_without_mutating_source();
        test_command_preview_rejects_invalid_stale_and_read_only_sources();
        test_validated_document_fork_preserves_authority_and_isolation();
        test_session_read_only_latch_survives_navigation_and_fork();
    } catch (const std::exception& error) {
        std::cerr << "document_tests: unexpected exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "document_tests: unexpected non-standard exception\n";
        return 1;
    }
    return 0;
}
