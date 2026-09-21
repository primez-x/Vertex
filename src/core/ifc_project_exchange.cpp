#include "sketch/ifc_project_exchange.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/wall_semantics.hpp"

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
        // A bounded IFC4 subset, not a claim of externally qualified MVD conformance.
        result += "FILE_DESCRIPTION(('Vertex IFC4 exchange subset'),'2;1');\n";
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
    result.front() = alphabet[static_cast<unsigned char>(result.front()) & 3U];
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
    int representation_context{};
    int storey{};
    std::vector<int> contained_products;
    std::size_t ordinal{};
    std::map<std::string, int, std::less<>> product_ids;
    std::vector<std::pair<std::string, std::string>> opening_host_links;

    explicit ExportContext(const IfcExchangeLimits& limits) : limits(limits), builder(limits) {
        const auto person = builder.add("IFCPERSON", "$,$,'Vertex',$,$,$,$,$");
        const auto organization = builder.add("IFCORGANIZATION", "$,'Private',$,$,$");
        const auto person_org = builder.add("IFCPERSONANDORGANIZATION",
                                            ref(person) + "," + ref(organization) + ",$");
        const auto application = builder.add("IFCAPPLICATION",
                                             ref(organization) + ",'1.0','Vertex','VX'");
        owner_history = builder.add("IFCOWNERHISTORY", ref(person_org) + "," + ref(application) +
            ",$,.ADDED.,$,$,$,0");
        const auto unit = builder.add("IFCSIUNIT", "* ,.LENGTHUNIT.,$,.METRE.");
        const auto units = builder.add("IFCUNITASSIGNMENT", "(" + ref(unit) + ")");
        origin = builder.add("IFCCARTESIANPOINT", "(0.,0.,0.)");
        z_direction = builder.add("IFCDIRECTION", "(0.,0.,1.)");
        axis_placement = builder.add("IFCAXIS2PLACEMENT3D", ref(origin) + ",$,$");
        placement = builder.add("IFCLOCALPLACEMENT", "$," + ref(axis_placement));
        representation_context = builder.add("IFCGEOMETRICREPRESENTATIONCONTEXT",
            "$,'Model',3,0.0000001," + ref(axis_placement) + ",$");
        const auto project = builder.add("IFCPROJECT", root("project", "Vertex project") +
            ",$,$,$,(" + ref(representation_context) + ")," + ref(units));
        const auto site = builder.add("IFCSITE", root("site", "Default site") +
            ",$," + ref(placement) + ",$,$,.ELEMENT.,$,$,$,$,$");
        const auto building = builder.add("IFCBUILDING", root("building", "Default building") +
            ",$," + ref(placement) + ",$,$,.ELEMENT.,$,$,$");
        storey = builder.add("IFCBUILDINGSTOREY", root("storey", "Default storey") +
            ",$," + ref(placement) + ",$,$,.ELEMENT.,0.");
        aggregate(project, site, "project-site");
        aggregate(site, building, "site-building");
        aggregate(building, storey, "building-storey");
    }

    std::string root(std::string_view id, std::string_view name) {
        return step_string(guid_for(id, ++ordinal), limits) + "," + ref(owner_history) +
            "," + step_string(name, limits) + ",$";
    }

    void aggregate(int parent, int child, std::string_view id) {
        builder.add("IFCRELAGGREGATES", root(id, "") + "," + ref(parent) + ",(" + ref(child) + ")");
    }
};

void retain_properties(const Entity& entity, int product_id, ExportContext& context,
                       std::vector<IfcProjectDiagnostic>& diagnostics) {
    const auto payload = entity.properties.dump(-1, ' ', true);
    if (payload.size() > context.limits.max_string_bytes / 2) {
        add_diagnostic(diagnostics, entity.id, entity.type, "vertex_properties_not_exported");
        return;
    }
    const auto property = context.builder.add("IFCPROPERTYSINGLEVALUE",
        "'Properties',$,IFCTEXT(" + step_string(payload, context.limits) + "),$");
    const auto pset = context.builder.add("IFCPROPERTYSET",
        context.root("properties:" + entity.id, "Pset_VertexExchange_v1") + ",(" + ref(property) + ")");
    context.builder.add("IFCRELDEFINESBYPROPERTIES",
        context.root("property-link:" + entity.id, "") + ",(" + ref(product_id) + ")," + ref(pset));
}

void export_native_reference(const Entity& entity, ExportContext& context,
                             std::vector<IfcProjectDiagnostic>& diagnostics) {
    const auto product = context.builder.add("IFCBUILDINGELEMENTPROXY",
        context.root("reference:" + entity.id, entity.id) + "," + step_string(entity.type, context.limits) +
        "," + ref(context.placement) + ",$,$,.NOTDEFINED.");
    context.contained_products.push_back(product);
    auto retained = entity;
    // An imported reference already carries the original bounded native payload.
    // Reuse it verbatim so repeated export/import cycles neither grow a recursive
    // carrier envelope nor cross the retention limit for unchanged content.
    const auto prior = entity.extensions.find("ifc_vertex_properties");
    if (entity.type == "ifc_reference" && prior != entity.extensions.end() && prior->is_object()) {
        retained.properties = *prior;
    } else {
        retained.properties = Json{{"native_entity", {{"id", entity.id}, {"type", entity.type},
            {"required", entity.required}, {"properties", entity.properties}, {"extensions", entity.extensions}}}};
    }
    retain_properties(retained, product, context, diagnostics);
    add_diagnostic(diagnostics, entity.id, entity.type, "native_reference_only");
}

