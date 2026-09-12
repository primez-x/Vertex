#include "sketch/georeferencing_entity_codec.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

sketch::GeoreferencingContract fixture() {
    const sketch::GeoCrs crs{"EPSG:32613", "fixture projected metre CRS",
                             sketch::GeoCoordinateUnit::metre};
    const sketch::OfflineGeoResources resources{
        false, {{"proj/proj.db", std::string(64, 'a')}}};
    const sketch::AffineGeoTransform transform{2, 0, 10, 0, 3, 20};
    const std::vector<sketch::GeoControlPoint> points{
        {"b", 1, 0, 12, 20}, {"a", 0, 0, 10, 20}, {"c", 0, 1, 10, 26}};
    return sketch::GeoreferencingContract(crs, transform, points, resources);
}

template <typename F>
void rejects_document(F&& operation) {
    try {
        operation();
    } catch (const sketch::DocumentError& error) {
        require(error.code() == sketch::DocumentErrorCode::invalid_entity,
                "invalid georeferencing entity returned the wrong Document error");
        return;
    }
    throw std::runtime_error("invalid georeferencing entity accepted by Document");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        const auto contract = fixture();
        auto entity = sketch::make_georeferencing_entity("geo-1", contract);
        sketch::validate_georeferencing_entity(entity);
        require(sketch::decode_georeferencing_entity(entity).serialize() ==
                    contract.serialize(),
                "georeferencing entity decode changed the contract");

        auto document = sketch::Document::create({entity});
        const auto path = std::filesystem::temp_directory_path() /
            "property-studio-georeferencing-entity.bldproj";
        std::filesystem::remove(path);
        (void)sketch::ProjectStore::save(path, document.snapshot());
        const auto reopened = sketch::ProjectStore::load(path).document.snapshot();
        require(sketch::decode_georeferencing_entity(reopened.entities().at("geo-1"))
                    .serialize() == contract.serialize(),
                "georeferencing entity did not survive save/reopen");
        std::filesystem::remove(path);

        entity.properties["version"] = 2;
        rejects_document([&] { (void)sketch::Document::create({entity}); });
        entity = sketch::make_georeferencing_entity("geo-1", contract);
        entity.properties["extra"] = true;
        rejects_document([&] { (void)sketch::Document::create({entity}); });
        entity = sketch::make_georeferencing_entity("geo-1", contract);
        entity.properties["model"]["network_enabled"] = true;
        rejects_document([&] { (void)sketch::Document::create({entity}); });
        entity = sketch::make_georeferencing_entity("geo-1", contract);
        entity.properties["model"]["crs"]["extra"] = true;
        rejects_document([&] { (void)sketch::Document::create({entity}); });

        std::cout << "georeferencing entity codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
