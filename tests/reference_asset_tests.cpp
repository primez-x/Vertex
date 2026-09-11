#include "sketch/reference_asset.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid reference accepted");
}
std::vector<std::byte> bytes(std::string_view value) {
    std::vector<std::byte> result;
    for (unsigned char c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}
}
int main() {
    try {
        sketch::ReferenceAssetCatalog catalog;
        const auto source = bytes("%PDF-1.7\nfixture");
        catalog.import("plan", "references/plan.pdf", "application/pdf", source, 2);
        const auto hash = catalog.at("plan").sha256;
        catalog.calibrate("plan", {10, 20}, {210, 20}, "100 mm");
        require(std::abs(catalog.measure_metres("plan", {10, 20}, {210, 20}) - .1) < .0000001,
            "100mm calibration wrong");
        catalog.set_transform("plan", {3, 45, true, true, .25, false});
        require(catalog.at("plan").bytes == source && catalog.at("plan").sha256 == hash,
            "transform changed source");
        require(std::abs(catalog.measure_metres("plan", {10, 20}, {210, 20}) - .1) < .0000001,
            "view transform changed measurement");
        const auto json = catalog.snapshot();
        auto reopened = sketch::ReferenceAssetCatalog::restore(nlohmann::json::parse(json.dump()));
        require(reopened.snapshot() == json, "snapshot round trip changed provenance");
        require(std::abs(reopened.measure_metres("plan", {10, 20}, {210, 20}) - .1) < .0000001,
            "reopen changed measurement");
        require(!reopened.undo(), "restore manufactured undo history");
        require(catalog.undo() && catalog.at("plan").transform.scale == 1 &&
            catalog.at("plan").calibration.has_value(), "transform undo lost calibration");
        require(catalog.undo() && !catalog.at("plan").calibration, "calibration undo failed");
        const auto clean = catalog.snapshot();
        rejects([&] { catalog.calibrate("plan", {0, 0}, {0, 0}, "100 mm"); });
        rejects([&] { catalog.calibrate("plan", {0, 0}, {1, 0}, "-1 mm"); });
        rejects([&] { catalog.set_transform("plan", {0}); });
        rejects([&] { catalog.set_transform("plan", {1, std::numeric_limits<double>::quiet_NaN()}); });
        rejects([&] { catalog.set_transform("plan", {1, 0, false, false, 2}); });
        require(catalog.snapshot() == clean && !catalog.undo(), "failed commands changed catalog");
        for (const auto* path : {"../plan.pdf", "a/../plan.pdf", "C:/plan.pdf", "/plan.pdf",
                "a\\plan.pdf", "a//plan.pdf", "a/NUL.pdf", "a/plan.pdf ", "https://x/plan.pdf"})
            rejects([&] { catalog.import("bad", path, "application/pdf", source, 1); });
        rejects([&] { catalog.import("plan", "other.pdf", "application/pdf", source, 1); });
        rejects([&] { catalog.import("bad", "plan.pdf", "application/pdf", source); });
        rejects([&] { catalog.import("bad", "plan.pdf", "application/pdf", source, 0); });
        rejects([&] { catalog.import("bad", "plan.png", "image/png", source); });
        catalog.import("png", "reference.png", "image/png", bytes("\x89PNG\r\n\x1a\n"));
        catalog.import("jpg", "reference.jpg", "image/jpeg", bytes("\xff\xd8\xff"));
        catalog.import("bmp", "reference.bmp", "image/bmp", bytes("BM"));
        catalog.import("tiff", "reference.tiff", "image/tiff", bytes(std::string_view("II\x2a\0", 4)));
        auto tampered = json;
        tampered["assets"][0]["bytes"][8] = 0;
        rejects([&] { (void)sketch::ReferenceAssetCatalog::restore(tampered); });
        tampered = json;
        tampered["assets"][0]["calibration"]["metres_per_source_unit"] = 1;
        rejects([&] { (void)sketch::ReferenceAssetCatalog::restore(tampered); });
        tampered = json;
        tampered["assets"].push_back(tampered["assets"][0]);
        rejects([&] { (void)sketch::ReferenceAssetCatalog::restore(tampered); });
        tampered = json;
        tampered["assets"][0]["bytes"][0] = 256;
        rejects([&] { (void)sketch::ReferenceAssetCatalog::restore(tampered); });
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "reference_asset_tests: " << error.what() << '\n';
        return 1;
    }
}