void export_wall_construction(const Entity& entity, int product, ExportContext& context,
                              std::vector<IfcProjectDiagnostic>& diagnostics) {
    // The native model has occurrence layer stacks, not a shared wall-type catalog.
    // Give each construction its own type rather than invent shared type identity.
    const auto type = context.builder.add("IFCWALLTYPE",
        context.root("wall-type:" + entity.id, entity.id + " construction") +
        ",$,$,$,$,$,.NOTDEFINED.");
    context.builder.add("IFCRELDEFINESBYTYPE",
        context.root("wall-type-link:" + entity.id, "") + ",(" + ref(product) + ")," + ref(type));
    const auto layers = entity.properties.contains("layers")
        ? parse_wall_layers(entity.properties.at("layers"), entity.properties.at("thickness_m").get<double>())
        : std::vector<WallLayer>{};
    if (layers.empty()) {
        if (entity.properties.contains("material_assignment")) {
            const auto& assignment = entity.properties.at("material_assignment");
            const auto material = context.builder.add("IFCMATERIAL",
                step_string(assignment.at("material_id").get<std::string>(), context.limits) + "," +
                step_string("Native catalog: " + assignment.at("catalog_id").get<std::string>(), context.limits) + ",$");
            context.builder.add("IFCRELASSOCIATESMATERIAL",
                context.root("wall-material:" + entity.id, "") + ",(" + ref(product) + "," + ref(type) + ")," + ref(material));
        }
        return;
    }
    if (entity.properties.contains("material_assignment"))
        add_diagnostic(diagnostics, entity.id, entity.type, "wall_overall_material_retained_with_layers");
    std::string layer_list;
    for (const auto& layer : layers) {
        std::string material = "$";
        if (layer.material) {
            const auto id = context.builder.add("IFCMATERIAL",
                step_string(layer.material->material_id, context.limits) + "," +
                step_string("Native catalog: " + layer.material->catalog_id, context.limits) + ",$");
            material = ref(id);
        }
        const auto id = context.builder.add("IFCMATERIALLAYER", material + "," +
            real_text(layer.thickness) + ",$," + step_string(layer.id, context.limits) + ",$,$,$");
        if (!layer_list.empty()) layer_list += ',';
        layer_list += ref(id);
    }
    const auto set = context.builder.add("IFCMATERIALLAYERSET", "(" + layer_list + ")," +
        step_string(entity.id + " layers", context.limits) + ",$");
    // Direct layer-set assignment describes construction without claiming a
    // local material usage axis for the world-coordinate body representation.
    context.builder.add("IFCRELASSOCIATESMATERIAL",
        context.root("wall-material:" + entity.id, "") + ",(" + ref(product) + "," + ref(type) + ")," + ref(set));
    add_diagnostic(diagnostics, entity.id, entity.type,
                   "wall_layer_placement_not_exported");
}

