#include "sketch/pinc_import_candidate_codec.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace sketch {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(std::string message) { throw std::invalid_argument("Pinc candidate: " + message); }
void require(bool value, const char* message) { if (!value) invalid(message); }
void keys(const Json& j, const std::set<std::string, std::less<>>& expected) {
    require(j.is_object() && j.size() == expected.size(), "incorrect object schema");
    for (const auto& [name, value] : j.items()) { (void)value; require(expected.contains(name), "unknown field"); }
}
template<class T> void put(Json& j, const char* key, const T& value) { j[key] = value; }
template<class T> void put(Json& j, const char* key, const std::optional<T>& value) {
    if (value) put(j, key, *value); else j[key] = nullptr;
}
template<class T> void get(const Json& j, const char* key, T& value) {
    const auto& item = j.at(key);
    if constexpr (std::is_same_v<T, bool>) require(item.is_boolean(), "expected boolean");
    else if constexpr (std::is_integral_v<T>) {
        require(item.is_number_integer(), "expected integer");
        if (item.is_number_unsigned()) require(item.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()), "integer out of range");
        else {
            const auto n = item.get<std::int64_t>();
            if constexpr (std::is_unsigned_v<T>) require(n >= 0, "negative index");
            else require(n >= std::numeric_limits<T>::min(), "integer out of range");
            require(n < 0 || static_cast<std::uint64_t>(n) <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()), "integer out of range");
        }
    } else if constexpr (std::is_floating_point_v<T>) require(item.is_number(), "expected number");
    else if constexpr (std::is_same_v<T, std::string>) require(item.is_string(), "expected string");
    item.get_to(value);
}
template<class T> void get(const Json& j, const char* key, std::optional<T>& value) {
    if (j.at(key).is_null()) value.reset(); else { T result{}; get(j, key, result); value = std::move(result); }
}
} // namespace

// These local ADL mappings enumerate all protocol fields. Null optional values
// are explicit; missing fields, wrong types and additional fields are refused.
#define PINC_PUT(member) put(j, #member, p.member);
#define PINC_GET(member) get(j, #member, p.member);
#define PINC_KEY(member) expected.insert(#member);
#define PINC_WIRE(Type, ...) \
    static void to_json(Json& j, const Type& p) { j = Json::object(); NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(PINC_PUT, __VA_ARGS__)) } \
    static void from_json(const Json& j, Type& p) { std::set<std::string, std::less<>> expected; \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(PINC_KEY, __VA_ARGS__)) keys(j, expected); \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(PINC_GET, __VA_ARGS__)) }
static void to_json(Json& j, const PincImportDialect& dialect) {
    if (dialect == PincImportDialect::modern_v42) j = "modern_v42";
    else if (dialect == PincImportDialect::legacy_v2) j = "legacy_v2";
    else invalid("unsupported dialect");
}
static void from_json(const Json& j, PincImportDialect& dialect) {
    if (j == "modern_v42") dialect = PincImportDialect::modern_v42;
    else if (j == "legacy_v2") dialect = PincImportDialect::legacy_v2;
    else invalid("unsupported dialect");
}
PINC_WIRE(Vec2, x, y)
PINC_WIRE(Segment, start, end, sweep_radians)
PINC_WIRE(PincSourceReference, page_index, collection, scalar_id_json, json_pointer, identity)
PINC_WIRE(PincImportDiagnostic, source_pointer, code, message)
PINC_WIRE(PincDimensionPresentation, size_metres, font, color)
PINC_WIRE(PincSegmentPresentation, color, weight_pixels, line_type, show_dimension, dimension,
    dimension_offset_metres, shared_color, shared_weight_pixels, shared_line_type)
PINC_WIRE(PincImportSegment, source, geometry, source_kind, source_sagitta_metres, presentation, equivalent_sources)
PINC_WIRE(PincAssignmentPresentation, color, opacity, hatch, label_color, name_size_metres,
    calculation_size_metres, show_name, show_calculation, sync_boundary_color, boundary_color,
    boundary_weight_pixels, boundary_line_type)
