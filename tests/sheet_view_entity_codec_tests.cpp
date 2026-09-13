#include "sketch/project_store.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename F>
void rejects_document(F&& operation) {
    try {
        operation();
    } catch (const sketch::DocumentError& error) {
        require(error.code() == sketch::DocumentErrorCode::invalid_entity ||
                    error.code() == sketch::DocumentErrorCode::dangling_reference,
                "invalid sheet/view entity returned the wrong Document error");
        return;
    }
    throw std::runtime_error("invalid sheet/view entity accepted by Document");
}

sketch::SheetViewModel fixture() {
    sketch::CoordinatedView plan{"plan", "Ground floor"};
    sketch::DrawingSheet sheet;
    sheet.id = "sheet-a";
    sheet.number = "A101";
    sheet.viewports.push_back({"viewport-a", "plan", {10, 10, 180, 120}, 50});
    return sketch::SheetViewModel::create({plan}, {sheet});
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        const auto model = fixture();
        auto entity = sketch::make_sheet_view_entity("sheet-view", model);
        sketch::validate_sheet_view_entity(entity);
        require(sketch::decode_sheet_view_entity(entity).to_json() == model.to_json(),
                "sheet/view entity decode changed the semantic graph");

        auto document = sketch::Document::create({entity});
        const auto path = std::filesystem::temp_directory_path() /
            "property-studio-sheet-view-entity.bldproj";
        std::filesystem::remove(path);
        (void)sketch::ProjectStore::save(path, document.snapshot());
        const auto reopened = sketch::ProjectStore::load(path).document.snapshot();
        require(sketch::decode_sheet_view_entity(reopened.entities().at("sheet-view")).to_json() ==
                    model.to_json(),
                "sheet/view entity did not survive save/reopen");
        std::filesystem::remove(path);

        auto referenced_views = model.views();
        referenced_views[0].object_ids = {"wall-target"};
        const auto referenced_model = sketch::SheetViewModel::create(
            std::move(referenced_views), model.sheets());
        auto referenced_entity = sketch::make_sheet_view_entity("sheet-view", referenced_model);
        auto wall = sketch::Entity::create("wall");
        wall.id = "wall-target";
        auto referenced_document = sketch::Document::create({wall, referenced_entity});
        const auto referenced_path = std::filesystem::temp_directory_path() /
            "property-studio-sheet-view-references.bldproj";
        std::filesystem::remove(referenced_path);
        (void)sketch::ProjectStore::save(referenced_path, referenced_document.snapshot());
        const auto reopened_references = sketch::ProjectStore::load(referenced_path).document.snapshot();
        require(sketch::decode_sheet_view_entity(reopened_references.entities().at("sheet-view"))
                    .views().front().object_ids == std::vector<std::string>{"wall-target"},
                "coordinated-view object references did not survive save/reopen");
        std::filesystem::remove(referenced_path);

        referenced_entity.properties["model"]["views"][0]["object_ids"] = {"missing-object"};
        rejects_document([&] { (void)sketch::Document::create({wall, referenced_entity}); });

        entity.properties["version"] = 2;
        rejects_document([&] { (void)sketch::Document::create({entity}); });
        entity = sketch::make_sheet_view_entity("sheet-view", model);
        entity.properties["model"]["sheets"][0]["viewports"][0]["view_id"] = "missing";
        rejects_document([&] { (void)sketch::Document::create({entity}); });
        entity = sketch::make_sheet_view_entity("sheet-view", model);
        entity.properties["extra"] = true;
        rejects_document([&] { (void)sketch::Document::create({entity}); });

        std::cout << "sheet/view entity codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
