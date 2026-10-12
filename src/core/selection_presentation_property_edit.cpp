#include "sketch/selection_presentation_property_edit.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_store.hpp"
#include "sketch/site_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Key = std::pair<std::string, std::string>;
constexpr std::size_t target_limit = 1000;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;

[[noreturn]] void reject(const char* message) {
    throw std::invalid_argument(std::string("Presentation Properties preparation: ") + message);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
struct Budget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("wire byte budget exceeded");
        bytes += count;
    }
    void quoted(std::string_view text) {
        add(2);
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c >= 0x80) {
                const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                if (!tail || tail >= text.size() - i) reject("invalid UTF-8 string");
                const auto first = static_cast<unsigned char>(text[i + 1]);
                if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
                    (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) reject("invalid UTF-8 string");
                for (std::size_t j = 1; j <= tail; ++j) {
                    const auto byte = static_cast<unsigned char>(text[i + j]);
                    if (byte < 0x80 || byte > 0xbf) reject("invalid UTF-8 string");
                }
                add(tail + 1); i += tail;
            } else add(c == '"' || c == '\\' ? 2 : c < 0x20 ? 6 : 1);
        }
    }
    void visit(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 100000) reject("JSON complexity budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON value");
        if (value.is_object()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); visit(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) visit(child, depth + 1);
        } else if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
