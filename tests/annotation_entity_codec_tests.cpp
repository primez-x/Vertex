#include "sketch/annotation_entity_codec.hpp"
#include "sketch/project_store.hpp"
#include "sketch/site_frame.hpp"
#include "support/noninteractive_errors.hpp"

#include <filesystem>
#include <cmath>
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

std::vector<sketch::Entity> framed_fixture(sketch::Entity owner) {
    const auto entity = [](std::string id, std::string type, nlohmann::json properties) {
        return sketch::Entity{std::move(id),std::move(type),std::move(properties),false,nlohmann::json::object()};
    };
    return {
        entity("property-1","property",{{"site_frame",{{"version",1},{"origin_m",{100,200,10}},
            {"rotation_radians",0},{"vertical_datum",{{"identifier","survey"},{"height_at_origin_m",150}}}}}}),
        entity("building-1","building",{{"property_id","property-1"},{"site_placement",{
            {"version",1},{"translation_m",{3,4,2}},{"rotation_radians",0}}}}),
        entity("floor-1","floor",{{"building_id","building-1"}}),
        entity("layer-ground","layer",{{"floor_id","floor-1"}}),
        entity("building-2","building",{{"property_id","property-1"},{"site_placement",{
            {"version",1},{"translation_m",{30,40,5}},{"rotation_radians",0}}}}),
        entity("floor-2","floor",{{"building_id","building-2"}}),
        entity("layer-other","layer",{{"floor_id","floor-2"}}),std::move(owner)};
}