void export_product(const DocumentSnapshot& document, const Entity& entity,
                    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
    const auto& type = entity.type;
    std::string product_type;
    Boundary boundary;
    bool closed = false;
    bool use_solid = false;
    double depth = 0.0;
    double local_elevation = 0.0;

    if (type == "wall") {
        const auto baseline = read_baseline(entity);
        if (!baseline) {
            add_diagnostic(diagnostics, entity.id, type, "wall_baseline_not_representable");
            return;
        }
        boundary = {*baseline};
        product_type = "IFCWALL";
        local_elevation = entity.properties.value("elevation_m", 0.0);
        if (entity.properties.is_object()) {
            const auto thickness = entity.properties.value("thickness_m", 0.0);
            const auto height = entity.properties.value("height_m", 0.0);
            const auto length = std::hypot(baseline->end.x - baseline->start.x,
                                           baseline->end.y - baseline->start.y);
            if (!std::isfinite(thickness) || !std::isfinite(height) ||
                !(thickness > kTolerance) || !(height > kTolerance) || !(length > kTolerance)) {
                add_diagnostic(diagnostics, entity.id, type, "wall_profile_metadata_missing");
                return;
            }
            if (std::abs(baseline->sweep_radians) > kTolerance ||
                std::abs(entity.properties.value("slope_rise_m", 0.0)) > kTolerance) {
                add_diagnostic(diagnostics, entity.id, type, "wall_body_not_representable");
                return;
            }
            const Vec2 normal{-(baseline->end.y - baseline->start.y) / length * thickness / 2,
                               (baseline->end.x - baseline->start.x) / length * thickness / 2};
            const Vec2 a{baseline->start.x - normal.x, baseline->start.y - normal.y};
            const Vec2 b{baseline->end.x - normal.x, baseline->end.y - normal.y};
            const Vec2 c{baseline->end.x + normal.x, baseline->end.y + normal.y};
            const Vec2 d{baseline->start.x + normal.x, baseline->start.y + normal.y};
            boundary = {{a,b,0}, {b,c,0}, {c,d,0}, {d,a,0}};
            depth = height;
            use_solid = true;
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
        local_elevation = entity.properties.value("elevation_m", 0.0);
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
    } else if (type == "opening") {
        if (!entity.properties.is_object()) {
            add_diagnostic(diagnostics, entity.id, type,
                           "opening_properties_not_representable");
            return;
        }
        const auto host_id = entity.properties.value("wall_id", std::string{});
        const auto host = document.entities().find(host_id);
        if (host == document.entities().end() || host->second.type != "wall") {
            add_diagnostic(diagnostics, entity.id, type, "opening_host_missing");
            return;
        }
        const auto baseline = read_baseline(host->second);
        if (!baseline) {
            add_diagnostic(diagnostics, entity.id, type,
                           "opening_host_baseline_not_representable");
            return;
        }
        if (std::abs(baseline->sweep_radians) > kTolerance) {
            add_diagnostic(diagnostics, entity.id, type,
                           "opening_curved_host_not_representable");
            return;
        }
        const auto read_number = [&](const char* key, double fallback) {
            const auto found = entity.properties.find(key);
            if (found == entity.properties.end()) return fallback;
            return found->is_number() ? found->get<double>()
                                     : std::numeric_limits<double>::quiet_NaN();
        };
        const auto offset = read_number("offset_m", std::numeric_limits<double>::quiet_NaN());
        const auto width = read_number("width_m", std::numeric_limits<double>::quiet_NaN());
        const auto sill = read_number("sill_m", std::numeric_limits<double>::quiet_NaN());
        const auto height = read_number("height_m", std::numeric_limits<double>::quiet_NaN());
        double wall_thickness = 0.0;
        for (const auto* key : {"thickness_m", "thickness"}) {
            const auto found = host->second.properties.find(key);
            if (found != host->second.properties.end() && found->is_number()) {
                wall_thickness = found->get<double>();
                break;
            }
        }
        double wall_height = 0.0;
        double wall_elevation = 0.0;
        for (const auto* key : {"height_m", "height"}) {
            const auto found = host->second.properties.find(key);
            if (found != host->second.properties.end() && found->is_number()) {
                wall_height = found->get<double>();
                break;
            }
        }
        for (const auto* key : {"elevation_m", "elevation"}) {
            const auto found = host->second.properties.find(key);
            if (found != host->second.properties.end() && found->is_number()) {
                wall_elevation = found->get<double>();
                break;
            }
        }
        // A void feature must not outlive an unsupported host body: IFC openings
        // require a host relationship, so retain both as native references.
        if (!std::isfinite(wall_height) || wall_height <= kTolerance ||
            std::abs(host->second.properties.value("slope_rise_m", 0.0)) > kTolerance ||
            !host->second.properties.contains("height_m") ||
            !host->second.properties.contains("thickness_m")) {
            add_diagnostic(diagnostics, entity.id, type, "opening_host_body_not_representable");
            return;
        }
        const auto length = std::hypot(baseline->end.x - baseline->start.x,
                                       baseline->end.y - baseline->start.y);
        if (!std::isfinite(offset) || !std::isfinite(width) || !std::isfinite(sill) ||
            !std::isfinite(height) || !std::isfinite(wall_thickness) ||
            !std::isfinite(length) || !(length > kTolerance) ||
            !(offset >= -kTolerance) || !(width > kTolerance) || !(sill >= -kTolerance) ||
            !(height > kTolerance) || !(wall_thickness > kTolerance) ||
            offset + width > length + kTolerance) {
            add_diagnostic(diagnostics, entity.id, type,
                           "opening_dimensions_not_representable");
            return;
        }
        if (std::isfinite(wall_height) && wall_height > kTolerance &&
            sill + height > wall_height + kTolerance) {
            add_diagnostic(diagnostics, entity.id, type,
                           "opening_exceeds_host_height");
            return;
        }
        const Vec2 tangent{(baseline->end.x - baseline->start.x) / length,
                           (baseline->end.y - baseline->start.y) / length};
        const Vec2 normal{-tangent.y, tangent.x};
        const auto at = [&](double along, double across) {
            return Vec2{baseline->start.x + tangent.x * along + normal.x * across,
                        baseline->start.y + tangent.y * along + normal.y * across};
        };
        const auto half = wall_thickness * 0.5;
        const auto first = at(offset, -half);
        const auto second = at(offset + width, -half);
        const auto third = at(offset + width, half);
        const auto fourth = at(offset, half);
        boundary = {{first, second, 0.0}, {second, third, 0.0},
                    {third, fourth, 0.0}, {fourth, first, 0.0}};
        product_type = "IFCOPENINGELEMENT";
        depth = height;
        use_solid = true;
        local_elevation = wall_elevation + sill;
        context.opening_host_links.emplace_back(entity.id, host_id);
        if (entity.properties.contains("opening_assembly")) {
            add_diagnostic(diagnostics, entity.id, type,
                           "opening_assembly_not_exported");
        }
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
    const auto polyline_points = [&] {
        std::vector<int> ids;
        ids.reserve(points.size());
        for (const auto point : points) {
            const auto id = context.builder.add("IFCCARTESIANPOINT",
                "(" + real_text(point.x) + "," + real_text(point.y) +
                (use_solid && closed ? ")" : ",0.)"));
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
            ref(context.representation_context) + ",'Body','SweptSolid',(" + ref(solid) + ")");
    } else {
        shape = context.builder.add("IFCSHAPEREPRESENTATION",
            ref(context.representation_context) + ",'Footprint','Curve3D',(" + ref(polyline_points) + ")");
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
    std::string placement = ref(context.placement);
    if (std::abs(local_elevation) > kTolerance) {
        const auto location = context.builder.add("IFCCARTESIANPOINT",
            "(0.,0.," + real_text(local_elevation) + ")");
        const auto axis = context.builder.add("IFCAXIS2PLACEMENT3D",
            ref(location) + ",$,$");
        const auto local = context.builder.add("IFCLOCALPLACEMENT",
            "$," + ref(axis));
        placement = ref(local);
    }
    int product_id{};
    if (product_type == "IFCWALL") {
        product_id = context.builder.add(product_type,
            global_id + "," + ref(context.owner_history) + "," + name + "," +
            description + ",$," + placement + "," + ref(product_shape) + ",$,.NOTDEFINED.");
    } else if (product_type == "IFCSLAB") {
        product_id = context.builder.add(product_type,
            global_id + "," + ref(context.owner_history) + "," + name + "," +
            description + ",$," + placement + "," + ref(product_shape) + ",$,.FLOOR.");
    } else if (product_type == "IFCOPENINGELEMENT") {
        product_id = context.builder.add(product_type,
            global_id + "," + ref(context.owner_history) + "," + name + "," +
            description + ",$," + placement + "," + ref(product_shape) + ",$,.OPENING.");
    } else {
        product_id = context.builder.add(product_type,
            global_id + "," + ref(context.owner_history) + "," + name + "," +
            description + ",$," + placement + "," + ref(product_shape) + ",$,.NOTDEFINED.");
    }
    if (product_id > 0) context.product_ids[entity.id] = product_id;
    if (type != "opening") context.contained_products.push_back(product_id);
    if (type == "wall") export_wall_construction(entity, product_id, context, diagnostics);
    if (type == "wall" || type == "slab" || type == "opening") {
        retain_properties(entity, product_id, context, diagnostics);
    }
}

struct GeometryResult {
    std::optional<Boundary> boundary;
    std::optional<double> depth;
    Vec2 translation{};
    bool has_translation{};
    bool rotated{};
    bool reliable{true};
    double elevation{};
};

// Only translation-only placements are mapped. Parent placements are composed
// explicitly; representation contexts must never overwrite a product placement.
Point3 placement_translation(const ParsedStep& parsed, int id, std::set<int>& visited,
                             bool& unsupported, std::size_t& count,
                             const IfcExchangeLimits& limits) {
    require(visited.size() < 128 && visited.insert(id).second);
    const auto* record = find_record(parsed, id);
    require(record != nullptr);
    const auto fields = split_top_level(record->args, count, limits);
    if (record->type == "IFCLOCALPLACEMENT") {
        require(fields.size() == 2);
        Point3 parent;
        if (fields[0] != "$") {
            const auto parent_id = reference(fields[0]);
            require(parent_id.has_value());
            parent = placement_translation(parsed, *parent_id, visited, unsupported, count, limits);
        }
        const auto axis = reference(fields[1]);
        require(axis.has_value());
        const auto local = placement_translation(parsed, *axis, visited, unsupported, count, limits);
        return {parent.x + local.x, parent.y + local.y, parent.z + local.z};
    }
    require(record->type == "IFCAXIS2PLACEMENT3D" && fields.size() == 3);
    if (fields[1] != "$" || fields[2] != "$") unsupported = true;
    const auto location = reference(fields[0]);
    require(location.has_value());
    const auto* point = find_record(parsed, *location);
    require(point != nullptr);
    return point_record(*point, count, limits);
}

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
        require(std::abs(value.z) <= kTolerance);
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
    const auto product_fields = split_top_level(product.args, argument_count, limits);
    require(product_fields.size() >= 7);
    if (product_fields[5] != "$") {
        const auto placement = reference(product_fields[5]);
        require(placement.has_value());
        std::set<int> placements;
        const auto value = placement_translation(parsed, *placement, placements, result.rotated,
                                                 argument_count, limits);
        result.translation = {value.x, value.y};
        result.elevation = value.z;
        result.has_translation = true;
    }
    if (const auto shape = reference(product_fields[6])) pending.push(*shape);
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
                result.reliable = false;
                add_diagnostic(diagnostics, "#" + std::to_string(record->id), record->type,
                              "polyline_not_reconstructable");
            }
        } else if (record->type == "IFCEXTRUDEDAREASOLID") {
            try {
                const auto fields = split_top_level(record->args, argument_count, limits);
                require(fields.size() == 4);
                const auto candidate = number<double>(fields[3]);
                if (result.depth || candidate <= kTolerance) result.reliable = false;
                if (candidate > kTolerance) result.depth = candidate;
                const auto direction_id = reference(fields[2]);
                require(direction_id.has_value());
                const auto* direction = find_record(parsed, *direction_id);
                require(direction && direction->type == "IFCDIRECTION");
                const auto direction_fields = split_top_level(direction->args, argument_count, limits);
                require(direction_fields.size() == 1);
                const auto components = split_top_level(inner_list(direction_fields[0]), argument_count, limits);
                require(components.size() == 3);
                if (std::abs(number<double>(components[0])) > kTolerance ||
                    std::abs(number<double>(components[1])) > kTolerance ||
                    number<double>(components[2]) <= kTolerance) result.reliable = false;
            } catch (...) {
                result.reliable = false;
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
                    result.translation.x += value.x;
                    result.translation.y += value.y;
                    result.elevation += value.z;
                    result.has_translation = true;
                }
                if (fields[1] != "$" || fields[2] != "$") result.rotated = true;
            } catch (...) {
                result.reliable = false;
                add_diagnostic(diagnostics, "#" + std::to_string(record->id), record->type,
                              "placement_not_reconstructable");
            }
        } else if (record->type == "IFCINDEXEDPOLYCURVE") {
            result.reliable = false;
            add_diagnostic(diagnostics, "#" + std::to_string(record->id), record->type,
                          "indexed_curve_not_reconstructable");
        }
        // Contexts and profile metadata are not geometry or product placement.
        if (record->type == "IFCPRODUCTDEFINITIONSHAPE") {
            const auto fields = split_top_level(record->args, argument_count, limits);
            require(fields.size() == 3);
            const auto items = list_references(fields[2], argument_count, limits);
            if (items.size() != 1) result.reliable = false;
            for (const auto child : items) pending.push(child);
        } else if (record->type == "IFCSHAPEREPRESENTATION") {
            const auto fields = split_top_level(record->args, argument_count, limits);
            require(fields.size() == 4);
            const auto items = list_references(fields[3], argument_count, limits);
            if (items.size() != 1) result.reliable = false;
            for (const auto child : items) pending.push(child);
        } else {
            if (record->type != "IFCPOLYLINE" && record->type != "IFCPOLYLOOP" &&
                record->type != "IFCEXTRUDEDAREASOLID" && record->type != "IFCAXIS2PLACEMENT3D" &&
                record->type != "IFCCARTESIANPOINT" && record->type != "IFCDIRECTION" &&
                record->type != "IFCARBITRARYCLOSEDPROFILEDEF") result.reliable = false;
            for (const auto child : references(record->args)) pending.push(child);
        }
    }
    if (!result.reliable)
        add_diagnostic(diagnostics, "#" + std::to_string(product.id), product.type,
                       "geometry_semantics_not_reconstructed");
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

