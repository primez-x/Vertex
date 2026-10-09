#include "sketch/dxf_exchange.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace sketch {
namespace {
[[noreturn]] void invalid() { throw std::invalid_argument("invalid_or_excessive_dxf"); }
void require(bool value) { if (!value) invalid(); }
void validate_limits(const DxfExchangeLimits& l) {
    const DxfExchangeLimits cap;
    require(l.max_bytes > 0 && l.max_bytes <= cap.max_bytes && l.max_pairs > 0 && l.max_pairs <= cap.max_pairs &&
        l.max_entities > 0 && l.max_entities <= cap.max_entities && l.max_vertices > 0 && l.max_vertices <= cap.max_vertices &&
        l.max_string_bytes > 0 && l.max_string_bytes <= cap.max_string_bytes);
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
template<class T> T number(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    T value{};
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    require(!s.empty() && result.ec == std::errc{} && result.ptr == s.data() + s.size());
    if constexpr (std::is_floating_point_v<T>) require(std::isfinite(value) && std::abs(value) <= 1e12);
    return value;
}
void printable(std::string_view s, const DxfExchangeLimits& l) {
    require(s.size() <= l.max_string_bytes);
    for (unsigned char c : s) require(c >= 32 && c <= 126);
}
constexpr std::string_view vertex_appid = "VERTEX_ENTITY_V1";
constexpr std::size_t xdata_limit = 16 * 1024;
bool utf8_string(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto first = static_cast<unsigned char>(s[i++]);
        if (first < 0x80) { if (first < 32 || first == 127) return false; continue; }
        unsigned count = first >= 0xc2 && first <= 0xdf ? 1 :
            first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (!count || s.size() - i < count) return false;
        const auto next = static_cast<unsigned char>(s[i]);
        if ((first == 0xe0 && next < 0xa0) || (first == 0xed && next >= 0xa0) ||
            (first == 0xf0 && next < 0x90) || (first == 0xf4 && next >= 0x90)) return false;
        while (count--) if ((static_cast<unsigned char>(s[i++]) & 0xc0) != 0x80) return false;
    }
    return true;
}
struct Pair { int code; std::string_view value; };
using Record = std::span<const Pair>;
// XDATA never supplies entity geometry fields. Unknown applications are ignored
// after bounded validation; the native application accepts string chunks only.
Record without_xdata(Record r) {
    const auto first = std::find_if(r.begin(), r.end(), [](const auto& p) { return p.code == 1001; });
    return r.first(static_cast<std::size_t>(first - r.begin()));
}
std::string block_xdata(Record r, bool& malformed) {
    std::string payload;
    bool native = false, seen = false;
    std::size_t aggregate = 0;
    for (const auto& p : r.subspan(without_xdata(r).size())) {
        require(p.code >= 1000 && p.code <= 1071);
        require(p.value.size() + 3 <= xdata_limit - aggregate);
        aggregate += p.value.size() + 3;
        if (p.code == 1001) {
            native = p.value == vertex_appid;
            if (native) { if (seen) malformed = true; seen = true; }
        } else if (native) {
            if (p.code != 1000 || !utf8_string(p.value)) malformed = true;
            else payload.append(p.value);
        }
    }
    if (seen && payload.empty()) malformed = true;
    return payload;
}
std::optional<std::string_view> field(Record r, int code) {
    std::optional<std::string_view> value;
    for (const auto& p : r) if (p.code == code) { require(!value.has_value()); value = p.value; }
    return value;
}
std::string_view mandatory(Record r, int code) { const auto v = field(r, code); require(v.has_value()); return *v; }
double real(Record r, int code, double fallback = 0) { const auto v = field(r, code); return v ? number<double>(*v) : fallback; }
int integer(Record r, int code, int fallback = 0) { const auto v = field(r, code); return v ? number<int>(*v) : fallback; }
DxfPoint point(Record r, int x, int y) { return {number<double>(mandatory(r, x)), number<double>(mandatory(r, y))}; }
bool member(int c, std::initializer_list<int> codes) { return std::find(codes.begin(), codes.end(), c) != codes.end(); }
std::string block_identity(std::string_view name) {
    std::string identity(name);
    for (auto& c : identity) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return identity;
}
std::string layer(Record r, const DxfExchangeLimits& l) {
    const auto v = field(r, 8).value_or("0"); printable(v, l); require(!v.empty()); return std::string(v);
}
// Default metadata is accepted. Appearance, external references and extension
// data outside this whitelist prevent conversion of the entire entity.
bool supported(Record r, std::initializer_list<int> specific, const DxfExchangeLimits& l) {
    bool ok = true;
    for (const auto& p : r) {
        if (member(p.code, specific) || member(p.code, {5, 100, 330, 8})) continue;
        if (member(p.code, {30, 31, 38, 39, 210, 220})) { if (number<double>(p.value) != 0) ok = false; }
        else if (p.code == 230) { if (number<double>(p.value) != 1) ok = false; }
        else if (p.code == 67) { if (number<int>(p.value) != 0) ok = false; }
        else if (p.code == 410) { if (p.value != "Model") ok = false; }
        else ok = false;
        printable(p.value, l);
    }
    return ok;
}
// Repeated codes belong to their subclass, particularly ARC_DIMENSION's 70
// and 71. Never resolve these through a whole-record lookup.
std::optional<Record> subclass(Record r, std::string_view name) {
    std::optional<Record> found;
    for (std::size_t i = 0; i < r.size(); ++i) {
        if (r[i].code != 100 || r[i].value != name) continue;
        require(!found.has_value());
        const auto first = i + 1;
        auto end = first;
        while (end < r.size() && r[end].code != 100) ++end;
        found = r.subspan(first, end - first);
    }
    return found;
}
bool dimension_defaults(Record r, const DxfExchangeLimits& l, bool legacy) {
    const auto attachment = integer(r, 71, 5);
    const auto spacing = integer(r, 72, 1);
    const auto style = field(r, 3).value_or("Standard");
    // Group 42 is only a cache; finite bounded data is admitted but never used
    // in place of the extension geometry when calculating a measurement.
    (void)real(r, 42);
    return supported(r, {1, 2, 3, 10, 20, 11, 21, 12, 22, 32, 13, 23, 33, 14, 24, 34, 50, 70, 71, 72,
                          73, 74, 75, 41, 42, 43, 44, 51, 52, 53, 280}, l) &&
        (attachment == 5 || (legacy && attachment == 0)) &&
        (spacing == 1 || (legacy && spacing == 0)) &&
        integer(r, 73) == 0 && integer(r, 74) == 0 && integer(r, 75) == 0 &&
        integer(r, 280) == 0 && real(r, 41, 1) == 1 &&
        real(r, 43) == 0 && real(r, 44) == 0 && real(r, 51) == 0 && real(r, 52) == 0 &&
        real(r, 30) == 0 && real(r, 31) == 0 && real(r, 33) == 0 && real(r, 34) == 0 &&
        real(r, 12) == 0 && real(r, 22) == 0 && real(r, 32) == 0 &&
        (style == "Standard" || style == "STANDARD" || (legacy && style.empty()));
}
double degrees(DxfPoint vector) {
    auto value = std::atan2(vector.y, vector.x) * 180.0 / std::numbers::pi;
    if (value < 0) value += 360.0;
    return value == 360.0 ? 0.0 : value;
}
struct ArcDimensionGeometry { double radius, dimension_radius, start, end, sweep; };
std::optional<ArcDimensionGeometry> arc_dimension_geometry(const DxfArcDimension& v) {
    const DxfPoint a{v.extension_start.x - v.center.x, v.extension_start.y - v.center.y};
    const DxfPoint b{v.extension_end.x - v.center.x, v.extension_end.y - v.center.y};
    const auto radius = std::hypot(a.x, a.y);
    const auto other_radius = std::hypot(b.x, b.y);
    const auto picture_radius = std::hypot(v.dimension_arc.x - v.center.x, v.dimension_arc.y - v.center.y);
    const auto start = degrees(a), end = degrees(b);
    auto sweep = end - start;
    if (sweep <= 0) sweep += 360.0;
    if (!std::isfinite(radius) || !std::isfinite(other_radius) || !std::isfinite(picture_radius) ||
        radius <= std::numeric_limits<double>::epsilon() || picture_radius <= std::numeric_limits<double>::epsilon() ||
        std::abs(radius - other_radius) > 1e-9 * std::max(radius, other_radius) ||
        start == end || !(sweep > 0 && sweep < 360)) return {};
    return ArcDimensionGeometry{radius, picture_radius, start, end, sweep};
}
struct DimensionPicture {
    std::string name;
    std::size_t entity_index, dimension_index;
    bool arc;
};
void bounded_point(DxfPoint p) {
    require(std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) <= 1e12 && std::abs(p.y) <= 1e12);
}
void dimension_text(DxfBlock& block, DxfPoint position, double height, double rotation,
                    std::string_view override_text, double measurement, const DxfExchangeLimits& l) {
    bounded_point(position);
    require(std::isfinite(height) && height > 0 && height <= 1e12 &&
        std::isfinite(rotation) && std::abs(rotation) <= 1e12 &&
        std::isfinite(measurement) && measurement >= 0 && measurement <= 1e12);
    printable(override_text, l);
    require(override_text.find('\\') == std::string_view::npos && override_text.find("%%") == std::string_view::npos);
    if (override_text == " ") return;
    char buffer[64];
    const auto conversion = std::to_chars(buffer, buffer + sizeof(buffer), measurement,
        std::chars_format::general, 12);
    require(conversion.ec == std::errc{});
    const std::string actual(buffer, conversion.ptr);
    std::string text;
    if (override_text.empty()) text = actual;
    else {
        // DXF's <> token substitutes the analytical measurement.
        for (std::size_t cursor = 0; cursor < override_text.size();) {
            if (override_text.substr(cursor, 2) == "<>") { text += actual; cursor += 2; }
            else text += override_text[cursor++];
            require(text.size() <= l.max_string_bytes);
        }
    }
    printable(text, l);
    block.labels.push_back({position, height, rotation, std::move(text), "0"});
}
void dimension_tick(DxfBlock& block, DxfPoint point, DxfPoint tangent, double height) {
    // Two simple drafting ticks avoid font/arrow-block dependencies.
    const DxfPoint delta{(tangent.x - tangent.y) * height * 0.35,
                         (tangent.y + tangent.x) * height * 0.35};
    block.lines.push_back({{point.x - delta.x, point.y - delta.y},
                           {point.x + delta.x, point.y + delta.y}, "0"});
}
DxfBlock dimension_picture(const DxfDimension& v, const DxfExchangeLimits& l) {
    bounded_point(v.extension_start); bounded_point(v.extension_end); bounded_point(v.dimension_line);
    require(std::isfinite(v.rotation_degrees) && std::abs(v.rotation_degrees) <= 1e12);
    const DxfPoint delta{v.extension_end.x - v.extension_start.x, v.extension_end.y - v.extension_start.y};
    const auto length = std::hypot(delta.x, delta.y);
    require(length > std::numeric_limits<double>::epsilon());
    const auto radians = std::fmod(v.rotation_degrees, 360.0) * std::numbers::pi / 180.0;
    const DxfPoint direction = v.aligned ? DxfPoint{delta.x / length, delta.y / length} :
                                         DxfPoint{std::cos(radians), std::sin(radians)};
    const DxfPoint normal{-direction.y, direction.x};
    const auto project = [&](DxfPoint p) {
        const auto offset = (v.dimension_line.x - p.x) * normal.x + (v.dimension_line.y - p.y) * normal.y;
        return DxfPoint{p.x + normal.x * offset, p.y + normal.y * offset};
    };
    const auto start = project(v.extension_start), end = project(v.extension_end);
    DxfBlock block;
    const auto measurement = v.aligned ? length : std::abs(delta.x * direction.x + delta.y * direction.y);
    dimension_text(block, v.text_position, v.text_height, v.text_rotation_degrees, v.text, measurement, l);
    block.lines.push_back({v.extension_start, start, "0"});
    block.lines.push_back({v.extension_end, end, "0"});
    block.lines.push_back({start, end, "0"});
    dimension_tick(block, start, direction, v.text_height);
    dimension_tick(block, end, direction, v.text_height);
    return block;
}
DxfBlock dimension_picture(const DxfArcDimension& v, const DxfExchangeLimits& l) {
    bounded_point(v.extension_start); bounded_point(v.extension_end); bounded_point(v.center); bounded_point(v.dimension_arc);
    const auto geometry = arc_dimension_geometry(v); require(geometry.has_value());
    const auto& g = *geometry;
    const auto radial_point = [&](DxfPoint p) {
        const auto radius = std::hypot(p.x - v.center.x, p.y - v.center.y);
        return DxfPoint{v.center.x + (p.x - v.center.x) * (g.dimension_radius / radius),
                         v.center.y + (p.y - v.center.y) * (g.dimension_radius / radius)};
    };
    const auto start = radial_point(v.extension_start), end = radial_point(v.extension_end);
    DxfBlock block;
    dimension_text(block, v.text_position, v.text_height, v.text_rotation_degrees, v.text,
        g.radius * (g.sweep * std::numbers::pi / 180.0), l);
    block.lines.push_back({v.extension_start, start, "0"});
    block.lines.push_back({v.extension_end, end, "0"});
    block.arcs.push_back({v.center, g.dimension_radius, g.start, g.end, "0"});
    const auto tangent = [&](double angle) {
        const auto radians = angle * std::numbers::pi / 180.0;
        return DxfPoint{-std::sin(radians), std::cos(radians)};
    };
    dimension_tick(block, start, tangent(g.start), v.text_height);
    dimension_tick(block, end, tangent(g.end), v.text_height);
    return block;
}
void entity(DxfImportResult& result, DxfDrawing& destination, std::string_view type, Record r,
    std::size_t index, std::size_t& vertices, const DxfExchangeLimits& l,
    bool allow_insert = true, std::vector<DimensionPicture>* pictures = nullptr) {
    auto diagnostic = [&](const char* code) { result.diagnostics.push_back({index, std::string(type), code}); };
    bool malformed_xdata = false;
    (void)block_xdata(r, malformed_xdata);
    if (without_xdata(r).size() != r.size()) diagnostic("xdata_not_activated");
    r = without_xdata(r);
    if (type != "LINE" && type != "ARC" && type != "CIRCLE" && type != "LWPOLYLINE" && type != "TEXT" &&
        type != "DIMENSION" && type != "ARC_DIMENSION" && type != "HATCH" && type != "INSERT") {
        diagnostic("unsupported_entity"); return;
    }
    const auto entity_layer = layer(r, l);
    if (type == "LINE") {
        DxfLine v{point(r, 10, 20), point(r, 11, 21), entity_layer};
        if (!supported(r, {10, 20, 11, 21}, l)) diagnostic("unsupported_feature");
        else destination.lines.push_back(std::move(v));
    } else if (type == "ARC") {
        DxfArc v{point(r, 10, 20), number<double>(mandatory(r, 40)),
            number<double>(mandatory(r, 50)), number<double>(mandatory(r, 51)), entity_layer};
        require(v.radius > 0 && v.start_degrees >= 0 && v.start_degrees < 360 && v.end_degrees >= 0 && v.end_degrees < 360 && v.start_degrees != v.end_degrees);
        if (!supported(r, {10, 20, 40, 50, 51}, l)) diagnostic("unsupported_feature");
        else destination.arcs.push_back(std::move(v));
    } else if (type == "CIRCLE") {
        DxfCircle v{point(r, 10, 20), number<double>(mandatory(r, 40)), entity_layer};
        require(v.radius > 0);
        if (!supported(r, {10, 20, 40}, l)) diagnostic("unsupported_feature");
        else destination.circles.push_back(std::move(v));
    } else if (type == "TEXT") {
        DxfLabel v{point(r, 10, 20), number<double>(mandatory(r, 40)), real(r, 50),
            std::string(mandatory(r, 1)), entity_layer};
        require(v.height > 0); printable(v.text, l);
        const bool plain = integer(r, 71) == 0 && integer(r, 72) == 0 && integer(r, 73) == 0 &&
            real(r, 41, 1) == 1 && real(r, 51) == 0 && field(r, 7).value_or("STANDARD") == "STANDARD" &&
            v.text.find('\\') == std::string::npos && v.text.find("%%") == std::string::npos;
        if (!supported(r, {10, 20, 40, 50, 1, 71, 72, 73, 41, 51, 7}, l) || !plain) diagnostic("unsupported_feature");
        else destination.labels.push_back(std::move(v));
    } else if (type == "DIMENSION") {
        DxfDimension v{point(r, 13, 23), point(r, 14, 24), point(r, 10, 20),
            point(r, 11, 21), real(r, 50), std::string(field(r, 1).value_or("")), entity_layer};
        printable(v.text, l);
        const auto kind = integer(r, 70, -1);
        // Bit 128 changes text placement, not the analytical dimension type.
        // Removing only that documented flag still rejects every unknown bit.
        const auto base_kind = kind & ~128;
        const bool legacy = kind >= 0 && base_kind == 0 && !field(r, 2).has_value();
        const bool linear = legacy || (kind >= 0 && (base_kind == 32 || base_kind == 33));
        v.aligned = base_kind == 33;
        if (v.aligned && !field(r, 50)) v.rotation_degrees = degrees({v.extension_end.x - v.extension_start.x,
                                                                   v.extension_end.y - v.extension_start.y});
        v.text_rotation_degrees = real(r, 53);
        const bool plain = dimension_defaults(r, l, legacy) &&
            v.text.find('\\') == std::string::npos && v.text.find("%%") == std::string::npos;
        if (!supported(r, {1, 2, 3, 10, 20, 11, 21, 12, 22, 32, 13, 23, 33, 14, 24, 34, 50, 70,
                           71, 72, 73, 74, 75, 41, 42, 43, 44, 51, 52, 53, 280}, l) || !linear || !plain ||
            std::hypot(v.extension_end.x - v.extension_start.x,
                       v.extension_end.y - v.extension_start.y) <= std::numeric_limits<double>::epsilon()) {
            diagnostic("unsupported_feature");
        } else {
            if (!legacy) {
                if (const auto name = field(r, 2)) {
                    require(!name->empty()); printable(*name, l); require(pictures != nullptr);
                    pictures->push_back({std::string(*name), index, destination.dimensions.size(), false});
                } else diagnostic("dimension_picture_not_retained");
            }
            destination.dimensions.push_back(std::move(v));
        }
    } else if (type == "ARC_DIMENSION") {
        const auto common = subclass(r, "AcDbDimension");
        const auto arc = subclass(r, "AcDbArcDimension");
        int stage = 0;
        bool valid_subclasses = true;
        for (const auto& p : r) if (p.code == 100) {
            if (p.value == "AcDbEntity" && stage == 0) stage = 1;
            else if (p.value == "AcDbDimension" && stage <= 1) stage = 2;
            else if (p.value == "AcDbArcDimension" && stage == 2) stage = 3;
            else valid_subclasses = false;
        }
        if (!common || !arc || !valid_subclasses) { diagnostic("unsupported_feature"); return; }
        DxfArcDimension v{point(*arc, 13, 23), point(*arc, 14, 24), point(*arc, 15, 25),
            point(*common, 10, 20), point(*common, 11, 21), real(*common, 53),
            std::string(field(*common, 1).value_or("")), entity_layer};
        printable(v.text, l);
        const bool plain_common = supported(*common, {1, 2, 3, 10, 20, 11, 21, 12, 22, 32,
            70, 71, 72, 73, 74, 75, 41, 42, 43, 44, 51, 52, 53, 280}, l) &&
            dimension_defaults(*common, l, false) &&
            integer(*common, 70, -1) >= 0 &&
            ((integer(*common, 70, -1) & ~128) == 37 ||
                ((integer(*common, 70, -1) & ~128) == 5 && !field(*common, 2)));
        const bool plain_arc = supported(*arc, {13, 23, 33, 14, 24, 34, 15, 25, 35,
            40, 41, 70, 71, 16, 26, 36, 17, 27, 37}, l) &&
            integer(*arc, 70) == 0 && integer(*arc, 71) == 0 && real(*arc, 40) == 0 && real(*arc, 41) == 0 &&
            real(*arc, 33) == 0 && real(*arc, 34) == 0 && real(*arc, 35) == 0 &&
            real(*arc, 16) == 0 && real(*arc, 26) == 0 && real(*arc, 36) == 0 &&
            real(*arc, 17) == 0 && real(*arc, 27) == 0 && real(*arc, 37) == 0;
        // The prefix can carry only ordinary entity metadata. It must not
        // smuggle dimension geometry outside the declared subclasses.
        const auto marker = std::find_if(r.begin(), r.end(), [](const auto& p) {
            return p.code == 100 && p.value == "AcDbDimension";
        });
        const bool plain_prefix = supported(r.first(static_cast<std::size_t>(marker - r.begin())), {}, l);
        if (!plain_common || !plain_arc || !plain_prefix || !arc_dimension_geometry(v) ||
            v.text.find('\\') != std::string::npos || v.text.find("%%") != std::string::npos) {
            diagnostic("unsupported_feature");
        } else {
            if (const auto name = field(*common, 2)) {
                require(!name->empty()); printable(*name, l); require(pictures != nullptr);
                pictures->push_back({std::string(*name), index, destination.arc_dimensions.size(), true});
            } else diagnostic("dimension_picture_not_retained");
            destination.arc_dimensions.push_back(std::move(v));
        }
    } else if (type == "HATCH") {
        DxfHatch v{{}, integer(r, 70) == 1, entity_layer};
        const auto path_count = integer(r, 91, -1);
        const auto path_flags = integer(r, 92, -1);
        const auto has_bulge = integer(r, 72, -1);
        const auto closed = integer(r, 73, -1);
        const auto vertex_count = integer(r, 93, -1);
        const auto pattern = field(r, 2).value_or("");
        const auto associative = integer(r, 71, -1);
        const auto hatch_style = integer(r, 75, -1);
        const auto pattern_type = integer(r, 76, -1);
        const auto source_count = integer(r, 97, -1);
        bool after_path = false;
        bool have_y = false;
        int seen_vertices = 0;
        for (const auto& p : r) {
            if (p.code == 93) {
                after_path = true;
                continue;
            }
            if (!after_path) continue;
            if (p.code == 10) {
                require((v.boundary.empty() || have_y) && seen_vertices < vertex_count);
                v.boundary.push_back({number<double>(p.value), 0.0});
                have_y = false;
                ++seen_vertices;
            } else if (p.code == 20) {
                require(!v.boundary.empty() && !have_y);
                v.boundary.back().y = number<double>(p.value);
                have_y = true;
            }
        }
        require(path_count >= 0 && path_flags >= 0 && vertex_count >= 0 &&
                static_cast<std::size_t>(vertex_count) <= l.max_vertices - vertices);
        vertices += static_cast<std::size_t>(vertex_count);
        // Group 92 is bit-coded: 1 is external, 2 selects polyline data.
        // A sole polygon loop may omit the external bit; export canonicalizes it.
        const bool shape = path_count == 1 && (path_flags == 2 || path_flags == 3) && has_bulge == 0 && closed == 1 &&
            associative == 0 && hatch_style == 0 && pattern_type == 1 && source_count == 0 &&
            pattern == "SOLID" && v.solid && seen_vertices == vertex_count && have_y &&
            v.boundary.size() >= 3;
        const bool planar = supported(r, {2, 10, 20, 30, 70, 71, 72, 73, 75, 76, 91, 92,
                                          93, 97, 210, 220, 230}, l) &&
            real(r, 30) == 0 && real(r, 210, 0) == 0 && real(r, 220, 0) == 0 &&
            real(r, 230, 1) == 1;
        if (!shape || !planar) {
            diagnostic("unsupported_feature");
        } else {
            destination.hatches.push_back(std::move(v));
        }
    } else if (type == "INSERT") {
        DxfInsert v{std::string(mandatory(r, 2)), point(r, 10, 20), real(r, 41, 1),
                    real(r, 42, 1), real(r, 50), entity_layer};
        printable(v.block_name, l);
        const bool plain = integer(r, 66, 0) == 0 && real(r, 40, 1) == 1 &&
                           real(r, 43, 1) == 1 && real(r, 30) == 0;
        const bool valid_scale = std::isfinite(v.scale_x) && std::isfinite(v.scale_y) &&
                                 v.scale_x != 0.0 && v.scale_y != 0.0;
        if (!allow_insert || !supported(r, {2, 10, 20, 30, 41, 42, 43, 50, 66}, l) ||
            !plain || !valid_scale) {
            diagnostic("unsupported_feature");
        } else {
            destination.inserts.push_back(std::move(v));
        }
    } else {
        const int count = number<int>(mandatory(r, 90));
        require(count >= 2 && static_cast<std::size_t>(count) <= l.max_vertices - vertices);
        vertices += static_cast<std::size_t>(count);
        const auto flags = integer(r, 70);
        bool ok = supported(r, {90, 70, 10, 20, 42, 40, 41, 43, 91}, l) && (flags == 0 || flags == 1);
        if (real(r, 43) != 0) ok = false;
        DxfPolyline v{{}, flags == 1, entity_layer};
        bool have_y = false, have_bulge = false;
        for (const auto& p : r) {
            if (p.code == 10) {
                require(v.vertices.empty() || have_y);
                require(v.vertices.size() < static_cast<std::size_t>(count));
                v.vertices.push_back({{number<double>(p.value), 0}, 0});
                have_y = false; have_bulge = false;
            } else if (p.code == 20) {
                require(!v.vertices.empty() && !have_y); v.vertices.back().point.y = number<double>(p.value); have_y = true;
            } else if (p.code == 42) {
                require(!v.vertices.empty() && !have_bulge); v.vertices.back().bulge = number<double>(p.value); have_bulge = true;
            } else if (p.code == 40 || p.code == 41) {
                require(!v.vertices.empty()); if (number<double>(p.value) != 0) ok = false;
            }
        }
        require(have_y && v.vertices.size() == static_cast<std::size_t>(count));
        if (!ok) diagnostic("unsupported_feature"); else destination.polylines.push_back(std::move(v));
    }
}

