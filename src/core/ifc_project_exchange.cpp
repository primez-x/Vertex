#include "sketch/ifc_project_exchange.hpp"

#include "sketch/boundary_entity.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sketch {
namespace {

using Json = nlohmann::json;
constexpr double kTolerance = 1e-7;

[[noreturn]] void invalid() { throw std::invalid_argument("invalid_or_excessive_ifc"); }

void require(bool value) {
    if (!value) invalid();
}

void validate_limits(const IfcExchangeLimits& limits) {
    const IfcExchangeLimits cap;
    require(limits.max_bytes > 0 && limits.max_bytes <= 256 * 1024 * 1024);
    require(limits.max_records > 0 && limits.max_records <= 500'000);
    require(limits.max_arguments > 0 && limits.max_arguments <= 4'000'000);
    require(limits.max_string_bytes > 0 && limits.max_string_bytes <= 65'536);
    (void)cap;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n'))
        value.remove_suffix(1);
    return value;
}

std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto c = static_cast<unsigned char>(character);
        result.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
    }
    return result;
}

template <typename Number>
Number number(std::string_view value) {
    value = trim(value);
    if (!value.empty() && value.front() == '+') value.remove_prefix(1);
    Number result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(!value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size());
    if constexpr (std::is_floating_point_v<Number>)
        require(std::isfinite(result) && std::abs(result) <= 1e12);
    return result;
}

void printable(std::string_view value, const IfcExchangeLimits& limits) {
    require(value.size() <= limits.max_string_bytes);
    for (const auto character : value) {
        const auto c = static_cast<unsigned char>(character);
        require(c == '\t' || c == '\r' || c == '\n' || (c >= 32 && c <= 126));
    }
}

std::vector<std::string> split_top_level(std::string_view value, std::size_t& argument_count,
                                         const IfcExchangeLimits& limits) {
    value = trim(value);
    if (value.empty()) return {};
    std::vector<std::string> result;
    std::size_t start = 0;
    int depth = 0;
    bool quoted = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto c = value[index];
        if (c == '\'' ) {
            if (quoted && index + 1 < value.size() && value[index + 1] == '\'') {
                ++index;
            } else {
                quoted = !quoted;
            }
            continue;
        }
        if (quoted) continue;
        if (c == '(') ++depth;
        else if (c == ')') {
            --depth;
            require(depth >= 0);
        } else if (c == ',' && depth == 0) {
            result.emplace_back(trim(value.substr(start, index - start)));
            start = index + 1;
        }
    }
    require(!quoted && depth == 0);
    result.emplace_back(trim(value.substr(start)));
    for (const auto& item : result) {
        require(!item.empty());
        if (++argument_count > limits.max_arguments) invalid();
        printable(item, limits);
    }
    return result;
}

std::string_view inner_list(std::string_view value) {
    value = trim(value);
    require(value.size() >= 2 && value.front() == '(' && value.back() == ')');
    return value.substr(1, value.size() - 2);
}

std::optional<int> reference(std::string_view value) {
    value = trim(value);
    if (value.size() < 2 || value.front() != '#') return std::nullopt;
    const auto digits = value.substr(1);
    int id{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || id <= 0)
        invalid();
    return id;
}

std::vector<int> references(std::string_view value) {
    std::vector<int> result;
    bool quoted = false;
    for (std::size_t index = 0; index < value.size();) {
        if (value[index] == '\'') {
            if (quoted && index + 1 < value.size() && value[index + 1] == '\'') index += 2;
            else { quoted = !quoted; ++index; }
            continue;
        }
        if (quoted) { ++index; continue; }
        if (value[index] != '#') { ++index; continue; }
        const auto begin = index++;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') ++index;
        const auto parsed = reference(value.substr(begin, index - begin));
        if (parsed) result.push_back(*parsed);
    }
    return result;
}

std::string decode_string(std::string_view value, const IfcExchangeLimits& limits) {
    value = trim(value);
    if (value == "$") return {};
    require(value.size() >= 2 && value.front() == '\'' && value.back() == '\'');
    std::string result;
    result.reserve(value.size() - 2);
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        if (value[index] == '\'' && index + 1 < value.size() - 1 && value[index + 1] == '\'') {
            result.push_back('\'');
            ++index;
        } else {
            result.push_back(value[index]);
        }
    }
    printable(result, limits);
    return result;
}