std::map<int, Json> vertex_properties(const ParsedStep& parsed, std::size_t& count,
                                       const IfcExchangeLimits& limits, std::set<int>& retained_records) {
    std::map<int, Json> result;
    for (const auto& relation : parsed.records) {
        if (relation.type != "IFCRELDEFINESBYPROPERTIES") continue;
        const auto fields = split_top_level(relation.args, count, limits);
        require(fields.size() == 6);
        const auto pset_id = reference(fields[5]);
        if (!pset_id) continue;
        const auto* pset = find_record(parsed, *pset_id);
        require(pset != nullptr);
        if (pset->type != "IFCPROPERTYSET") continue;
        const auto pset_fields = split_top_level(pset->args, count, limits);
        require(pset_fields.size() == 5);
        if (decode_string(pset_fields[2], limits) != "Pset_VertexExchange_v1") continue;
        const auto properties = list_references(pset_fields[4], count, limits);
        require(properties.size() == 1);
        const auto* property = find_record(parsed, properties.front());
        require(property && property->type == "IFCPROPERTYSINGLEVALUE");
        const auto property_fields = split_top_level(property->args, count, limits);
        require(property_fields.size() == 4 && decode_string(property_fields[0], limits) == "Properties");
        const auto value = trim(property_fields[2]);
        require(value.starts_with("IFCTEXT(") && value.back() == ')');
        const auto payload = decode_string(value.substr(8, value.size() - 9), limits);
        // Bound JSON nesting independently of the STEP byte limits.
        const auto metadata = Json::parse(payload, [&](int depth, Json::parse_event_t, Json&) {
            require(depth <= 32); return true;
        }, false);
        require(metadata.is_object());
        for (const auto id : list_references(fields[4], count, limits)) {
            require(find_record(parsed, id) && result.emplace(id, metadata).second);
        }
        retained_records.insert(relation.id);
        retained_records.insert(pset->id);
        retained_records.insert(property->id);
    }
    return result;
}

