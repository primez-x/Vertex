#pragma once

#include "sketch/geometry.hpp"
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sketch {

// Caller limits may only reduce these defaults. Counts include original legacy
// area records as well as normalized records; pair work counts admitted sources.
struct PincImportLimits {
    std::size_t max_bytes{64*1024*1024};
    std::size_t max_json_depth{64};
    std::size_t max_json_nodes{200'000};
    std::size_t max_string_bytes{32*1024*1024};
    std::size_t max_pages{128};
    std::size_t max_calculation_edges_per_page{2048};
    std::size_t max_interior_edges_per_page{2048};
    std::size_t max_total_edges{8192};
    std::size_t max_records{100'000};
    std::uint64_t max_calculation_pairs{250'000};
};
enum class PincImportDialect { modern_v42, legacy_v2 };

// Identity includes occurrence and collection; scalar_id_json preserves scalar
// type (e.g. string "1" differs from number 1); integral floating IDs canonicalize
// to integers within the JS exact-integer range. These are never native IDs.
struct PincSourceReference {
    std::size_t page_index{};
    std::string collection;
    std::optional<std::string> scalar_id_json;
    std::string json_pointer;
    std::string identity;
};
struct PincImportDiagnostic {
    std::string source_pointer;
    std::string code;
    std::string message;
};
struct PincDimensionPresentation {
    double size_metres{.62*.3048};
    std::string font{"Arial"};
    std::string color{"#2d5b82"};
};
struct PincSegmentPresentation {
    std::string color{"#1f5f94"};
    double weight_pixels{2}; // Source's visual Thickness, never physical depth.
    std::string line_type{"solid"};
    bool show_dimension{true};
    PincDimensionPresentation dimension;
    std::optional<Vec2> dimension_offset_metres;
    std::optional<std::string> shared_color;
    std::optional<double> shared_weight_pixels;
    std::optional<std::string> shared_line_type;
};
struct PincImportSegment {
    PincSourceReference source;
    Segment geometry;
    std::string source_kind;
    double source_sagitta_metres{};
    PincSegmentPresentation presentation;
    // Legacy exact analytical duplicates retain all their source references.
    // Geometry/presentation is the first source's, never a near-edge merge.
    std::vector<PincSourceReference> equivalent_sources;
};
struct PincAssignmentPresentation {
    std::string color;
    double opacity{.15};
    std::string hatch{"none"};
    std::string label_color{"#173f62"};
    double name_size_metres{.78*.3048};
    double calculation_size_metres{.65*.3048};
    bool show_name{true};
    bool show_calculation{true};
    bool sync_boundary_color{true};
    std::string boundary_color;
    double boundary_weight_pixels{2};
    std::string boundary_line_type{"solid"};
};
struct PincImportAssignment {
    PincSourceReference source;
    std::optional<std::string> face_key;
    std::string code;
    std::string name;
    bool known_category{}; // Descriptive only; no eligibility/facts are inferred.
    PincAssignmentPresentation presentation;
    std::optional<Vec2> name_position_metres;
    std::optional<Vec2> calculation_position_metres;
    std::optional<Vec2> cached_anchor_metres;
    std::optional<double> cached_area_square_metres;
    // A modern key is unresolved topology evidence; legacy entries refer to
    // original area segments. Neither is sampled-centroid face authority.
    std::vector<PincSourceReference> source_segment_references;
    bool references_resolved{};
    std::optional<std::size_t> legacy_area_index;
};
struct PincLegacyArea {
    PincSourceReference source;
    std::vector<PincImportSegment> segments; // Original order/orientation intact.
};
struct PincVisualWallReference {
    std::string type; // calc or interior
    PincSourceReference target;
    double parameter{};
};
struct PincImportSymbol {
    PincSourceReference source;
    std::string kind;
    Vec2 centre_metres;
    double width_metres{};
    double depth_metres{};
    double rotation_radians{};
    bool mirror_x{};
    bool mirror_y{};
    std::string door_hinge{"left"};
    int door_side{1};
    std::optional<PincVisualWallReference> wall_reference;
};
struct PincImportText {
    PincSourceReference source;
    std::string text;
    Vec2 position_metres;
    double size_metres{.75*.3048};
    double rotation_radians{};
    std::string color{"#163d63"};
    std::string font{"Segoe UI"};
    std::string alignment{"center"};
    bool bold{};
    bool italic{};
};
struct PincImportUnderlay {
    std::string source_pointer;
    Vec2 top_left_metres;
    double width_metres{};
    double opacity{};
    bool supported_raster_descriptor{};
    std::string mime_type;
    // Descriptor only; image bytes are not decoded or trusted in this layer.
    // Unsupported descriptors stay in the once-retained original source asset.
    std::optional<std::string> data_url;
};
struct PincImportPage {
    PincSourceReference source;
    std::string name;
    std::vector<PincImportSegment> calculation_segments;
    std::vector<PincImportSegment> interior_segments;
    std::vector<PincImportAssignment> assignments;
    std::vector<PincLegacyArea> legacy_areas;
    std::vector<PincImportSymbol> symbols;
    std::vector<PincImportText> texts;
    std::optional<PincImportUnderlay> underlay;
    bool ghost_previous{};
    bool show_print_guide{true};
    PincDimensionPresentation dimension;
};
struct PincImportProject {
    PincImportDialect dialect{};
    std::string source_version;
    std::optional<std::string> source_file_name;
    std::size_t current_page{};
    std::vector<PincImportPage> pages;
    std::vector<PincImportDiagnostic> diagnostics;
};

// Shared by the separate untrusted candidate transport. These helpers enforce
// hard/downward-only limits and duplicate-key/resource preflight, but confer no
// source schema or native authority. Identity ignores the supplied identity field.
void validate_pinc_import_limits(const PincImportLimits& limits);
[[nodiscard]] nlohmann::json parse_pinc_json_bounded(
    std::span<const std::byte> bytes, const PincImportLimits& limits = {});
[[nodiscard]] std::string pinc_source_identity(const PincSourceReference& source);

// Detached independently authored parser. Does not mutate a Document, execute
// source code, derive graph faces, decode/fetch references, or infer facts.
// Known modern source requires format PincSketch/version 4.2. Legacy accepts
// inspected numeric 2.* dialects. Unknown fields are diagnostics plus pointers;
// the caller must retain original bytes once. Invalid input/limits throw
// invalid_argument atomically, with a JSON pointer in structural diagnostics.
[[nodiscard]] PincImportProject parse_pinc_project(
    std::span<const std::byte> bytes, const PincImportLimits& limits = {});

} // namespace sketch
