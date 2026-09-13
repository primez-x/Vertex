#pragma once

#include "sketch/geometry.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Presentation-only records; deliberately carry no analytical classification.
struct AnnotationStyle {
    std::string font_family{"sans-serif"};
    double text_height_metres{0.15};
    double stroke_width_metres{0.002};
    std::string stroke_color{"#000000"};
    std::string fill_color{"#FFFFFF"};
    std::string fill_pattern{"none"}; // none, solid, hatch
    bool bold{};
    bool italic{};
};

struct AnnotationPlacement {
    Vec2 position;
    double rotation_radians{};
    double scale{1.0};
};

struct LabelTemplate {
    std::string id;
    std::string category;
    std::string content;
};

struct LabelInstance {
    std::string id;
    std::string template_id;
    std::string content;
    AnnotationStyle style;
    AnnotationPlacement placement;
    bool visible{true};
};

struct PresentationOverride {
    std::string target_kind; // area, object, output_view
    std::string target_id;
    AnnotationStyle style;
    bool visible{true};
};

struct SymbolStroke { Vec2 start; Vec2 end; };
struct SymbolDefinition {
    std::string id;
    std::string family;
    std::string category;
    double width_metres{};
    double depth_metres{};
    Vec2 anchor; // local origin at centre; preview vertices in metres
    double minimum_scale{0.01};
    double maximum_scale{100.0};
    std::vector<SymbolStroke> preview;
};

struct SymbolInstance {
    std::string id;
    std::string symbol_id;
    AnnotationPlacement placement;
    AnnotationStyle style;
    bool visible{true};
};

struct AnnotationState {
    std::vector<LabelInstance> labels;
    std::vector<SymbolInstance> symbols;
    std::vector<PresentationOverride> overrides;
};

[[nodiscard]] std::vector<LabelTemplate> default_label_templates();
[[nodiscard]] std::vector<SymbolDefinition> default_symbol_catalog();
// Empty query/category match all; query matching is case-insensitive and covers
// stable ID, family, and category without mutating the catalog records.
[[nodiscard]] std::vector<SymbolDefinition> filter_symbol_catalog(
    const std::vector<SymbolDefinition>&, std::string_view query,
    std::string_view category = {});
[[nodiscard]] LabelInstance instantiate_label(const LabelTemplate&, std::string id);
// Empty query/category match all; filtering does not mutate records.
[[nodiscard]] std::vector<LabelTemplate> filter_label_templates(
    const std::vector<LabelTemplate>&, std::string_view query, std::string_view category = {});
void validate_annotation_state(const AnnotationState&, const std::vector<SymbolDefinition>&);
void validate_symbol_catalog(const std::vector<SymbolDefinition>&);
[[nodiscard]] std::vector<SymbolStroke> placed_symbol_preview(
    const SymbolDefinition&, const AnnotationPlacement&);
[[nodiscard]] nlohmann::json encode_annotation_state(
    const AnnotationState&, const std::vector<SymbolDefinition>&);
[[nodiscard]] AnnotationState decode_annotation_state(
    const nlohmann::json&, const std::vector<SymbolDefinition>&);

} // namespace sketch