std::optional<double> positive_property(const Json& metadata, const char* key) {
    const auto found = metadata.find(key);
    if (found == metadata.end() || !found->is_number()) return std::nullopt;
    const auto value = found->get<double>();
    if (!std::isfinite(value) || value <= kTolerance || value > 1e12) return std::nullopt;
    return value;
}

std::optional<Segment> reconstructed_wall_axis(const GeometryResult& geometry, const Json& metadata) {
    if (!geometry.boundary) return std::nullopt;
    // Keep compatibility with the older axis-only exchange subset.
    if (geometry.boundary->size() == 1 && !geometry.depth)
        return geometry.boundary->front();
    const auto thickness = positive_property(metadata, "thickness_m");
    const auto height = positive_property(metadata, "height_m");
    if (!thickness || !height || !geometry.depth || geometry.boundary->size() != 4 ||
        std::abs(*geometry.depth - *height) > kTolerance || !metadata.contains("baseline")) return std::nullopt;
    auto axis = read_segment(metadata.at("baseline"));
    if (!axis || std::abs(axis->sweep_radians) > kTolerance ||
        same_point(axis->start, axis->end)) return std::nullopt;
    // Native payload coordinates are product-local. Apply only the placement
    // translation accepted by the geometry decoder, then check all corners.
    axis->start.x += geometry.translation.x; axis->end.x += geometry.translation.x;
    axis->start.y += geometry.translation.y; axis->end.y += geometry.translation.y;
    const auto length = std::hypot(axis->end.x - axis->start.x, axis->end.y - axis->start.y);
    const Vec2 n{-(axis->end.y - axis->start.y) / length * *thickness / 2,
                 (axis->end.x - axis->start.x) / length * *thickness / 2};
    const Vec2 corners[] = {{axis->start.x - n.x, axis->start.y - n.y},
                           {axis->end.x - n.x, axis->end.y - n.y},
                           {axis->end.x + n.x, axis->end.y + n.y},
                           {axis->start.x + n.x, axis->start.y + n.y}};
    for (std::size_t i = 0; i < 4; ++i)
        if (!same_point((*geometry.boundary)[i].start, corners[i]) ||
            !same_point((*geometry.boundary)[i].end, corners[(i + 1) % 4])) return std::nullopt;
    return axis;
}