void parse_block_section(DxfImportResult& result, const std::vector<Pair>& pairs,
                        std::size_t begin, std::size_t end, std::size_t& entity_count,
                        std::size_t& vertices, const DxfExchangeLimits& limits) {
    std::set<std::string, std::less<>> names;
    std::size_t cursor = begin;
    while (cursor < end) {
        require(++entity_count <= limits.max_entities);
        require(pairs[cursor].code == 0 && pairs[cursor].value == "BLOCK");
        const std::size_t header_begin = ++cursor;
        while (cursor < end && pairs[cursor].code != 0) ++cursor;
        const Record full_header(pairs.data() + header_begin, cursor - header_begin);
        bool malformed_xdata = false;
        auto native_json = block_xdata(full_header, malformed_xdata);
        const Record header = without_xdata(full_header);
        if (std::any_of(full_header.begin(), full_header.end(), [](const auto& pair) {
                return pair.code == 1001 && pair.value != vertex_appid;
            })) result.diagnostics.push_back({0, "BLOCK", "foreign_xdata_not_activated"});
        if (malformed_xdata) {
            native_json = "!invalid_vertex_xdata";
            result.diagnostics.push_back({0, "BLOCK", "invalid_vertex_xdata"});
        }
        const auto name_value = mandatory(header, 2);
        printable(name_value, limits);
        require(!name_value.empty() && names.insert(block_identity(name_value)).second);
        const auto base = point(header, 10, 20);
        const auto block_flags = integer(header, 70, 0);
        const bool plain_header = (block_flags == 0 || (block_flags == 1 && name_value.front() == '*')) && real(header, 30) == 0 &&
                                  field(header, 3).value_or(name_value) == name_value;
        const auto diagnostics_before = result.diagnostics.size();
        if (!supported(header, {2, 3, 10, 20, 30, 70}, limits) || !plain_header) {
            result.diagnostics.push_back({0, "BLOCK", "unsupported_feature"});
        }

        DxfDrawing contents;
        bool closed = false;
        while (cursor < end) {
            require(pairs[cursor].code == 0);
            const auto type = pairs[cursor++].value;
            const std::size_t first = cursor;
            while (cursor < end && pairs[cursor].code != 0) ++cursor;
            const Record record(pairs.data() + first, cursor - first);
            if (type == "ENDBLK") {
                require(supported(record, {}, limits));
                closed = true;
                break;
            }
            require(++entity_count <= limits.max_entities);
            if (type == "DIMENSION" || type == "ARC_DIMENSION" || type == "HATCH" || type == "INSERT") {
                result.diagnostics.push_back({0, std::string(type), "unsupported_entity"});
                continue;
            }
            entity(result, contents, type, record, 0, vertices, limits, false);
        }
        require(closed);
        if (!plain_header) continue;
        if (!native_json.empty() && result.diagnostics.size() != diagnostics_before) {
            native_json = "!unsupported_vertex_block";
            result.diagnostics.push_back({0, "BLOCK", "native_block_geometry_unsupported"});
        }
        result.drawing.blocks.push_back({std::string(name_value), base,
                                         std::move(contents.lines), std::move(contents.arcs),
                                         std::move(contents.polylines), std::move(contents.labels),
                                         std::move(native_json), std::move(contents.circles)});
    }
}