std::string step_string(std::string_view value, const IfcExchangeLimits& limits) {
    printable(value, limits);
    std::string result("'");
    for (const auto c : value) {
        result.push_back(c);
        if (c == '\'') result.push_back('\'');
    }
    result.push_back('\'');
    return result;
}

std::string real_text(double value) {
    require(std::isfinite(value) && std::abs(value) <= 1e12);
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general, 17);
    require(result.ec == std::errc{});
    std::string text(buffer, result.ptr);
    if (text == "-0" || text == "-0.0") text = "0";
    return text;
}

struct StepRecord {
    int id{};
    std::string type;
    std::string args;
};

struct ParsedStep {
    std::vector<StepRecord> records;
    std::map<int, std::size_t> by_id;
    std::size_t argument_count{};
};

std::size_t statement_end(std::string_view text, std::size_t cursor, std::size_t end) {
    bool quoted = false;
    int depth = 0;
    for (std::size_t index = cursor; index < end; ++index) {
        const auto c = text[index];
        if (c == '\'') {
            if (quoted && index + 1 < end && text[index + 1] == '\'') ++index;
            else quoted = !quoted;
        } else if (!quoted) {
            if (c == '(') ++depth;
            else if (c == ')') { --depth; require(depth >= 0); }
            else if (c == ';' && depth == 0) return index;
        }
    }
    invalid();
}

StepRecord parse_record(std::string_view statement, const IfcExchangeLimits& limits) {
    statement = trim(statement);
    require(!statement.empty() && statement.front() == '#');
    const auto equal = statement.find('=');
    require(equal != std::string_view::npos);
    const auto id = number<int>(statement.substr(1, equal - 1));
    require(id > 0);
    const auto open = statement.find('(', equal + 1);
    require(open != std::string_view::npos);
    const auto close = statement.rfind(')');
    require(close != std::string_view::npos && close > open);
    require(trim(statement.substr(close + 1)).empty());
    const auto type_view = trim(statement.substr(equal + 1, open - equal - 1));
    require(!type_view.empty());
    printable(type_view, limits);
    return {id, upper(type_view), std::string(statement.substr(open + 1, close - open - 1))};
}

ParsedStep parse_step(std::string_view bytes, const IfcExchangeLimits& limits) {
    validate_limits(limits);
    require(bytes.size() <= limits.max_bytes && !bytes.empty());
    for (const auto c : bytes) {
        const auto value = static_cast<unsigned char>(c);
        require(value == '\t' || value == '\r' || value == '\n' || (value >= 32 && value <= 126));
    }
    const auto text = std::string(bytes);
    const auto normalized = upper(text);
    const auto iso = normalized.find("ISO-10303-21;");
    const auto header = normalized.find("HEADER;", iso == std::string::npos ? 0 : iso);
    const auto data = normalized.find("DATA;", header == std::string::npos ? 0 : header);
    const auto data_end = normalized.find("ENDSEC;", data == std::string::npos ? 0 : data + 5);
    const auto finish = normalized.find("END-ISO-10303-21;", data_end == std::string::npos ? 0 : data_end + 7);
    const auto header_end = normalized.find("ENDSEC;", header == std::string::npos ? 0 : header + 7);
    const auto schema = normalized.find("FILE_SCHEMA", header == std::string::npos ? 0 : header);
    const auto schema_end = schema == std::string::npos ? std::string::npos : normalized.find(';', schema);
    require(iso == 0 && header != std::string::npos && data != std::string::npos &&
            data_end != std::string::npos && finish != std::string::npos &&
            header_end != std::string::npos && header_end < data && data < data_end &&
            schema != std::string::npos && schema < header_end && schema_end != std::string::npos &&
            normalized.substr(schema, schema_end - schema).find("IFC4") != std::string::npos);
    const auto finish_end = finish + std::string_view("END-ISO-10303-21;").size();
    require(trim(normalized.substr(finish_end)).empty());

    ParsedStep result;
    std::size_t cursor = data + 5;
    while (cursor < data_end) {
        while (cursor < data_end && (text[cursor] == ' ' || text[cursor] == '\t' ||
                                     text[cursor] == '\r' || text[cursor] == '\n')) ++cursor;
        if (cursor >= data_end) break;
        const auto end = statement_end(text, cursor, data_end);
        auto record = parse_record(std::string_view(text).substr(cursor, end - cursor), limits);
        require(result.records.size() < limits.max_records);
        require(result.by_id.emplace(record.id, result.records.size()).second);
        result.records.push_back(std::move(record));
        cursor = end + 1;
    }
    require(cursor == data_end);
    for (const auto& record : result.records)
        (void)split_top_level(record.args, result.argument_count, limits);
    return result;
}