std::string text(const Json& value, std::size_t limit, bool nonempty = true) {
    if (!value.is_string()) reject("string field required");
    const auto& result = value.get_ref<const std::string&>();
    if (result.size() > limit || (nonempty && result.empty())) reject("string field budget exceeded");
    return result;
}
std::string identity(const Json& value) {
    auto result = text(value, 256);
    if (result.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:") != std::string::npos)
        reject("invalid stable identity");
    return result;
}
double number(const Json& value) {
    if (!value.is_number()) reject("numeric field required");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) reject("finite numeric field required");
    if (value.is_number_unsigned()) {
        if (result < 0 || result >= 18446744073709551616.0 ||
            static_cast<std::uint64_t>(result) != value.get<std::uint64_t>()) reject("numeric field loses integer precision");
    } else if (value.is_number_integer()) {
        if (result < -9223372036854775808.0 || result >= 9223372036854775808.0 ||
            static_cast<std::int64_t>(result) != value.get<std::int64_t>()) reject("numeric field loses integer precision");
    }
    return result;
}
void boolean(const Json& value) { if (!value.is_boolean()) reject("boolean field required"); }
void positive(const Json& value) { if (number(value) <= 0) reject("positive field required"); }
void fraction(const Json& value) {
    const auto result = number(value);
    if (result < 0 || result > 1) reject("opacity or intensity must be between zero and one");
}
void color(const Json& value) {
    const auto result = text(value, 7);
    if (result.size() != 7 || result.front() != '#' ||
        result.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) reject("color must be #RRGGBB");
}
void choice(const Json& value, std::initializer_list<const char*> choices) {
    if (!value.is_string() || std::none_of(choices.begin(), choices.end(), [&](const char* option) {
        return value == option;
    })) reject("unsupported appearance enum");
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid source revision");
}
void digest(const Json& value) {
    const auto result = text(value, 64);
    if (result.size() != 64 || result.find_first_not_of("0123456789abcdef") != std::string::npos)
        reject("source fence must be canonical SHA-256");
}
Quantity quantity(const Json& value, int sign) {
    fields(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    fields(value.at("exact_metres"), {"numerator", "denominator"});
    (void)text(value.at("original_expression"), 4096);
    const auto result = decode_constraint_quantity_receipt(value);
    if (!std::isfinite(result.metres) || (sign == 1 && result.metres <= 0) || (sign == 0 && result.metres < 0))
        reject("quantity has invalid dimension sign");
    return result;
}
Json quantity(const Quantity& value) {
    const char* unit = nullptr;
    switch (value.entered_unit) {
    case Unit::metre: unit = "m"; break;
    case Unit::millimetre: unit = "mm"; break;
    case Unit::centimetre: unit = "cm"; break;
    case Unit::foot: unit = "ft"; break;
    case Unit::inch: unit = "in"; break;
    default: reject("unsupported quantity unit");
    }
    if (value.original_expression.size() > 4096) reject("quantity expression budget exceeded");
    Json result{{"version", 1}, {"original_expression", value.original_expression}, {"entered_unit", unit},
        {"exact_metres", {{"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result, -1);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        reject("quantity is internally inconsistent");
    return result;
}
template<class T> Json nullable(const std::optional<T>& value) { return value ? Json(*value) : Json(nullptr); }
Json quantity(const std::optional<Quantity>& value) { return value ? quantity(*value) : Json(nullptr); }
Json position(const std::optional<std::array<Quantity, 2>>& value) {
    return value ? Json::array({quantity((*value)[0]), quantity((*value)[1])}) : Json(nullptr);
}
void validate_position(const Json& value) {
    if (value.is_null()) return;
    if (!value.is_array() || value.size() != 2) reject("position requires two quantities");
    for (const auto& coordinate : value) (void)quantity(coordinate, -1);
}
Json palette(const SymbolSvgPalette& value) {
    if (value.profile.size() > 256 || value.outline_color.size() > 7 || value.surface_color.size() > 7)
        reject("palette string budget exceeded");
    return {{"profile", value.profile}, {"outline_color", value.outline_color}, {"surface_color", value.surface_color}};
}
void validate_palette(const Json& value) {
    fields(value, {"profile", "outline_color", "surface_color"});
    choice(value.at("profile"), {"white-outline-2"});
    color(value.at("outline_color")); color(value.at("surface_color"));
}
Json style(const SelectionAnnotationStylePropertyEdit& value) {
    for (const auto* field : {&value.font_family, &value.stroke_color, &value.fill_color, &value.fill_pattern,
                              &value.text_alignment, &value.line_pattern})
        if (*field && (*field)->size() > 256) reject("style string budget exceeded");
    Json opacity = nullptr;
    if (value.fill_opacity) opacity = {{"value", nullable(*value.fill_opacity)}};
    return {{"font_family", nullable(value.font_family)}, {"text_height", quantity(value.text_height)},
        {"stroke_width", quantity(value.stroke_width)}, {"stroke_color", nullable(value.stroke_color)},
        {"fill_color", nullable(value.fill_color)}, {"fill_pattern", nullable(value.fill_pattern)},
        {"bold", nullable(value.bold)}, {"italic", nullable(value.italic)},
        {"text_alignment", nullable(value.text_alignment)}, {"fill_opacity", std::move(opacity)},
        {"line_pattern", nullable(value.line_pattern)}};
}
void validate_style(const Json& value) {
    fields(value, {"font_family", "text_height", "stroke_width", "stroke_color", "fill_color", "fill_pattern",
        "bold", "italic", "text_alignment", "fill_opacity", "line_pattern"});
    if (!value.at("font_family").is_null()) (void)text(value.at("font_family"), 256);
    if (!value.at("text_height").is_null()) (void)quantity(value.at("text_height"), 1);
    if (!value.at("stroke_width").is_null()) (void)quantity(value.at("stroke_width"), 0);
    for (const auto* key : {"stroke_color", "fill_color"}) if (!value.at(key).is_null()) color(value.at(key));
    if (!value.at("fill_pattern").is_null()) choice(value.at("fill_pattern"), {"none", "solid", "hatch", "cross", "horizontal", "dots"});
    for (const auto* key : {"bold", "italic"}) if (!value.at(key).is_null()) boolean(value.at(key));
    if (!value.at("text_alignment").is_null()) choice(value.at("text_alignment"), {"left", "center", "right"});
    if (!value.at("line_pattern").is_null()) choice(value.at("line_pattern"), {"solid", "dash", "dot", "dashdot"});
    if (!value.at("fill_opacity").is_null()) {
        fields(value.at("fill_opacity"), {"value"});
        if (!value.at("fill_opacity").at("value").is_null()) fraction(value.at("fill_opacity").at("value"));
    }
}
Json presentation(const SelectionAnnotationPresentationPropertyEdit& value, bool pose) {
    if (value.layer_id && value.layer_id->size() > 256) reject("layer identity budget exceeded");
    Json result{{"rotation_radians", nullable(value.rotation_radians)}, {"visible", nullable(value.visible)},
        {"style", value.style ? style(*value.style) : Json(nullptr)}, {"layer_id", nullable(value.layer_id)}};
    if (pose) result["position"] = position(value.position);
    return result;
}
void validate_presentation(const Json& value, bool pose) {
    if (pose) fields(value, {"rotation_radians", "visible", "style", "layer_id", "position"});
    else fields(value, {"rotation_radians", "visible", "style", "layer_id"});
    if (!value.at("rotation_radians").is_null()) (void)number(value.at("rotation_radians"));
    if (!value.at("visible").is_null()) boolean(value.at("visible"));
    if (!value.at("style").is_null()) validate_style(value.at("style"));
    if (!value.at("layer_id").is_null()) (void)identity(value.at("layer_id"));
    if (pose) validate_position(value.at("position"));
}
const char* family_name(SelectionPresentationPropertyFamily family) {
    switch (family) {
    case SelectionPresentationPropertyFamily::symbol: return "symbol";
    case SelectionPresentationPropertyFamily::label: return "label";
    case SelectionPresentationPropertyFamily::reference: return "reference";
    }
    reject("unsupported selected family");
}
void source_metadata(const DocumentSnapshot& source) {
    if (!source.is_editable()) reject("captured source is read-only");
    if (source.history().empty() || source.history().size() > ProjectStore::maximum_revision_count ||
        source.revision() >= source.history().size() || source.history()[source.revision()].revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size()))
        reject("invalid retained source history or save state");
}
void authenticate_source(const DocumentSnapshot& source, const Json& request) {
    source_metadata(source);
    const auto saved = request.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(request.at("source_saved_revision"))};
    if (source.revision() != revision(request.at("expected_revision")) || source.saved_revision_optional() != saved ||
        request.at("source_snapshot_digest") != document_snapshot_digest(source) ||
        request.at("source_authoring_digest") != document_authoring_source_digest_v2(source) ||
        request.at("source_entities_digest") != entity_map_digest(source.entities())) reject("captured full source changed");
}
Key qualified(const Json& row) { return {identity(row.at("owner_id")), identity(row.at("child_id"))}; }

void validate_request(const Json& value) {
    Budget{}.visit(value); // Before copying, dumping or traversing nested codecs.
    fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "selected", "symbols", "labels", "references", "unchanged_policy"});
    if (!value.at("version").is_number_integer() ||
        (value.at("version") != 1 && value.at("version") != 2)) reject("unsupported request version");
    const bool pose = value.at("version") == 2;
    bool supplied_pose = false;
    (void)revision(value.at("expected_revision"));
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    for (const auto* key : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(key));
    choice(value.at("unchanged_policy"), {"reject", "retain"});
    const auto& selected = value.at("selected");
    if (!selected.is_array() || selected.empty() || selected.size() > target_limit) reject("invalid selected roster budget");
    std::map<Key, std::string> declared, covered;
    std::set<std::string> annotation_owners, reference_owners;
    Key previous;
    for (const auto& row : selected) {
        if (!row.is_object() || !row.contains("family")) reject("missing selected family");
        choice(row.at("family"), {"symbol", "label", "reference"});
        const auto family = row.at("family").get<std::string>();
        Key key;
        if (family == "reference") {
            fields(row, {"family", "entity_id"});
            key = {identity(row.at("entity_id")), {}};
            reference_owners.insert(key.first);
        } else {
            fields(row, {"family", "owner_id", "child_id"});
            key = qualified(row);
            annotation_owners.insert(key.first);
        }
        if ((!previous.first.empty() && key <= previous) || !declared.emplace(key, family).second)
            reject("selected roster must be sorted and unambiguous");
        previous = key;
    }
    for (const auto& id : reference_owners) if (annotation_owners.contains(id)) reject("reference aliases annotation owner");
    std::size_t count = 0;
    for (const auto* lane : {"symbols", "labels", "references"}) {
        const auto& rows = value.at(lane);
        if (!rows.is_array() || rows.size() > target_limit - count) reject("typed target budget exceeded");
        count += rows.size();
        Key prior;
        for (const auto& row : rows) {
            const bool symbol = std::string_view(lane) == "symbols", label = std::string_view(lane) == "labels";
            Key key;
            if (symbol || label) {
                if (symbol && pose) fields(row, {"owner_id", "child_id", "presentation", "width", "depth", "svg_palette",
                    "flip_horizontal", "flip_vertical"});
                else if (symbol) fields(row, {"owner_id", "child_id", "presentation", "width", "depth", "svg_palette"});
                else fields(row, {"owner_id", "child_id", "presentation", "content"});
                key = qualified(row);
                validate_presentation(row.at("presentation"), pose);
                if (pose && !row.at("presentation").at("position").is_null()) supplied_pose = true;
                if (symbol) {
                    for (const auto* dimension : {"width", "depth"})
                        if (!row.at(dimension).is_null()) (void)quantity(row.at(dimension), 1);
                    if (!row.at("svg_palette").is_null()) {
                        fields(row.at("svg_palette"), {"value"});
                        if (!row.at("svg_palette").at("value").is_null()) validate_palette(row.at("svg_palette").at("value"));
                    }
                } else if (!row.at("content").is_null()) (void)text(row.at("content"), 65536, false);
            } else {
                if (pose) fields(row, {"entity_id", "position", "rotation_degrees", "scale", "visible", "intensity",
                    "flip_horizontal", "flip_vertical"});
                else fields(row, {"entity_id", "position", "rotation_degrees", "scale", "visible", "intensity"});
                key = {identity(row.at("entity_id")), {}};
                validate_position(row.at("position"));
                if (!row.at("rotation_degrees").is_null()) (void)number(row.at("rotation_degrees"));
                if (!row.at("scale").is_null()) positive(row.at("scale"));
                if (!row.at("visible").is_null()) boolean(row.at("visible"));
                if (!row.at("intensity").is_null()) fraction(row.at("intensity"));
            }
            if (pose && !label) for (const auto* mirror : {"flip_horizontal", "flip_vertical"}) {
                if (row.at(mirror).is_null()) continue;
                boolean(row.at(mirror)); supplied_pose = true;
            }
            if ((!prior.first.empty() && key <= prior) ||
                !covered.emplace(key, symbol ? "symbol" : label ? "label" : "reference").second)
                reject("typed lanes must be sorted and unambiguous");
            prior = key;
        }
    }
    if (declared != covered) reject("selected roster must exactly match all typed members and families");
    if (pose && !supplied_pose) reject("request version 2 requires a supplied pose field");
    if (value.dump().size() > byte_limit) reject("wire byte budget exceeded"); // Also verifies portable UTF-8.
}

Json encode_request(const DocumentSnapshot& source, const SelectionPresentationPropertyEditRequest& request) {
    source_metadata(source);
    if (request.selected.empty() || request.selected.size() > target_limit || request.symbols.size() > target_limit ||
        request.labels.size() > target_limit || request.references.size() > target_limit) reject("typed target budget exceeded");
    if (request.unchanged_policy != SelectionPresentationPropertyUnchangedPolicy::reject &&
        request.unchanged_policy != SelectionPresentationPropertyUnchangedPolicy::retain) reject("unsupported unchanged policy");
    const bool pose = std::any_of(request.symbols.begin(), request.symbols.end(), [](const auto& intent) {
        return intent.presentation.position.has_value() || intent.flip_horizontal.has_value() || intent.flip_vertical.has_value();
    }) || std::any_of(request.labels.begin(), request.labels.end(), [](const auto& intent) {
        return intent.presentation.position.has_value();
    }) || std::any_of(request.references.begin(), request.references.end(), [](const auto& intent) {
        return intent.flip_horizontal.has_value() || intent.flip_vertical.has_value();
    });
    Json selected = Json::array(), symbols = Json::array(), labels = Json::array(), references = Json::array();
    Budget budget;
    const auto append = [&](Json& array, Json row) { budget.visit(row); array.push_back(std::move(row)); };
    for (const auto& target : request.selected) {
        if (target.owner_id.size() > 256 || target.child_id.size() > 256) reject("selected identity budget exceeded");
        if (target.family == SelectionPresentationPropertyFamily::reference) {
            if (!target.child_id.empty()) reject("reference cannot carry annotation child identity");
            append(selected, {{"family", family_name(target.family)}, {"entity_id", target.owner_id}});
        } else append(selected, {{"family", family_name(target.family)}, {"owner_id", target.owner_id}, {"child_id", target.child_id}});
    }
    for (const auto& intent : request.symbols) {
        if (intent.owner_id.size() > 256 || intent.child_id.size() > 256) reject("symbol identity budget exceeded");
        Json svg = nullptr;
        if (intent.svg_palette) svg = {{"value", *intent.svg_palette ? palette(**intent.svg_palette) : Json(nullptr)}};
        Json row{{"owner_id", intent.owner_id}, {"child_id", intent.child_id}, {"presentation", presentation(intent.presentation, pose)},
            {"width", quantity(intent.width)}, {"depth", quantity(intent.depth)}, {"svg_palette", std::move(svg)}};
        if (pose) {
            row["flip_horizontal"] = nullable(intent.flip_horizontal);
            row["flip_vertical"] = nullable(intent.flip_vertical);
        }
        append(symbols, std::move(row));
    }
    for (const auto& intent : request.labels) {
        if (intent.owner_id.size() > 256 || intent.child_id.size() > 256 ||
            (intent.content && intent.content->size() > 65536)) reject("label field budget exceeded");
        append(labels, {{"owner_id", intent.owner_id}, {"child_id", intent.child_id}, {"presentation", presentation(intent.presentation, pose)},
            {"content", nullable(intent.content)}});
    }
    for (const auto& intent : request.references) {
        if (intent.entity_id.size() > 256) reject("reference identity budget exceeded");
        Json row{{"entity_id", intent.entity_id}, {"position", position(intent.position)},
            {"rotation_degrees", nullable(intent.rotation_degrees)}, {"scale", nullable(intent.scale)},
            {"visible", nullable(intent.visible)}, {"intensity", nullable(intent.intensity)}};
        if (pose) {
            row["flip_horizontal"] = nullable(intent.flip_horizontal);
            row["flip_vertical"] = nullable(intent.flip_vertical);
        }
        append(references, std::move(row));
    }
    const auto sort = [](Json& array, bool roster, bool reference) {
        std::sort(array.begin(), array.end(), [&](const Json& a, const Json& b) {
            const auto key = [&](const Json& row) -> Key {
                if (reference || (roster && row.at("family") == "reference")) return {row.at("entity_id").get<std::string>(), {}};
                return {row.at("owner_id").get<std::string>(), row.at("child_id").get<std::string>()};
            };
            return key(a) < key(b);
        });
    };
    sort(selected, true, false); sort(symbols, false, false); sort(labels, false, false); sort(references, false, true);
    Json result{{"version", pose ? 2 : 1}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"selected", std::move(selected)}, {"symbols", std::move(symbols)}, {"labels", std::move(labels)},
        {"references", std::move(references)},
        {"unchanged_policy", request.unchanged_policy == SelectionPresentationPropertyUnchangedPolicy::retain ? "retain" : "reject"}};
    validate_request(result);
    return result;
}