class Writer {
public:
    explicit Writer(const DxfExchangeLimits& limits) : limits_(limits) {}
    void put(int code, std::string_view value) {
        if (code == 1000) require(value.size() <= limits_.max_string_bytes && utf8_string(value));
        else printable(value, limits_);
        const auto c = std::to_string(code);
        require(++pairs_ <= limits_.max_pairs && c.size() + value.size() + 2 <= limits_.max_bytes - bytes.size());
        bytes += c; bytes += '\n'; bytes += value; bytes += '\n';
    }
    void put(int code, double value) {
        require(std::isfinite(value) && std::abs(value) <= 1e12);
        char buffer[64];
        const auto r = std::to_chars(buffer, buffer + sizeof(buffer), value == 0 ? 0.0 : value,
            std::chars_format::general, std::numeric_limits<double>::max_digits10);
        require(r.ec == std::errc{}); put(code, std::string_view(buffer, r.ptr));
    }
    void xy(const DxfPoint& p, int x = 10, int y = 20) { put(x, p.x); put(y, p.y); }
    void begin(const char* type, const std::string& layer, const char* subclass) {
        require(!layer.empty()); put(0, type); put(100, "AcDbEntity"); put(8, layer); put(100, subclass);
    }
    std::string bytes;
private:
    const DxfExchangeLimits& limits_;
    std::size_t pairs_{};
};
} // namespace

