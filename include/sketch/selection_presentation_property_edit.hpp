#pragma once

#include "sketch/annotation_catalog.hpp"
#include "sketch/document.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

enum class SelectionPresentationPropertyFamily { symbol, label, reference };
enum class SelectionPresentationPropertyUnchangedPolicy { reject, retain };

struct SelectionPresentationPropertyTarget {
    SelectionPresentationPropertyFamily family;
    // Actual annotation owner or reference entity; child_id is empty for references.
    std::string owner_id;
    std::string child_id;
};

// Optional fields retain their exact original source when absent or equal.
// Nested optional opacity/palette distinguishes retention from explicit clearing.
// Strings for appearance choices are validated against the annotation schema's
// closed enums and #RRGGBB colors. Quantities retain exact entered-unit receipts.
struct SelectionAnnotationStylePropertyEdit {
    std::optional<std::string> font_family;
    std::optional<Quantity> text_height;
    std::optional<Quantity> stroke_width;
    std::optional<std::string> stroke_color;
    std::optional<std::string> fill_color;
    std::optional<std::string> fill_pattern;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<std::string> text_alignment;
    std::optional<std::optional<double>> fill_opacity;
    std::optional<std::string> line_pattern;
};

struct SelectionAnnotationPresentationPropertyEdit {
    std::optional<double> rotation_radians;
    std::optional<bool> visible;
    std::optional<SelectionAnnotationStylePropertyEdit> style;
    // Actual layer on the child's existing property/building/floor/level.
    // Resolve declared owner context, or the original child layer for a
    // context-free carrier; contradictory owner context cannot fall back.
    // No owner recontextualization, coordinate conversion or material authority.
    std::optional<std::string> layer_id;
};

struct SelectionSymbolPropertyEditIntent {
    std::string owner_id;
    std::string child_id;
    SelectionAnnotationPresentationPropertyEdit presentation;
    // Physical saved-definition footprint, including placement.scale.
    std::optional<Quantity> width;
    std::optional<Quantity> depth;
    std::optional<std::optional<SymbolSvgPalette>> svg_palette;
};

struct SelectionLabelPropertyEditIntent {
    std::string owner_id;
    std::string child_id;
    SelectionAnnotationPresentationPropertyEdit presentation;
    std::optional<std::string> content;
};

struct SelectionReferencePropertyEditIntent {
    std::string entity_id;
    std::optional<std::array<Quantity, 2>> position;
    std::optional<double> rotation_degrees;
    std::optional<double> scale;
    std::optional<bool> visible;
    std::optional<double> intensity;
};

struct SelectionPresentationPropertyEditRequest {
    std::vector<SelectionPresentationPropertyTarget> selected;
    std::vector<SelectionSymbolPropertyEditIntent> symbols;
    std::vector<SelectionLabelPropertyEditIntent> labels;
    std::vector<SelectionReferencePropertyEditIntent> references;
    SelectionPresentationPropertyUnchangedPolicy unchanged_policy{
        SelectionPresentationPropertyUnchangedPolicy::reject};
};

// Original-source preparation only: a complete independently derived entity
// map, without a Document command, snapshot admission token or publication
// authority. The enclosing semantic compositor must admit the final result.
class SelectionPresentationPropertyEditPreparation final {
public:
    [[nodiscard]] const nlohmann::json& request() const noexcept;
    [[nodiscard]] const std::map<std::string, Entity, std::less<>>& candidate_entities() const noexcept;
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept;
private:
    SelectionPresentationPropertyEditPreparation(nlohmann::json,
        std::map<std::string, Entity, std::less<>>);
    nlohmann::json request_;
    std::map<std::string, Entity, std::less<>> candidate_;
    std::string source_digest_;
    friend SelectionPresentationPropertyEditPreparation replay_selection_presentation_property_edit_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
};

// Closed canonical v1 with full snapshot, authoring-v2, entity-map and saved
// revision fences. All members, including retained ones, appear exactly once
// in both the sorted selected roster and their typed lane. Qualified annotation
// identities may repeat across owners but never alias symbols and labels in
// one owner. References use their actual entity identity. Strict nullable patch
// fields, 1..1000 members, 4 MiB wire, 100000 values, depth 64. No caller maps,
// names, definitions, asset/calibration replacement, flips or model-plan changes.
// Envelope fields: version, expected_revision, source_snapshot_digest,
// source_authoring_digest, source_entities_digest, source_saved_revision,
// selected, symbols, labels, references, unchanged_policy. Annotation selected
// rows are {family,owner_id,child_id}; reference rows are {family,entity_id}.
// Annotation lanes carry presentation:{rotation_radians,visible,style,layer_id}
// with nullable fields. Style carries exactly this struct's eleven nullable
// fields. fill_opacity and svg_palette use null to retain, {value:null} to
// clear, or {value:<typed value>} to author. Palette values have exactly profile,
// outline_color,surface_color. Quantities use the shared exact v1 receipt shape.
// Symbol rows additionally have width,depth,svg_palette; label rows have content.
// Reference rows carry entity_id,position,rotation_degrees,scale,visible,intensity.
// Roster ordering is owner/entity ID then child ID; each lane follows that order.
[[nodiscard]] nlohmann::json validate_selection_presentation_property_edit_request(const nlohmann::json&);
[[nodiscard]] SelectionPresentationPropertyEditPreparation prepare_selection_presentation_property_edit(
    const DocumentSnapshot&, const SelectionPresentationPropertyEditRequest&);
[[nodiscard]] SelectionPresentationPropertyEditPreparation replay_selection_presentation_property_edit_preparation(
    const DocumentSnapshot&, const nlohmann::json&);
void require_selection_presentation_property_edit_source_current(
    const DocumentSnapshot&, const SelectionPresentationPropertyEditPreparation&);

} // namespace sketch