const StepRecord* find_record(const ParsedStep& parsed, int id) {
    const auto found = parsed.by_id.find(id);
    return found == parsed.by_id.end() ? nullptr : &parsed.records[found->second];
}

struct Point3 { double x{}, y{}, z{}; };

Point3 point_record(const StepRecord& record, std::size_t& argument_count,
                    const IfcExchangeLimits& limits) {
    require(record.type == "IFCCARTESIANPOINT");
    const auto outer = split_top_level(record.args, argument_count, limits);
    require(outer.size() == 1);
    const auto coordinates = split_top_level(inner_list(outer.front()), argument_count, limits);
    require(coordinates.size() >= 2 && coordinates.size() <= 3);
    Point3 point{number<double>(coordinates[0]), number<double>(coordinates[1]),
                 coordinates.size() == 3 ? number<double>(coordinates[2]) : 0.0};
    require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z));
    return point;
}

std::vector<int> list_references(std::string_view value, std::size_t& argument_count,
                                 const IfcExchangeLimits& limits) {
    const auto fields = split_top_level(inner_list(value), argument_count, limits);
    std::vector<int> result;
    for (const auto& field : fields) {
        const auto id = reference(field);
        require(id.has_value());
        result.push_back(*id);
    }
    return result;
}

bool same_point(Vec2 left, Vec2 right) {
    return std::hypot(left.x - right.x, left.y - right.y) <= kTolerance;
}

Json boundary_json(const Boundary& boundary) {
    Json result = Json::array();
    for (const auto& segment : boundary) {
        result.push_back({{"start", {segment.start.x, segment.start.y}},
                          {"end", {segment.end.x, segment.end.y}},
                          {"sweep_radians", segment.sweep_radians}});
    }
    return result;
}

std::optional<Vec2> read_point(const Json& value) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
        return std::nullopt;
    const Vec2 point{value[0].get<double>(), value[1].get<double>()};
    return std::isfinite(point.x) && std::isfinite(point.y) ? std::optional<Vec2>(point) : std::nullopt;
}

std::optional<Segment> read_segment(const Json& value) {
    if (!value.is_object() || !value.contains("start") || !value.contains("end") ||
        !value.contains("sweep_radians")) return std::nullopt;
    const auto start = read_point(value.at("start"));
    const auto end = read_point(value.at("end"));
    if (!start || !end || !value.at("sweep_radians").is_number()) return std::nullopt;
    const auto sweep = value.at("sweep_radians").get<double>();
    return std::isfinite(sweep) ? std::optional<Segment>(Segment{*start, *end, sweep}) : std::nullopt;
}