DxfImportResult parse_dxf_ascii(std::string_view bytes, const DxfExchangeLimits& l) {
    validate_limits(l); require(!bytes.empty() && bytes.size() <= l.max_bytes);
    std::vector<Pair> pairs;
    auto line = [&]() {
        require(!bytes.empty());
        const auto end = bytes.find('\n');
        auto value = bytes.substr(0, end);
        if (end == std::string_view::npos) bytes = {}; else bytes.remove_prefix(end + 1);
        if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
        return value;
    };
    while (!bytes.empty()) {
        require(pairs.size() < l.max_pairs);
        const auto c = line(); require(c.size() <= 16); const auto code = number<int>(c); require(code >= 0 && code <= 1071);
        const auto v = line();
        if (code == 1000) {
            // Invalid native UTF-8 is a metadata activation failure; keep the
            // independently readable block geometry available as fallback.
            require(v.size() <= l.max_string_bytes);
            for (const unsigned char c : v) require(c >= 32 && c != 127);
        }
        else printable(v, l);
        pairs.push_back({code, v});
    }
    DxfImportResult result;
    std::vector<DimensionPicture> pictures;
    std::size_t i = 0, entity_count = 0, entity_ordinal = 0, vertices = 0;
    bool header = false, blocks = false, entities = false, version = false, eof = false, units = false;
    bool vertex_registered = false;
    while (i < pairs.size()) {
        const auto p = pairs[i++]; require(p.code == 0);
        if (p.value == "EOF") { require(i == pairs.size()); eof = true; break; }
        require(p.value == "SECTION" && i < pairs.size() && pairs[i].code == 2);
        const auto section = pairs[i++].value;
        const auto begin = i;
        while (i < pairs.size() && !(pairs[i].code == 0 && pairs[i].value == "ENDSEC")) {
            require(!(pairs[i].code == 0 && (pairs[i].value == "SECTION" || pairs[i].value == "EOF"))); ++i;
        }
        require(i < pairs.size()); const auto end = i++;
        if (section == "HEADER") {
            require(!header); header = true;
            for (std::size_t j = begin; j < end;) {
                require(pairs[j].code == 9); const auto name = pairs[j++].value;
                const auto first = j; while (j < end && pairs[j].code != 9) ++j;
                if (name == "$ACADVER") { require(!version && j == first + 1 && pairs[first].code == 1 && pairs[first].value == "AC1027"); version = true; }
                else if (name == "$INSUNITS") { require(!units && j == first + 1 && pairs[first].code == 70); units = true;
                    result.drawing.insertion_units = number<int>(pairs[first].value); require(result.drawing.insertion_units >= 0 && result.drawing.insertion_units <= 20); }
                else result.diagnostics.push_back({0, "HEADER", "unsupported_header_variable"});
            }
        } else if (section == "TABLES") {
            bool appid_table = false, in_table = false;
            for (std::size_t j = begin; j < end;) {
                require(pairs[j].code == 0);
                const auto type = pairs[j++].value;
                const auto first = j;
                while (j < end && pairs[j].code != 0) ++j;
                const Record record(pairs.data() + first, j - first);
                if (type == "TABLE") {
                    require(!in_table); in_table = true;
                    appid_table = mandatory(record, 2) == "APPID";
                } else if (type == "ENDTAB") {
                    require(in_table); in_table = false; appid_table = false;
                } else if (type == "APPID" && appid_table && field(record, 2).value_or("") == vertex_appid &&
                           integer(record, 70, 0) == 0 && supported(record, {2, 70}, l))
                    vertex_registered = true;
                else if (type != "APPID" || !appid_table)
                    result.diagnostics.push_back({0, "TABLES", "unsupported_table_record"});
            }
            require(!in_table);
        } else if (section == "BLOCKS") {
            require(!blocks); blocks = true;
            parse_block_section(result, pairs, begin, end, entity_count, vertices, l);
        } else if (section == "ENTITIES") {
            require(!entities); entities = true;
            for (std::size_t j = begin; j < end;) {
                require(pairs[j].code == 0 && ++entity_count <= l.max_entities);
                const auto type = pairs[j++].value; const auto first = j;
                while (j < end && pairs[j].code != 0) ++j;
                entity(result, result.drawing, type, Record(pairs.data() + first, j - first),
                       ++entity_ordinal, vertices, l, true, &pictures);
            }
        } else result.diagnostics.push_back({0, std::string(section), "unsupported_section"});
    }
    if (!vertex_registered) for (auto& block : result.drawing.blocks) {
        if (!block.vertex_entity_json.empty()) {
            block.vertex_entity_json = "!unregistered_vertex_xdata";
            result.diagnostics.push_back({0, "BLOCK", "unregistered_vertex_xdata"});
        }
    }
    std::map<std::string, std::size_t, std::less<>> block_indices;
    for (std::size_t index = 0; index < result.drawing.blocks.size(); ++index)
        require(block_indices.emplace(block_identity(result.drawing.blocks[index].name), index).second);
    for (const auto& insert : result.drawing.inserts)
        require(block_indices.contains(block_identity(insert.block_name)));
    for (const auto& picture : pictures) {
        const auto found = block_indices.find(block_identity(picture.name));
        require(found != block_indices.end());
        auto& block = result.drawing.blocks[found->second];
        // Picture references are display data, never native semantic imports.
        if (!block.vertex_entity_json.empty()) {
            block.vertex_entity_json.clear();
            result.diagnostics.push_back({picture.entity_index, picture.arc ? "ARC_DIMENSION" : "DIMENSION",
                "dimension_picture_xdata_not_activated"});
        }
        if (block.labels.size() == 1) {
            if (picture.arc) result.drawing.arc_dimensions[picture.dimension_index].text_height = block.labels.front().height;
            else result.drawing.dimensions[picture.dimension_index].text_height = block.labels.front().height;
        }
    }
    require(header && version && entities && eof);
    return result;
}