void test_framed_owner(const sketch::AnnotationState& state) {
    auto owner=sketch::make_annotation_entity("framed",state);
    owner.properties["version"]=3;
    owner.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    owner.properties["state"]["symbols"][0]["placement"]["layer_id"]="layer-other";
    owner.extensions["vendor_owner"]={{"retain","exact"},{"presentation_frame","opaque vendor text"}};
    owner.properties["state"]["labels"][0]["vendor_label"]={{"presentation_frame",{{"mode","opaque"}}}};
    owner.properties["state"]["symbols"][0]["placement"]["vendor_placement"]={{"retain",7}};
    sketch::validate_annotation_entity(owner);
    auto document=sketch::Document::create(framed_fixture(owner));
    const std::vector<sketch::SiteAnnotationTarget> targets{{"framed","label-1"},{"framed","symbol-1"}};
    const auto placed=sketch::resolve_site_annotation_presentations(document.snapshot(),targets);
    require(placed.at(targets[0]).source_frame.building_id=="building-1" &&
        placed.at(targets[1]).source_frame.building_id=="building-2",
        "unscoped v3 children must resolve their own layer building");
    const auto label=sketch::site_transform_point({2,3,0},placed.at(targets[0]).forward);
    const auto symbol=sketch::site_transform_point({4,5,0},placed.at(targets[1]).forward);
    require(std::abs(label.x-105)<1e-9 && std::abs(label.y-207)<1e-9 && std::abs(label.z-12)<1e-9 &&
        std::abs(symbol.x-134)<1e-9 && std::abs(symbol.y-245)<1e-9 && std::abs(symbol.z-15)<1e-9,
        "child building pose must be applied once to authored coordinates");
    auto moved=owner;
    moved.properties["state"]["labels"][0]["placement"]["x"]=7;
    moved.properties["state"]["labels"][0]["placement"]["y"]=8;
    (void)document.apply(sketch::ApplyEntityChanges{document.revision(),
        {sketch::EntityChange::upsert(moved)}, {},"Move framed label"});
    require(document.snapshot().entities().at("framed")==moved,
        "transform command must preserve exact outer frame and unrelated child metadata");
    (void)document.undo(document.revision());
    require(document.snapshot().entities().at("framed")==owner,"framed transform undo must restore exact owner");
    (void)document.redo(document.revision());
    require(document.snapshot().entities().at("framed")==moved,"framed transform redo must restore exact owner");
    auto world=owner;world.properties["presentation_frame"]["mode"]="world";
    const auto world_document=sketch::Document::create(framed_fixture(world));
    const auto world_placed=sketch::resolve_site_annotation_presentations(world_document.snapshot(),targets);
    const auto point=sketch::site_transform_point({2,3,0},world_placed.at(targets[0]).forward);
    require(point.x==2 && point.y==3 && point.z==0,"explicit world owner retains child coordinates");
    require(world_document.snapshot().entities().at("framed")==world,
        "supported owner frame must preserve opaque vendor label, placement and owner metadata");
    auto sibling=owner;sibling.id="framed-other";
    sibling.properties["state"]["labels"][0]["placement"]["layer_id"]="layer-other";
    auto local_children=framed_fixture(owner);local_children.push_back(sibling);
    const auto local_document=sketch::Document::create(local_children);
    const std::vector<sketch::SiteAnnotationTarget> local_targets{{"framed","label-1"},{"framed-other","label-1"}};
    const auto local_placed=sketch::resolve_site_annotation_presentations(local_document.snapshot(),local_targets);
    require(local_placed.at(local_targets[0]).source_frame.building_id=="building-1" &&
        local_placed.at(local_targets[1]).source_frame.building_id=="building-2",
        "repeated child IDs remain owner-local and resolve each child's layer");
    for(const auto* collection:{"labels","symbols"})for(const bool in_placement:{false,true}) {
        auto invalid=owner;
        auto& child=invalid.properties["state"][collection][0];
        auto& target=in_placement ? child["placement"] : child;
        target["presentation_frame"]=owner.properties.at("presentation_frame");
        bool codec_refused=false;
        try { (void)sketch::decode_annotation_entity(invalid); }
        catch(const std::invalid_argument&) { codec_refused=true; }
        require(codec_refused,"framed annotation codec must reject presentation frames on children and placements");
        rejects_document([&]{(void)sketch::Document::create(framed_fixture(invalid));});
    }
    auto scoped=owner;
    for(const auto* key:{"property_id","building_id","floor_id","layer_id"})
        scoped.properties[key]=key==std::string_view("property_id") ? "property-1" :
            key==std::string_view("building_id") ? "building-1" :
            key==std::string_view("floor_id") ? "floor-1" : "layer-ground";
    sketch::validate_annotation_entity(scoped);
    for(const auto* key:{"property_id","building_id","floor_id","layer_id"}) {
        auto invalid=scoped;invalid.properties.erase(key);
        rejects_document([&]{(void)sketch::Document::create(framed_fixture(invalid));});
    }
    for(const auto version:{0,1,2,4}) {
        auto invalid=owner;invalid.properties["version"]=version;
        rejects_document([&]{(void)sketch::Document::create(framed_fixture(invalid));});
    }
    for(const auto frame:{nlohmann::json(nullptr),nlohmann::json{{"version",1}},
        nlohmann::json{{"mode","world"}},nlohmann::json{{"version",2},{"mode","world"}},
        nlohmann::json{{"version",1},{"mode","unknown"}},
        nlohmann::json{{"version",1},{"mode","world"},{"extra",true}}}) {
        auto invalid=owner;invalid.properties["presentation_frame"]=frame;
        rejects_document([&]{(void)sketch::Document::create(framed_fixture(invalid));});
    }
    for(const auto* key:{"presentation_frame","state","schema"}) {
        auto invalid=owner;invalid.properties.erase(key);
        rejects_document([&]{(void)sketch::Document::create(framed_fixture(invalid));});
    }
    auto invalid=owner;invalid.properties["extra"]=true;
    rejects_document([&]{(void)sketch::Document::create(framed_fixture(invalid));});
    invalid=owner;invalid.properties["level_id"]="level-1";
    rejects_document([&]{(void)sketch::Document::create(framed_fixture(invalid));});
    auto missing=framed_fixture(owner);missing.erase(missing.begin()+3);
    rejects_document([&]{(void)sketch::Document::create(missing);});
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        const auto state = fixture();
        test_framed_owner(state);
        auto entity = sketch::make_annotation_entity("annotations", state);
        sketch::validate_annotation_entity(entity);
        const auto decoded = sketch::decode_annotation_entity(entity);
        require(decoded.labels.front().placement.layer_id == "layer-ground" &&
                    decoded.symbols.front().placement.layer_id == "layer-ground",
                "annotation drawing-layer ownership changed during entity decode");
        require(sketch::encode_annotation_state(decoded, sketch::default_symbol_catalog()) ==
                    sketch::encode_annotation_state(state, sketch::default_symbol_catalog()),
                "annotation state changed during entity decode");

        const sketch::AnnotationEntityContext context{"property-1","building-1","floor-1","layer-ground","level-1"};
        const auto scoped=sketch::make_annotation_entity("scoped-annotations",state,context);
        require(scoped.properties.at("version")==2 && scoped.properties.size()==8 &&
            scoped.properties.at("property_id")=="property-1" && scoped.properties.at("layer_id")=="layer-ground" &&
            scoped.properties.at("level_id")=="level-1" &&
            sketch::decode_annotation_entity(scoped).labels.front().placement.layer_id=="layer-ground",
            "scoped outer v2 preserves complete context without changing inner state");
        for(const auto key:{"property_id","building_id","floor_id","layer_id"}) {
            auto invalid=scoped;invalid.properties.erase(key);
            rejects_document([&]{(void)sketch::Document::create({invalid});});
            invalid=scoped;invalid.properties[key]="bad id";
            rejects_document([&]{(void)sketch::Document::create({invalid});});
        }
        for(const auto key:{"level_id","property_id"})for(const auto value:{nlohmann::json(nullptr),nlohmann::json(17),nlohmann::json("")}) {
            auto invalid=scoped;invalid.properties[key]=value;
            rejects_document([&]{(void)sketch::Document::create({invalid});});
        }
        auto invalid_scope=scoped;invalid_scope.properties["version"]=1;
        rejects_document([&]{(void)sketch::Document::create({invalid_scope});});
        invalid_scope=scoped;invalid_scope.properties["unknown"]=true;
        rejects_document([&]{(void)sketch::Document::create({invalid_scope});});
        invalid_scope=scoped;invalid_scope.properties["version"]=3;
        rejects_document([&]{(void)sketch::Document::create({invalid_scope});});
        auto context_without_level=context;context_without_level.level_id.reset();
        const auto scoped_without_level=sketch::make_annotation_entity("scoped-annotations",state,context_without_level);
        require(scoped_without_level.properties.size()==7 && !scoped_without_level.properties.contains("level_id"),
            "scoped outer v2 permits absent level only");
        auto invalid_context=context;invalid_context.floor_id="bad id";
        try {(void)sketch::make_annotation_entity("annotations",state,invalid_context);
            throw std::runtime_error("builder accepted invalid drawing context");}
        catch(const std::invalid_argument&) {}

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
        auto future_dimension=dimension_entity;future_dimension.properties["state"]["version"]=10;
        rejects_document([&]{(void)sketch::Document::create({future_dimension});});
        std::filesystem::remove(path);
        auto palette_state = dimension_state;
        const auto palette_svg = sketch::filter_symbol_catalog(sketch::default_symbol_catalog(),
            "Basin Oval", "01_bathroom").front();
        palette_state.symbols.push_back({"palette-symbol", palette_svg.id,
            {{8.0, 9.0}, 0.7, 1.3, "layer-ground"}, {}, false});
        auto& palette_symbol = palette_state.symbols.back();
        palette_symbol.definition = palette_svg;
        palette_symbol.pinned_svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"M0 0L1 1\"/></svg>";
        palette_symbol.width_scale = 0.5;
        palette_symbol.depth_scale = 1.8;
        palette_symbol.flip_horizontal = true;
        palette_symbol.svg_palette = sketch::SymbolSvgPalette{"white-outline-2", "#A1b2C3", "#456789"};
        auto palette_entity = sketch::make_annotation_entity("palette-annotations", palette_state);
        palette_entity.properties["version"]=3;
        palette_entity.properties["presentation_frame"]={{"version",1},{"mode","building"}};
        palette_entity.required = true;
        palette_entity.extensions["vendor_palette_owner"] = {{"retain", 18}};
        palette_entity.properties["state"]["labels"][0]["vendor_label"] = "retain";
        palette_entity.properties["state"]["overrides"][0]["vendor_area"] = "retain";
        const auto palette_document = sketch::Document::create(framed_fixture(palette_entity));
        (void)sketch::ProjectStore::save(path, palette_document.snapshot());
        const auto palette_saved = sketch::ProjectStore::load(path).document.snapshot().entities().at("palette-annotations");
        const auto palette_reopened = sketch::decode_annotation_entity(palette_saved);
        require(palette_saved == palette_entity && palette_saved.properties.at("state").at("version") == 7 &&
                palette_reopened.symbols.back().svg_palette == palette_symbol.svg_palette &&
                palette_reopened.symbols.back().pinned_svg == palette_symbol.pinned_svg &&
                !palette_reopened.symbols.front().svg_palette && palette_reopened.labels.front().model_plan &&
                palette_reopened.overrides.back().paper_text_height_mm == 5.0,
            "Native save/reopen retains v7 palettes, exact artwork, transforms and opaque sibling metadata with older presentation features");
        auto palette_edited=palette_saved;
        palette_edited.properties["state"]["symbols"][1]["svg_palette"]["surface_color"]="#abcdef";
        auto palette_edit_document=sketch::Document::create(framed_fixture(palette_saved));
        (void)palette_edit_document.apply(sketch::ApplyEntityChanges{palette_edit_document.revision(),
            {sketch::EntityChange::upsert(palette_edited)}, {},"Change framed symbol palette"});
        require(palette_edit_document.snapshot().entities().at("palette-annotations")==palette_edited,
            "palette edit must preserve exact v3 frame and opaque siblings");
        (void)palette_edit_document.undo(palette_edit_document.revision());
        require(palette_edit_document.snapshot().entities().at("palette-annotations")==palette_saved,
            "palette undo must restore exact framed owner");
        (void)palette_edit_document.redo(palette_edit_document.revision());
        require(palette_edit_document.snapshot().entities().at("palette-annotations")==palette_edited,
            "palette redo must restore exact framed owner");
        auto malformed_palette_entity = palette_entity;
        malformed_palette_entity.properties["state"]["symbols"][1]["svg_palette"] = nullptr;
        rejects_document([&] { (void)sketch::Document::create({malformed_palette_entity}); });
        malformed_palette_entity = palette_entity;
        malformed_palette_entity.properties["state"]["symbols"][1]["svg_palette"]["extra"] = true;
        rejects_document([&] { (void)sketch::Document::create({malformed_palette_entity}); });
        malformed_palette_entity = palette_entity;
        malformed_palette_entity.properties["state"]["version"] = 6;
        rejects_document([&] { (void)sketch::Document::create({malformed_palette_entity}); });
        std::filesystem::remove(path);
        auto role_state=palette_state;
        role_state.labels.front().style.text_alignment="left";
        sketch::PresentationOverride area_name{"area_name","area-1",{},false};
        area_name.plan_label_offset=sketch::Vec2{-1.25,.5};area_name.plan_label_rotation_radians=.75;
        area_name.paper_text_height_mm=3;area_name.style.text_alignment="right";area_name.inherit_appearance=true;
        sketch::PresentationOverride area_calculation{"area_calculation","area-1",{},true};
        area_calculation.plan_label_offset=sketch::Vec2{2,-.75};area_calculation.plan_label_rotation_radians=-.25;
        area_calculation.paper_text_height_mm=6;area_calculation.style.bold=true;
        role_state.overrides.push_back(area_name);role_state.overrides.push_back(area_calculation);
        auto role_entity=sketch::make_annotation_entity("role-annotations",role_state);
        role_entity.properties["version"]=3;
        role_entity.properties["presentation_frame"]={{"version",1},{"mode","building"}};
        role_entity.extensions["vendor_owner"]="retain";
        role_entity.properties["state"]["labels"][0]["vendor_label"]={{"retain",1}};
        role_entity.properties["state"]["overrides"][2]["vendor_role"]={{"retain",2}};
        auto role_document=sketch::Document::create(framed_fixture(role_entity));
        (void)sketch::ProjectStore::save(path,role_document.snapshot());
        auto role_reopened=sketch::ProjectStore::load(path).document;
        const auto role_saved=role_reopened.snapshot().entities().at("role-annotations");
        const auto roles_decoded=sketch::decode_annotation_entity(role_saved);
        require(role_saved==role_entity && role_saved.properties.at("state").at("version")==8 &&
            roles_decoded.labels.front().style.text_alignment=="left" && roles_decoded.symbols.back().svg_palette &&
            roles_decoded.overrides[2].target_kind=="area_name" && !roles_decoded.overrides[2].visible &&
            roles_decoded.overrides[2].style.text_alignment=="right" && roles_decoded.overrides[3].target_kind=="area_calculation" &&
            roles_decoded.overrides[3].visible && roles_decoded.overrides[3].paper_text_height_mm==6,
            "native reopen must retain v8 independent roles/alignment, v7 palettes and raw unknown siblings");
        auto role_edit=role_entity;role_edit.properties["state"]["overrides"][3]["visible"]=false;
        (void)role_reopened.apply(sketch::ApplyEntityChanges{role_reopened.revision(),{sketch::EntityChange::upsert(role_edit)}, {},"Hide only calculation callout"});
        require(!sketch::decode_annotation_entity(role_reopened.snapshot().entities().at("role-annotations")).overrides[3].visible &&
            role_reopened.snapshot().entities().at("role-annotations").properties.at("state").at("overrides")[2]==role_entity.properties.at("state").at("overrides")[2],
            "editing calculation role changed the independent name or its raw metadata");
        (void)role_reopened.undo(role_reopened.revision());
        require(role_reopened.snapshot().entities().at("role-annotations")==role_entity,"undo lost exact role/alignment evidence");
        (void)role_reopened.redo(role_reopened.revision());
        require(role_reopened.snapshot().entities().at("role-annotations")==role_edit,"redo lost exact role/alignment evidence");
        auto invalid_role=role_entity;invalid_role.properties["state"]["version"]=7;
        rejects_document([&]{(void)sketch::Document::create({invalid_role});});
        invalid_role=role_entity;invalid_role.properties["state"]["overrides"][2]["hatch_scale"]=1;
        rejects_document([&]{(void)sketch::Document::create({invalid_role});});
        invalid_role=role_entity;invalid_role.properties["state"]["labels"][0]["style"]["text_alignment"]="justify";
        rejects_document([&]{(void)sketch::Document::create({invalid_role});});
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
        historical.symbols.front().svg_palette = sketch::SymbolSvgPalette{};
        auto historical_entity = sketch::make_annotation_entity("annotations", historical);
        historical_entity.properties["version"]=3;
        historical_entity.properties["presentation_frame"]={{"version",1},{"mode","building"}};
        historical_entity.properties["state"]["labels"][0]["vendor_label"]={{"retain",1}};
        auto unsupported_symbol = historical_entity;
        unsupported_symbol.properties["state"]["symbols"][0]["vendor_symbol"]={{"retain",2}};
        rejects_document([&] { (void)sketch::Document::create(framed_fixture(unsupported_symbol)); });
        historical_entity.extensions["vendor_symbols"]["symbol-1"]={{"retain",2}};
        historical_entity.properties["state"]["overrides"][0]["vendor_override"]={{"retain",3}};
        historical_entity.extensions["owner_context"] = "retain-me";
        historical_entity.required = true;
        auto migration_document = sketch::Document::create(framed_fixture(historical_entity));
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
