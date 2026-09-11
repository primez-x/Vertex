#pragma once

#include "sketch/quantity.hpp"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

struct ReferencePoint { double x{}; double y{}; };
struct ReferenceCalibration {
    ReferencePoint first;
    ReferencePoint second;
    std::string known_distance;
    Unit input_unit{Unit::millimetre};
    double metres_per_source_unit{};
};
struct ReferenceTransform {
    double scale{1};
    double rotation_degrees{};
    bool flip_horizontal{};
    bool flip_vertical{};
    double intensity{1};
    bool visible{true};
};
struct ReferenceAsset {
    std::string id;
    // Portable project-relative provenance; never opened by this module.
    std::string source_path;
    std::string mime_type;
    std::string sha256;
    std::vector<std::byte> bytes;
    // One-based page request. Existence requires a downstream PDF decoder.
    std::optional<unsigned> page;
    std::optional<ReferenceCalibration> calibration;
    ReferenceTransform transform;
};

// Offline, deterministic catalog. Returned records are const; commands cannot
// mutate imported bytes. Import validates container signatures, not decodability.
class ReferenceAssetCatalog {
public:
    const ReferenceAsset& import(std::string id, std::string source_path,
        std::string mime_type, std::vector<std::byte> bytes,
        std::optional<unsigned> page = std::nullopt);
    const ReferenceAsset& at(const std::string& id) const;
    void calibrate(const std::string& id, ReferencePoint first, ReferencePoint second,
        std::string known_distance, Unit input_unit = Unit::millimetre);
    void set_transform(const std::string& id, ReferenceTransform transform);
    bool undo();
    // Measurement is in source coordinates and intentionally ignores view scale.
    double measure_metres(const std::string& id, ReferencePoint first, ReferencePoint second) const;
    nlohmann::json snapshot() const;
    static ReferenceAssetCatalog restore(const nlohmann::json& snapshot);
private:
    struct Edit {
        std::string id;
        std::optional<ReferenceCalibration> calibration;
        ReferenceTransform transform;
    };
    std::map<std::string, ReferenceAsset> assets_;
    std::vector<Edit> undo_;
};

} // namespace sketch