std::string export_dxf_ascii(const DxfDrawing& d, const DxfExchangeLimits& l) {
    validate_limits(l);
    require(d.insertion_units >= 0 && d.insertion_units <= 20);
    // Incremental counts avoid overflow on caller-controlled containers.
    std::size_t count = 0;
    for (auto size : {d.lines.size(), d.arcs.size(), d.polylines.size(), d.dimensions.size(),
                      d.hatches.size(), d.labels.size(), d.circles.size(), d.arc_dimensions.size()}) {
        require(size <= l.max_entities - count); count += size;
    }
    require(d.blocks.size() <= l.max_entities - count); count += d.blocks.size();
    require(d.inserts.size() <= l.max_entities - count); count += d.inserts.size();
    std::set<std::string, std::less<>> block_names;
    for (const auto& block : d.blocks) {
        printable(block.name, l);
        require(!block.name.empty() && block_names.insert(block_identity(block.name)).second);
        for (auto size : {block.lines.size(), block.arcs.size(), block.polylines.size(), block.labels.size(), block.circles.size()}) {
            require(size <= l.max_entities - count); count += size;
        }
    }
    for (const auto& insert : d.inserts) {
        printable(insert.block_name, l);
        require(block_names.contains(block_identity(insert.block_name)));
    }
    std::vector<DxfBlock> pictures;
    std::size_t picture_number = 0;
    const auto add_picture = [&](const auto& dimension) {
        // Count before construction/output. Each picture has a header, five
        // primitives, and at most one text entity; suppression omits the text.
        const std::size_t picture_count = dimension.text == " " ? 6 : 7;
        require(picture_count <= l.max_entities - count);
        auto block = dimension_picture(dimension, l);
        do { block.name = "*D" + std::to_string(++picture_number); }
        while (block_names.contains(block_identity(block.name)));
        printable(block.name, l);
        require(block_names.insert(block_identity(block.name)).second);
        count += picture_count;
        pictures.push_back(std::move(block));
    };
    for (const auto& dimension : d.dimensions) add_picture(dimension);
    for (const auto& dimension : d.arc_dimensions) add_picture(dimension);
    Writer w(l);
    const auto write_line = [&](const DxfLine& v) {
        w.begin("LINE", v.layer, "AcDbLine"); w.xy(v.start); w.put(30, 0.0);
        w.xy(v.end, 11, 21); w.put(31, 0.0);
    };
    const auto write_arc = [&](const DxfArc& v) {
        require(v.radius > 0 && v.start_degrees >= 0 && v.start_degrees < 360 &&
                v.end_degrees >= 0 && v.end_degrees < 360 &&
                v.start_degrees != v.end_degrees);
        w.begin("ARC", v.layer, "AcDbCircle"); w.xy(v.center); w.put(30, 0.0);
        w.put(40, v.radius); w.put(100, "AcDbArc");
        w.put(50, v.start_degrees); w.put(51, v.end_degrees);
    };
    const auto write_circle = [&](const DxfCircle& v) {
        require(v.radius > 0);
        w.begin("CIRCLE", v.layer, "AcDbCircle"); w.xy(v.center); w.put(30, 0.0);
        w.put(40, v.radius);
    };
    std::size_t vertices = 0;
    const auto write_polyline = [&](const DxfPolyline& v) {
        require(v.vertices.size() >= 2 && v.vertices.size() <= l.max_vertices - vertices);
        vertices += v.vertices.size();
        w.begin("LWPOLYLINE", v.layer, "AcDbPolyline");
        w.put(90, std::to_string(v.vertices.size())); w.put(70, v.closed ? "1" : "0");
        for (const auto& vertex : v.vertices) { w.xy(vertex.point); w.put(42, vertex.bulge); }
    };
    const auto write_label = [&](const DxfLabel& v) {
        require(v.height > 0 && v.text.find('\\') == std::string::npos &&
                v.text.find("%%") == std::string::npos);
        w.begin("TEXT", v.layer, "AcDbText"); w.xy(v.position); w.put(30, 0.0);
        w.put(40, v.height); w.put(1, v.text); w.put(50, v.rotation_degrees);
        w.put(100, "AcDbText");
    };
    w.put(0, "SECTION"); w.put(2, "HEADER"); w.put(9, "$ACADVER"); w.put(1, "AC1027");
    w.put(9, "$INSUNITS"); w.put(70, std::to_string(d.insertion_units)); w.put(0, "ENDSEC");
    const bool native_metadata = std::any_of(d.blocks.begin(), d.blocks.end(),
        [](const auto& block) { return !block.vertex_entity_json.empty(); });
    if (native_metadata) {
        w.put(0, "SECTION"); w.put(2, "TABLES"); w.put(0, "TABLE"); w.put(2, "APPID");
        w.put(70, "1"); w.put(0, "APPID"); w.put(100, "AcDbSymbolTableRecord");
        w.put(100, "AcDbRegAppTableRecord"); w.put(2, vertex_appid); w.put(70, "0");
        w.put(0, "ENDTAB"); w.put(0, "ENDSEC");
    }
    if (!d.blocks.empty() || !pictures.empty()) {
        w.put(0, "SECTION"); w.put(2, "BLOCKS");
        const auto write_block = [&](const DxfBlock& block, bool anonymous) {
            w.begin("BLOCK", "0", "AcDbBlockBegin"); w.put(2, block.name); w.put(3, block.name);
            w.xy(block.base); w.put(30, 0.0); w.put(70, anonymous ? "1" : "0");
            if (!block.vertex_entity_json.empty()) {
                const auto& payload = block.vertex_entity_json;
                require(utf8_string(payload));
                std::size_t aggregate = vertex_appid.size() + 3;
                w.put(1001, vertex_appid);
                for (std::size_t cursor = 0; cursor < payload.size();) {
                    auto end = std::min(payload.size(), cursor + l.max_string_bytes);
                    while (end < payload.size() && end > cursor &&
                           (static_cast<unsigned char>(payload[end]) & 0xc0) == 0x80) --end;
                    require(end > cursor && end - cursor + 3 <= xdata_limit - aggregate);
                    aggregate += end - cursor + 3;
                    w.put(1000, std::string_view(payload).substr(cursor, end - cursor)); cursor = end;
                }
            }
            for (const auto& v : block.lines) write_line(v);
            for (const auto& v : block.arcs) write_arc(v);
            for (const auto& v : block.circles) write_circle(v);
            for (const auto& v : block.polylines) write_polyline(v);
            for (const auto& v : block.labels) write_label(v);
            w.begin("ENDBLK", "0", "AcDbBlockEnd");
        };
        for (const auto& block : d.blocks) write_block(block, false);
        for (const auto& block : pictures) write_block(block, true);
        w.put(0, "ENDSEC");
    }
    w.put(0, "SECTION"); w.put(2, "ENTITIES");
    for (const auto& v : d.lines) write_line(v);
    for (const auto& v : d.arcs) write_arc(v);
    for (const auto& v : d.circles) write_circle(v);
    for (const auto& v : d.polylines) write_polyline(v);
    std::size_t picture_index = 0;
    const auto write_dimension_common = [&](const char* type, const std::string& entity_layer,
        DxfPoint definition, DxfPoint text_position, int kind, double text_rotation, const std::string& text) {
        w.begin(type, entity_layer, "AcDbDimension");
        w.put(280, "0"); w.put(2, pictures[picture_index++].name); w.put(3, "Standard");
        w.xy(definition); w.put(30, 0.0);
        w.xy(text_position, 11, 21); w.put(31, 0.0);
        // All retained text positions are explicit, including automatic
        // placements already computed by the authoritative native document.
        w.put(70, std::to_string(kind | 128)); w.put(71, "5"); w.put(1, text); w.put(53, text_rotation);
    };
    for (const auto& v : d.dimensions) {
        require(std::isfinite(v.rotation_degrees) && std::abs(v.rotation_degrees) <= 1e12);
        require(v.text.find('\n') == std::string::npos && v.text.find('\r') == std::string::npos);
        printable(v.text, l);
        write_dimension_common("DIMENSION", v.layer, v.dimension_line, v.text_position,
            v.aligned ? 33 : 32, v.text_rotation_degrees, v.text);
        w.put(100, "AcDbAlignedDimension");
        w.xy(v.extension_start, 13, 23); w.put(33, 0.0);
        w.xy(v.extension_end, 14, 24); w.put(34, 0.0);
        w.put(50, v.rotation_degrees);
        if (!v.aligned) {
            w.put(100, "AcDbRotatedDimension");
        }
    }
    for (const auto& v : d.arc_dimensions) {
        write_dimension_common("ARC_DIMENSION", v.layer, v.dimension_arc, v.text_position,
            37, v.text_rotation_degrees, v.text);
        w.put(100, "AcDbArcDimension");
        w.xy(v.extension_start, 13, 23); w.put(33, 0.0);
        w.xy(v.extension_end, 14, 24); w.put(34, 0.0);
        w.xy(v.center, 15, 25); w.put(35, 0.0);
        // Reserved undocumented angles have no measurement authority.
        w.put(40, 0.0); w.put(41, 0.0); w.put(70, "0"); w.put(71, "0");
        w.xy({}, 16, 26); w.put(36, 0.0); w.xy({}, 17, 27); w.put(37, 0.0);
    }
    std::size_t hatch_vertices = vertices;
    for (const auto& v : d.hatches) {
        require(v.solid && v.boundary.size() >= 3 &&
                v.boundary.size() <= l.max_vertices - hatch_vertices);
        hatch_vertices += v.boundary.size();
        w.begin("HATCH", v.layer, "AcDbHatch");
        w.put(10, 0.0); w.put(20, 0.0); w.put(30, 0.0);
        w.put(210, 0.0); w.put(220, 0.0); w.put(230, 1.0);
        w.put(2, "SOLID"); w.put(70, "1"); w.put(71, "0"); w.put(91, "1");
        w.put(92, "3"); w.put(72, "0"); w.put(73, "1");
        w.put(93, std::to_string(v.boundary.size()));
        for (const auto& point : v.boundary) { w.xy(point); }
        w.put(97, "0"); w.put(75, "0"); w.put(76, "1");
    }
    for (const auto& v : d.labels) {
        write_label(v);
    }
    for (const auto& v : d.inserts) {
        require(std::isfinite(v.scale_x) && std::isfinite(v.scale_y) &&
                v.scale_x != 0.0 && v.scale_y != 0.0 &&
                std::isfinite(v.rotation_degrees));
        w.begin("INSERT", v.layer, "AcDbBlockReference");
        w.put(2, v.block_name); w.xy(v.insertion); w.put(30, 0.0);
        w.put(41, v.scale_x); w.put(42, v.scale_y); w.put(43, 1.0);
        w.put(50, v.rotation_degrees); w.put(66, "0");
    }
    w.put(0, "ENDSEC"); w.put(0, "EOF");
    return std::move(w.bytes);
}
} // namespace sketch
