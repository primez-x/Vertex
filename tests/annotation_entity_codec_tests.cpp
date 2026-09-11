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
    state.symbols.push_back({"symbol-1", catalog.front().id, {{4.0, 5.0}, 0.25, 1.5}, {}, true});
    state.overrides.push_back({"area", "area-1", {}, false});
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
        require(sketch::encode_annotation_state(decoded, sketch::default_symbol_catalog()) ==
                    sketch::encode_annotation_state(state, sketch::default_symbol_catalog()),
                "annotation state changed during entity decode");

        auto document = sketch::Document::create({entity});
        const auto path = std::filesystem::temp_directory_path() /
            "property-studio-annotation-entity.bldproj";
        std::filesystem::remove(path);
        (void)sketch::ProjectStore::save(path, document.snapshot());
        const auto reopened = sketch::ProjectStore::load(path).document.snapshot();
        const auto reopened_state = sketch::decode_annotation_entity(
            reopened.entities().at("annotations"));
        require(sketch::encode_annotation_state(reopened_state, sketch::default_symbol_catalog()) ==
                    sketch::encode_annotation_state(state, sketch::default_symbol_catalog()),
                "annotation entity did not survive save/reopen");
        std::filesystem::remove(path);

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