const Entity& owner(const DocumentSnapshot& source, const std::string& id, const char* type) {
    const auto found = source.entities().find(id);
    if (found == source.entities().end() || found->second.id != id || found->second.type != type)
        reject("actual selected owner is missing or has the wrong identity or type");
    return found->second;
}
void same_context(const DrawingContext& a, const DrawingContext& b) {
    if (a.property_id != b.property_id || a.building_id != b.building_id || a.floor_id != b.floor_id || a.level_id != b.level_id)
        reject("annotation layer must share its owner's actual property, building, floor and level");
}
void validate_child_context(const ProjectOrganization& organization, const Entity& entity, const AnnotationPlacement& placement) {
    // Legacy context-free overlays retain their historical representation. A
    // declared owner context or child layer must resolve, including identity edits.
    const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
        entity.properties.contains("floor_id") || entity.properties.contains("layer_id") || entity.properties.contains("level_id");
    const auto context = organization.drawing_context(entity.id);
    if (scoped && !context) reject("annotation owner context is unresolved or contradictory");
    if (entity.properties.contains("level_id") &&
        (!context || entity.properties.at("level_id") != context->level_id)) reject("annotation owner level contradicts its actual floor");
    if (!placement.layer_id.empty()) {
        const auto layer = organization.nodes.find(placement.layer_id);
        const auto target = organization.drawing_context(placement.layer_id);
        if (layer == organization.nodes.end() || layer->second.type != "layer" || !target)
            reject("annotation child layer is not a real resolved layer");
        if (context) same_context(*context, *target);
    }
}
void assign_layer(const ProjectOrganization& organization, const Entity& source, Json& placement, const Json& request) {
    if (request.is_null()) return;
    auto context = organization.drawing_context(source.id);
    const bool scoped = source.properties.contains("property_id") || source.properties.contains("building_id") ||
        source.properties.contains("floor_id") || source.properties.contains("layer_id") || source.properties.contains("level_id");
    // A context-free carrier can legitimately contain independently scoped
    // children. Resolve this child's ORIGINAL layer, never the requested layer,
    // as its migration source. A contradictory declared owner cannot fall back.
    if (!context && !scoped && placement.contains("layer_id") && placement.at("layer_id").is_string())
        context = organization.drawing_context(placement.at("layer_id").get<std::string>());
    const auto id = identity(request);
    const auto found = organization.nodes.find(id);
    const auto destination = organization.drawing_context(id);
    if (!context || found == organization.nodes.end() || found->second.type != "layer" || !destination)
        reject("layer assignment requires resolved original child context and actual destination layer");
    same_context(*context, *destination);
    if (!placement.contains("layer_id") || placement.at("layer_id") != request) placement["layer_id"] = request;
}

