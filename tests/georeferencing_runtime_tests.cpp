#include "sketch/document.hpp"
#include "sketch/georeferencing_runtime.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.is_open(), "could not open PROJ fixture");
    const auto size = input.tellg();
    require(size > 0, "PROJ fixture is empty");
    input.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(input.good() || input.eof(), "could not read PROJ fixture");
    return bytes;
}

sketch::GeoreferencingContract fixture(const std::filesystem::path& resource_root,
                                       bool valid_definition = true) {
    const auto database = resource_root / "proj" / "proj.db";
    const auto ini = resource_root / "proj" / "proj.ini";
    const auto database_hash = sketch::sha256_hex(read_bytes(database));
    const auto ini_hash = sketch::sha256_hex(read_bytes(ini));
    const sketch::GeoCrs crs{
        "EPSG:32613",
        valid_definition ? "+proj=utm +zone=13 +datum=WGS84 +units=m +no_defs"
                         : "not a CRS definition",
        sketch::GeoCoordinateUnit::metre};
    const sketch::OfflineGeoResources resources{
        false,
        {{"proj/proj.db", database_hash}, {"proj/proj.ini", ini_hash}}};
    const sketch::AffineGeoTransform transform{1, 0, 10, 0, 1, 20};
    const std::vector<sketch::GeoControlPoint> points{{"origin", 0, 0, 10, 20}};
    return sketch::GeoreferencingContract(crs, transform, points, resources);
}

template <typename F>
void rejects(F&& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid PROJ runtime state accepted");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected the PROJ resource root argument");
        const std::filesystem::path root = argv[1];
        const auto contract = fixture(root);
        const auto report = sketch::verify_georeferencing_runtime(
            contract, sketch::GeoreferencingRuntimeOptions{root, 32ULL * 1024 * 1024});
        require(!report.proj_version.empty(), "PROJ did not report a version");
        require(report.crs_identifier == "EPSG:32613", "CRS identifier was not retained");
        require(report.database_relative_path == "proj/proj.db",
                "PROJ database path was not retained");
        require(!report.network_enabled, "PROJ networking was enabled");
        require(report.crs_identifier_validated && report.crs_definition_validated,
                "CRS declarations were not validated");
        require(report.identity_transform_validated,
                "PROJ could not construct the declared identity operation");
        require(report.resources.size() == 2, "unexpected verified resource count");
        require(report.to_json().at("network_enabled") == false,
                "runtime report advertised network access");

        auto tampered = contract.to_json();
        tampered["offline_resources"][0]["sha256"] = std::string(64, '0');
        rejects([&] {
            (void)sketch::verify_georeferencing_runtime(
                sketch::GeoreferencingContract::from_json(tampered),
                sketch::GeoreferencingRuntimeOptions{root, 32ULL * 1024 * 1024});
        });

        const auto malformed = fixture(root, false);
        rejects([&] {
            (void)sketch::verify_georeferencing_runtime(
                malformed, sketch::GeoreferencingRuntimeOptions{root, 32ULL * 1024 * 1024});
        });

        const sketch::OfflineGeoResources missing_database{
            false, {{"proj/proj.ini", sketch::sha256_hex(read_bytes(root / "proj" / "proj.ini"))}}};
        const sketch::GeoreferencingContract no_database(
            contract.crs(), contract.transform(), contract.control_points(), missing_database);
        rejects([&] {
            (void)sketch::verify_georeferencing_runtime(
                no_database, sketch::GeoreferencingRuntimeOptions{root, 32ULL * 1024 * 1024});
        });

        rejects([&] {
            (void)sketch::verify_georeferencing_runtime(
                contract, sketch::GeoreferencingRuntimeOptions{root, 1});
        });

        std::cout << "georeferencing runtime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