PINC_WIRE(PincImportAssignment, source, face_key, code, name, known_category, presentation,
    name_position_metres, calculation_position_metres, cached_anchor_metres,
    cached_area_square_metres, source_segment_references, references_resolved, legacy_area_index)
PINC_WIRE(PincLegacyArea, source, segments)
PINC_WIRE(PincVisualWallReference, type, target, parameter)
PINC_WIRE(PincImportSymbol, source, kind, centre_metres, width_metres, depth_metres,
    rotation_radians, mirror_x, mirror_y, door_hinge, door_side, wall_reference)
PINC_WIRE(PincImportText, source, text, position_metres, size_metres, rotation_radians,
    color, font, alignment, bold, italic)
PINC_WIRE(PincImportUnderlay, source_pointer, top_left_metres, width_metres, opacity,
    supported_raster_descriptor, mime_type, data_url)
PINC_WIRE(PincImportPage, source, name, calculation_segments, interior_segments,
    assignments, legacy_areas, symbols, texts, underlay, ghost_previous, show_print_guide, dimension)
PINC_WIRE(PincImportProject, dialect, source_version, source_file_name, current_page, pages, diagnostics)
#undef PINC_WIRE
#undef PINC_KEY
#undef PINC_GET
#undef PINC_PUT

namespace {
bool point_equal(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
bool segment_equal(const Segment& a, const Segment& b) {
    return point_equal(a.start, b.start) && point_equal(a.end, b.end) && a.sweep_radians == b.sweep_radians;
}
bool equivalent(const Segment& a, const Segment& b) {
    return segment_equal(a, b) || (point_equal(a.start, b.end) && point_equal(a.end, b.start) && a.sweep_radians == -b.sweep_radians);
}
bool ref_equal(const PincSourceReference& a, const PincSourceReference& b) {
    return a.page_index == b.page_index && a.collection == b.collection && a.scalar_id_json == b.scalar_id_json &&
        a.json_pointer == b.json_pointer && a.identity == b.identity;
}
std::string escape(std::string_view input) {
    std::string result;
    for (char c : input) { if (c == '~') result += "~0"; else if (c == '/') result += "~1"; else result += c; }
    return result;
}
class Validator {
public:
    void run(const PincImportProject& project) {
        require(!project.pages.empty() && project.pages.size() <= limits.max_pages && project.current_page < project.pages.size(), "page count or current page out of range");
        const bool legacy = project.dialect == PincImportDialect::legacy_v2;
        require(legacy || project.dialect == PincImportDialect::modern_v42, "unsupported dialect");
        text(project.source_version, true);
        if (legacy) require(project.source_version.size() > 2 && project.source_version.starts_with("2.") &&
            std::all_of(project.source_version.begin() + 2, project.source_version.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }) &&
            project.source_version.back() >= '0' && project.source_version.back() <= '9', "legacy version mismatch");
        else require(project.source_version == "4.2", "modern version mismatch");
        if (project.source_file_name) text(*project.source_file_name);
        for (std::size_t i = 0; i < project.pages.size(); ++i) page(project.pages[i], i, legacy);
        for (const auto& d : project.diagnostics) { record(); pointer(d.source_pointer); text(d.code, true); text(d.message, true); }
    }
private:
    PincImportLimits limits;
    std::size_t strings{}, records{}, edges{};
    std::uint64_t pairs{};
    void record() { require(++records <= limits.max_records, "record budget exceeded"); }
    void text(const std::string& value, bool nonempty = false) {
        require(value.size() <= limits.max_string_bytes - strings, "aggregate string budget exceeded");
        strings += value.size(); require(value.find('\0') == std::string::npos && (!nonempty || !value.empty()), "invalid string");
    }
    void pointer(const std::string& value) {
        text(value); require(value.empty() || value.front() == '/', "invalid source JSON pointer");
        for (std::size_t i = 0; i < value.size(); ++i) if (value[i] == '~') {
            require(++i < value.size() && (value[i] == '0' || value[i] == '1'), "invalid JSON pointer escape");
        }
    }
    void finite(double value, double bound = 1e6) { require(std::isfinite(value) && std::abs(value) <= bound, "nonfinite or excessive quantity"); }
    void positive(double value, double bound = 1e6) { finite(value, bound); require(value > 0, "nonpositive size"); }
    void interval(double value) { finite(value, 1); require(value >= 0, "negative fraction"); }
    void point(Vec2 value) { finite(value.x); finite(value.y); }
    void point(const std::optional<Vec2>& value) { if (value) point(*value); }
    void color(const std::string& value) {
        text(value); require(value.size() == 7 && value.front() == '#' &&
            std::all_of(value.begin() + 1, value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }), "invalid color");
    }
    void choice(const std::string& value, std::initializer_list<const char*> allowed) {
        text(value); require(std::any_of(allowed.begin(), allowed.end(), [&](const auto* item) { return value == item; }), "unsupported enum");
    }
    void line(const std::string& value) { choice(value, {"solid", "dash", "dot", "dashdot"}); }
    void dimension(const PincDimensionPresentation& p) { positive(p.size_metres); text(p.font, true); color(p.color); }
    void reference(const PincSourceReference& source, std::size_t index) {
        require(source.page_index == index, "source crosses page occurrence");
        text(source.collection, true); pointer(source.json_pointer); text(source.identity, true);
        if (source.scalar_id_json) text(*source.scalar_id_json, true);
        require(source.identity == pinc_source_identity(source), "noncanonical source identity");
    }
    void primary(const PincSourceReference& source, std::size_t index, const std::string& collection,
                 const std::string& path, std::set<std::string, std::less<>>& ids, bool already_valid = false) {
        if (!already_valid) reference(source, index);
        require(source.collection == collection && source.json_pointer == path, "source occurrence or collection mismatch");
        require(ids.insert(source.identity).second, "duplicate primary source identity");
    }
    void segment(const PincImportSegment& s, std::size_t index) {
        record(); reference(s.source, index); point(s.geometry.start); point(s.geometry.end); finite(s.source_sagitta_metres);
        choice(s.source_kind, {"line", "arc"}); finite(s.geometry.sweep_radians, 2 * std::numbers::pi);
        const double chord = std::hypot(s.geometry.end.x - s.geometry.start.x, s.geometry.end.y - s.geometry.start.y);
        require(chord > 0, "zero length segment");
        double expected = 0;
        if (s.source_kind == "arc" && std::abs(s.source_sagitta_metres) >= .001 * .3048 && chord >= .001 * .3048)
            expected = arc_from_chord_height(s.geometry.start, s.geometry.end, s.source_sagitta_metres).sweep_radians;
        require(s.geometry.sweep_radians == expected, "normalized geometry disagrees with source kind or sagitta");
        const auto bounds = segment_bounds(s.geometry); point(bounds.minimum); point(bounds.maximum);
        const auto& p = s.presentation; color(p.color); positive(p.weight_pixels); line(p.line_type); dimension(p.dimension); point(p.dimension_offset_metres);
        if (p.shared_color) color(*p.shared_color);
        if (p.shared_weight_pixels) positive(*p.shared_weight_pixels);
        if (p.shared_line_type) line(*p.shared_line_type);
        for (const auto& r : s.equivalent_sources) { record(); reference(r, index); }
    }
    void assignment(const PincImportAssignment& a, std::size_t index) {
        record(); reference(a.source, index); text(a.code, true); text(a.name); if (a.face_key) text(*a.face_key);
        static const std::set<std::string, std::less<>> known{"GLA1","GLA2","GLA3","GLA4","GBA","BSMT-F","BSMT-U","GAR","DGAR","ADU","OUT","CAR","PORCH","PATIO","DECK","BALC","STG","LOW","OPEN","NCA","SITE","UND"};
        require(a.known_category == known.contains(a.code), "forged descriptive category membership");
        const auto& p = a.presentation; color(p.color); interval(p.opacity); choice(p.hatch, {"none","diagonal","cross","horizontal","dots"});
        color(p.label_color); positive(p.name_size_metres); positive(p.calculation_size_metres);
        color(p.boundary_color); positive(p.boundary_weight_pixels); line(p.boundary_line_type);
        point(a.name_position_metres); point(a.calculation_position_metres); point(a.cached_anchor_metres);
        if (a.cached_area_square_metres) { finite(*a.cached_area_square_metres, 1e12); require(*a.cached_area_square_metres >= 0, "negative cached area"); }
        for (const auto& r : a.source_segment_references) { record(); reference(r, index); }
    }
    void raster(const PincImportUnderlay& u, const std::string& root) {
        record(); pointer(u.source_pointer); require(u.source_pointer == root + "/underlay", "underlay occurrence mismatch");
        point(u.top_left_metres); positive(u.width_metres); interval(u.opacity); text(u.mime_type);
        if (!u.supported_raster_descriptor) { require(!u.data_url, "unsupported underlay acquired image payload"); return; }
        require(u.mime_type == "image/png" || u.mime_type == "image/jpeg" || u.mime_type == "image/bmp" || u.mime_type == "image/tiff", "unsupported raster MIME");
        require(u.data_url.has_value(), "missing raster descriptor"); text(*u.data_url, true);
        const auto prefix = "data:" + u.mime_type + ";base64,";
        require(u.data_url->starts_with(prefix), "raster descriptor MIME mismatch");
        const auto& data = *u.data_url;
        require(data.size() > prefix.size() && (data.size() - prefix.size()) % 4 == 0, "invalid base64 descriptor length");
        std::size_t padding = 0;
        for (std::size_t i = prefix.size(); i < data.size(); ++i) {
            const char c = data[i];
            if (c == '=') require(++padding <= 2 && i >= data.size() - 2, "invalid base64 padding");
            else require(!padding && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/'), "invalid base64 descriptor");
        }
    }
    void page(const PincImportPage& p, std::size_t index, bool legacy) {
        record(); const std::string root = p.source.json_pointer;
        require(root == "/pages/" + std::to_string(index) || (legacy && index == 0 && root.empty()), "page source pointer mismatch");
        std::set<std::string, std::less<>> ids; primary(p.source, index, "pages", root, ids);
        text(p.name); dimension(p.dimension);
        require(p.calculation_segments.size() <= limits.max_calculation_edges_per_page && p.interior_segments.size() <= limits.max_interior_edges_per_page, "page edge budget exceeded");
        std::size_t raw = 0;
        for (const auto& area : p.legacy_areas) { require(area.segments.size() <= limits.max_total_edges - raw, "original edge budget exceeded"); raw += area.segments.size(); }
        require(!legacy ? p.legacy_areas.empty() : raw <= limits.max_calculation_edges_per_page, "legacy area dialect or page source budget mismatch");
        const auto count = p.calculation_segments.size() + p.interior_segments.size() + raw;
        require(count <= limits.max_total_edges - edges, "aggregate edge budget exceeded"); edges += count;
        const auto work = static_cast<std::uint64_t>(p.calculation_segments.size()) * (p.calculation_segments.empty() ? 0 : p.calculation_segments.size() - 1) / 2;
        require(work <= limits.max_calculation_pairs - pairs, "aggregate calculation pair budget exceeded"); pairs += work;
        std::map<std::string, const PincImportSegment*, std::less<>> originals;
        ids.clear();
        for (std::size_t i = 0; i < p.legacy_areas.size(); ++i) {
            const auto& area = p.legacy_areas[i]; record();
            const auto path = root + "/areas/" + std::to_string(i);
            primary(area.source, index, "areas", path, ids);
            std::set<std::string, std::less<>> segment_ids;
            for (std::size_t j = 0; j < area.segments.size(); ++j) {
                const auto& s = area.segments[j]; segment(s, index);
                primary(s.source, index, "areas/" + std::to_string(i) + "/segments", path + "/segments/" + std::to_string(j), segment_ids, true);
                require(s.equivalent_sources.empty(), "original legacy evidence contains normalized sources"); originals.emplace(s.source.identity, &s);
            }
        }
        ids.clear(); std::set<std::string, std::less<>> consumed;
        for (std::size_t i = 0; i < p.calculation_segments.size(); ++i) {
            const auto& s = p.calculation_segments[i]; segment(s, index);
            if (!legacy) {
                primary(s.source, index, "calcWalls", root + "/calcWalls/" + std::to_string(i), ids, true);
                require(s.equivalent_sources.empty(), "modern edge has legacy equivalents");
            } else {
                for (std::size_t previous = 0; previous < i; ++previous)
                    require(!equivalent(s.geometry, p.calculation_segments[previous].geometry), "legacy exact duplicates were not normalized");
                const auto found = originals.find(s.source.identity);
                require(found != originals.end() && ref_equal(s.source, found->second->source) && segment_equal(s.geometry, found->second->geometry) &&
                    s.source_kind == found->second->source_kind && s.source_sagitta_metres == found->second->source_sagitta_metres &&
                    Json(s.presentation) == Json(found->second->presentation), "normalized edge differs from first legacy source");
                require(consumed.insert(s.source.identity).second, "duplicate normalized legacy edge");
                for (const auto& ref : s.equivalent_sources) {
                    const auto original = originals.find(ref.identity);
                    require(original != originals.end() && ref_equal(ref, original->second->source) && equivalent(s.geometry, original->second->geometry) &&
                        consumed.insert(ref.identity).second, "invalid legacy exact duplicate source");
                }
            }
        }
        require(!legacy || consumed.size() == originals.size(), "original legacy sources lost during normalization");
        ids.clear();
        for (std::size_t i = 0; i < p.interior_segments.size(); ++i) {
            const auto& s = p.interior_segments[i]; segment(s, index);
            const auto collection = legacy ? "interiors" : "interiorWalls";
            primary(s.source, index, collection, root + "/" + collection + "/" + std::to_string(i), ids, true);
            require(s.equivalent_sources.empty(), "interior edge has legacy equivalents");
        }
        std::map<std::string, std::vector<PincSourceReference>, std::less<>> modern_ids;
        for (const auto& s : p.calculation_segments) if (s.source.scalar_id_json) {
            const auto scalar = Json::parse(*s.source.scalar_id_json);
            modern_ids[scalar.is_string() ? scalar.get<std::string>() : scalar.dump()].push_back(s.source);
        }
        ids.clear();
        for (const auto& a : p.assignments) {
            assignment(a, index);
            if (legacy) {
                require(!a.face_key && a.legacy_area_index && *a.legacy_area_index < p.legacy_areas.size(), "legacy assignment index mismatch");
                const auto& area = p.legacy_areas[*a.legacy_area_index];
                require(ref_equal(a.source, area.source) && ids.insert(a.source.identity).second && a.references_resolved &&
                    a.source_segment_references.size() == area.segments.size(), "legacy assignment source mismatch");
                for (std::size_t j = 0; j < area.segments.size(); ++j)
                    require(ref_equal(a.source_segment_references[j], area.segments[j].source), "legacy assignment order or source mismatch");
            } else {
                require(a.face_key.has_value() && !a.legacy_area_index && a.source.scalar_id_json == std::optional<std::string>(Json(*a.face_key).dump()), "modern assignment identity mismatch");
                primary(a.source, index, "assignments", root + "/assignments/" + escape(*a.face_key), ids, true);
                bool resolved = !a.face_key->empty(); std::set<std::string, std::less<>> used; std::vector<PincSourceReference> expected;
                std::size_t start = 0;
                for (;;) {
                    const auto end = a.face_key->find('|', start); const auto part = a.face_key->substr(start, end == std::string::npos ? end : end - start);
                    const auto found = modern_ids.find(part);
                    if (part.empty() || !used.insert(part).second || found == modern_ids.end() || found->second.size() != 1) resolved = false;
                    else expected.push_back(found->second.front());
                    if (end == std::string::npos) break; start = end + 1;
                }
                require(a.references_resolved == resolved && a.source_segment_references.size() == expected.size(), "forged assignment resolution evidence");
                for (std::size_t j = 0; j < expected.size(); ++j) require(ref_equal(a.source_segment_references[j], expected[j]), "assignment references wrong page or wall collection");
            }
        }
        ids.clear();
        for (std::size_t i = 0; i < p.symbols.size(); ++i) {
            const auto& s = p.symbols[i]; record(); primary(s.source, index, "symbols", root + "/symbols/" + std::to_string(i), ids);
            text(s.kind, true); point(s.centre_metres); positive(s.width_metres); positive(s.depth_metres); finite(s.rotation_radians, 1e6);
            choice(s.door_hinge, {"left", "right"}); require(s.door_side == 1 || s.door_side == -1, "invalid door side");
            if (s.wall_reference) {
                const auto& r = *s.wall_reference; choice(r.type, {"calc", "interior"}); reference(r.target, index); interval(r.parameter);
                const auto& walls = r.type == "calc" ? p.calculation_segments : p.interior_segments;
                require(std::any_of(walls.begin(), walls.end(), [&](const auto& edge) {
                    return edge.geometry.sweep_radians == 0 && (ref_equal(edge.source, r.target) ||
                        std::any_of(edge.equivalent_sources.begin(), edge.equivalent_sources.end(),
                            [&](const auto& original) { return ref_equal(original, r.target); }));
                }), "visual reference does not target a same-page straight wall");
            }
        }
        ids.clear();
        for (std::size_t i = 0; i < p.texts.size(); ++i) {
            const auto& t = p.texts[i]; record(); primary(t.source, index, "texts", root + "/texts/" + std::to_string(i), ids);
            text(t.text); text(t.font, true); point(t.position_metres); positive(t.size_metres); finite(t.rotation_radians, 1e6);
            color(t.color); choice(t.alignment, {"left", "center", "right"});
        }
        if (p.underlay) raster(*p.underlay, root);
    }
};
void validate(const PincImportProject& project) { Validator{}.run(project); }
} // namespace

std::vector<std::byte> encode_pinc_import_candidate(const PincImportProject& project) {
    try {
        validate(project);
        const Json envelope{{"format", "VertexPincCandidate"}, {"kind", "pinc-project"}, {"version", 1}, {"project", project}};
        const auto text = envelope.dump();
        const auto bytes = std::span(reinterpret_cast<const std::byte*>(text.data()), text.size());
        (void)parse_pinc_json_bounded(bytes);
        return {bytes.begin(), bytes.end()};
    } catch (const nlohmann::json::exception& error) { invalid(error.what()); }
}
PincImportProject decode_pinc_import_candidate(std::span<const std::byte> bytes) {
    try {
        const auto envelope = parse_pinc_json_bounded(bytes);
        keys(envelope, {"format", "kind", "version", "project"});
        require(envelope.at("format") == "VertexPincCandidate" && envelope.at("kind") == "pinc-project" &&
            envelope.at("version").is_number_integer() && envelope.at("version") == 1, "unsupported protocol format, kind or version");
        auto project = envelope.at("project").get<PincImportProject>();
        validate(project); return project;
    } catch (const nlohmann::json::exception& error) { invalid(error.what()); }
}
} // namespace sketch
