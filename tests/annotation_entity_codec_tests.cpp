#include "sketch/annotation_entity_codec.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

sketch::AnnotationState fixture() {
    const auto labels = sketch::default_label_templates();
    const auto catalog = sketch::default_symbol_catalog();
    sketch::AnnotationState state;
    state.labels.push_back(sketch::instantiate_label(labels.front(), "label-1"));
    state.labels.front().placement.position = {2.0, 3.0};
    state.labels.front().placement.layer_id = "layer-ground";
    state.symbols.push_back({"symbol-1", catalog.front().id,
                             {{4.0, 5.0}, 0.25, 1.5, "layer-ground"}, {}, true});
    state.symbols.front().width_scale = 1.7;
    state.symbols.front().depth_scale = 0.4;
    state.symbols.front().flip_horizontal = true;
    state.symbols.front().flip_vertical = true;
    state.overrides.push_back({"area", "area-1", {}, false});
    state.overrides.back().plan_label_offset = sketch::Vec2{0.25, -1.75};
    return state;
}

template <typename F>
void rejects_document(F&& operation) {
    try {
        operation();
    } catch (const sketch::DocumentError& error) {
        require(error.code() == sketch::DocumentErrorCode::invalid_entity,
                "invalid annotation entity returned the wrong Document error");
        return;
    }
    throw std::runtime_error("invalid annotation entity accepted by Document");
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        const auto state = fixture();
        auto entity = sketch::make_annotation_entity("annotations", state);
        sketch::validate_annotation_entity(entity);
        const auto decoded = sketch::decode_annotation_entity(entity);
        require(decoded.labels.front().placement.layer_id == "layer-ground" &&
                    decoded.symbols.front().placement.layer_id == "layer-ground",
                "annotation drawing-layer ownership changed during entity decode");
        require(sketch::encode_annotation_state(decoded, sketch::default_symbol_catalog()) ==
                    sketch::encode_annotation_state(state, sketch::default_symbol_catalog()),
                "annotation state changed during entity decode");

        auto document = sketch::Document::create({entity});
        const auto path = std::filesystem::temp_directory_path() /
            "vertex-annotation-entity.bldproj";
        std::filesystem::remove(path);

        auto plan_state=state;
        plan_state.labels.front().model_plan=true;
        auto plan_entity=sketch::make_annotation_entity("plan-annotations",plan_state);
        auto plan_document=sketch::Document::create({plan_entity});
        (void)sketch::ProjectStore::save(path,plan_document.snapshot());
        const auto plan_reopened=sketch::ProjectStore::load(path).document.snapshot();
        require(plan_reopened.entities().at("plan-annotations")==plan_entity &&
            sketch::decode_annotation_entity(plan_reopened.entities().at("plan-annotations")).labels.front().model_plan,
            "native project save/reopen must retain version 5 plan anchors");
        std::filesystem::remove(path);
        auto dimension_state=plan_state;
        dimension_state.overrides.front().inherit_appearance=true;
        sketch::PresentationOverride dimension;
        dimension.target_kind="wall_dimension";
        dimension.target_id="wall-1";
        dimension.visible=false;
        dimension.style.font_family="Inter";
        dimension.style.stroke_color="#123456";
        dimension.style.bold=true;
        dimension.plan_label_offset=sketch::Vec2{-0.5,0.75};
        dimension.paper_text_height_mm=5.0;
        dimension.plan_label_rotation_radians=-0.25;
        dimension.inherit_appearance=true;
        dimension_state.overrides.push_back(dimension);
        auto dimension_entity=sketch::make_annotation_entity("dimension-annotations",dimension_state);
        dimension_entity.required=true;
        dimension_entity.extensions["vendor_dimension_owner"]={{"retain",17}};
        dimension_entity.properties["state"]["labels"][0]["vendor_label"]="retain";
        dimension_entity.properties["state"]["overrides"][0]["vendor_area"]="retain";
        auto dimension_document=sketch::Document::create({dimension_entity});
        (void)sketch::ProjectStore::save(path,dimension_document.snapshot());
        const auto dimension_reopened=sketch::ProjectStore::load(path).document.snapshot();
        const auto& dimension_saved=dimension_reopened.entities().at("dimension-annotations");
        const auto dimension_decoded=sketch::decode_annotation_entity(dimension_saved);
        require(dimension_saved==dimension_entity && dimension_saved.properties.at("state").at("version")==6 &&
            dimension_decoded.labels.size()==1 && dimension_decoded.overrides.size()==2 &&
            dimension_decoded.labels.front().model_plan && dimension_decoded.overrides.front().plan_label_offset &&
            dimension_decoded.overrides.front().inherit_appearance && dimension_decoded.overrides.back().target_kind=="wall_dimension" &&
            dimension_decoded.overrides.back().paper_text_height_mm==5.0 &&
            dimension_decoded.overrides.back().plan_label_rotation_radians==-0.25 &&
            dimension_decoded.overrides.back().plan_label_offset &&
            dimension_decoded.overrides.back().plan_label_offset->x==-0.5 &&
            dimension_decoded.overrides.back().inherit_appearance && !dimension_decoded.overrides.back().visible,
            "Native save/reopen must retain v6 wall callouts, v5 plan anchors, v4 area placements and opaque sibling metadata together");
        auto future_dimension=dimension_entity;future_dimension.properties["state"]["version"]=7;
        rejects_document([&]{(void)sketch::Document::create({future_dimension});});
        std::filesystem::remove(path);
        (void)sketch::ProjectStore::save(path, document.snapshot());
        const auto reopened = sketch::ProjectStore::load(path).document.snapshot();
        const auto reopened_state = sketch::decode_annotation_entity(
            reopened.entities().at("annotations"));
        require(sketch::encode_annotation_state(reopened_state, sketch::default_symbol_catalog()) ==
                    sketch::encode_annotation_state(state, sketch::default_symbol_catalog()),
                "annotation entity did not survive save/reopen");
        std::filesystem::remove(path);

        // An old pinned definition is valid even when the installed catalog has
        // moved on. Only an explicit Document command may replace it.
        auto historical = fixture();
        const auto svg = sketch::filter_symbol_catalog(sketch::default_symbol_catalog(),
            "Toilet Close Coupled", "01_bathroom").front();
        historical.symbols.front().symbol_id = svg.id;
        historical.symbols.front().definition = svg;
        historical.symbols.front().definition->artwork_revision = 99;
        historical.symbols.front().pinned_svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"M0 0L1 1\"/></svg>";
        historical.symbols.front().visible = false;
        auto historical_entity = sketch::make_annotation_entity("annotations", historical);
        historical_entity.extensions["owner_context"] = "retain-me";
        historical_entity.required = true;
        auto migration_document = sketch::Document::create({historical_entity});
        const auto command = sketch::make_symbol_migration_command(
            migration_document.snapshot(), "annotations", "symbol-1", "<svg/>");
        (void)migration_document.apply(command);
        const auto migrated_entity = migration_document.snapshot().entities().at("annotations");
        const auto migrated_state = sketch::decode_annotation_entity(migrated_entity);
        require(!sketch::symbol_requires_migration(migrated_state.symbols.front(), sketch::default_symbol_catalog()),
                "migration command did not adopt current revision");
        auto preserved = migrated_entity;
        preserved.properties["state"]["symbols"][0]["definition"] = historical_entity.properties["state"]["symbols"][0]["definition"];
        preserved.properties["state"]["symbols"][0]["pinned_svg"] = historical_entity.properties["state"]["symbols"][0]["pinned_svg"];
        require(preserved == historical_entity,
                "migration changed label, transform, layer, style, visibility or entity metadata");
        (void)migration_document.undo(migration_document.revision());
        require(migration_document.snapshot().entities().at("annotations") == historical_entity,
                "undo did not restore exact historical artwork");
        const auto historical_receipt = sketch::ProjectStore::save(path, migration_document.snapshot());
        auto migration_reopened = sketch::ProjectStore::load(path).document;
        require(migration_reopened.snapshot().entities().at("annotations") == historical_entity,
                "save/reopen lost historical artwork");
        (void)migration_reopened.redo(migration_reopened.revision());
        require(migration_reopened.snapshot().entities().at("annotations") == migrated_entity,
                "redo after reopen did not restore migrated artwork");
        sketch::SaveOptions migration_save_options;
        migration_save_options.expected_destination_sha256 = historical_receipt.file_sha256;
        const auto migration_receipt = sketch::ProjectStore::save(path, migration_reopened.snapshot(), migration_save_options);
        auto migrated_reopened = sketch::ProjectStore::load(path).document;
        (void)migrated_reopened.undo(migrated_reopened.revision());
        require(migrated_reopened.snapshot().entities().at("annotations") == historical_entity,
                "undo after migrated save/reopen did not restore historical artwork");
        std::filesystem::remove(path);
        if (migration_receipt.backup_path) std::filesystem::remove(*migration_receipt.backup_path);

        entity.properties["version"] = 2;
        rejects_document([&] { (void)sketch::Document::create({entity}); });
        entity = sketch::make_annotation_entity("annotations", state);
        entity.properties["extra"] = true;
        rejects_document([&] { (void)sketch::Document::create({entity}); });
        entity = sketch::make_annotation_entity("annotations", state);
        entity.properties["state"]["symbols"][0]["symbol_id"] = "missing";
        rejects_document([&] { (void)sketch::Document::create({entity}); });

        std::cout << "annotation entity codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