std::optional<Boundary> read_boundary(const Entity& entity) {
    try {
        if (can_recognize_boundary_entity_type(entity.type)) {
            const auto version = inspect_boundary_entity_version(entity);
            if (version.format == BoundaryEntityFormat::identified_v1)
                return boundary_geometry(decode_identified_boundary_entity(entity));
            if (version.format == BoundaryEntityFormat::unsupported_version)
                return std::nullopt;
        }
        const auto* value = entity.properties.is_object() && entity.properties.contains("boundary")
            ? &entity.properties.at("boundary") : nullptr;
        if (!value && entity.properties.is_object() && entity.properties.contains("segments"))
            value = &entity.properties.at("segments");
        if (!value || !value->is_array() || value->empty()) return std::nullopt;
        Boundary result;
        for (const auto& item : *value) {
            const auto segment = read_segment(item);
            if (!segment) return std::nullopt;
            result.push_back(*segment);
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Segment> read_baseline(const Entity& entity) {
    if (!entity.properties.is_object() || !entity.properties.contains("baseline")) return std::nullopt;
    return read_segment(entity.properties.at("baseline"));
}

void add_diagnostic(std::vector<IfcProjectDiagnostic>& output, std::string id,
                    std::string kind, std::string code) {
    output.push_back({std::move(id), std::move(kind), std::move(code)});
}

class StepBuilder {
public:
    explicit StepBuilder(const IfcExchangeLimits& limits) : limits_(limits) {}

    int add(std::string type, std::string args) {
        require(records_.size() < limits_.max_records);
        records_.push_back({next_id_++, std::move(type), std::move(args)});
        return records_.back().id;
    }

    std::string finish() const {
        std::string result;
        result.reserve(records_.size() * 80 + 256);
        result += "ISO-10303-21;\nHEADER;\n";
        result += "FILE_DESCRIPTION(('ViewDefinition [CoordinationView_V2.0]'),'2;1');\n";
        result += "FILE_NAME('vertex-project.ifc','1970-01-01T00:00:00',('Vertex'),('Vertex'),'Vertex','Vertex','');\n";
        result += "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n";
        for (const auto& record : records_) {
            result += '#';
            result += std::to_string(record.id);
            result += '=';
            result += record.type;
            result += '(';
            result += record.args;
            result += ");\n";
        }
        result += "ENDSEC;\nEND-ISO-10303-21;\n";
        require(result.size() <= limits_.max_bytes);
        return result;
    }

private:
    const IfcExchangeLimits& limits_;
    int next_id_{1};
    std::vector<StepRecord> records_;
};

std::string ref(int id) { return '#' + std::to_string(id); }

std::string guid_for(std::string_view source, std::size_t ordinal) {
    std::uint64_t hash = 1469598103934665603ULL ^ static_cast<std::uint64_t>(ordinal);
    for (const auto c : source) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    static constexpr char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";
    std::string result(22, '0');
    for (auto& c : result) {
        c = alphabet[hash & 63U];
        hash = (hash >> 6U) ^ (hash * 0x9e3779b97f4a7c15ULL);
    }
    return result;
}

std::vector<Vec2> linear_points(const Boundary& boundary, bool& closed) {
    require(!boundary.empty());
    std::vector<Vec2> result;
    result.reserve(boundary.size() + 1);
    for (const auto& segment : boundary) {
        require(std::isfinite(segment.sweep_radians));
        if (std::abs(segment.sweep_radians) > kTolerance) return {};
        result.push_back(segment.start);
    }
    closed = same_point(boundary.back().end, boundary.front().start);
    if (!closed) result.push_back(boundary.back().end);
    else result.push_back(boundary.front().start);
    return result;
}

struct ExportContext {
    const IfcExchangeLimits& limits;
    StepBuilder builder;
    int owner_history{};
    int origin{};
    int z_direction{};
    int axis_placement{};
    int placement{};
    std::size_t ordinal{};

    explicit ExportContext(const IfcExchangeLimits& limits) : limits(limits), builder(limits) {
        const auto person = builder.add("IFCPERSON", "$,$,'Vertex',$,$,$,$");
        const auto organization = builder.add("IFCORGANIZATION", "$,'Private',$,$");
        const auto person_org = builder.add("IFCPERSONANDORGANIZATION",
                                            ref(person) + "," + ref(organization) + ",$");
        const auto application = builder.add("IFCAPPLICATION",
                                             ref(organization) + ",'1.0','Vertex','VX'");
        owner_history = builder.add("IFCOWNERHISTORY", ref(person_org) + "," + ref(application) +
            ",$,.ADDED.,$,$,$,0");
        const auto unit = builder.add("IFCSIUNIT", "* ,.LENGTHUNIT.,$,.METRE.");
        builder.add("IFCUNITASSIGNMENT", "(" + ref(unit) + ")");
        origin = builder.add("IFCCARTESIANPOINT", "(0.,0.,0.)");
        z_direction = builder.add("IFCDIRECTION", "(0.,0.,1.)");
        axis_placement = builder.add("IFCAXIS2PLACEMENT3D", ref(origin) + ",$,$");
        placement = builder.add("IFCLOCALPLACEMENT", "$," + ref(axis_placement));
    }
};

void export_product(const DocumentSnapshot& document, const Entity& entity,
                    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
    const auto& type = entity.type;
    std::string product_type;
    Boundary boundary;
    bool closed = false;
    bool use_solid = false;
    double depth = 0.0;

    if (type == "wall") {
        const auto baseline = read_baseline(entity);
        if (!baseline) {
            add_diagnostic(diagnostics, entity.id, type, "wall_baseline_not_representable");
            return;
        }
        boundary = {*baseline};
        product_type = "IFCWALLSTANDARDCASE";
        if (entity.properties.is_object()) {
            const auto thickness = entity.properties.value("thickness_m", 0.0);
            const auto height = entity.properties.value("height_m", 0.0);
            if (!(thickness > kTolerance) || !(height > kTolerance))
                add_diagnostic(diagnostics, entity.id, type, "wall_profile_metadata_missing");
            else
                add_diagnostic(diagnostics, entity.id, type, "wall_thickness_height_axis_only");
        }
    } else if (type == "slab") {
        if (!entity.properties.is_object() || !entity.properties.contains("boundary")) {
            add_diagnostic(diagnostics, entity.id, type, "slab_boundary_not_representable");
            return;
        }
        const auto decoded = read_boundary(entity);
        if (!decoded) {
            add_diagnostic(diagnostics, entity.id, type, "slab_boundary_not_representable");
            return;
        }
        boundary = *decoded;
        product_type = "IFCSLAB";
        const auto thickness = entity.properties.value("thickness_m", 0.0);
        if (std::isfinite(thickness) && thickness > kTolerance) {
            depth = thickness;
            use_solid = true;
        } else {
            add_diagnostic(diagnostics, entity.id, type, "slab_thickness_not_exported");
        }
        if (entity.properties.contains("holes") && entity.properties.at("holes").is_array() &&
            !entity.properties.at("holes").empty())
            add_diagnostic(diagnostics, entity.id, type, "slab_holes_not_exported");
    } else if (type == "boundary" || type == "measurement_boundary" || type == "room_boundary") {
        const auto decoded = read_boundary(entity);
        if (!decoded) {
            add_diagnostic(diagnostics, entity.id, type, "boundary_not_representable");
            return;
        }
        boundary = *decoded;
        product_type = "IFCBUILDINGELEMENTPROXY";
    } else if (type == "property" || type == "building" || type == "floor" || type == "layer" ||
               type == "sheet" || type == "view" || type == "sheet_view_model" ||
               type == "reference_asset" || type == "dxf_source") {
        return;
    } else {
        add_diagnostic(diagnostics, entity.id, type, "entity_not_representable");
        return;
    }

    bool linear_closed = false;
    const auto points = linear_points(boundary, linear_closed);
    if (points.empty()) {
        add_diagnostic(diagnostics, entity.id, type, "curved_geometry_not_representable");
        return;
    }
    closed = linear_closed;
    if (product_type == "IFCWALLSTANDARDCASE" && closed)
        add_diagnostic(diagnostics, entity.id, type, "wall_axis_closed");
    const auto polyline_points = [&] {
        std::vector<int> ids;
        ids.reserve(points.size());
        for (const auto point : points) {
            const auto id = context.builder.add("IFCCARTESIANPOINT",
                "(" + real_text(point.x) + "," + real_text(point.y) + ",0.)");
            ids.push_back(id);
        }
        std::string args = "(";
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (index) args += ',';
            args += ref(ids[index]);
        }
        args += ')';
        return context.builder.add("IFCPOLYLINE", std::move(args));
    }();

    int shape{};
    if (use_solid && closed) {
        const auto profile = context.builder.add("IFCARBITRARYCLOSEDPROFILEDEF",
            ".AREA.,$," + ref(polyline_points));
        const auto solid = context.builder.add("IFCEXTRUDEDAREASOLID",
            ref(profile) + "," + ref(context.axis_placement) + "," + ref(context.z_direction) +
            "," + real_text(depth));
        shape = context.builder.add("IFCSHAPEREPRESENTATION",
            "$,'Body','SweptSolid',(" + ref(solid) + ")");
    } else {
        const auto identifier = product_type == "IFCWALLSTANDARDCASE" ? "Axis" : "Footprint";
        shape = context.builder.add("IFCSHAPEREPRESENTATION",
            "$,'" + std::string(identifier) + "','Curve2D',(" + ref(polyline_points) + ")");
    }
    const auto product_shape = context.builder.add("IFCPRODUCTDEFINITIONSHAPE",
        "$,$,(" + ref(shape) + ")");
    const auto name = step_string(entity.id, context.limits);
    std::string classification;
    if (entity.properties.is_object())
        classification = entity.properties.value("classification", entity.type);
    const auto description = step_string(entity.type + ":" + classification,
                                         context.limits);
    const auto global_id = step_string(guid_for(entity.id, ++context.ordinal), context.limits);
    const auto placement = ref(context.placement);
    if (product_type == "IFCWALLSTANDARDCASE") {
        context.builder.add(product_type, global_id + "," + ref(context.owner_history) + "," +
            name + "," + description + ",$," + placement + "," + ref(product_shape) + ",$");
    } else if (product_type == "IFCSLAB") {
        context.builder.add(product_type, global_id + "," + ref(context.owner_history) + ","+
            name + "," + description + ",$," + placement + "," + ref(product_shape) + ",$,.FLOOR.");
    } else {
        context.builder.add(product_type, global_id + "," + ref(context.owner_history) + ","+
            name + "," + description + ",$," + placement + "," + ref(product_shape) + ",$");
    }
    (void)document;
}

struct GeometryResult {
    std::optional<Boundary> boundary;
    std::optional<double> depth;
    Vec2 translation{};
    bool has_translation{};
    bool rotated{};
};

std::optional<std::vector<Vec2>> geometry_points(const ParsedStep& parsed, const StepRecord& record,
                                                 std::size_t& argument_count,
                                                 const IfcExchangeLimits& limits) {
    if (record.type != "IFCPOLYLINE" && record.type != "IFCPOLYLOOP") return std::nullopt;
    const auto fields = split_top_level(record.args, argument_count, limits);
    require(fields.size() == 1);
    const auto ids = list_references(fields[0], argument_count, limits);
    std::vector<Vec2> points;
    points.reserve(ids.size());
    for (const auto id : ids) {
        const auto point = find_record(parsed, id);
        require(point && point->type == "IFCCARTESIANPOINT");
        const auto value = point_record(*point, argument_count, limits);
        points.push_back({value.x, value.y});
    }
    return points;
}

std::optional<Boundary> boundary_from_points(std::vector<Vec2> points, bool close) {
    if (points.size() < 2) return std::nullopt;
    const auto repeated = same_point(points.front(), points.back());
    if ((close || repeated) && !repeated) points.push_back(points.front());
    if (points.size() < 2) return std::nullopt;
    Boundary result;
    result.reserve(points.size() - 1);
    for (std::size_t index = 0; index + 1 < points.size(); ++index)
        result.push_back({points[index], points[index + 1], 0.0});
    if (result.empty()) return std::nullopt;
    return result;
}

GeometryResult geometry_for(const ParsedStep& parsed, const StepRecord& product,
                            std::vector<IfcProjectDiagnostic>& diagnostics,
                            std::size_t& argument_count, const IfcExchangeLimits& limits) {
    GeometryResult result;
    std::queue<int> pending;
    std::set<int> visited;
    for (const auto id : references(product.args)) pending.push(id);
    while (!pending.empty()) {
        const auto id = pending.front();
        pending.pop();
        if (!visited.insert(id).second) continue;
        const auto record = find_record(parsed, id);
        if (!record) continue;
        if (record->type == "IFCPOLYLINE" || record->type == "IFCPOLYLOOP") {
            try {
                if (!result.boundary) {
                    const auto points = geometry_points(parsed, *record, argument_count, limits);
                    if (points) result.boundary = boundary_from_points(
                        *points, record->type == "IFCPOLYLOOP");
                }
            } catch (...) {
                add_diagnostic(diagnostics, "#" + std::to_string(record->id), record->type,
                              "polyline_not_reconstructable");
            }
        } else if (record->type == "IFCEXTRUDEDAREASOLID") {
            try {
                const auto fields = split_top_level(record->args, argument_count, limits);
                require(fields.size() == 4);
                const auto candidate = number<double>(fields[3]);
                if (candidate > kTolerance) result.depth = candidate;
            } catch (...) {
                add_diagnostic(diagnostics, "#" + std::to_string(record->id), record->type,
                              "extrusion_depth_not_reconstructable");
            }
        } else if (record->type == "IFCAXIS2PLACEMENT3D") {
            try {
                const auto fields = split_top_level(record->args, argument_count, limits);
                require(fields.size() == 3);
                const auto location = reference(fields[0]);
                if (location) {
                    const auto point = find_record(parsed, *location);
                    require(point && point->type == "IFCCARTESIANPOINT");
                    const auto value = point_record(*point, argument_count, limits);
                    result.translation = {value.x, value.y};
                    result.has_translation = true;
                }
                if (fields[1] != "$" || fields[2] != "$") result.rotated = true;
            } catch (...) {
                add_diagnostic(diagnostics, "#" + std::to_string(record->id), record->type,
                              "placement_not_reconstructable");
            }
        } else if (record->type == "IFCINDEXEDPOLYCURVE") {
            add_diagnostic(diagnostics, "#" + std::to_string(record->id), record->type,
                          "indexed_curve_not_reconstructable");
        }
        for (const auto child : references(record->args)) pending.push(child);
    }
    if (result.boundary && result.has_translation) {
        for (auto& segment : *result.boundary) {
            segment.start.x += result.translation.x;
            segment.start.y += result.translation.y;
            segment.end.x += result.translation.x;
            segment.end.y += result.translation.y;
        }
    }
    return result;
}

bool is_product(std::string_view type) {
    return type == "IFCWALL" || type == "IFCWALLSTANDARDCASE" || type == "IFCSLAB" ||
           type == "IFCROOF" || type == "IFCSPACE" || type == "IFCDOOR" ||
           type == "IFCWINDOW" || type == "IFCOPENINGELEMENT" ||
           type == "IFCBUILDINGELEMENTPROXY";
}

bool is_structural(std::string_view type) {
    static constexpr std::string_view values[] = {
        "IFCPERSON", "IFCORGANIZATION", "IFCPERSONANDORGANIZATION", "IFCAPPLICATION",
        "IFCOWNERHISTORY", "IFCSIUNIT", "IFCUNITASSIGNMENT", "IFCPROJECT", "IFCSITE",
        "IFCBUILDING", "IFCBUILDINGSTOREY", "IFCGEOMETRICREPRESENTATIONCONTEXT",
        "IFCGEOMETRICREPRESENTATIONSUBCONTEXT", "IFCCARTESIANPOINT", "IFCDIRECTION",
        "IFCAXIS2PLACEMENT3D", "IFCLOCALPLACEMENT", "IFCARBITRARYCLOSEDPROFILEDEF",
        "IFCEXTRUDEDAREASOLID", "IFCSHAPEREPRESENTATION", "IFCPRODUCTDEFINITIONSHAPE",
        "IFCPOLYLINE", "IFCPOLYLOOP", "IFCFACEOUTERBOUND", "IFCFACE", "IFCCLOSEDSHELL",
        "IFCSOLIDMODEL", "IFCCONVERSIONBASEDUNIT", "IFCMEASUREWITHUNIT", "IFCDIMENSIONALEXPONENTS",
        "IFCGEOMETRICREPRESENTATIONCONTEXT", "IFCGEOMETRICREPRESENTATIONSUBCONTEXT"};
    return std::find(std::begin(values), std::end(values), type) != std::end(values);
}

bool is_relationship(std::string_view type) {
    return type.size() >= 6 && type.substr(0, 6) == "IFCREL";
}

std::string product_string(const StepRecord& record, std::size_t index,
                           std::size_t& argument_count, const IfcExchangeLimits& limits) {
    const auto fields = split_top_level(record.args, argument_count, limits);
    if (index >= fields.size()) return {};
    if (trim(fields[index]) == "$") return {};
    try { return decode_string(fields[index], limits); }
    catch (...) { return {}; }
}

} // namespace

IfcProjectExportResult export_project_ifc(const DocumentSnapshot& document,
                                          const IfcExchangeLimits& limits) {
    validate_limits(limits);
    IfcProjectExportResult result;
    ExportContext context(limits);
    for (const auto& [id, entity] : document.entities()) {
        (void)id;
        export_product(document, entity, context, result.diagnostics);
    }
    try {
        result.step = context.builder.finish();
    } catch (...) {
        add_diagnostic(result.diagnostics, {}, "PROJECT", "mapped_step_not_serializable");
        result.step.clear();
    }
    return result;
}

IfcProjectImportResult import_project_ifc(std::string_view bytes,
                                          const IfcExchangeLimits& limits) {
    const auto parsed = parse_step(bytes, limits);
    IfcProjectImportResult result;
    std::size_t argument_count = parsed.argument_count;
    for (const auto& record : parsed.records) {
        if (is_product(record.type)) {
            const auto geometry = geometry_for(parsed, record, result.diagnostics,
                                               argument_count, limits);
            if (!geometry.boundary) {
                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                              "product_geometry_missing");
                continue;
            }
            const auto name = product_string(record, 2, argument_count, limits);
            const auto description = product_string(record, 3, argument_count, limits);
            std::string classification = "ifc_product";
            if (record.type == "IFCWALL" || record.type == "IFCWALLSTANDARDCASE") classification = "ifc_wall_axis";
            else if (record.type == "IFCSLAB") classification = "ifc_slab";
            else if (record.type == "IFCROOF") classification = "ifc_roof";
            else if (record.type == "IFCSPACE") classification = "ifc_space";
            else if (record.type == "IFCDOOR" || record.type == "IFCWINDOW" ||
                     record.type == "IFCOPENINGELEMENT") classification = "ifc_opening";
            else if (record.type == "IFCBUILDINGELEMENTPROXY" && description.rfind("boundary:", 0) == 0)
                classification = description.substr(std::string("boundary:").size());
            if (geometry.rotated)
                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                              "placement_rotation_unsupported");
            if (record.type == "IFCDOOR" || record.type == "IFCWINDOW" ||
                record.type == "IFCOPENINGELEMENT")
                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                              "opening_host_unbound");
            Json properties{{"boundary", boundary_json(*geometry.boundary)},
                             {"classification", classification}, {"ifc_type", record.type}};
            if (!name.empty()) properties["ifc_name"] = name;
            if (!description.empty()) properties["ifc_description"] = description;
            if (geometry.depth) properties["ifc_extrusion_depth_m"] = *geometry.depth;
            const auto id = "ifc-" + std::to_string(record.id);
            result.entities.push_back(Entity{id, "boundary", std::move(properties), false,
                Json{{"ifc_source", {{"record_id", record.id}, {"record_type", record.type}}}}});
            continue;
        }
        if (is_relationship(record.type)) {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "relationship_not_reconstructed");
        } else if (record.type == "IFCPROPERTYSET" || record.type == "IFCPROPERTYSINGLEVALUE" ||
                   record.type == "IFCMATERIAL" || record.type == "IFCMATERIALLAYER" ||
                   record.type == "IFCMATERIALLAYERSET" || record.type == "IFCINDEXEDPOLYCURVE") {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "property_or_geometry_not_reconstructed");
        } else if (!is_structural(record.type)) {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "unsupported_entity");
        }
    }
    result.source_retention_required = !result.diagnostics.empty();
    return result;
}

} // namespace sketch
