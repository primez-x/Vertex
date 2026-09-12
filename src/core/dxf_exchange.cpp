#include "sketch/dxf_exchange.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
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
struct Pair { int code; std::string_view value; };
using Record = std::span<const Pair>;
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
void entity(DxfImportResult& result, std::string_view type, Record r, std::size_t index,
    std::size_t& vertices, const DxfExchangeLimits& l) {
    auto diagnostic = [&](const char* code) { result.diagnostics.push_back({index, std::string(type), code}); };
    if (type != "LINE" && type != "ARC" && type != "LWPOLYLINE" && type != "TEXT" &&
        type != "DIMENSION" && type != "HATCH") {
        diagnostic("unsupported_entity"); return;
    }
    const auto entity_layer = layer(r, l);
    if (type == "LINE") {
        DxfLine v{point(r, 10, 20), point(r, 11, 21), entity_layer};
        if (!supported(r, {10, 20, 11, 21}, l)) diagnostic("unsupported_feature");
        else result.drawing.lines.push_back(std::move(v));
    } else if (type == "ARC") {
        DxfArc v{point(r, 10, 20), number<double>(mandatory(r, 40)),
            number<double>(mandatory(r, 50)), number<double>(mandatory(r, 51)), entity_layer};
        require(v.radius > 0 && v.start_degrees >= 0 && v.start_degrees < 360 && v.end_degrees >= 0 && v.end_degrees < 360 && v.start_degrees != v.end_degrees);
        if (!supported(r, {10, 20, 40, 50, 51}, l)) diagnostic("unsupported_feature");
        else result.drawing.arcs.push_back(std::move(v));
    } else if (type == "TEXT") {
        DxfLabel v{point(r, 10, 20), number<double>(mandatory(r, 40)), real(r, 50),
            std::string(mandatory(r, 1)), entity_layer};
        require(v.height > 0); printable(v.text, l);
        const bool plain = integer(r, 71) == 0 && integer(r, 72) == 0 && integer(r, 73) == 0 &&
            real(r, 41, 1) == 1 && real(r, 51) == 0 && field(r, 7).value_or("STANDARD") == "STANDARD" &&
            v.text.find('\\') == std::string::npos && v.text.find("%%") == std::string::npos;
        if (!supported(r, {10, 20, 40, 50, 1, 71, 72, 73, 41, 51, 7}, l) || !plain) diagnostic("unsupported_feature");
        else result.drawing.labels.push_back(std::move(v));
    } else if (type == "DIMENSION") {
        DxfDimension v{point(r, 13, 23), point(r, 14, 24), point(r, 10, 20),
            point(r, 11, 21), real(r, 50), std::string(field(r, 1).value_or("")), entity_layer};
        printable(v.text, l);
        const bool linear = field(r, 70).has_value() && integer(r, 70) == 0;
        const bool plain = integer(r, 71) == 0 && integer(r, 72) == 0 &&
            integer(r, 73) == 0 && integer(r, 74) == 0 &&
            real(r, 41, 1) == 1 && real(r, 42) == 0 && real(r, 43) == 0 &&
            real(r, 44) == 0 && real(r, 51) == 0 &&
            field(r, 3).value_or("").empty();
        if (!supported(r, {10, 20, 30, 11, 21, 31, 13, 23, 33, 14, 24, 34, 50, 1, 70,
                           71, 72, 73, 74, 41, 42, 43, 44, 51, 3}, l) || !linear || !plain ||
            std::hypot(v.extension_end.x - v.extension_start.x,
                       v.extension_end.y - v.extension_start.y) <= std::numeric_limits<double>::epsilon()) {
            diagnostic("unsupported_feature");
        } else {
            result.drawing.dimensions.push_back(std::move(v));
        }
    } else if (type == "HATCH") {
        DxfHatch v{{}, integer(r, 70) == 1, entity_layer};
        const auto path_count = integer(r, 91, -1);
        const auto path_flags = integer(r, 92, -1);
        const auto edge_type = integer(r, 72, -1);
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
        const bool shape = path_count == 1 && path_flags == 1 && edge_type == 0 && closed == 1 &&
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
            result.drawing.hatches.push_back(std::move(v));
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
        if (!ok) diagnostic("unsupported_feature"); else result.drawing.polylines.push_back(std::move(v));
    }
}
class Writer {
public:
    explicit Writer(const DxfExchangeLimits& limits) : limits_(limits) {}
    void put(int code, std::string_view value) {
        printable(value, limits_);
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
        const auto v = line(); printable(v, l); pairs.push_back({code, v});
    }
    DxfImportResult result;
    std::size_t i = 0, entity_count = 0, vertices = 0;
    bool header = false, entities = false, version = false, eof = false, units = false;
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
        } else if (section == "ENTITIES") {
            require(!entities); entities = true;
            for (std::size_t j = begin; j < end;) {
                require(pairs[j].code == 0 && ++entity_count <= l.max_entities);
                const auto type = pairs[j++].value; const auto first = j;
                while (j < end && pairs[j].code != 0) ++j;
                entity(result, type, Record(pairs.data() + first, j - first), entity_count, vertices, l);
            }
        } else result.diagnostics.push_back({0, std::string(section), "unsupported_section"});
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
                      d.hatches.size(), d.labels.size()}) {
        require(size <= l.max_entities - count); count += size;
    }
    Writer w(l);
    w.put(0, "SECTION"); w.put(2, "HEADER"); w.put(9, "$ACADVER"); w.put(1, "AC1027");
    w.put(9, "$INSUNITS"); w.put(70, std::to_string(d.insertion_units)); w.put(0, "ENDSEC"); w.put(0, "SECTION"); w.put(2, "ENTITIES");
    for (const auto& v : d.lines) { w.begin("LINE", v.layer, "AcDbLine"); w.xy(v.start); w.put(30, 0.0); w.xy(v.end, 11, 21); w.put(31, 0.0); }
    for (const auto& v : d.arcs) {
        require(v.radius > 0 && v.start_degrees >= 0 && v.start_degrees < 360 && v.end_degrees >= 0 && v.end_degrees < 360 && v.start_degrees != v.end_degrees);
        w.begin("ARC", v.layer, "AcDbCircle"); w.xy(v.center); w.put(30, 0.0); w.put(40, v.radius); w.put(100, "AcDbArc"); w.put(50, v.start_degrees); w.put(51, v.end_degrees);
    }
    std::size_t vertices = 0;
    for (const auto& v : d.polylines) {
        require(v.vertices.size() >= 2 && v.vertices.size() <= l.max_vertices - vertices); vertices += v.vertices.size();
        w.begin("LWPOLYLINE", v.layer, "AcDbPolyline"); w.put(90, std::to_string(v.vertices.size())); w.put(70, v.closed ? "1" : "0");
        for (const auto& vertex : v.vertices) { w.xy(vertex.point); w.put(42, vertex.bulge); }
    }
    for (const auto& v : d.dimensions) {
        require(std::isfinite(v.rotation_degrees) && std::abs(v.rotation_degrees) <= 1e12);
        require(v.text.find('\n') == std::string::npos && v.text.find('\r') == std::string::npos);
        printable(v.text, l);
        w.begin("DIMENSION", v.layer, "AcDbDimension");
        w.put(10, v.dimension_line.x); w.put(20, v.dimension_line.y); w.put(30, 0.0);
        w.xy(v.text_position, 11, 21); w.put(31, 0.0);
        w.xy(v.extension_start, 13, 23); w.put(33, 0.0);
        w.xy(v.extension_end, 14, 24); w.put(34, 0.0);
        w.put(70, "0"); w.put(50, v.rotation_degrees); w.put(1, v.text);
        w.put(100, "AcDbAlignedDimension");
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
        w.put(92, "1"); w.put(72, "0"); w.put(73, "1");
        w.put(93, std::to_string(v.boundary.size()));
        for (const auto& point : v.boundary) { w.xy(point); }
        w.put(97, "0"); w.put(75, "0"); w.put(76, "1");
    }
    for (const auto& v : d.labels) {
        require(v.height > 0 && v.text.find('\\') == std::string::npos && v.text.find("%%") == std::string::npos);
        w.begin("TEXT", v.layer, "AcDbText"); w.xy(v.position); w.put(30, 0.0); w.put(40, v.height); w.put(1, v.text); w.put(50, v.rotation_degrees); w.put(100, "AcDbText");
    }
    w.put(0, "ENDSEC"); w.put(0, "EOF");
    return std::move(w.bytes);
}
} // namespace sketch