bool metre_units(const ParsedStep& parsed, std::size_t& count, const IfcExchangeLimits& limits) {
    bool found = false;
    for (const auto& record : parsed.records) {
        if (record.type == "IFCCONVERSIONBASEDUNIT") return false;
        if (record.type != "IFCSIUNIT") continue;
        const auto fields = split_top_level(record.args, count, limits);
        require(fields.size() == 4);
        if (upper(fields[1]) != ".LENGTHUNIT.") continue;
        if (found || fields[2] != "$" || upper(fields[3]) != ".METRE.") return false;
        found = true;
    }
    return found;
}

} // namespace

IfcProjectExportResult export_project_ifc(const DocumentSnapshot& document,
                                          const IfcExchangeLimits& limits) {
    validate_limits(limits);
    IfcProjectExportResult result;
    ExportContext context(limits);
    for (const auto& [id, entity] : document.entities()) {
        export_product(document, entity, context, result.diagnostics);
        if (!context.product_ids.contains(id) &&
            (entity.required || (!result.diagnostics.empty() && result.diagnostics.back().source_id == id) ||
             entity.type == "wall" || entity.type == "slab" ||
             entity.type == "opening" || entity.type == "roof" || entity.type == "room" ||
             entity.type == "ifc_reference" ||
             entity.type == "building" || entity.type == "floor" || entity.type == "property"))
            export_native_reference(entity, context, result.diagnostics);
    }
    if (!context.contained_products.empty()) {
        std::string products;
        for (const auto id : context.contained_products) {
            if (!products.empty()) products += ',';
            products += ref(id);
        }
        context.builder.add("IFCRELCONTAINEDINSPATIALSTRUCTURE",
            context.root("containment", "") + ",(" + products + ")," + ref(context.storey));
    }
    for (const auto& [opening_id, host_id] : context.opening_host_links) {
        const auto opening = context.product_ids.find(opening_id);
        const auto host = context.product_ids.find(host_id);
        if (opening == context.product_ids.end() || host == context.product_ids.end()) {
            add_diagnostic(result.diagnostics, opening_id, "opening",
                           "opening_host_relationship_not_exported");
            continue;
        }
        const auto global_id = step_string(
            guid_for("void:" + opening_id + ":" + host_id, ++context.ordinal), limits);
        context.builder.add("IFCRELVOIDSELEMENT",
            global_id + "," + ref(context.owner_history) + ",$,$," +
            ref(host->second) + "," + ref(opening->second));
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
    std::set<int> retained_metadata_records;
    const auto metadata_by_id = vertex_properties(parsed, argument_count, limits, retained_metadata_records);
    const auto supported_units = metre_units(parsed, argument_count, limits);
    if (!supported_units) add_diagnostic(result.diagnostics, {}, "PROJECT", "length_units_not_reconstructed");
    for (const auto& record : parsed.records) {
        if (is_product(record.type)) {
            const auto geometry = geometry_for(parsed, record, result.diagnostics,
                                               argument_count, limits);
            if (!geometry.boundary) {
                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                              "product_geometry_missing");
                if (const auto metadata = metadata_by_id.find(record.id); metadata != metadata_by_id.end()) {
                    result.entities.push_back(Entity{"ifc-" + std::to_string(record.id), "ifc_reference",
                        Json{{"ifc_name", product_string(record, 2, argument_count, limits)},
                             {"ifc_type", record.type}}, false,
                        Json{{"ifc_source", {{"record_id", record.id}, {"record_type", record.type},
                                             {"arguments", record.args}}},
                             {"ifc_vertex_properties", metadata->second}}});
                }
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
            Json properties{{"boundary", boundary_json(*geometry.boundary)},
                             {"classification", classification}, {"ifc_type", record.type}};
            if (!name.empty()) properties["ifc_name"] = name;
            if (!description.empty()) properties["ifc_description"] = description;
            if (geometry.depth) properties["ifc_extrusion_depth_m"] = *geometry.depth;
            properties["elevation_m"] = geometry.elevation;
            std::string entity_type = "boundary";
            const auto metadata_entry = metadata_by_id.find(record.id);
            const auto metadata = metadata_entry == metadata_by_id.end() ? Json::object() : metadata_entry->second;
            if (supported_units && !geometry.rotated && geometry.reliable && classification == "ifc_wall_axis") {
                const auto thickness = positive_property(metadata, "thickness_m");
                const auto height = positive_property(metadata, "height_m");
                const auto axis = reconstructed_wall_axis(geometry, metadata);
                if (thickness && height && axis && !same_point(axis->start, axis->end)) {
                    entity_type = "wall";
                    properties["baseline"] = boundary_json(Boundary{*axis})[0];
                    properties["thickness_m"] = *thickness;
                    properties["height_m"] = *height;
                    if (metadata.contains("layers")) {
                        try {
                            auto layers = parse_wall_layers(metadata.at("layers"), *thickness);
                            bool retained_materials = false;
                            for (auto& layer : layers) {
                                retained_materials = retained_materials || layer.material.has_value();
                                layer.material.reset();
                            }
                            properties["layers"] = wall_layers_json(layers);
                            if (retained_materials)
                                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                                               "wall_material_references_retained");
                        } catch (const std::exception&) {
                            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                                           "wall_layers_not_reconstructed");
                        }
                    }
                } else {
                    add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                                   "wall_dimensions_not_reconstructed");
                }
            } else if (supported_units && !geometry.rotated && geometry.reliable && classification == "ifc_slab" && geometry.depth &&
                       geometry.boundary->size() >= 3 &&
                       same_point(geometry.boundary->back().end, geometry.boundary->front().start)) {
                const auto fields = split_top_level(record.args, argument_count, limits);
                const auto kind = fields.size() > 8 ? upper(fields[8]) : "$";
                if (kind == ".FLOOR." || kind == ".BASESLAB.") {
                    entity_type = "slab";
                    properties["thickness_m"] = *geometry.depth;
                    properties["holes"] = Json::array();
                    properties["element_kind"] = kind == ".BASESLAB." ? "foundation" : "floor";
                    if (metadata.contains("element_kind") && metadata["element_kind"].is_string()) {
                        const auto native_kind = metadata["element_kind"].get<std::string>();
                        if (native_kind == "slab" || native_kind == "floor" || native_kind == "ceiling" || native_kind == "foundation")
                            properties["element_kind"] = native_kind;
                    }
                } else {
                    add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                                   "slab_kind_not_reconstructed");
                }
            }
            const auto id = "ifc-" + std::to_string(record.id);
            result.entities.push_back(Entity{id, entity_type, std::move(properties), false,
                Json{{"ifc_source", {{"record_id", record.id}, {"record_type", record.type},
                                     {"arguments", record.args}}}}});
            if (metadata_entry != metadata_by_id.end()) {
                result.entities.back().extensions["ifc_vertex_properties"] = metadata;
                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                               "vertex_properties_partially_reconstructed");
            }
            continue;
        }
        if (retained_metadata_records.contains(record.id)) continue;
        if (is_relationship(record.type) && record.type != "IFCRELVOIDSELEMENT") {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "relationship_not_reconstructed");
        } else if (record.type == "IFCPROPERTYSET" || record.type == "IFCPROPERTYSINGLEVALUE" ||
                   record.type == "IFCMATERIAL" || record.type == "IFCMATERIALLAYER" ||
                   record.type == "IFCMATERIALLAYERSET" || record.type == "IFCINDEXEDPOLYCURVE") {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "property_or_geometry_not_reconstructed");
        } else if (!is_structural(record.type) && record.type != "IFCRELVOIDSELEMENT") {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "unsupported_entity");
        }
    }
    std::map<std::string, std::vector<std::pair<std::string, int>>> hosts;
    for (const auto& record : parsed.records) {
        if (record.type != "IFCRELVOIDSELEMENT") continue;
        const auto fields = split_top_level(record.args, argument_count, limits);
        require(fields.size() == 6);
        const auto host = reference(fields[4]);
        const auto opening = reference(fields[5]);
        require(host && opening && find_record(parsed, *host) && find_record(parsed, *opening));
        hosts["ifc-" + std::to_string(*opening)].push_back({"ifc-" + std::to_string(*host), record.id});
    }
    std::set<int> reconstructed_relations;
    for (auto& opening : result.entities) {
        if (opening.properties.value("classification", "") != "ifc_opening") continue;
        const auto relation = hosts.find(opening.id);
        const Entity* host = nullptr;
        if (relation != hosts.end() && relation->second.size() == 1) {
            const auto found = std::find_if(result.entities.begin(), result.entities.end(), [&](const auto& entity) {
                return entity.id == relation->second.front().first && entity.type == "wall";
            });
            if (found != result.entities.end()) host = &*found;
        }
        const auto footprint = read_boundary(opening);
        const auto baseline = host ? read_baseline(*host) : std::nullopt;
        const auto rotated = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
            return diagnostic.source_id == "#" + std::to_string(opening.extensions["ifc_source"]["record_id"].get<int>()) &&
                   (diagnostic.code == "placement_rotation_unsupported" ||
                    diagnostic.code == "geometry_semantics_not_reconstructed");
        });
        bool recovered = false;
        if (baseline && footprint && footprint->size() == 4 && !rotated &&
            opening.properties.contains("ifc_extrusion_depth_m")) {
            const auto length = std::hypot(baseline->end.x - baseline->start.x, baseline->end.y - baseline->start.y);
            const Vec2 tangent{(baseline->end.x - baseline->start.x) / length,
                               (baseline->end.y - baseline->start.y) / length};
            double low = std::numeric_limits<double>::infinity(), high = -low;
            double across_low = low, across_high = high;
            for (const auto& segment : *footprint) {
                const auto dx = segment.start.x - baseline->start.x;
                const auto dy = segment.start.y - baseline->start.y;
                const auto along = dx * tangent.x + dy * tangent.y;
                const auto across = -dx * tangent.y + dy * tangent.x;
                low = std::min(low, along); high = std::max(high, along);
                across_low = std::min(across_low, across); across_high = std::max(across_high, across);
            }
            bool rectangle = same_point(footprint->back().end, footprint->front().start);
            std::set<std::pair<int, int>> corners;
            for (const auto& segment : *footprint) {
                const auto px = segment.start.x - baseline->start.x;
                const auto py = segment.start.y - baseline->start.y;
                const auto u = px * tangent.x + py * tangent.y;
                const auto v = -px * tangent.y + py * tangent.x;
                const auto u_side = std::abs(u - low) <= kTolerance ? 0 : std::abs(u - high) <= kTolerance ? 1 : -1;
                const auto v_side = std::abs(v - across_low) <= kTolerance ? 0 : std::abs(v - across_high) <= kTolerance ? 1 : -1;
                rectangle = rectangle && u_side >= 0 && v_side >= 0 && corners.emplace(u_side, v_side).second;
                const auto dx = segment.end.x - segment.start.x;
                const auto dy = segment.end.y - segment.start.y;
                const auto along = dx * tangent.x + dy * tangent.y;
                const auto across = -dx * tangent.y + dy * tangent.x;
                rectangle = rectangle && (std::abs(along) <= kTolerance || std::abs(across) <= kTolerance) &&
                            std::hypot(dx, dy) > kTolerance;
            }
            const auto sill = opening.properties["elevation_m"].get<double>() - host->properties["elevation_m"].get<double>();
            if (rectangle && low >= -kTolerance && high <= length + kTolerance && high - low > kTolerance &&
                across_high - across_low > kTolerance && std::abs(across_low + across_high) <= kTolerance && sill >= -kTolerance &&
                std::abs(across_high - across_low - host->properties["thickness_m"].get<double>()) <= kTolerance &&
                sill + opening.properties["ifc_extrusion_depth_m"].get<double>() <= host->properties["height_m"].get<double>() + kTolerance) {
                opening.type = "opening";
                opening.properties["wall_id"] = host->id;
                opening.properties["offset_m"] = std::max(0.0, low);
                opening.properties["width_m"] = high - low;
                opening.properties["sill_m"] = std::max(0.0, sill);
                opening.properties["height_m"] = opening.properties["ifc_extrusion_depth_m"];
                const auto kind = opening.properties["ifc_type"].get<std::string>();
                opening.properties["opening_kind"] = kind == "IFCDOOR" ? "door" : kind == "IFCWINDOW" ? "window" : "opening";
                if (opening.extensions.contains("ifc_vertex_properties")) {
                    const auto& metadata = opening.extensions["ifc_vertex_properties"];
                    if (metadata.contains("opening_kind") && metadata["opening_kind"].is_string() &&
                        (metadata["opening_kind"] == "door" || metadata["opening_kind"] == "window"))
                        opening.properties["opening_kind"] = metadata["opening_kind"];
                }
                reconstructed_relations.insert(relation->second.front().second);
                recovered = true;
            }
        }
        if (!recovered) add_diagnostic(result.diagnostics,
            "#" + std::to_string(opening.extensions["ifc_source"]["record_id"].get<int>()),
            opening.properties["ifc_type"].get<std::string>(), "opening_host_unbound");
    }
    for (const auto& record : parsed.records) {
        if (record.type == "IFCRELVOIDSELEMENT" && !reconstructed_relations.contains(record.id))
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                           "relationship_not_reconstructed");
    }
    result.source_retention_required = !result.diagnostics.empty();
    return result;
}

} // namespace sketch