void promote(Entity& entity, const AnnotationState& original, int minimum_version) {
    auto& state = entity.properties.at("state");
    const auto version = state.at("version").get<int>();
    if (version >= minimum_version) return;
    // Schema promotion uses the ORIGINAL decoded artwork, adds only required
    // symbol fields, and never serializes whole siblings/styles/definitions.
    if (version < 3) {
        const auto encoded = encode_annotation_state(original, default_symbol_catalog());
        for (std::size_t i = 0; i < original.symbols.size(); ++i)
            for (const auto* key : {"definition", "pinned_svg", "width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
                if (!state.at("symbols").at(i).contains(key)) state["symbols"][i][key] = encoded.at("symbols").at(i).at(key);
    }
    state["version"] = std::max(3, minimum_version);
}
void patch(Json& raw, const char* key, const Json& value, const Json& effective) {
    if (!value.is_null() && value != effective) raw[key] = value;
}
void patch_style(Entity& candidate, const AnnotationState& original, Json& raw, const AnnotationStyle& old, const Json& request) {
    if (request.is_null()) return;
    patch(raw, "font_family", request.at("font_family"), old.font_family);
    for (const auto* dimension : {"text_height", "stroke_width"}) {
        if (request.at(dimension).is_null()) continue;
        const auto value = quantity(request.at(dimension), std::string_view(dimension) == "text_height" ? 1 : 0).metres;
        const auto previous = std::string_view(dimension) == "text_height" ? old.text_height_metres : old.stroke_width_metres;
        if (value != previous) raw[std::string(dimension) + "_metres"] = value;
    }
    patch(raw, "stroke_color", request.at("stroke_color"), old.stroke_color);
    patch(raw, "fill_color", request.at("fill_color"), old.fill_color);
    patch(raw, "bold", request.at("bold"), old.bold);
    patch(raw, "italic", request.at("italic"), old.italic);
    const auto& fill = request.at("fill_pattern");
    if (!fill.is_null() && fill != old.fill_pattern) {
        if (fill == "cross" || fill == "horizontal" || fill == "dots") promote(candidate, original, 9);
        raw["fill_pattern"] = fill;
    }
    const auto& alignment = request.at("text_alignment");
    if (!alignment.is_null() && alignment != old.text_alignment) {
        promote(candidate, original, 8); raw["text_alignment"] = alignment;
    }
    const auto& line = request.at("line_pattern");
    if (!line.is_null() && line != old.line_pattern) {
        promote(candidate, original, 9); raw["line_pattern"] = line;
    }
    if (!request.at("fill_opacity").is_null()) {
        const auto& value = request.at("fill_opacity").at("value");
        if (value != nullable(old.fill_opacity)) {
            if (value.is_null()) raw.erase("fill_opacity");
            else { promote(candidate, original, 9); raw["fill_opacity"] = value; }
        }
    }
}
void patch_presentation(Entity& candidate, const Entity& source, const AnnotationState& original,
    const ProjectOrganization& organization, Json& raw, const AnnotationPlacement& placement,
    const AnnotationStyle& old_style, bool visible, const Json& request) {
    if (request.contains("position") && !request.at("position").is_null()) {
        // The request retains exact quantities; the saved annotation schema owns
        // numeric coordinates only. Retain each equal raw axis independently.
        for (std::size_t i = 0; i < 2; ++i) {
            const auto wanted = quantity(request.at("position").at(i), -1).metres;
            const auto previous = i == 0 ? placement.position.x : placement.position.y;
            if (wanted != previous) raw["placement"][i == 0 ? "x" : "y"] = wanted;
        }
    }
    patch(raw.at("placement"), "rotation_radians", request.at("rotation_radians"), placement.rotation_radians);
    patch(raw, "visible", request.at("visible"), visible);
    assign_layer(organization, source, raw.at("placement"), request.at("layer_id"));
    patch_style(candidate, original, raw.at("style"), old_style, request.at("style"));
}

struct AnnotationOwner {
    const Entity* source;
    AnnotationState original;
    Entity candidate;
};
void replay_annotations(const DocumentSnapshot& source, const Json& request, Entities& result) {
    const auto organization = organize_project(source);
    std::vector<SiteAnnotationTarget> frame_targets;
    for (const auto* lane : {"symbols", "labels"}) for (const auto& row : request.at(lane))
        frame_targets.push_back({row.at("owner_id").get<std::string>(), row.at("child_id").get<std::string>()});
    if (!frame_targets.empty()) (void)resolve_site_annotation_presentations(source.entities(), frame_targets);
    std::map<std::string, AnnotationOwner> owners;
    for (const auto* lane : {"symbols", "labels"}) for (const auto& row : request.at(lane)) {
        const auto id = row.at("owner_id").get<std::string>();
        auto found = owners.find(id);
        if (found == owners.end()) {
            const auto& entity = owner(source, id, kAnnotationEntityType);
            found = owners.emplace(id, AnnotationOwner{&entity, decode_annotation_entity(entity), entity}).first;
        }
        auto& item = found->second;
        const auto child_id = row.at("child_id").get<std::string>();
        const bool symbol = std::string_view(lane) == "symbols";
        std::size_t matches = 0, index = 0;
        for (std::size_t i = 0; i < item.original.symbols.size(); ++i) if (item.original.symbols[i].id == child_id) {
            ++matches; if (symbol) index = i; else reject("label target actually names a symbol");
        }
        for (std::size_t i = 0; i < item.original.labels.size(); ++i) if (item.original.labels[i].id == child_id) {
            ++matches; if (!symbol) index = i; else reject("symbol target actually names a label");
        }
        if (matches != 1) reject("selected annotation child is missing or ambiguous");
        auto& raw = item.candidate.properties.at("state").at(lane).at(index);
        if (symbol) {
            const auto& old = item.original.symbols.at(index);
            validate_child_context(organization, *item.source, old.placement);
            patch_presentation(item.candidate, *item.source, item.original, organization, raw,
                old.placement, old.style, old.visible, row.at("presentation"));
            if (request.at("version") == 2) for (const auto* mirror : {"flip_horizontal", "flip_vertical"}) {
                const auto& value = row.at(mirror);
                const bool previous = std::string_view(mirror) == "flip_horizontal" ? old.flip_horizontal : old.flip_vertical;
                if (!value.is_null() && value != previous) {
                    promote(item.candidate, item.original, 3);
                    raw[mirror] = value;
                }
            }
            // Appearance authoring uses the saved definition directly. The
            // rendering resolver intentionally suppresses stale external SVG
            // paths and must not remove saved palette eligibility here.
            const auto catalog = default_symbol_catalog();
            const auto entry = std::find_if(catalog.begin(), catalog.end(), [&](const auto& value) { return value.id == old.symbol_id; });
            if (!old.definition && entry == catalog.end()) reject("symbol definition is missing");
            const auto& definition = old.definition ? *old.definition : *entry;
            for (const auto* dimension : {"width", "depth"}) {
                if (row.at(dimension).is_null()) continue;
                const bool width = std::string_view(dimension) == "width";
                const auto base = width ? definition.width_metres : definition.depth_metres;
                const auto axis = width ? old.width_scale : old.depth_scale;
                const auto original_size = base * (old.placement.scale * axis);
                const auto wanted = quantity(row.at(dimension), 1).metres;
                if (wanted == original_size) continue;
                const auto next = (wanted / base) / old.placement.scale;
                if (!std::isfinite(next) || next <= 0) reject("symbol physical axis scale overflow");
                promote(item.candidate, item.original, 3);
                raw[width ? "width_scale" : "depth_scale"] = next;
            }
            if (!row.at("svg_palette").is_null()) {
                const auto& value = row.at("svg_palette").at("value");
                const auto previous = old.svg_palette ? palette(*old.svg_palette) : Json(nullptr);
                if (previous != value) {
                    if (value.is_null()) raw.erase("svg_palette");
                    else {
                        if (!definition.svg_asset) reject("palette requires saved SVG-backed symbol definition");
                        promote(item.candidate, item.original, 7);
                        raw["svg_palette"] = value; raw["svg_palette"]["version"] = 1;
                    }
                }
            }
        } else {
            const auto& old = item.original.labels.at(index);
            validate_child_context(organization, *item.source, old.placement);
            patch_presentation(item.candidate, *item.source, item.original, organization, raw,
                old.placement, old.style, old.visible, row.at("presentation"));
            patch(raw, "content", row.at("content"), old.content);
        }
    }
    for (auto& [id, item] : owners) {
        // Admits saved-definition scale limits, style/version semantics,
        // immutable pinned artwork and all original sibling identities.
        validate_annotation_entity(item.candidate);
        result.at(id) = std::move(item.candidate);
    }
    if (!frame_targets.empty()) (void)resolve_site_annotation_presentations(result, frame_targets);
}

double reference_number(const Entity& entity, const char* key, double fallback) {
    return entity.properties.contains(key) ? number(entity.properties.at(key)) : fallback;
}
bool reference_boolean(const Entity& entity, const char* key, bool fallback) {
    if (!entity.properties.contains(key)) return fallback;
    boolean(entity.properties.at(key)); return entity.properties.at(key).get<bool>();
}
void validate_reference(const DocumentSnapshot& source, const Entity& entity) {
    const auto& p = entity.properties;
    if (!p.is_object() || !p.contains("asset_id") || !p.contains("position_m")) reject("reference source fields are missing");
    for (const auto* key : {"asset_id", "render_asset_id"}) {
        if (!p.contains(key)) continue;
        const auto id = identity(p.at(key));
        if (!source.assets().contains(id) || source.assets().at(id).id != id) reject("reference asset is missing or ambiguous");
    }
    if (!p.at("position_m").is_array() || p.at("position_m").size() != 2) reject("invalid reference position");
    for (const auto& coordinate : p.at("position_m")) (void)number(coordinate);
    if (reference_number(entity, "scale", 1) <= 0 || reference_number(entity, "metres_per_source_unit", .01) <= 0)
        reject("reference scale and calibration must remain finite and positive");
    const auto intensity = reference_number(entity, "intensity", 1);
    if (intensity < 0 || intensity > 1) reject("invalid reference intensity");
    (void)reference_number(entity, "rotation_degrees", 0);
    for (const auto* key : {"visible", "flip_horizontal", "flip_vertical"}) (void)reference_boolean(entity, key, std::string_view(key) == "visible");
}
void replay_references(const DocumentSnapshot& source, const Json& request, Entities& result) {
    std::vector<std::string> frame_targets;
    for (const auto& row : request.at("references")) frame_targets.push_back(row.at("entity_id").get<std::string>());
    if (!frame_targets.empty()) (void)resolve_site_presentations(source.entities(), frame_targets);
    for (const auto& row : request.at("references")) {
        const auto id = row.at("entity_id").get<std::string>();
        const auto& original = owner(source, id, "reference_asset");
        validate_reference(source, original);
        auto candidate = original;
        auto& p = candidate.properties;
        if (!row.at("position").is_null()) {
            for (std::size_t i = 0; i < 2; ++i) {
                const auto wanted = quantity(row.at("position").at(i), -1).metres;
                if (wanted != number(original.properties.at("position_m").at(i))) p["position_m"][i] = wanted;
            }
        }
        patch(p, "rotation_degrees", row.at("rotation_degrees"), reference_number(original, "rotation_degrees", 0));
        patch(p, "scale", row.at("scale"), reference_number(original, "scale", 1));
        patch(p, "visible", row.at("visible"), reference_boolean(original, "visible", true));
        patch(p, "intensity", row.at("intensity"), reference_number(original, "intensity", 1));
        if (request.at("version") == 2) for (const auto* mirror : {"flip_horizontal", "flip_vertical"})
            patch(p, mirror, row.at(mirror), reference_boolean(original, mirror, false));
        validate_reference(source, candidate);
        result.at(id) = std::move(candidate);
    }
    if (!frame_targets.empty()) (void)resolve_site_presentations(result, frame_targets);
}
bool exact(const Entities& a, const Entities& b) {
    if (a.size() != b.size()) return false;
    auto left = a.begin(), right = b.begin();
    for (; left != a.end(); ++left, ++right) if (left->first != right->first || !(left->second == right->second) ||
        left->second.properties.dump() != right->second.properties.dump() || left->second.extensions.dump() != right->second.extensions.dump()) return false;
    return true;
}
} // namespace

SelectionPresentationPropertyEditPreparation::SelectionPresentationPropertyEditPreparation(Json request, Entities candidate)
    : request_(std::move(request)), candidate_(std::move(candidate)),
      source_digest_(request_.at("source_snapshot_digest").get<std::string>()) {}
const Json& SelectionPresentationPropertyEditPreparation::request() const noexcept { return request_; }
const Entities& SelectionPresentationPropertyEditPreparation::candidate_entities() const noexcept { return candidate_; }
const std::string& SelectionPresentationPropertyEditPreparation::source_snapshot_digest() const noexcept { return source_digest_; }

Json validate_selection_presentation_property_edit_request(const Json& request) {
    validate_request(request); return request;
}
SelectionPresentationPropertyEditPreparation prepare_selection_presentation_property_edit(
    const DocumentSnapshot& source, const SelectionPresentationPropertyEditRequest& request) {
    return replay_selection_presentation_property_edit_preparation(source, encode_request(source, request));
}
SelectionPresentationPropertyEditPreparation replay_selection_presentation_property_edit_preparation(
    const DocumentSnapshot& source, const Json& request) {
    validate_request(request);
    authenticate_source(source, request);
    auto result = source.entities();
    replay_annotations(source, request, result);
    replay_references(source, request, result);
    if (request.at("unchanged_policy") == "reject" && exact(result, source.entities())) reject("request retains the complete selection unchanged");
    return SelectionPresentationPropertyEditPreparation(request, std::move(result));
}
void require_selection_presentation_property_edit_source_current(
    const DocumentSnapshot& source, const SelectionPresentationPropertyEditPreparation& preparation) {
    authenticate_source(source, preparation.request());
}

} // namespace sketch
