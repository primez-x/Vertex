#include "sketch/ifc_project_exchange.hpp"
#include "sketch/physical_wall_room.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_import_worker.hpp"
#include "sketch/ifc_native_geometry.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/stair_semantics.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/document_wall.hpp"

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

constexpr std::size_t kMaximumMetadataDepth = 32;
constexpr std::size_t kMaximumRetainedMetadataCharge = 64 * 1024 * 1024;

std::size_t metadata_charge(const Json& metadata) {
    require(metadata.is_object());
    std::size_t charge = 0;
    const auto add_charge = [&](std::size_t bytes) {
        require(bytes <= kMaximumRetainedMetadataCharge - charge);
        charge += bytes;
    };
    const auto visit = [&](const auto& self, const Json& value, std::size_t depth) -> void {
        require(depth <= kMaximumMetadataDepth);
        add_charge(128);
        if (value.is_string()) add_charge(value.get_ref<const std::string&>().size());
        else if (value.is_object()) for (auto i = value.begin(); i != value.end(); ++i) {
            add_charge(i.key().size() + 128);
            self(self, i.value(), depth + 1);
        }
        else if (value.is_array()) for (const auto& member : value) self(self, member, depth + 1);
    };
    visit(visit, metadata, 0);
    return charge;
}

void validate_limits(const IfcExchangeLimits& limits) {
    const IfcExchangeLimits cap;
    require(limits.max_bytes > 0 && limits.max_bytes <= 256 * 1024 * 1024);
    require(limits.max_records > 0 && limits.max_records <= 500'000);
    require(limits.max_arguments > 0 && limits.max_arguments <= 4'000'000);
    require(limits.max_string_bytes > 0 && limits.max_string_bytes <= 65'536);
    require(limits.max_mesh_vertices > 0 && limits.max_mesh_vertices <= 1'000'000);
    require(limits.max_mesh_triangles > 0 && limits.max_mesh_triangles <= 2'000'000);
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
        if (c == '(') { ++depth; require(depth <= 128); }
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
        auto field_limits = limits;
        // Aggregate coordinate/index lists are bounded by file and mesh budgets;
        // max_string_bytes remains the limit for decoded STEP string values.
        if (item.front() == '(') field_limits.max_string_bytes = limits.max_bytes;
        printable(item, field_limits);
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
    // STEP distinguishes INTEGER from REAL lexically, including defined length
    // measures. to_chars emits whole-valued doubles without a decimal point.
    if (text.find_first_of(".eE") == std::string::npos) text += '.';
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
    bool coordinate_operations{};
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
        const auto& added_type = result.records.back().type;
        result.coordinate_operations = result.coordinate_operations ||
            added_type == "IFCCOORDINATEOPERATION" || added_type == "IFCMAPCONVERSION" ||
            added_type == "IFCRIGIDOPERATION";
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

// Supported native mesh placement subset: a proper rigid frame with global Z
// up. Its derived Y is Z cross X, never a reflection or an independent scale.
struct RigidFrame {
    Point3 origin;
    Vec2 x{1.0, 0.0};
};

Point3 world_point(const RigidFrame& frame, Point3 local) {
    return {frame.origin.x + frame.x.x * local.x - frame.x.y * local.y,
            frame.origin.y + frame.x.y * local.x + frame.x.x * local.y,
            frame.origin.z + local.z};
}

bool bounded_point(Point3 point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) &&
        std::abs(point.x) <= 1e12 && std::abs(point.y) <= 1e12 && std::abs(point.z) <= 1e12;
}

bool same_frame(const RigidFrame& left, const RigidFrame& right) {
    return std::abs(left.origin.x-right.origin.x) <= kTolerance &&
        std::abs(left.origin.y-right.origin.y) <= kTolerance &&
        std::abs(left.origin.z-right.origin.z) <= kTolerance &&
        std::hypot(left.x.x-right.x.x, left.x.y-right.x.y) <= kTolerance;
}

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
    explicit StepBuilder(const IfcExchangeLimits& limits) : limits_(limits) {
        serialized_bytes_ = finish(false).size();
    }

    int add(std::string type, std::string args) {
        require(records_.size() < limits_.max_records);
        const auto bytes = 6 + std::to_string(next_id_).size() + type.size() + args.size();
        if (bounded_add_) require(serialized_bytes_ <= limits_.max_bytes && bytes <= limits_.max_bytes - serialized_bytes_);
        records_.push_back({next_id_++, std::move(type), std::move(args)});
        serialized_bytes_ += bytes;
        return records_.back().id;
    }

    struct Checkpoint { std::size_t records; int next_id; std::size_t bytes; bool bounded; };
    [[nodiscard]] Checkpoint checkpoint() const { return {records_.size(), next_id_, serialized_bytes_, bounded_add_}; }
    void rollback(Checkpoint point) { records_.resize(point.records); next_id_ = point.next_id; serialized_bytes_ = point.bytes; bounded_add_ = point.bounded; }
    void bounded_add(bool bounded) { bounded_add_ = bounded; }

    std::string finish(bool bounded = true) const {
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
        if (bounded) require(result.size() <= limits_.max_bytes);
        return result;
    }

private:
    const IfcExchangeLimits& limits_;
    int next_id_{1};
    std::size_t serialized_bytes_{};
    bool bounded_add_{};
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
    int project{};
    int world_placement{};
    int storey{};
    int default_storey{};
    std::map<int, std::vector<int>> contained_products;
    std::map<std::string,int,std::less<>> spatial_ids;
    std::map<std::string,int,std::less<>> spatial_placements;
    std::map<std::string,int,std::less<>> material_ids;
    SitePresentationPlacement presentation;
    std::map<std::string,SitePresentationPlacement,std::less<>> site_placements;
    std::map<std::string,AssemblyExpansion,std::less<>> assembly_expansions;
    std::map<std::string,std::vector<const Entity*>,std::less<>> hosted_openings;
    const Entity* authored_entity{};
    std::size_t ordinal{};
    std::map<std::string, int, std::less<>> product_ids;
    std::vector<std::pair<std::string, std::string>> opening_host_links;
    std::vector<std::pair<std::string, std::string>> railing_host_links;
    std::set<std::string, std::less<>> fresh_stair_proofs;
    project_import_detail::GeometryBudget native_work;
    project_import_detail::GeometryBudget join_work{0, 0, "ifc_native_join_work_budget_exceeded"};
    std::size_t mesh_vertices{};
    std::size_t mesh_triangles{};
    std::size_t retained_metadata_charge{};

    explicit ExportContext(const IfcExchangeLimits& limits, bool authored_spatial) : limits(limits), builder(limits) {
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
        world_placement = placement;
        representation_context = builder.add("IFCGEOMETRICREPRESENTATIONCONTEXT",
            "$,'Model',3,0.0000001," + ref(axis_placement) + ",$");
        project = builder.add("IFCPROJECT", root("project", "Vertex project") +
            ",$,$,$,(" + ref(representation_context) + ")," + ref(units));
        if (!authored_spatial) default_hierarchy();
    }

    void default_hierarchy() {
        if (default_storey) { storey=default_storey; return; }
        const auto site = builder.add("IFCSITE", root("site", "Default site") +
            ",$," + ref(world_placement) + ",$,$,.ELEMENT.,$,$,$,$,$");
        const auto building = builder.add("IFCBUILDING", root("building", "Default building") +
            ",$," + ref(world_placement) + ",$,$,.ELEMENT.,$,$,$");
        storey = builder.add("IFCBUILDINGSTOREY", root("storey", "Default storey") +
            ",$," + ref(world_placement) + ",$,$,.ELEMENT.,0.");
        default_storey=storey;
        aggregate(project, site, "project-site");
        aggregate(site, building, "site-building");
        aggregate(building, storey, "building-storey");
    }

    void contain(int product) { require(storey>0); contained_products[storey].push_back(product); }

    int rigid_placement(const SiteRigidTransform& pose, int parent=0) {
        const auto& t=pose.translation_m;
        const auto point=builder.add("IFCCARTESIANPOINT","("+real_text(t.x)+","+real_text(t.y)+","+real_text(t.z)+")");
        const auto x=builder.add("IFCDIRECTION","("+real_text(std::cos(pose.rotation_radians))+","+
            real_text(std::sin(pose.rotation_radians))+",0.)");
        const auto axis=builder.add("IFCAXIS2PLACEMENT3D",ref(point)+","+ref(z_direction)+","+ref(x));
        return builder.add("IFCLOCALPLACEMENT",(parent ? ref(parent) : "$")+","+ref(axis));
    }

    std::string root(std::string_view id, std::string_view name) {
        return step_string(guid_for(id, ++ordinal), limits) + "," + ref(owner_history) +
            "," + step_string(name, limits) + ",$";
    }

    void aggregate(int parent, int child, std::string_view id) {
        builder.add("IFCRELAGGREGATES", root(id, "") + "," + ref(parent) + ",(" + ref(child) + ")");
    }
};

void retain_properties(const Entity& input, int product_id, ExportContext& context,
                       std::vector<IfcProjectDiagnostic>& diagnostics) {
    auto entity=input;
    if (context.authored_entity && input.id==context.authored_entity->id &&
        context.presentation.source_frame.mode!=SiteFrameMode::world && !input.properties.contains("native_entity")) {
        const auto& source=*context.authored_entity;
        entity.properties["_vertex_ifc_site_source"]={{"version",1},{"id",source.id},{"type",source.type},
            {"required",source.required},{"properties",source.properties},{"extensions",source.extensions},
            {"property_id",context.presentation.source_frame.property_id},
            {"building_id",context.presentation.source_frame.building_id},
            {"translation_m",{context.presentation.forward.translation_m.x,context.presentation.forward.translation_m.y,
                context.presentation.forward.translation_m.z}},
            {"rotation_radians",context.presentation.forward.rotation_radians}};
    }
    std::size_t charge = 0;
    try { charge = metadata_charge(entity.properties); }
    catch (const std::invalid_argument&) {
        add_diagnostic(diagnostics, entity.id, entity.type, "vertex_properties_not_exported");
        return;
    }
    if (charge > kMaximumRetainedMetadataCharge - context.retained_metadata_charge) {
        add_diagnostic(diagnostics, entity.id, entity.type, "vertex_properties_not_exported");
        return;
    }
    const auto payload = entity.properties.dump(-1, ' ', true);
    // split_top_level bounds the whole IFCTEXT('...') field. Reserve its
    // eleven bytes and worst-case apostrophe doubling, not just decoded text.
    const auto chunk_size = context.limits.max_string_bytes > 11
        ? (context.limits.max_string_bytes - 11) / 2 : 0;
    if (payload.size() > chunk_size) {
        const auto native=entity.properties.find("native_entity");
        const bool retained_room=native!=entity.properties.end() && native->is_object() &&
            native->value("type",std::string{})=="room_boundary" && native->contains("extensions") &&
            native->at("extensions").is_object() && native->at("extensions").contains("physical_wall_room");
        const auto retained_type = native != entity.properties.end() && native->is_object()
            ? native->value("type", std::string{}) : std::string{};
        // Organization and import receipts are inert source descriptors, not
        // active geometry. Preserve their complete metadata under the same
        // bounded, hashed carrier, including when an ifc_reference reuses it.
        const bool retained_provenance = retained_type == "property" || retained_type == "building" ||
            retained_type == "floor" || retained_type == "annotation_state" || retained_type == "ifc_source";
        if (((entity.type == "room_boundary" && entity.extensions.contains("physical_wall_room")) || retained_room ||
             retained_provenance || entity.type=="assembly_instance" || entity.type=="terrain_surface" ||
             entity.properties.contains("_vertex_ifc_site_source") || entity.properties.contains("_vertex_ifc_join") ||
             ((entity.type == "roof" || entity.type == "room" || entity.type == "stair" || entity.type == "railing") && entity.properties.contains("_vertex_ifc_entity"))) &&
            context.limits.max_string_bytes >= 512 && payload.size() <= 8*1024*1024) {
            const auto chunks = (payload.size() + chunk_size - 1) / chunk_size;
            if (chunks <= 4096) {
                const Json manifest{{"version",1},{"chunks",chunks},{"bytes",payload.size()},
                    {"sha256",sha256_hex(std::as_bytes(std::span(payload.data(),payload.size())))}};
                const auto header = context.builder.add("IFCPROPERTYSINGLEVALUE",
                    "'MetadataManifest',$,IFCTEXT(" + step_string(manifest.dump(),context.limits) + "),$");
                std::string property_ids = ref(header);
                for (std::size_t i = 0; i < chunks; ++i) {
                    const auto property = context.builder.add("IFCPROPERTYSINGLEVALUE",
                        step_string("PropertiesChunk:"+std::to_string(i),context.limits)+",$,IFCTEXT(" +
                        step_string(std::string_view(payload).substr(i*chunk_size,chunk_size),context.limits) + "),$");
                    property_ids += ',' + ref(property);
                }
                const auto pset = context.builder.add("IFCPROPERTYSET",
                    context.root("properties:"+entity.id,"Pset_VertexExchange_v2") + ",("+property_ids+")");
                context.builder.add("IFCRELDEFINESBYPROPERTIES",
                    context.root("property-link:"+entity.id,"") + ",("+ref(product_id)+"),"+ref(pset));
                context.retained_metadata_charge += charge;
                return;
            }
        }
        add_diagnostic(diagnostics, entity.id, entity.type, "vertex_properties_not_exported");
        return;
    }
    const auto property = context.builder.add("IFCPROPERTYSINGLEVALUE",
        "'Properties',$,IFCTEXT(" + step_string(payload, context.limits) + "),$");
    const auto pset = context.builder.add("IFCPROPERTYSET",
        context.root("properties:" + entity.id, "Pset_VertexExchange_v1") + ",(" + ref(property) + ")");
    context.builder.add("IFCRELDEFINESBYPROPERTIES",
        context.root("property-link:" + entity.id, "") + ",(" + ref(product_id) + ")," + ref(pset));
    context.retained_metadata_charge += charge;
}

void export_native_reference(const Entity& entity, ExportContext& context,
                             std::vector<IfcProjectDiagnostic>& diagnostics) {
    const auto product = context.builder.add("IFCBUILDINGELEMENTPROXY",
        context.root("reference:" + entity.id, entity.id) + "," + step_string(entity.type, context.limits) +
        "," + ref(context.placement) + ",$,$,.NOTDEFINED.");
    context.contain(product);
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

void export_spatial_hierarchy(const DocumentSnapshot& document, ExportContext& context,
    std::vector<IfcProjectDiagnostic>& diagnostics) {
    const auto organization=organize_project(document);
    for (const auto kind : {"property","building","floor"})
        for (const auto& [id,e]:document.entities()) {
            if (e.type!=kind) continue;
            try {
                const auto& p=context.site_placements.at(id);
                const auto name=e.properties.value("name",id);
                const auto& node=organization.nodes.at(id);
                require(node.issues.empty());
                int parent=context.project, parent_placement=0;
                SiteRigidTransform pose=p.forward;
                double elevation=0;
                if (e.type!="property") {
                    const auto parent_id=e.type=="building" ? p.drawing_context.property_id : p.drawing_context.building_id;
                    require(context.spatial_ids.contains(parent_id));
                    parent=context.spatial_ids.at(parent_id); parent_placement=context.spatial_placements.at(parent_id);
                    const auto parent_pose=context.site_placements.at(parent_id).forward;
                    pose=compose_site_transforms(inverse_site_transform(parent_pose),p.forward);
                    if (e.type=="floor" && e.properties.contains("vertical_level_binding")) {
                        const auto binding=VerticalLevelBinding::from_json(e.properties.at("vertical_level_binding"));
                        const auto& graph=document.entities().at(binding.graph_entity_id);
                        require(graph.type=="vertical_levels");
                        const auto levels=VerticalLevelGraph::from_json(graph.properties.at("model"));
                        const auto level=std::find_if(levels.levels().begin(),levels.levels().end(),[&](const auto& value){
                            return value.id==binding.level_id;
                        });
                        require(level!=levels.levels().end()); elevation=level->elevation_m;
                        pose.translation_m.z+=elevation;
                    }
                }
                const auto placement=context.rigid_placement(pose,parent_placement);
                const auto product=context.builder.add(e.type=="property" ? "IFCSITE" : e.type=="building" ? "IFCBUILDING" : "IFCBUILDINGSTOREY",
                    context.root("spatial:"+id,name)+",$,"+ref(placement)+",$,$,.ELEMENT.,"+
                    (e.type=="property" ? "$,$,$,$,$" : e.type=="building" ? "$,$,$" : real_text(elevation)));
                context.spatial_ids[id]=product; context.spatial_placements[id]=placement;
                context.aggregate(parent,product,"spatial-parent:"+id);
            } catch (const std::exception&) {
                add_diagnostic(diagnostics,id,e.type,"site_spatial_hierarchy_not_exported");
            }
        }
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

Wall native_wall(const Entity& entity) {
    auto normalized = entity;
    if (!normalized.properties.contains("elevation_m") && !normalized.properties.contains("elevation"))
        normalized.properties["elevation_m"] = 0.0;
    Wall wall;
    std::string error;
    require(read_document_wall(normalized, {}, wall, error));
    // This body carrier remains one wall mesh. The retained layer metadata
    // and material associations are exported separately.
    wall.layers.clear();
    validate_wall_semantics(wall);
    return wall;
}

HostedOpening native_opening(const Entity& entity) {
    return {entity.id, entity.properties.at("offset_m").get<double>(),
        entity.properties.at("width_m").get<double>(), entity.properties.at("sill_m").get<double>(),
        entity.properties.at("height_m").get<double>()};
}

std::optional<DoorOperation> native_operation(const Entity& entity) {
    if (!entity.properties.contains("door_operation")) return std::nullopt;
    return decode_door_operation(entity.properties.at("door_operation"));
}

std::string door_operation_enum(const std::optional<DoorOperation>& operation) {
    if (!operation) return ".USERDEFINED.";
    if (operation->kind == DoorOperationKind::double_hinged) return ".DOUBLE_DOOR_SINGLE_SWING.";
    // This profile has one movable and one fixed panel; DOUBLE_DOOR_SLIDING
    // instead specifies two movable panels. Retain an explicit description.
    if (operation->kind == DoorOperationKind::sliding) return ".USERDEFINED.";
    // IfcDoorTypeOperationEnum defines hinge side while looking along local +Y.
    // fill_frame makes +Y the swing side, reversing +X for native right swings.
    // https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcsharedbldgelements/lexical/ifcdoortypeoperationenum.htm
    return operation->hinge_at_end == operation->swing_left
        ? ".SINGLE_SWING_RIGHT." : ".SINGLE_SWING_LEFT.";
}

std::string window_partition_enum(const OpeningAssembly& profile) {
    // IFC partition describes panel arrangement, independently of panel operation.
    switch (profile.window_layout) {
    case WindowLayoutKind::double_fixed: case WindowLayoutKind::sliding:
        return ".DOUBLE_PANEL_VERTICAL.";
    case WindowLayoutKind::triple_fixed: return ".TRIPLE_PANEL_VERTICAL.";
    case WindowLayoutKind::bay: return ".USERDEFINED.";
    case WindowLayoutKind::fixed: case WindowLayoutKind::casement: return ".SINGLE_PANEL.";
    }
    throw std::invalid_argument("Unknown window layout");
}

std::string window_partition_label(const OpeningAssembly& profile,
                                   const IfcExchangeLimits& limits) {
    // IFC4 permits a partition label only with USERDEFINED. The bay has
    // three projecting facets, rather than three panels in one plane.
    // https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcsharedbldgelements/lexical/ifcwindow.htm
    return profile.window_layout == WindowLayoutKind::bay ? step_string("BAY_WINDOW", limits) : "$";
}

std::string door_operation_label(const std::optional<DoorOperation>& operation,
                                 const IfcExchangeLimits& limits) {
    // IFC4 permits this label only with USERDEFINED. NOTDEFINED would describe
    // a lining with no panel, while this native profile contains a closed leaf.
    // https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcsharedbldgelements/lexical/ifcdoor.htm
    if (operation && operation->kind == DoorOperationKind::sliding)
        return step_string("Two-track sliding door; one fixed panel", limits);
    return operation ? "$" : step_string("Closed leaf; hinge and swing unspecified", limits);
}

#ifdef SKETCH_IFC_NATIVE_GEOMETRY
RigidFrame fill_frame(const Wall& wall, const HostedOpening& opening,
                      const std::optional<DoorOperation>& operation) {
    const auto span = hosted_opening_span(wall.baseline, opening.offset, opening.width);
    const double length = std::hypot(span.end.x-span.start.x, span.end.y-span.start.y);
    require(length > kTolerance && std::isfinite(length));
    const bool reverse = operation && !operation->swing_left;
    const auto origin = reverse ? span.end : span.start;
    const double sign = reverse ? -1.0 : 1.0;
    RigidFrame frame{{origin.x, origin.y, wall.elevation + opening.sill},
        {sign*(span.end.x-span.start.x)/length, sign*(span.end.y-span.start.y)/length}};
    require(bounded_point(frame.origin));
    return frame;
}

double fill_overall_width(const Wall& wall, const HostedOpening& opening,
                          const RigidFrame& frame) {
    // IFC OverallWidth spans the opening body along local X. Native curved
    // opening.width instead measures stations along the directed host arc.
    const auto span = hosted_opening_span(wall.baseline, opening.offset, opening.width);
    auto footprint = wall_plan_footprint(span, {}, wall.thickness);
    const auto local = [&](Vec2 point) -> Vec2 {
        const double dx=point.x-frame.origin.x, dy=point.y-frame.origin.y;
        return {dx*frame.x.x+dy*frame.x.y,-dx*frame.x.y+dy*frame.x.x};
    };
    for (auto& segment : footprint) {
        segment.start=local(segment.start); segment.end=local(segment.end);
        // A proper rigid rotation preserves each directed arc sweep.
    }
    const auto bounds = boundary_bounds(footprint);
    const double width = bounds.maximum.x-bounds.minimum.x;
    require(std::isfinite(width) && width>kTolerance && width<=1e12);
    return width;
}

int fill_placement(const RigidFrame& frame, ExportContext& context) {
    const auto origin = context.builder.add("IFCCARTESIANPOINT", "(" +
        real_text(frame.origin.x) + "," + real_text(frame.origin.y) + "," + real_text(frame.origin.z) + ")");
    const auto x = context.builder.add("IFCDIRECTION", "(" + real_text(frame.x.x) + "," + real_text(frame.x.y) + ",0.)");
    const auto axis = context.builder.add("IFCAXIS2PLACEMENT3D", ref(origin) + "," + ref(context.z_direction) + "," + ref(x));
    return context.builder.add("IFCLOCALPLACEMENT", ref(context.placement) + "," + ref(axis));
}

int mesh_shape(const std::vector<IfcNativeMesh>& meshes, ExportContext& context,
    std::vector<int>* mesh_items = nullptr) {
    std::string items;
    for (const auto& mesh : meshes) {
        require(mesh.vertices.size() <= context.limits.max_mesh_vertices - context.mesh_vertices &&
                mesh.triangles.size() <= context.limits.max_mesh_triangles - context.mesh_triangles);
        context.mesh_vertices += mesh.vertices.size();
        context.mesh_triangles += mesh.triangles.size();
        std::string coordinates = "(";
        for (const auto& point : mesh.vertices) {
            if (coordinates.size() > 1) coordinates += ',';
            coordinates += "(" + real_text(point[0]) + "," + real_text(point[1]) + "," + real_text(point[2]) + ")";
        }
        coordinates += ')';
        const auto points = context.builder.add("IFCCARTESIANPOINTLIST3D", std::move(coordinates));
        std::string indices = "(";
        for (const auto& triangle : mesh.triangles) {
            if (indices.size() > 1) indices += ',';
            indices += "(" + std::to_string(triangle[0] + 1) + "," +
                std::to_string(triangle[1] + 1) + "," + std::to_string(triangle[2] + 1) + ")";
        }
        indices += ')';
        const auto item = context.builder.add("IFCTRIANGULATEDFACESET",
            ref(points) + ",$,.T.," + indices + ",$");
        if (mesh_items) mesh_items->push_back(item);
        if (!items.empty()) items += ',';
        items += ref(item);
    }
    require(!items.empty());
    const auto representation = context.builder.add("IFCSHAPEREPRESENTATION",
        ref(context.representation_context) + ",'Body','Tessellation',(" + items + ")");
    return context.builder.add("IFCPRODUCTDEFINITIONSHAPE", "$,$,(" + ref(representation) + ")");
}

Entity mesh_metadata(const Entity& entity, std::string_view role) {
    auto retained = entity;
    retained.properties["_vertex_ifc_mesh"] = {{"version", 1}, {"role", role},
        {"max_deviation_m", ifc_native_mesh_deviation_m}};
    return retained;
}

void export_native_join(const DocumentSnapshot& document, const Entity& entity,
    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
    const auto checkpoint = context.builder.checkpoint();
    const auto ordinal = context.ordinal, vertices = context.mesh_vertices,
        triangles = context.mesh_triangles, metadata_bytes = context.retained_metadata_charge;
    const auto diagnostic_count = diagnostics.size();
    try {
        context.builder.bounded_add(true);
        const bool walls = entity.type == "wall_join";
        const auto ids = walls ? parse_wall_join(entity.properties, entity.id).wall_ids
                               : parse_roof_join(entity.properties, entity.id).roof_ids;
        // Reserve aggregate boolean work before OCCT sees any source geometry.
        // Failed candidates retain their work charge to bound repeated failures.
        context.join_work.charge(ids.size());
        std::vector<Entity> resolved;
        std::vector<Wall> decoded;
        Json sources = Json::array(), openings = Json::array();
        for (const auto& id : ids) {
            const auto& source = document.entities().at(id);
            require(source.type == (walls ? "wall" : "roof"));
            const auto& placement = context.site_placements.at(id);
            require(placement.source_frame == context.presentation.source_frame &&
                placement.drawing_context.property_id == context.presentation.drawing_context.property_id &&
                placement.drawing_context.building_id == context.presentation.drawing_context.building_id);
            resolved.push_back(resolve_vertical_placement(document, source));
            sources.push_back({{"id", source.id}, {"type", source.type}, {"required", source.required},
                {"properties", source.properties}, {"extensions", source.extensions},
                {"resolved_properties", resolved.back().properties},
                {"physical_context", {{"property_id",placement.drawing_context.property_id},
                    {"building_id",placement.drawing_context.building_id},{"floor_id",placement.drawing_context.floor_id},
                    {"layer_id",placement.drawing_context.layer_id},{"level_id",placement.drawing_context.level_id}}}});
            if (walls) {
                const auto& hosted = context.hosted_openings[id];
                for (const auto* opening : hosted)
                    openings.push_back({{"id", opening->id}, {"type", opening->type}, {"required", opening->required},
                        {"properties", opening->properties}, {"extensions", opening->extensions}});
                Wall wall;
                std::string error;
                require(read_document_wall(resolved.back(), hosted, wall, error));
                decoded.push_back(std::move(wall));
            }
        }
        const auto geometry = walls
            ? ifc_native_wall_join_mesh(parse_wall_join(entity.properties, entity.id), decoded,
                context.limits.max_mesh_vertices - vertices, context.limits.max_mesh_triangles - triangles)
            : ifc_native_roof_join_mesh(parse_roof_join(entity.properties, entity.id), resolved,
                context.limits.max_mesh_vertices - vertices, context.limits.max_mesh_triangles - triangles);
        require(!geometry.regions.empty() && std::isfinite(geometry.net_volume_m3) && geometry.net_volume_m3 > 0);
        std::vector<IfcNativeMesh> meshes;
        Json regions = Json::array();
        std::vector<std::optional<Json>> assignments;
        std::vector<std::optional<std::string>> colors;
        std::vector<std::pair<std::size_t, std::size_t>> mesh_ranges;
        for (const auto& region : geometry.regions) {
            const auto member = std::find(ids.begin(), ids.end(), region.source_id);
            require(member != ids.end());
            const auto i = static_cast<std::size_t>(member - ids.begin());
            const auto& binding = entity.properties.contains("material_assignment") ? entity : resolved[i];
            std::optional<Json> assignment;
            std::optional<std::string> color;
            if (region.layer_id && !entity.properties.contains("material_assignment")) {
                const auto& layers = resolved[i].properties.at("layers");
                const auto layer = std::find_if(layers.begin(), layers.end(), [&](const Json& candidate) {
                    return candidate.at("id").get<std::string>() == *region.layer_id;
                });
                require(layer != layers.end());
                if (layer->contains("material_assignment")) assignment = layer->at("material_assignment");
            } else if (binding.properties.contains("material_assignment"))
                assignment = binding.properties.at("material_assignment");
            if (assignment) {
                const auto& catalog = document.entities().at(assignment->at("catalog_id").get<std::string>());
                require(catalog.type == "assembly_model");
                const auto model = AssemblyModel::from_json(catalog.properties.at("model"));
                const auto material = std::find_if(model.materials().begin(), model.materials().end(), [&](const auto& candidate) {
                    return candidate.id == assignment->at("material_id").get<std::string>();
                });
                require(material != model.materials().end());
                color = material->color_srgb;
            }
            assignments.push_back(assignment); colors.push_back(color);
            mesh_ranges.emplace_back(meshes.size(), region.meshes.size());
            meshes.insert(meshes.end(), region.meshes.begin(), region.meshes.end());
            regions.push_back({{"source_id", region.source_id}, {"authored_priority", i},
                {"layer_id", region.layer_id ? Json(*region.layer_id) : Json(nullptr)},
                {"gross_volume_m3", region.gross_volume_m3}, {"net_volume_m3", region.net_volume_m3},
                {"first_mesh", mesh_ranges.back().first}, {"mesh_count", region.meshes.size()},
                {"material_assignment", assignment ? *assignment : Json(nullptr)},
                {"color_srgb", color ? Json(*color) : Json(nullptr)}});
        }
        std::vector<int> items;
        const auto shape = mesh_shape(meshes, context, &items);
        const auto product = context.builder.add(walls ? "IFCWALL" : "IFCROOF",
            context.root(entity.id, entity.id) + ",$," + ref(context.placement) + "," + ref(shape) + ",$,.NOTDEFINED.");
        auto retained = mesh_metadata(entity, entity.type);
        retained.properties["_vertex_ifc_join"] = {{"version", 1}, {"native_entity", {
            {"id", entity.id}, {"type", entity.type}, {"required", entity.required},
            {"properties", entity.properties}, {"extensions", entity.extensions}}},
            {"members", std::move(sources)}, {"hosted_openings", std::move(openings)},
            {"regions", std::move(regions)}, {"net_volume_m3", geometry.net_volume_m3},
            {"priority", "earlier_member_owns_overlap"}};
        retain_properties(retained, product, context, diagnostics);
        require(diagnostics.size() == diagnostic_count); // complete source or no physical occurrence
        std::string constituents;
        for (std::size_t i = 0; i < geometry.regions.size(); ++i) {
            const auto& region = geometry.regions[i];
            if (!assignments[i] || region.meshes.empty()) continue;
            const auto& assignment = *assignments[i];
            const auto material = context.builder.add("IFCMATERIAL",
                step_string(assignment.at("material_id").get<std::string>(), context.limits) + "," +
                step_string("Native catalog: " + assignment.at("catalog_id").get<std::string>(), context.limits) + ",$");
            const auto constituent = context.builder.add("IFCMATERIALCONSTITUENT",
                step_string(Json{{"source_id",region.source_id},{"layer_id",region.layer_id ? Json(*region.layer_id) : Json(nullptr)}}.dump(),
                    context.limits) + ",$," + ref(material) + "," +
                real_text(region.net_volume_m3 / geometry.net_volume_m3) + ",$");
            if (!constituents.empty()) constituents += ',';
            constituents += ref(constituent);
            if (colors[i]) {
                const auto& color = *colors[i];
                require(color.size() == 7 && color[0] == '#');
                const auto channel = [&](std::size_t index) {
                    unsigned value{};
                    const auto parsed = std::from_chars(color.data() + index, color.data() + index + 2, value, 16);
                    require(parsed.ec == std::errc{} && parsed.ptr == color.data() + index + 2);
                    return real_text(value / 255.0);
                };
                const auto rgb = context.builder.add("IFCCOLOURRGB", "$," + channel(1) + ',' + channel(3) + ',' + channel(5));
                const auto surface = context.builder.add("IFCSURFACESTYLESHADING", ref(rgb) + ",$");
                const auto style = context.builder.add("IFCSURFACESTYLE", "$,.BOTH.,(" + ref(surface) + ")");
                const auto range = mesh_ranges[i];
                for (std::size_t mesh = range.first; mesh < range.first + range.second; ++mesh)
                    context.builder.add("IFCSTYLEDITEM", ref(items.at(mesh)) + ",(" + ref(style) + "),$");
            }
        }
        if (!constituents.empty()) {
            const auto set = context.builder.add("IFCMATERIALCONSTITUENTSET", "$,$,(" + constituents + ")");
            context.builder.add("IFCRELASSOCIATESMATERIAL", context.root("join-material:" + entity.id, "") +
                ",(" + ref(product) + ")," + ref(set));
        }
        const auto quantity = context.builder.add("IFCQUANTITYVOLUME", "'NetVolume',$,$," + real_text(geometry.net_volume_m3) + ",$");
        const auto quantities = context.builder.add("IFCELEMENTQUANTITY", context.root("join-quantity:" + entity.id,
            walls ? "Qto_WallBaseQuantities" : "Qto_RoofBaseQuantities") + ",$,(" + ref(quantity) + ")");
        context.builder.add("IFCRELDEFINESBYPROPERTIES", context.root("join-quantity-link:" + entity.id, "") +
            ",(" + ref(product) + ")," + ref(quantities));
        auto products = context.product_ids;
        products[entity.id] = product;
        for (const auto& id : ids) products[id] = product;
        auto contained = context.contained_products;
        require(context.storey > 0); contained[context.storey].push_back(product);
        context.product_ids.swap(products); context.contained_products.swap(contained);
        context.builder.bounded_add(checkpoint.bounded);
    } catch (const std::exception& error) {
        context.builder.rollback(checkpoint);
        context.ordinal = ordinal; context.mesh_vertices = vertices; context.mesh_triangles = triangles;
        context.retained_metadata_charge = metadata_bytes;
        diagnostics.resize(diagnostic_count);
        add_diagnostic(diagnostics, entity.id, entity.type,
            std::string_view(error.what()) == "ifc_mesh_budget_exceeded" ||
            std::string_view(error.what()) == "ifc_native_join_work_budget_exceeded"
                ? "native_join_budget_exceeded" : "native_join_geometry_not_representable");
    }
}

std::string roof_enum(const Json& properties) {
    const auto form = properties.value("form", std::string{});
    if (form == "sloped_roof_panel") return ".SHED_ROOF.";
    if (form == "gable_roof") return ".GABLE_ROOF.";
    if (form == "hip_roof") return ".HIP_ROOF.";
    return ".NOTDEFINED.";
}

bool detach_native_context(Json& properties) {
    bool detached = false;
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "parent_id", "phase_id",
        "property_ids", "building_ids", "floor_ids", "layer_ids", "parent_ids", "phase_ids",
        "vertical_level_binding", "level_connection"})
        detached = properties.erase(key) != 0 || detached;
    const auto placement = properties.find("vertical_placement");
    if (placement != properties.end() && placement->is_object() &&
        placement->value("mode", std::string{}) == "level") {
        properties.erase(placement);
        detached = true;
    }
    return detached;
}

// Count untrusted work before the semantic decoder allocates topology or
// enters its pairwise landing checks. Hosted post bounds use a conservative
// path length; exact supported stations are validated by the canonical decoder.
std::size_t stair_railing_work(const Entity& entity, const Entity* host = nullptr) {
    try {
        return project_import_detail::native_stair_railing_work(entity, host);
    } catch (const std::exception&) {
        invalid();
    }
}

// Imported topology receives fresh live identities. Compare those schema-owned
// names by ordered canonical role without rewriting the retained source proof
// or arbitrary user/vendor fields that happen to contain the same strings.
std::map<std::string, std::string, std::less<>> stair_comparison_child_remap(
    const Json& captured, const Json& current) {
    std::map<std::string, std::string, std::less<>> result;
    if (!captured.is_object() || !current.is_object() ||
        (captured.value("version", 0) != 2 && captured.value("version", 0) != 3 && captured.value("version", 0) != 4) ||
        captured.value("version", 0) != current.value("version", 0) ||
        captured.value("form", std::string{}) != "multi_flight_stair" ||
        current.value("form", std::string{}) != "multi_flight_stair") return result;
    for (const auto* key : {"flights", "landings"}) {
        if (!captured.contains(key) || !current.contains(key) || !captured.at(key).is_array() ||
            !current.at(key).is_array() || captured.at(key).size() != current.at(key).size() ||
            captured.at(key).size() > 256) return {};
        for (std::size_t i = 0; i < captured.at(key).size(); ++i) {
            const auto& before = captured.at(key)[i]; const auto& after = current.at(key)[i];
            if (!before.is_object() || !after.is_object() || !before.contains("id") || !after.contains("id") ||
                !before.at("id").is_string() || !after.at("id").is_string()) return {};
            if (!result.emplace(before.at("id").get<std::string>(), after.at("id").get<std::string>()).second)
                return {};
        }
    }
    return result;
}

Entity stair_railing_metadata(const DocumentSnapshot& document, const Entity& resolved,
                             bool retain_source = true) {
    auto retained = mesh_metadata(resolved, resolved.type);
    const auto& authored = document.entities().at(resolved.id);
    const auto prior = authored.extensions.find("ifc_vertex_properties");
    if (retain_source && prior != authored.extensions.end() && prior->is_object() && prior->contains("_vertex_ifc_entity")) {
        auto active = *prior;
        active.erase("_vertex_ifc_mesh"); active.erase("_vertex_ifc_entity"); active.erase("_vertex_ifc_host");
        detach_native_context(active);
        active.erase("vertical_placement");
        auto current = resolved.properties;
        detach_native_context(current); current.erase("vertical_placement");
        auto current_extensions = authored.extensions;
        current_extensions.erase("ifc_source"); current_extensions.erase("ifc_vertex_properties");
        const auto& original_envelope = prior->at("_vertex_ifc_entity");
        const bool unchanged_extensions = original_envelope.is_object() && original_envelope.contains("extensions") &&
            original_envelope.at("extensions") == current_extensions;
        // Recovery deliberately admits a non-required live carrier. Preserve
        // the original source flag until the live carrier is explicitly made
        // required; comparing against that foreign flag would lose provenance
        // on an untouched import whose source was required.
        const bool unchanged_required = !authored.required;
        const bool unchanged_authority = !authored.properties.contains("vertical_placement") &&
            !authored.properties.contains("level_connection") && !authored.properties.contains("phase_id");
        if (resolved.type == "stair") {
            const auto remap = stair_comparison_child_remap(active, current);
            if (!remap.empty()) for (const auto* key : {"flights", "landings"})
                for (auto& child : active.at(key)) child["id"] = remap.at(child.at("id").get<std::string>());
        }
        bool unchanged_host = true;
        if (resolved.type == "railing" && active.contains("host") && authored.properties.contains("host")) {
            unchanged_host = false;
            auto& old_host = active["host"];
            const auto& new_host = authored.properties.at("host");
            const auto found = document.entities().find(new_host.at("stair_id").get<std::string>());
            if (found != document.entities().end() && found->second.extensions.contains("ifc_vertex_properties")) {
                const auto& proof = found->second.extensions.at("ifc_vertex_properties");
                unchanged_host = prior->contains("_vertex_ifc_host") && prior->at("_vertex_ifc_host") ==
                    stair_railing_metadata(document, resolve_vertical_placement(document,found->second)).properties;
                if (proof.contains("_vertex_ifc_entity") && proof.at("_vertex_ifc_entity").value("id", std::string{}) ==
                    old_host.value("stair_id", std::string{})) {
                    const auto remap = stair_comparison_child_remap(
                        proof.at("_vertex_ifc_entity").at("properties"), found->second.properties);
                    const auto remap_host_child = [&](const char* key) {
                        if (old_host.contains(key) && old_host.at(key).is_string()) {
                            const auto replacement = remap.find(old_host.at(key).get<std::string>());
                            if (replacement != remap.end()) old_host[key] = replacement->second;
                        }
                    };
                    if (active.value("version", 0) == 2 && current.value("version", 0) == 2 &&
                        active.value("form", std::string{}) == "stair_flight_railing" &&
                        current.value("form", std::string{}) == "stair_flight_railing") remap_host_child("flight_id");
                    else if (active.value("version", 0) == 3 && current.value("version", 0) == 3 &&
                        active.value("form", std::string{}) == "stair_landing_railing" &&
                        current.value("form", std::string{}) == "stair_landing_railing")
                        for (const auto* key : {"landing_id", "incoming_flight_id", "outgoing_flight_id"}) remap_host_child(key);
                    if (old_host.contains("flight_id") && old_host.at("flight_id") == old_host.at("stair_id"))
                        old_host["flight_id"] = found->second.id;
                    old_host["stair_id"] = found->second.id;
                }
            }
        }
        if (unchanged_host && unchanged_extensions && unchanged_required && unchanged_authority && active == current) {
            // An unchanged recovered carrier keeps its original captured
            // source and opaque context rather than wrapping imported IDs.
            retained.properties = *prior;
            return retained;
        }
    }
    auto extensions = authored.extensions;
    extensions.erase("ifc_source"); extensions.erase("ifc_vertex_properties");
    retained.properties["_vertex_ifc_entity"] = {{"version",1},{"id",authored.id},{"type",authored.type},
        {"required",authored.required},{"properties",authored.properties},{"extensions",std::move(extensions)}};
    return retained;
}

// A fresh rail proof names the live host/context/children. Its stair and sibling
// rails must therefore use that same live proof vocabulary in this export.
// Inspect each hosted member once, retaining only one flag per changed host;
// no geometry is constructed and immutable document evidence stays untouched.
std::set<std::string, std::less<>> fresh_stair_export_clusters(const DocumentSnapshot& document) {
    std::set<std::string, std::less<>> result;
    const auto fresh_authority = [](const Entity& entity) {
        return entity.required || entity.properties.contains("vertical_placement") ||
            entity.properties.contains("level_connection") || entity.properties.contains("phase_id");
    };
    for (const auto& [id, entity] : document.entities()) {
        (void)id;
        if (entity.type != "railing" || !entity.properties.is_object()) continue;
        const auto host = entity.properties.find("host");
        if (host == entity.properties.end() || !host->is_object()) continue;
        const auto stair_id = host->find("stair_id");
        if (stair_id == host->end() || !stair_id->is_string()) continue;
        const auto& host_id = stair_id->get_ref<const std::string&>();
        const auto stair = document.entities().find(host_id);
        if (stair == document.entities().end() || stair->second.type != "stair" || result.contains(host_id)) continue;
        const auto prior = entity.extensions.find("ifc_vertex_properties");
        if (prior != entity.extensions.end() && !fresh_authority(entity) && !fresh_authority(stair->second)) {
            try {
                // Authority above already requires new proof. For an eligible
                // detached import neither member has placement to resolve, so
                // comparison never needs a per-member organization traversal.
                if (stair_railing_metadata(document, entity).properties == *prior)
                    continue;
            } catch (const std::exception&) {
                // Ordinary native export still diagnoses malformed authority or
                // evidence. It must not leave another member using stale proof.
            }
        }
        result.insert(host_id);
    }
    return result;
}

void export_native_stair_or_railing(const DocumentSnapshot& document, const Entity& entity,
    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
    try {
        std::optional<Entity> host;
        if (entity.type == "railing" && entity.properties.contains("host")) {
            const auto& h = entity.properties.at("host");
            require(h.is_object() && h.contains("stair_id") && h.at("stair_id").is_string());
            const auto found = document.entities().find(h.at("stair_id").get<std::string>());
            require(found != document.entities().end() && found->second.type == "stair");
            host = resolve_vertical_placement(document, found->second);
        }
        const auto work = stair_railing_work(entity, host ? &*host : nullptr);
        context.native_work.charge(work);
        // Decode, native construction and final document admission may each
        // reconstruct topology. Reserve their repeated overlap work as well.
        for (int i=0; i<3; ++i) context.native_work.charge_cross(work);
        require(work <= std::min(context.limits.max_mesh_vertices - context.mesh_vertices,
            context.limits.max_mesh_triangles - context.mesh_triangles) / 64);
        const auto meshes = entity.type == "stair"
            ? ifc_native_stair_mesh(entity, work * 64, work * 64)
            : ifc_native_railing_mesh(entity, host ? &*host : nullptr, work * 64, work * 64);
        const auto shape = mesh_shape(meshes, context);
        const auto product = context.builder.add(entity.type == "stair" ? "IFCSTAIR" : "IFCRAILING",
            context.root(entity.id, entity.id) + ",$," + ref(context.placement) + "," + ref(shape) + ",$,.NOTDEFINED.");
        context.product_ids[entity.id] = product;
        if (host) context.railing_host_links.emplace_back(entity.id, host->id);
        else context.contain(product);
        const bool retain_source = !context.fresh_stair_proofs.contains(host ? host->id : entity.id);
        auto retained = stair_railing_metadata(document, entity, retain_source);
        if (host) retained.properties["_vertex_ifc_host"] = stair_railing_metadata(document, *host, retain_source).properties;
        retain_properties(retained, product, context, diagnostics);
    } catch (const std::exception&) {
        add_diagnostic(diagnostics, entity.id, entity.type, "native_stair_or_railing_geometry_not_representable");
    }
}

void export_native_roof_or_room(const DocumentSnapshot& document, const Entity& entity,
    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
    try {
        const bool roof = entity.type == "roof";
        auto meshes = roof
            ? ifc_native_roof_mesh(entity, context.limits.max_mesh_vertices - context.mesh_vertices,
                context.limits.max_mesh_triangles - context.mesh_triangles)
            : ifc_native_room_mesh(entity, context.limits.max_mesh_vertices - context.mesh_vertices,
                context.limits.max_mesh_triangles - context.mesh_triangles);
        // World metre meshes use an identity occurrence placement. The native
        // bridge resolves no placement, so level offsets are applied once.
        const auto shape = mesh_shape(meshes, context);
        const auto product = context.builder.add(roof ? "IFCROOF" : "IFCSPACE",
            context.root(entity.id, entity.id) + ",$," + ref(context.placement) + "," + ref(shape) +
            (roof ? ",$," + roof_enum(entity.properties) : ",$,.ELEMENT.,.INTERNAL.,$"));
        context.product_ids[entity.id] = product;
        if (roof) context.contain(product);
        else context.aggregate(context.storey, product, "storey-space:" + entity.id);
        auto retained = mesh_metadata(entity, roof ? "roof" : "room");
        const auto& authored = document.entities().at(entity.id);
        auto extensions = authored.extensions;
        auto authored_properties = authored.properties;
        bool authored_required = authored.required;
        // Imported provenance is already retained below. Re-export must not
        // recursively wrap it, or overwrite human strings that resemble IDs.
        const auto prior = extensions.find("ifc_vertex_properties");
        if (prior != extensions.end() && prior->is_object() && prior->contains("_vertex_ifc_entity")) {
            const auto source = prior->at("_vertex_ifc_entity");
            if (source.is_object() && source.contains("extensions") && source.at("extensions").is_object()) {
                auto prior_active = *prior;
                prior_active.erase("_vertex_ifc_mesh"); prior_active.erase("_vertex_ifc_entity");
                detach_native_context(prior_active);
                auto current_active = authored.properties;
                // Import assigns a destination hierarchy without changing the
                // captured physical source. Other authority edits remain live
                // differences and must produce a new source proof.
                for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
                    current_active.erase(key);
                if (!authored.required && prior_active == current_active &&
                    source.contains("properties") && source.at("properties").is_object()) {
                    // The resolved carrier must use the same source context
                    // as its reused envelope; leave no new destination fields
                    // beside the restored original hierarchy.
                    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
                        retained.properties.erase(key);
                    authored_properties = source.at("properties");
                    if (source.contains("required") && source.at("required").is_boolean())
                        authored_required = source.at("required").get<bool>();
                    // Keep original context as resolved source metadata only.
                    // The next import compares this envelope, then detaches it.
                    auto detached = authored_properties;
                    detach_native_context(detached);
                    for (const auto& [key, value] : authored_properties.items())
                        if (!detached.contains(key)) retained.properties[key] = value;
                }
                extensions.erase("ifc_vertex_properties");
                extensions.erase("ifc_source");
                for (const auto* key : {"ifc_source", "ifc_vertex_properties"})
                    if (source.at("extensions").contains(key)) extensions[key] = source.at("extensions").at(key);
            }
        }
        retained.properties["_vertex_ifc_entity"] = {{"version",1},{"type",entity.type},
            {"required",authored_required},{"properties",std::move(authored_properties)},{"extensions",std::move(extensions)}};
        retain_properties(retained, product, context, diagnostics);
    } catch (const std::exception& error) {
        add_diagnostic(diagnostics, entity.id, entity.type,
            std::string_view(error.what()) == "ifc_mesh_budget_exceeded"
                ? "native_mesh_budget_exceeded" : "native_roof_or_room_geometry_not_representable");
    }
}

void export_fill(const DocumentSnapshot& document, const Entity& entity, int void_id,
    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
    if (!entity.properties.contains("opening_assembly")) return;
    const auto host = resolve_vertical_placement(document,
        document.entities().at(entity.properties.at("wall_id").get<std::string>()));
    const auto wall = native_wall(host);
    const auto opening = native_opening(entity);
    const auto profile = parse_opening_assembly(entity.properties.at("opening_assembly"));
    const auto operation = native_operation(entity);
    require(entity.properties.value("opening_kind", std::string{}) == opening_assembly_kind_name(profile.kind));
    require(profile.kind == OpeningAssemblyKind::door || !operation);
    auto meshes = ifc_native_fill_mesh(wall, opening, profile, operation,
        context.limits.max_mesh_vertices - context.mesh_vertices,
        context.limits.max_mesh_triangles - context.mesh_triangles);
    const auto frame = fill_frame(wall, opening, operation);
    // The bridge supplies the actual world geometry, including curved frames
    // and swung leaves. Only its coordinate basis changes here.
    for (auto& mesh : meshes)
        for (auto& point : mesh.vertices) {
            const double dx = point[0]-frame.origin.x, dy = point[1]-frame.origin.y;
            point = {dx*frame.x.x + dy*frame.x.y, -dx*frame.x.y + dy*frame.x.x,
                     point[2]-frame.origin.z};
        }
    const auto shape = mesh_shape(meshes, context);
    const auto placement = fill_placement(frame, context);
    const bool door = profile.kind == OpeningAssemblyKind::door;
    const auto fill = context.builder.add(door ? "IFCDOOR" : "IFCWINDOW",
        context.root("fill:" + entity.id, entity.id + " fill") + ",$," + ref(placement) +
        "," + ref(shape) + ",$," + real_text(opening.height) + "," + real_text(fill_overall_width(wall, opening, frame)) +
        (door ? ",.DOOR.," + door_operation_enum(operation) + "," + door_operation_label(operation, context.limits)
              : ",.WINDOW.," + window_partition_enum(profile) + "," + window_partition_label(profile, context.limits)));
    context.builder.add("IFCRELFILLSELEMENT", context.root("fills:" + entity.id, "") +
        "," + ref(void_id) + "," + ref(fill));
    context.contain(fill);
    auto retained = mesh_metadata(entity, "fill");
    retained.properties["_vertex_ifc_host"] = host.properties;
    retain_properties(retained, fill, context, diagnostics);
}

bool export_curved_native(const DocumentSnapshot& document, const Entity& entity,
    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
    Entity host = entity;
    if (entity.type == "opening") host = resolve_vertical_placement(document,
        document.entities().at(entity.properties.at("wall_id").get<std::string>()));
    const auto axis = read_baseline(host);
    if (!axis) return false;
    if (std::abs(axis->sweep_radians) <= kTolerance && !host.properties.contains("top_plane") &&
        std::abs(host.properties.contains("slope_rise_m")
            ? host.properties.value("slope_rise_m", 0.0)
            : host.properties.value("slope_rise", 0.0)) <= kTolerance)
        return false;
    const auto wall = native_wall(host);
    const auto gradient = wall_top_gradient(wall);
    if (std::abs(axis->sweep_radians) <= kTolerance && gradient.x == 0.0 && gradient.y == 0.0)
        return false;
    const auto meshes = entity.type == "wall"
        ? ifc_native_wall_mesh(wall, context.limits.max_mesh_vertices - context.mesh_vertices,
                              context.limits.max_mesh_triangles - context.mesh_triangles)
        : ifc_native_void_mesh(wall, native_opening(entity),
            context.limits.max_mesh_vertices - context.mesh_vertices,
            context.limits.max_mesh_triangles - context.mesh_triangles);
    const auto shape = mesh_shape(meshes, context);
    const auto product = context.builder.add(entity.type == "wall" ? "IFCWALL" : "IFCOPENINGELEMENT",
        context.root(entity.id, entity.id) + ",$," + ref(context.placement) + "," + ref(shape) +
        (entity.type == "wall" ? ",$,.NOTDEFINED." : ",$,.OPENING."));
    context.product_ids[entity.id] = product;
    auto retained = mesh_metadata(entity, entity.type == "wall" ? "wall" : "void");
    if (entity.type == "wall") {
        context.contain(product);
        export_wall_construction(entity, product, context, diagnostics);
    } else {
        context.opening_host_links.emplace_back(entity.id, host.id);
        retained.properties["_vertex_ifc_host"] = host.properties;
    }
    retain_properties(retained, product, context, diagnostics);
    if (entity.type == "opening") export_fill(document, entity, product, context, diagnostics);
    return true;
}
#endif

void export_terrain(const Entity& entity, ExportContext& context,
    std::vector<IfcProjectDiagnostic>& diagnostics) {
    try {
        const auto terrain=TerrainSurface::from_json(entity.properties.at("model"));
        require(terrain.points().size()<=context.limits.max_mesh_vertices-context.mesh_vertices &&
            terrain.triangles().size()<=context.limits.max_mesh_triangles-context.mesh_triangles);
        context.mesh_vertices+=terrain.points().size(); context.mesh_triangles+=terrain.triangles().size();
        std::string points="(",triangles="(";
        for (const auto& p:terrain.points()) {
            if (points.size()>1) points+=',';
            points+="("+real_text(p.x_m)+","+real_text(p.y_m)+","+real_text(p.elevation_m)+")";
        }
        for (const auto& t:terrain.triangles()) {
            if (triangles.size()>1) triangles+=',';
            triangles+="("+std::to_string(t.point_indices[0]+1)+","+std::to_string(t.point_indices[1]+1)+","+
                std::to_string(t.point_indices[2]+1)+")";
        }
        const auto coordinates=context.builder.add("IFCCARTESIANPOINTLIST3D",points+")");
        const auto faces=context.builder.add("IFCTRIANGULATEDFACESET",ref(coordinates)+",$,.F.,"+triangles+"),$");
        const auto repr=context.builder.add("IFCSHAPEREPRESENTATION",ref(context.representation_context)+",'Body','Tessellation',("+ref(faces)+")");
        const auto shape=context.builder.add("IFCPRODUCTDEFINITIONSHAPE","$,$,("+ref(repr)+")");
        const auto product=context.builder.add("IFCGEOGRAPHICELEMENT",context.root(entity.id,entity.id)+",$,"+
            ref(context.placement)+","+ref(shape)+",$,.TERRAIN.");
        context.product_ids[entity.id]=product; context.contain(product);
        auto retained=entity;
        retained.properties["_vertex_ifc_entity"]={{"id",entity.id},{"type",entity.type},{"required",entity.required},
            {"properties",entity.properties},{"extensions",entity.extensions}};
        retain_properties(retained,product,context,diagnostics);
    } catch (const std::exception&) { add_diagnostic(diagnostics,entity.id,entity.type,"terrain_geometry_not_exported"); }
}

void export_independent_assembly(const DocumentSnapshot& document, const Entity& entity,
    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics) {
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    try {
        const auto& expansion=context.assembly_expansions.at(entity.id);
        require(!expansion.profiles.empty());
        std::vector<std::vector<IfcNativeMesh>> profile_meshes;
        std::size_t vertices=context.mesh_vertices, triangles=context.mesh_triangles;
        for (const auto& expanded:expansion.profiles) {
            const auto& profile=expanded.profile;
            const auto& transform=expanded.transform;
            std::size_t work=profile.outer.size();
            for (const auto& hole:profile.holes) work+=hole.size();
            context.native_work.charge(work);
            // Native room admission and final construction each check edges.
            context.native_work.charge_cross(work);
            context.native_work.charge_cross(work);
            const auto boundary=[&](const Boundary& source) {
                Boundary result=source;
                for (auto& s:result) {
                    const auto a=transform_assembly_point({s.start.x,s.start.y,0},transform);
                    const auto b=transform_assembly_point({s.end.x,s.end.y,0},transform);
                    s.start={a.x,a.y}; s.end={b.x,b.y};
                    if (transform.mirrored_y) s.sweep_radians=-s.sweep_radians;
                }
                return result;
            };
            Json holes=Json::array(); for (const auto& hole:profile.holes) holes.push_back(boundary_json(boundary(hole)));
            Entity room{"ifc-assembly-profile","room",{{"boundary",boundary_json(boundary(profile.outer))},
                {"holes",holes},{"height_m",profile.height_m*transform.scale},
                {"elevation_m",profile.elevation_m*transform.scale+transform.translation_m.z}}};
            auto meshes=ifc_native_room_mesh(room,context.limits.max_mesh_vertices-vertices,
                context.limits.max_mesh_triangles-triangles);
            for (const auto& mesh:meshes) {
                require(mesh.vertices.size()<=context.limits.max_mesh_vertices-vertices &&
                    mesh.triangles.size()<=context.limits.max_mesh_triangles-triangles);
                vertices+=mesh.vertices.size(); triangles+=mesh.triangles.size();
            }
            profile_meshes.push_back(std::move(meshes));
        }
        const auto root=context.builder.add("IFCELEMENTASSEMBLY",context.root(entity.id,entity.id)+",'Independent profile assembly',"+
            ref(context.placement)+",$,$,.NOTDEFINED.,.USERDEFINED.");
        context.product_ids[entity.id]=root; context.contain(root);
        Json provenance=Json::array();
        for (std::size_t i=0;i<expansion.profiles.size();++i) {
            const auto& p=expansion.profiles[i]; const auto shape=mesh_shape(profile_meshes[i],context);
            const auto child_id=entity.id+":profile:"+std::to_string(i);
            const auto child=context.builder.add("IFCBUILDINGELEMENTPROXY",context.root(child_id,p.profile.id)+",$,"+
                ref(context.placement)+","+ref(shape)+",$,.ELEMENT.");
            context.aggregate(root,child,"assembly-profile:"+child_id);
            if (p.material_id) {
                const auto catalog_id=decode_document_assembly_instance(entity).assembly_catalog_id;
                const auto key=Json::array({catalog_id,*p.material_id}).dump();
                auto material=context.material_ids.find(key);
                if (material==context.material_ids.end()) {
                    const auto model=AssemblyModel::from_json(document.entities().at(catalog_id).properties.at("model"));
                    const auto found=std::find_if(model.materials().begin(),model.materials().end(),[&](const auto& m){return m.id==*p.material_id;});
                    require(found!=model.materials().end());
                    material=context.material_ids.emplace(key,context.builder.add("IFCMATERIAL",step_string(found->name,context.limits)+",$,$")).first;
                }
                context.builder.add("IFCRELASSOCIATESMATERIAL",context.root("assembly-material:"+child_id,"")+",("+ref(child)+"),"+ref(material->second));
            }
            provenance.push_back({{"part_path",p.part_path},{"type_id",p.type_id},{"profile_id",p.profile.id},
                {"transform",encode_assembly_transform(p.transform)},{"material_id",p.material_id ? Json(*p.material_id) : Json(nullptr)},
                {"geometric_volume_m3",p.volume_m3}});
        }
        auto retained=entity;
        const auto catalog_id=decode_document_assembly_instance(entity).assembly_catalog_id;
        const auto& catalog=document.entities().at(catalog_id);
        retained.properties["_vertex_ifc_assembly_source"]={{"version",1},{"id",entity.id},{"required",entity.required},
            {"properties",entity.properties},{"extensions",entity.extensions},
            {"catalog",{{"id",catalog.id},{"properties",catalog.properties},{"extensions",catalog.extensions}}},
            {"profiles",provenance},{"geometric_volume_m3",expansion.volume_m3}};
        // Declared quantities stay in the exact authored instance/catalog. They
        // are never serialized as geometric volume or scaled by site placement.
        retain_properties(retained,root,context,diagnostics);
    } catch (const std::exception&) { add_diagnostic(diagnostics,entity.id,entity.type,"independent_assembly_geometry_not_exported"); }
#else
    (void)document; (void)context;
    add_diagnostic(diagnostics,entity.id,entity.type,"independent_assembly_runtime_unavailable");
#endif
}

void export_product(const DocumentSnapshot& document, const Entity& entity,
                    ExportContext& context, std::vector<IfcProjectDiagnostic>& diagnostics,
                    const PhysicalWallRoomCheck* physical_room = nullptr) {
    const auto& type = entity.type;
    if (type=="terrain_surface") { export_terrain(entity,context,diagnostics); return; }
    if (type=="assembly_instance") { export_independent_assembly(document,entity,context,diagnostics); return; }
    std::string product_type;
    Boundary boundary;
    std::vector<Boundary> physical_holes;
    bool closed = false;
    bool use_solid = false;
    double depth = 0.0;
    double local_elevation = 0.0;

#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    if (type == "wall_join" || type == "roof_join") {
        export_native_join(document, entity, context, diagnostics);
        return;
    }
    if (type == "stair" || type == "railing") {
        export_native_stair_or_railing(document, entity, context, diagnostics);
        return;
    }
    if (type == "roof" || type == "room") {
        export_native_roof_or_room(document, entity, context, diagnostics);
        return;
    }
    if ((type == "wall" || type == "opening") &&
        export_curved_native(document, entity, context, diagnostics)) return;
#endif

#ifndef SKETCH_IFC_NATIVE_GEOMETRY
    if (type == "wall_join" || type == "roof_join") {
        add_diagnostic(diagnostics, entity.id, type, "native_join_runtime_unavailable");
        return;
    }
    if (type == "stair" || type == "railing") {
        add_diagnostic(diagnostics, entity.id, type, "native_stair_or_railing_runtime_unavailable");
        return;
    }
    if (type == "roof" || type == "room") {
        add_diagnostic(diagnostics, entity.id, type, "native_roof_or_room_runtime_unavailable");
        return;
    }
#endif

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
            const auto gradient = wall_top_gradient(native_wall(entity));
            if (std::abs(baseline->sweep_radians) > kTolerance ||
                gradient.x != 0.0 || gradient.y != 0.0) {
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
        const bool source_bound = type == "room_boundary" && entity.extensions.contains("physical_wall_room");
        if (source_bound && (!physical_room || !physical_room->current)) {
            add_diagnostic(diagnostics, entity.id, type, "physical_room_source_stale_or_runtime_unavailable");
            return;
        }
        const auto decoded = source_bound ? std::optional<Boundary>{physical_room->boundary} : read_boundary(entity);
        if (!decoded) {
            add_diagnostic(diagnostics, entity.id, type, "boundary_not_representable");
            return;
        }
        boundary = *decoded;
        if (source_bound) physical_holes = physical_room->holes;
        product_type = source_bound ? "IFCSPACE" : "IFCBUILDINGELEMENTPROXY";
#ifdef SKETCH_PHYSICAL_ROOMS
        if (source_bound) {
            const auto drawing = organize_project(document.entities()).drawing_context(entity.id);
            require(drawing.has_value());
            local_elevation = validate_retained_physical_wall_room_lineage(entity, *drawing).effective_elevation_m;
        }
#endif
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
        const auto resolved_host = resolve_vertical_placement(document, host->second);
        const auto baseline = read_baseline(resolved_host);
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
            const auto found = resolved_host.properties.find(key);
            if (found != resolved_host.properties.end() && found->is_number()) {
                wall_thickness = found->get<double>();
                break;
            }
        }
        double wall_height = 0.0;
        double wall_elevation = 0.0;
        for (const auto* key : {"height_m", "height"}) {
            const auto found = resolved_host.properties.find(key);
            if (found != resolved_host.properties.end() && found->is_number()) {
                wall_height = found->get<double>();
                break;
            }
        }
        for (const auto* key : {"elevation_m", "elevation"}) {
            const auto found = resolved_host.properties.find(key);
            if (found != resolved_host.properties.end() && found->is_number()) {
                wall_elevation = found->get<double>();
                break;
            }
        }
        // A void feature must not outlive an unsupported host body: IFC openings
        // require a host relationship, so retain both as native references.
        if (!std::isfinite(wall_height) || wall_height <= kTolerance ||
            std::abs(resolved_host.properties.value("slope_rise_m", 0.0)) > kTolerance ||
            !resolved_host.properties.contains("height_m") ||
            !resolved_host.properties.contains("thickness_m")) {
            add_diagnostic(diagnostics, entity.id, type, "opening_host_body_not_representable");
            return;
        }
        const auto host_gradient = wall_top_gradient(native_wall(resolved_host));
        if (host_gradient.x != 0.0 || host_gradient.y != 0.0) {
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
#ifndef SKETCH_IFC_NATIVE_GEOMETRY
            add_diagnostic(diagnostics, entity.id, type,
                           "opening_assembly_not_exported");
#endif
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
    std::vector<std::vector<Vec2>> hole_points;
    for (const auto& hole : physical_holes) {
        bool hole_closed = false;
        auto points = linear_points(hole, hole_closed);
        if (points.empty() || !hole_closed) {
            add_diagnostic(diagnostics, entity.id, type, "physical_room_curved_holes_not_representable");
            return;
        }
        hole_points.push_back(std::move(points));
    }
    const auto make_polyline = [&](const std::vector<Vec2>& points) {
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
    };
    const auto polyline_points = make_polyline(points);

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
        std::string footprint_items = ref(polyline_points);
        for (const auto& points : hole_points) footprint_items += ',' + ref(make_polyline(points));
        shape = context.builder.add("IFCSHAPEREPRESENTATION",
            ref(context.representation_context) + ",'Footprint','Curve3D',(" + footprint_items + ")");
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
            ref(context.placement) + "," + ref(axis));
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
    } else if (product_type == "IFCSPACE") {
        product_id = context.builder.add(product_type,
            global_id + "," + ref(context.owner_history) + "," + name + "," +
            description + ",$," + placement + "," + ref(product_shape) + ",$,.ELEMENT.,.INTERNAL.,$");
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
    if (product_type == "IFCSPACE") context.aggregate(context.storey, product_id, "storey-space:" + entity.id);
    else if (type != "opening") context.contain(product_id);
    if (type == "wall") export_wall_construction(entity, product_id, context, diagnostics);
    if (type == "wall" || type == "slab" || type == "opening") {
        retain_properties(entity, product_id, context, diagnostics);
    }
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    if (type == "opening") export_fill(document, entity, product_id, context, diagnostics);
#endif
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

std::optional<Point3> unit_direction(const ParsedStep& parsed, std::string_view value,
    Point3 fallback, std::size_t& count, const IfcExchangeLimits& limits) {
    if (value == "$") return fallback;
    const auto id = reference(value);
    if (!id) return std::nullopt;
    const auto* record = find_record(parsed, *id);
    if (!record || record->type != "IFCDIRECTION") return std::nullopt;
    const auto fields = split_top_level(record->args, count, limits);
    if (fields.size() != 1) return std::nullopt;
    const auto coordinates = split_top_level(inner_list(fields[0]), count, limits);
    if (coordinates.size() != 3) return std::nullopt;
    const Point3 direction{number<double>(coordinates[0]), number<double>(coordinates[1]), number<double>(coordinates[2])};
    const double norm = std::hypot(direction.x, direction.y, direction.z);
    if (std::abs(norm-1.0) > kTolerance) return std::nullopt;
    return direction;
}

// Native fills accept only this bounded rigid Z-up subset. Parent rotations
// transform the child origin and X direction, rather than adding translations.
std::optional<RigidFrame> rigid_placement(const ParsedStep& parsed, int id,
    std::set<int>& visited, std::size_t& count, const IfcExchangeLimits& limits) {
    require(visited.size() < 128 && visited.insert(id).second);
    const auto* record = find_record(parsed, id);
    require(record != nullptr);
    if (record->type != "IFCLOCALPLACEMENT") return std::nullopt;
    const auto fields = split_top_level(record->args, count, limits);
    require(fields.size() == 2);
    RigidFrame parent;
    if (fields[0] != "$") {
        const auto parent_id = reference(fields[0]);
        require(parent_id.has_value());
        const auto resolved = rigid_placement(parsed, *parent_id, visited, count, limits);
        if (!resolved) return std::nullopt;
        parent = *resolved;
    }
    const auto axis_id = reference(fields[1]);
    require(axis_id.has_value());
    const auto* axis = find_record(parsed, *axis_id);
    if (!axis || axis->type != "IFCAXIS2PLACEMENT3D") return std::nullopt;
    const auto axis_fields = split_top_level(axis->args, count, limits);
    require(axis_fields.size() == 3);
    // IFC4 AxisAndRefDirProvision requires both directions or neither.
    if ((axis_fields[1] == "$") != (axis_fields[2] == "$")) return std::nullopt;
    const auto up = unit_direction(parsed, axis_fields[1], {0,0,1}, count, limits);
    const auto x = unit_direction(parsed, axis_fields[2], {1,0,0}, count, limits);
    if (!up || !x || std::abs(up->x)>kTolerance || std::abs(up->y)>kTolerance ||
        std::abs(up->z-1.0)>kTolerance || std::abs(x->z)>kTolerance) return std::nullopt;
    const auto origin_id = reference(axis_fields[0]);
    require(origin_id.has_value());
    const auto* origin = find_record(parsed, *origin_id);
    require(origin && origin->type == "IFCCARTESIANPOINT");
    const auto origin_fields = split_top_level(origin->args, count, limits);
    require(origin_fields.size() == 1);
    if (split_top_level(inner_list(origin_fields[0]), count, limits).size() != 3) return std::nullopt;
    const double length = std::hypot(x->x, x->y);
    const Vec2 unit_x{x->x/length, x->y/length};
    RigidFrame result{world_point(parent, point_record(*origin, count, limits)),
        {parent.x.x*unit_x.x-parent.x.y*unit_x.y, parent.x.y*unit_x.x+parent.x.x*unit_x.y}};
    if (!bounded_point(result.origin)) return std::nullopt;
    return result;
}

std::optional<RigidFrame> product_frame(const ParsedStep& parsed,
    const std::vector<std::string>& fields, std::size_t& count, const IfcExchangeLimits& limits) {
    require(fields.size() >= 7);
    const auto placement = reference(fields[5]);
    if (!placement) return std::nullopt;
    std::set<int> visited;
    return rigid_placement(parsed, *placement, visited, count, limits);
}

// Only translation-only swept solid placements are mapped. Parent placements are composed
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
           type == "IFCWINDOW" || type == "IFCOPENINGELEMENT" || type == "IFCSTAIR" ||
           type == "IFCSTAIRFLIGHT" || type == "IFCRAILING" ||
           type == "IFCBUILDINGELEMENTPROXY" || type == "IFCELEMENTASSEMBLY" || type == "IFCGEOGRAPHICELEMENT";
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
        "IFCCARTESIANPOINTLIST3D", "IFCTRIANGULATEDFACESET",
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
    std::size_t retained_charge=0;
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
        const auto dialect = decode_string(pset_fields[2], limits);
        if (dialect != "Pset_VertexExchange_v1" && dialect != "Pset_VertexExchange_v2") continue;
        const auto targets=list_references(fields[4],count,limits);
        // Version two is our single-owner source carrier, never a replicated
        // arbitrary attachment across a caller-controlled product list.
        require(!targets.empty() && (dialect!="Pset_VertexExchange_v2" || targets.size()==1));
        const auto properties = list_references(pset_fields[4], count, limits);
        const auto property_text = [&](int id, const std::string& expected_name) {
            const auto* property = find_record(parsed,id);
            require(property && property->type == "IFCPROPERTYSINGLEVALUE");
            const auto fields = split_top_level(property->args,count,limits);
            require(fields.size()==4 && decode_string(fields[0],limits)==expected_name);
            const auto value=trim(fields[2]);
            require(value.starts_with("IFCTEXT(") && value.back()==')');
            return decode_string(value.substr(8,value.size()-9),limits);
        };
        std::string payload;
        if (dialect == "Pset_VertexExchange_v1") {
            require(properties.size()==1);
            payload=property_text(properties.front(),"Properties");
        } else {
            require(properties.size()>=2 && properties.size()<=4097);
            const auto manifest=Json::parse(property_text(properties.front(),"MetadataManifest"),
                [&](int depth,Json::parse_event_t,Json&){ require(depth<=32); return true; },false);
            require(manifest.is_object() && manifest.size()==4 && manifest.contains("version") && manifest.at("version")==1 &&
                manifest.contains("chunks") && manifest.at("chunks").is_number_unsigned() &&
                manifest.contains("bytes") && manifest.at("bytes").is_number_unsigned() &&
                manifest.contains("sha256") && manifest.at("sha256").is_string());
            const auto bytes=manifest.at("bytes").get<std::size_t>();
            require(manifest.at("chunks").get<std::size_t>()==properties.size()-1 && bytes<=8*1024*1024 && bytes<=limits.max_bytes);
            payload.reserve(bytes);
            for (std::size_t i=1;i<properties.size();++i) {
                auto chunk=property_text(properties[i],"PropertiesChunk:"+std::to_string(i-1));
                require(chunk.size()<=bytes-payload.size()); payload+=chunk;
            }
            require(payload.size()==bytes && sha256_hex(std::as_bytes(std::span(payload.data(),payload.size())))==manifest.at("sha256").get<std::string>());
        }
        // Bound JSON nesting independently of the STEP byte limits.
        const auto metadata = Json::parse(payload, [&](int depth, Json::parse_event_t, Json&) {
            require(depth >= 0 && static_cast<std::size_t>(depth) <= kMaximumMetadataDepth); return true;
        }, false);
        const auto charge = metadata_charge(metadata);
        require(targets.size()<=(kMaximumRetainedMetadataCharge-retained_charge)/charge);
        retained_charge+=targets.size()*charge;
        for (const auto id : targets) {
            require(find_record(parsed, id) && result.emplace(id, metadata).second);
        }
        retained_records.insert(relation.id);
        retained_records.insert(pset->id);
        for (const auto id : properties) retained_records.insert(id);
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

// Retain the earlier declaration policy for legacy wall/slab/axis exchange.
// Roof/room native activation uses the actual project assignment below.
bool declared_metre_units(const ParsedStep& parsed, std::size_t& count, const IfcExchangeLimits& limits) {
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

#ifdef SKETCH_IFC_NATIVE_GEOMETRY
bool metre_units(const ParsedStep& parsed, std::size_t& count, const IfcExchangeLimits& limits) {
    const StepRecord* project = nullptr;
    for (const auto& record : parsed.records) if (record.type == "IFCPROJECT") {
        if (project) return false; // Multiple project owners are outside this subset.
        project = &record;
    }
    if (!project) return false;
    const auto project_fields = split_top_level(project->args, count, limits);
    if (project_fields.size() != 9) return false;
    const auto assignment_id = reference(project_fields[8]);
    if (!assignment_id) return false;
    const auto* assignment = find_record(parsed, *assignment_id);
    if (!assignment || assignment->type != "IFCUNITASSIGNMENT") return false;
    const auto assignment_fields = split_top_level(assignment->args, count, limits);
    if (assignment_fields.size() != 1) return false;
    const auto units = trim(assignment_fields[0]);
    if (units.size() < 2 || units.front() != '(' || units.back() != ')') return false;
    bool found = false;
    for (const auto& value : split_top_level(inner_list(units), count, limits)) {
        const auto unit_id = reference(value);
        if (!unit_id) return false;
        const auto* unit = find_record(parsed, *unit_id);
        if (!unit || unit->type != "IFCSIUNIT") return false;
        const auto fields = split_top_level(unit->args, count, limits);
        if (fields.size() != 4 || fields[0] != "*") return false;
        if (upper(fields[1]) != ".LENGTHUNIT.") continue;
        if (found || fields[2] != "$" || upper(fields[3]) != ".METRE.") return false;
        found = true;
    }
    return found;
}
#endif

std::optional<std::vector<IfcNativeMesh>> product_meshes(const ParsedStep& parsed,
    const StepRecord& product, std::size_t& count, const IfcExchangeLimits& limits,
    std::size_t& vertices, std::size_t& triangles) {
    const auto fields = split_top_level(product.args, count, limits);
    require(fields.size() >= 7);
    const auto shape_id = reference(fields[6]);
    if (!shape_id) return std::nullopt;
    const auto* shape = find_record(parsed, *shape_id);
    if (!shape || shape->type != "IFCPRODUCTDEFINITIONSHAPE") return std::nullopt;
    const auto shape_fields = split_top_level(shape->args, count, limits);
    require(shape_fields.size() == 3);
    const auto representations = list_references(shape_fields[2], count, limits);
    if (representations.size() != 1) return std::nullopt;
    const auto* representation = find_record(parsed, representations.front());
    if (!representation || representation->type != "IFCSHAPEREPRESENTATION") return std::nullopt;
    const auto rep_fields = split_top_level(representation->args, count, limits);
    require(rep_fields.size() == 4);
    if (rep_fields[1] != "'Body'" || rep_fields[2] != "'Tessellation'") return std::nullopt;
    // The native subset requires an identity world context; product placements
    // are resolved separately. Shifted contexts and coordinate operations do
    // not gain trust merely because native metadata is present.
    const auto context_id = reference(rep_fields[0]);
    if (!context_id) return std::nullopt;
    const auto* context = find_record(parsed, *context_id);
    if (!context || context->type != "IFCGEOMETRICREPRESENTATIONCONTEXT") return std::nullopt;
    const auto context_fields = split_top_level(context->args, count, limits);
    if (context_fields.size() != 6 || context_fields[2] != "3" || context_fields[5] != "$") return std::nullopt;
    const auto axis_id = reference(context_fields[4]);
    if (!axis_id) return std::nullopt;
    const auto* axis = find_record(parsed, *axis_id);
    if (!axis || axis->type != "IFCAXIS2PLACEMENT3D") return std::nullopt;
    const auto axis_fields = split_top_level(axis->args, count, limits);
    if (axis_fields.size() != 3 || axis_fields[1] != "$" || axis_fields[2] != "$") return std::nullopt;
    const auto origin_id = reference(axis_fields[0]);
    if (!origin_id) return std::nullopt;
    const auto* origin = find_record(parsed, *origin_id);
    if (!origin || origin->type != "IFCCARTESIANPOINT") return std::nullopt;
    const auto world = point_record(*origin, count, limits);
    if (std::abs(world.x) > kTolerance || std::abs(world.y) > kTolerance || std::abs(world.z) > kTolerance)
        return std::nullopt;
    if (parsed.coordinate_operations) return std::nullopt;
    std::vector<IfcNativeMesh> result;
    for (const auto id : list_references(rep_fields[3], count, limits)) {
        const auto* item = find_record(parsed, id);
        if (!item || item->type != "IFCTRIANGULATEDFACESET") return std::nullopt;
        const auto mesh_fields = split_top_level(item->args, count, limits);
        require(mesh_fields.size() == 5);
        if (mesh_fields[1] != "$" || mesh_fields[2] != ".T." || mesh_fields[4] != "$")
            return std::nullopt;
        const auto points_id = reference(mesh_fields[0]);
        require(points_id.has_value());
        const auto* points = find_record(parsed, *points_id);
        require(points && points->type == "IFCCARTESIANPOINTLIST3D");
        const auto point_fields = split_top_level(points->args, count, limits);
        require(point_fields.size() == 1);
        IfcNativeMesh mesh;
        for (const auto& point : split_top_level(inner_list(point_fields[0]), count, limits)) {
            require(++vertices <= limits.max_mesh_vertices);
            const auto coordinates = split_top_level(inner_list(point), count, limits);
            require(coordinates.size() == 3);
            mesh.vertices.push_back({number<double>(coordinates[0]), number<double>(coordinates[1]),
                                     number<double>(coordinates[2])});
        }
        require(mesh.vertices.size() >= 3);
        for (const auto& triangle : split_top_level(inner_list(mesh_fields[3]), count, limits)) {
            require(++triangles <= limits.max_mesh_triangles);
            const auto indices = split_top_level(inner_list(triangle), count, limits);
            require(indices.size() == 3);
            std::array<std::size_t, 3> values{};
            for (std::size_t i = 0; i < 3; ++i) {
                const auto value = number<std::size_t>(indices[i]);
                require(value > 0 && value <= mesh.vertices.size());
                values[i] = value - 1;
            }
            require(values[0] != values[1] && values[0] != values[2] && values[1] != values[2]);
            mesh.triangles.push_back(values);
        }
        require(!mesh.triangles.empty());
        result.push_back(std::move(mesh));
    }
    if (result.empty()) return std::nullopt;
    if (product.type == "IFCDOOR" || product.type == "IFCWINDOW" ||
        product.type == "IFCROOF" || product.type == "IFCSPACE" || product.type == "IFCSTAIR" ||
        product.type == "IFCRAILING") {
        const auto frame = product_frame(parsed, fields, count, limits);
        if (!frame) return std::nullopt;
        for (auto& mesh : result)
            for (auto& point : mesh.vertices) {
                const auto world_vertex = world_point(*frame, {point[0],point[1],point[2]});
                if (!bounded_point(world_vertex)) return std::nullopt;
                point = {world_vertex.x,world_vertex.y,world_vertex.z};
            }
    } else if (fields[5] != "$") {
        // Native wall and void mesh exports still use world coordinates under
        // identity placement. Their admission remains deliberately unchanged.
        const auto placement = reference(fields[5]); require(placement.has_value());
        bool rotated = false; std::set<int> visited;
        const auto translation = placement_translation(parsed, *placement, visited, rotated, count, limits);
        if (rotated || std::abs(translation.x) > kTolerance || std::abs(translation.y) > kTolerance ||
            std::abs(translation.z) > kTolerance) return std::nullopt;
    }
    return result;
}

bool native_mesh_role(const Json& properties, std::string_view role) {
    const auto tag = properties.find("_vertex_ifc_mesh");
    return tag != properties.end() && tag->is_object() && tag->contains("version") &&
        tag->at("version").is_number_integer() && tag->at("version") == 1 &&
        tag->contains("role") && tag->at("role").is_string() &&
        tag->at("role").get<std::string>() == role && tag->contains("max_deviation_m") &&
        tag->at("max_deviation_m").is_number() && tag->at("max_deviation_m").get<double>() ==
        ifc_native_mesh_deviation_m;
}

#ifdef SKETCH_IFC_NATIVE_GEOMETRY
bool matching_meshes(const std::vector<IfcNativeMesh>& source, const std::vector<IfcNativeMesh>& expected);

struct NativeReconstructionLedger {
    project_import_detail::GeometryBudget geometry;
    std::size_t vertices;
    std::size_t triangles;
    std::size_t attempts{1024};

    [[noreturn]] static void exhausted() {
        throw std::invalid_argument("ifc_native_reconstruction_budget_exceeded");
    }
    void begin_attempt() {
        if (!attempts) exhausted();
        --attempts; // Failed or mismatching carriers never refund work.
    }
    std::size_t reserve(const Entity& candidate, const Entity* host = nullptr) {
        if (candidate.type == "stair" || candidate.type == "railing") {
            const auto work = stair_railing_work(candidate, host);
            geometry.charge(work);
            for (int i=0; i<3; ++i) geometry.charge_cross(work);
            if (work > std::min(vertices, triangles) / 64) exhausted();
            const auto storage = work * 64;
            vertices -= storage; triangles -= storage;
            return storage;
        }
        // Analytical validators charge the shared ledger before topology work.
        // Keep charges already consumed even if later semantic checks fail.
        if (candidate.type == "roof") project_import_detail::validate_roof(candidate, geometry);
        else project_import_detail::validate_room(candidate, geometry);
        const auto available = std::min(vertices, triangles) / 64;
        std::size_t work = 0;
        const auto charge = [&](std::size_t count) {
            if (work > available || count > available - work) exhausted();
            work += count;
        };
        if (candidate.type == "roof") {
            const auto& p = candidate.properties;
            const auto form = p.at("form").get<std::string>();
            charge(form == "sloped_roof_panel" ? 1 : form == "gable_roof" ? 2 : 4);
            if (p.contains("roof_openings")) charge(p.at("roof_openings").size() * 4);
        } else {
            const auto charge_boundary = [&](const Json& boundary) {
                charge(boundary.size());
                for (const auto& edge : boundary) {
                    const auto angle = std::abs(edge.at("sweep_radians").get<double>());
                    if (angle <= 1e-7) continue;
                    const auto& a = edge.at("start"); const auto& b = edge.at("end");
                    const auto radius = std::hypot(b[0].get<double>() - a[0].get<double>(),
                        b[1].get<double>() - a[1].get<double>()) / (2 * std::abs(std::sin(angle * .5)));
                    const auto step = 2 * std::acos(std::clamp(1 - ifc_native_mesh_deviation_m * .5 / radius, -1.0, 1.0));
                    const auto needed = std::ceil(angle / step);
                    if (!std::isfinite(needed) || needed < 1 || needed > static_cast<double>(available - work)) exhausted();
                    charge(static_cast<std::size_t>(needed));
                }
            };
            const auto& p = candidate.properties;
            charge_boundary(p.at(p.contains("boundary") ? "boundary" : "segments"));
            if (p.contains("holes")) for (const auto& hole : p.at("holes")) charge_boundary(hole);
        }
        // Reserve a conservative per-edge/station/cut allowance before the
        // kernel, and pass that allowance as its actual tessellation ceiling.
        // Cumulative failed regeneration cannot consume a fresh exchange cap.
        const auto storage = work * 64;
        vertices -= storage; triangles -= storage;
        return storage;
    }
};

std::optional<Entity> reconstructed_native_stair_or_railing(const ParsedStep& parsed,
    const StepRecord& record, const Json& metadata, const std::vector<IfcNativeMesh>& meshes,
    std::size_t& count, const IfcExchangeLimits& limits, NativeReconstructionLedger& ledger,
    const Entity* host, const Json* host_metadata) {
    const auto type = record.type == "IFCSTAIR" ? "stair" : "railing";
    if (!native_mesh_role(metadata, type)) return std::nullopt;
    const auto source = metadata.find("_vertex_ifc_entity");
    if (source == metadata.end() || !source->is_object() || source->size() != 6 ||
        !source->contains("version") || !source->at("version").is_number_integer() || source->at("version") != 1 ||
        !source->contains("id") || !source->at("id").is_string() ||
        !source->contains("type") || source->at("type") != type ||
        !source->contains("required") || !source->at("required").is_boolean() ||
        !source->contains("properties") || !source->at("properties").is_object() ||
        !source->contains("extensions") || !source->at("extensions").is_object()) return std::nullopt;
    const auto fields = split_top_level(record.args, count, limits);
    if (fields.size() != 9 || fields[4] != "$" || fields[7] != "$" || fields[8] != ".NOTDEFINED.")
        return std::nullopt;
    auto properties = metadata;
    properties.erase("_vertex_ifc_mesh"); properties.erase("_vertex_ifc_entity");
    properties.erase("_vertex_ifc_host");
    auto authored = source->at("properties"), resolved = properties;
    const auto placement = authored.find("vertical_placement");
    if (placement != authored.end() && placement->is_object() && placement->value("mode", std::string{}) == "level") {
        if (!authored.contains("base_position_m") || !resolved.contains("base_position_m") ||
            !authored.at("base_position_m").is_array() || !resolved.at("base_position_m").is_array() ||
            authored.at("base_position_m").size() != 3 || resolved.at("base_position_m").size() != 3)
            return std::nullopt;
        authored.erase("vertical_placement"); resolved.erase("vertical_placement");
        authored["base_position_m"][2] = 0; resolved["base_position_m"][2] = 0;
    }
    if (authored != resolved) return std::nullopt;
    const bool hosted = properties.contains("host");
    if (hosted ? !host || !host_metadata || !metadata.contains("_vertex_ifc_host") ||
        metadata.at("_vertex_ifc_host") != *host_metadata : host || metadata.contains("_vertex_ifc_host"))
        return std::nullopt;
    if (hosted) {
        const auto& original_rail = source->at("properties");
        const auto& original_stair = host_metadata->at("_vertex_ifc_entity").at("properties");
        const auto portable = [](const Json& p, const char* key) {
            if (!p.contains(key) || !p.at(key).is_string()) return false;
            const auto& id = p.at(key).get_ref<const std::string&>();
            return !id.empty() && id.size() <= 128 && std::all_of(id.begin(),id.end(),[](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == ':';
            });
        };
        for (const auto* key : {"property_id","building_id","floor_id","layer_id"})
            if (!portable(original_rail,key) || !portable(original_stair,key)) return std::nullopt;
        for (const auto* key : {"property_id","building_id","floor_id"})
            if (original_rail.at(key) != original_stair.at(key)) return std::nullopt;
        const bool rail_phase = original_rail.contains("phase_id"), stair_phase = original_stair.contains("phase_id");
        if (rail_phase != stair_phase || (rail_phase && (!portable(original_rail,"phase_id") ||
            !portable(original_stair,"phase_id") || original_rail.at("phase_id") != original_stair.at("phase_id"))))
            return std::nullopt;
        if (original_rail.contains("vertical_placement")) return std::nullopt;
    }
    Entity candidate{source->at("id").get<std::string>(), type, std::move(properties), false, source->at("extensions")};
    detach_native_context(candidate.properties);
    candidate.properties.erase("vertical_placement");
    const auto storage = ledger.reserve(candidate, host);
    // Validate captured authoring as well as resolved physical parameters.
    // Detaching opaque context cannot hide malformed original stair topology,
    // level-connection syntax or rail host fields from the canonical decoder.
    if (candidate.type == "stair") (void)decode_stair_properties(candidate.id, source->at("properties"));
    else (void)decode_railing_properties(candidate.id, source->at("properties"));
    const auto expected = candidate.type == "stair" ? ifc_native_stair_mesh(candidate, storage, storage)
        : ifc_native_railing_mesh(candidate, host, storage, storage);
    if (!matching_meshes(meshes, expected)) return std::nullopt;
    candidate.extensions["ifc_source"] = {{"record_id",record.id},{"record_type",record.type},{"arguments",record.args}};
    candidate.extensions["ifc_vertex_properties"] = metadata;
    const auto detached_document = Document::create(project_import_detail::detached_ifc_validation_entities(
        host ? std::vector<Entity>{*host,candidate} : std::vector<Entity>{candidate}));
    if (!detached_document.snapshot().is_editable()) return std::nullopt;
    return candidate;
}

std::optional<Entity> reconstructed_native_roof_or_room(const ParsedStep& parsed,
    const StepRecord& record, const Json& metadata, const std::vector<IfcNativeMesh>& meshes,
    std::size_t& count, const IfcExchangeLimits& limits, NativeReconstructionLedger& ledger) {
    const bool roof = record.type == "IFCROOF";
    if (!native_mesh_role(metadata, roof ? "roof" : "room")) return std::nullopt;
    ledger.begin_attempt();
    const auto source = metadata.find("_vertex_ifc_entity");
    if (source == metadata.end() || !source->is_object() || source->size() != 5 ||
        !source->contains("version") || !source->at("version").is_number_integer() || source->at("version") != 1 ||
        !source->contains("type") || source->at("type") != (roof ? "roof" : "room") ||
        !source->contains("required") || !source->at("required").is_boolean() ||
        !source->contains("properties") || !source->at("properties").is_object() ||
        !source->contains("extensions") || !source->at("extensions").is_object()) return std::nullopt;
    const auto fields = split_top_level(record.args, count, limits);
    if (roof ? fields.size() != 9 || fields[8] != roof_enum(metadata)
             : fields.size() != 11 || fields[8] != ".ELEMENT." || fields[9] != ".INTERNAL." || fields[10] != "$")
        return std::nullopt;
    auto properties = metadata;
    properties.erase("_vertex_ifc_mesh");
    properties.erase("_vertex_ifc_entity");
    auto authored_properties = source->at("properties");
    auto resolved_properties = properties;
    const auto placement = authored_properties.find("vertical_placement");
    if (placement != authored_properties.end() && placement->is_object() &&
        placement->value("mode",std::string{}) == "level") {
        // Original level-relative Z remains retained source data. Only the
        // already resolved Z may differ; all other authored facts must agree.
        authored_properties.erase("vertical_placement"); resolved_properties.erase("vertical_placement");
        if (roof) {
            for (auto* value : {&authored_properties,&resolved_properties}) {
                if (!value->contains("base_position_m") || !value->at("base_position_m").is_array() ||
                    value->at("base_position_m").size() != 3 || !value->at("base_position_m").at(2).is_number()) return std::nullopt;
                (*value)["base_position_m"][2] = 0;
            }
        } else {
            for (auto* value : {&authored_properties,&resolved_properties}) {
                if (value->contains("elevation_m")) (*value)["elevation_m"] = 0;
                if (value->contains("elevation")) (*value)["elevation"] = 0;
            }
        }
    }
    if (authored_properties != resolved_properties) return std::nullopt;
    Entity candidate{"ifc-" + std::to_string(record.id), roof ? "roof" : "room", properties,
        false, source->at("extensions")};
    // Charge shared analytical and construction work before entering the
    // kernel, including candidates that will fail the exact mesh comparison.
    detach_native_context(candidate.properties);
    const auto storage = ledger.reserve(candidate);
    const auto expected = roof ? ifc_native_roof_mesh(candidate, storage, storage)
                               : ifc_native_room_mesh(candidate, storage, storage);
    if (!matching_meshes(meshes, expected)) return std::nullopt;
    // A resolved world-space import has no reconstructed native level graph.
    // Preserve its original placement/context in the source envelope, while
    // avoiding a second application of that level offset in the imported model.
    candidate.extensions["ifc_source"] = {{"record_id",record.id},{"record_type",record.type},{"arguments",record.args}};
    candidate.extensions["ifc_vertex_properties"] = metadata;
    // Retained metadata cannot bypass the ordinary document boundary (for
    // example reserved extensions or unsupported/dangling active references).
    if (!Document::create({candidate}).snapshot().is_editable()) return std::nullopt;
    return candidate;
}

bool matching_meshes(const std::vector<IfcNativeMesh>& source, const std::vector<IfcNativeMesh>& expected) {
    if (source.size() != expected.size()) return false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i].vertices.size() != expected[i].vertices.size() ||
            source[i].triangles != expected[i].triangles) return false;
        for (std::size_t j = 0; j < source[i].vertices.size(); ++j)
            for (std::size_t axis = 0; axis < 3; ++axis)
                if (std::abs(source[i].vertices[j][axis] - expected[i].vertices[j][axis]) > kTolerance)
                    return false;
    }
    return true;
}
#endif

bool same_native_host(const Entity& host, const Json& original) {
    for (const auto* key : {"baseline", "thickness_m", "height_m", "elevation_m"}) {
        if (!original.contains(key) || !host.properties.contains(key) ||
            original.at(key) != host.properties.at(key)) return false;
    }
    return true;
}

bool same_native_opening(const Entity& opening, const Json& metadata) {
    for (const auto* key : {"offset_m", "width_m", "sill_m", "height_m"}) {
        if (!metadata.contains(key) || !metadata.at(key).is_number() ||
            !opening.properties.contains(key) || std::abs(metadata.at(key).get<double>() -
                opening.properties.at(key).get<double>()) > kTolerance) return false;
    }
    return true;
}

} // namespace

IfcProjectExportResult export_project_ifc(const DocumentSnapshot& document,
                                          const IfcExchangeLimits& limits) {
    validate_limits(limits);
    IfcProjectExportResult result;
    const bool authored_spatial=std::any_of(document.entities().begin(),document.entities().end(),[](const auto& item){
        return item.second.type=="property";
    });
    ExportContext context(limits,authored_spatial);
    for (const auto& [id, entity] : document.entities()) {
        (void)id;
        if (entity.type == "opening" && entity.properties.is_object()) {
            const auto host = entity.properties.find("wall_id");
            if (host != entity.properties.end() && host->is_string())
                context.hosted_openings[host->get<std::string>()].push_back(&entity);
        }
    }
    std::vector<std::string> site_ids;
    site_ids.reserve(document.entities().size());
    // Annotation owners are containers of independently scoped children, not
    // physical model owners. IFC currently retains their exact inert source only.
    for (const auto& [id,e]:document.entities())
        if (e.type != "annotation_state") site_ids.push_back(id);
    try { context.site_placements=resolve_site_presentations(document,site_ids); }
    catch (const std::exception&) {
        add_diagnostic(result.diagnostics,{},"PROJECT","site_frame_batch_not_exported");
    }
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    try {
        AssemblyExpansionBudget assembly_budget;
        context.assembly_expansions=expand_document_assembly_instances(document.entities(),assembly_budget);
    } catch (const std::exception&) {
        add_diagnostic(result.diagnostics,{},"PROJECT","independent_assembly_expansion_not_exported");
    }
#endif
    export_spatial_hierarchy(document,context,result.diagnostics);
    std::set<std::string,std::less<>> unsupported_join_members;
    for (const auto& [id,e]:document.entities()) if (e.type=="wall_join" || e.type=="roof_join") {
        (void)id;
        const auto members=e.properties.find(e.type=="wall_join" ? "wall_ids" : "roof_ids");
        if (members!=e.properties.end() && members->is_array())
            for (const auto& member:*members) if (member.is_string()) unsupported_join_members.insert(member.get<std::string>());
    }
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    context.fresh_stair_proofs = fresh_stair_export_clusters(document);
#endif
#ifdef SKETCH_PHYSICAL_ROOMS
    const auto physical_rooms = physical_wall_room_checks(document);
#endif
    // Joins publish before members/openings so member aliases and void ownership
    // do not depend on source-ID lexical order. Failed joins leave all aliases
    // absent and their complete source members remain inert references.
    std::vector<const Entity*> export_order;
    for (const auto& [id, entity] : document.entities()) {
        (void)id;
        if (entity.type == "wall_join" || entity.type == "roof_join") export_order.push_back(&entity);
    }
    for (const auto& [id, entity] : document.entities()) {
        (void)id;
        if (entity.type != "wall_join" && entity.type != "roof_join") export_order.push_back(&entity);
    }
    for (const auto* source_entity : export_order) {
        const auto& entity = *source_entity;
        const auto& id = entity.id;
        context.authored_entity=&entity;
        context.presentation={}; context.placement=context.world_placement;
        if (entity.type == "annotation_state") {
            // No child geometry is emitted here. Never infer one frame from the
            // owner or a referenced target; any future child route must use the
            // typed annotation-child resolver and each child's own layer.
            context.default_hierarchy();
            export_native_reference(entity, context, result.diagnostics);
            continue;
        }
        bool placed=false;
        try {
            context.presentation=context.site_placements.at(id);
            const auto& drawing=context.presentation.drawing_context;
            const auto spatial=!drawing.floor_id.empty() ? drawing.floor_id :
                !drawing.building_id.empty() ? drawing.building_id : drawing.property_id;
            if (!spatial.empty()) {
                require(context.spatial_ids.contains(spatial));
                context.storey=context.spatial_ids.at(spatial);
            } else if (authored_spatial) {
                const auto site=std::find_if(document.entities().begin(),document.entities().end(),[&](const auto& item){
                    return item.second.type=="property" && context.spatial_ids.contains(item.first);
                });
                if (site!=document.entities().end()) context.storey=context.spatial_ids.at(site->first);
                else context.default_hierarchy();
            } else context.default_hierarchy();
            const auto& t=context.presentation.forward;
            if (std::abs(t.translation_m.x)>kTolerance || std::abs(t.translation_m.y)>kTolerance ||
                std::abs(t.translation_m.z)>kTolerance || std::abs(t.rotation_radians)>kTolerance)
                context.placement=context.rigid_placement(t);
            placed=true;
        } catch (const std::exception&) {
            add_diagnostic(result.diagnostics,id,entity.type,"site_frame_not_exported");
            // This identity placement is only for the inert source carrier.
            // No active source geometry can pass the refused frame below.
            context.presentation={}; context.default_hierarchy();
        }
        const PhysicalWallRoomCheck* physical_room = nullptr;
#ifdef SKETCH_PHYSICAL_ROOMS
        if (const auto found = physical_rooms.find(id); found != physical_rooms.end()) physical_room = &found->second;
#endif
        if (placed) {
            const auto host=entity.type=="opening" ? entity.properties.value("wall_id",std::string{}) : std::string{};
            if (unsupported_join_members.contains(id)) {
                if (!context.product_ids.contains(id))
                    add_diagnostic(result.diagnostics,id,entity.type,"joined_member_geometry_withheld");
            } else if (unsupported_join_members.contains(host) && !context.product_ids.contains(host))
                add_diagnostic(result.diagnostics,id,entity.type,"joined_host_geometry_withheld");
            else {
                // A manufactured opening owns its void and fill as one export.
                // Fill tessellation can refuse after the void has been written;
                // rewind that occurrence before retaining its inert source.
                const auto checkpoint = context.builder.checkpoint();
                const auto ordinal = context.ordinal, vertices = context.mesh_vertices,
                    triangles = context.mesh_triangles, metadata_bytes = context.retained_metadata_charge;
                const auto diagnostic_count = result.diagnostics.size(), opening_links = context.opening_host_links.size();
                const auto contained = context.contained_products.find(context.storey);
                const bool had_containment = contained != context.contained_products.end();
                const auto contained_count = had_containment ? contained->second.size() : 0;
                const auto prior_product = context.product_ids.find(id);
                const auto prior_id = prior_product == context.product_ids.end() ? std::optional<int>{} : prior_product->second;
                try {
                    const auto resolved=resolve_vertical_placement(document,entity);
                    export_product(document,resolved,context,result.diagnostics,physical_room);
                } catch (const std::exception&) {
                    if (entity.type == "opening") {
                        context.builder.rollback(checkpoint);
                        context.ordinal = ordinal; context.mesh_vertices = vertices; context.mesh_triangles = triangles;
                        context.retained_metadata_charge = metadata_bytes;
                        context.opening_host_links.resize(opening_links);
                        if (had_containment) context.contained_products.at(context.storey).resize(contained_count);
                        else context.contained_products.erase(context.storey);
                        if (prior_id) context.product_ids[id] = *prior_id;
                        else context.product_ids.erase(id);
                        result.diagnostics.resize(diagnostic_count);
                    }
                    add_diagnostic(result.diagnostics,id,entity.type,"local_geometry_not_exported");
                }
            }
        }
        // Retain the native source descriptor as well as the interoperable
        // footprint, including when stale geometry is withheld.
        if (entity.type == "room_boundary" && entity.extensions.contains("physical_wall_room") && context.product_ids.contains(id))
            export_native_reference(entity, context, result.diagnostics);
        if (!context.product_ids.contains(id) &&
            (entity.required || (!result.diagnostics.empty() && result.diagnostics.back().source_id == id) ||
             entity.type == "wall" || entity.type == "slab" ||
             entity.type == "opening" || entity.type == "roof" || entity.type == "room" ||
             entity.type == "stair" || entity.type == "railing" ||
             entity.type == "ifc_reference" ||
             entity.type == "building" || entity.type == "floor" || entity.type == "property"))
            export_native_reference(entity, context, result.diagnostics);
    }
    for (const auto& [rail_id, stair_id] : context.railing_host_links) {
        const auto rail = context.product_ids.find(rail_id);
        const auto stair = context.product_ids.find(stair_id);
        if (rail == context.product_ids.end()) continue;
        if (stair == context.product_ids.end()) {
            // Keep the rail's own context, even when its host was withheld.
            const auto& p=context.site_placements.at(rail_id);
            const auto floor=context.spatial_ids.find(p.drawing_context.floor_id);
            if (floor!=context.spatial_ids.end()) context.storey=floor->second;
            else context.default_hierarchy();
            context.contain(rail->second);
            add_diagnostic(result.diagnostics, rail_id, "railing", "native_railing_host_not_exported");
        } else context.aggregate(stair->second, rail->second, "stair-railing:" + rail_id);
    }
    for (const auto& [spatial_id, contained] : context.contained_products) if (!contained.empty()) {
        std::string products;
        for (const auto id : contained) {
            if (!products.empty()) products += ',';
            products += ref(id);
        }
        context.builder.add("IFCRELCONTAINEDINSPATIALSTRUCTURE",
            context.root("containment:"+std::to_string(spatial_id), "") + ",(" + products + ")," + ref(spatial_id));
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
    const auto supported_units = declared_metre_units(parsed, argument_count, limits);
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    const auto native_project_units = metre_units(parsed, argument_count, limits);
    NativeReconstructionLedger native_ledger{{0, 0, "ifc_native_reconstruction_budget_exceeded"},
        limits.max_mesh_vertices, limits.max_mesh_triangles};
#endif
    std::map<int, std::vector<IfcNativeMesh>> meshes_by_id;
    std::size_t mesh_vertices = 0, mesh_triangles = 0;
    for (const auto& record : parsed.records) {
        if (!is_product(record.type)) continue;
        if (auto mesh = product_meshes(parsed, record, argument_count, limits, mesh_vertices, mesh_triangles))
            meshes_by_id.emplace(record.id, std::move(*mesh));
    }
    if (!supported_units) add_diagnostic(result.diagnostics, {}, "PROJECT", "length_units_not_reconstructed");
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    // Decode stairs first regardless of STEP order. A rail can only bind to a
    // geometrically proved stair through one actual unambiguous IFC aggregate.
    std::map<int, std::vector<int>> aggregate_parents;
    for (const auto& relation : parsed.records) if (relation.type == "IFCRELAGGREGATES") {
        const auto fields = split_top_level(relation.args, argument_count, limits);
        require(fields.size() == 6);
        const auto parent = reference(fields[4]); require(parent.has_value());
        for (const auto child : list_references(fields[5], argument_count, limits))
            aggregate_parents[child].push_back(*parent);
    }
    std::map<int, Entity> admitted_stairs, admitted_rails;
    std::set<std::string> admitted_native_ids;
    for (const auto kind : {"IFCSTAIR", "IFCRAILING"}) for (const auto& record : parsed.records) {
        if (record.type != kind || !metadata_by_id.contains(record.id)) continue;
        const auto& metadata = metadata_by_id.at(record.id);
        if (!metadata.contains("_vertex_ifc_mesh")) continue;
        bool recovered = false;
        try {
            native_ledger.begin_attempt();
            const Entity* host = nullptr;
            const Json* host_metadata = nullptr;
            bool link_valid = true;
            const auto parents = aggregate_parents.find(record.id);
            if (record.type == "IFCRAILING") {
                if (metadata.contains("host")) {
                    link_valid = parents != aggregate_parents.end() && parents->second.size() == 1 &&
                        admitted_stairs.contains(parents->second[0]);
                    if (link_valid) {
                        const auto id = parents->second[0];
                        host = &admitted_stairs.at(id); host_metadata = &metadata_by_id.at(id);
                    }
                } else link_valid = parents == aggregate_parents.end();
            }
            if (native_project_units && link_valid && meshes_by_id.contains(record.id)) {
                if (auto candidate = reconstructed_native_stair_or_railing(parsed, record, metadata,
                    meshes_by_id.at(record.id), argument_count, limits, native_ledger, host, host_metadata)) {
                    if (admitted_native_ids.insert(candidate->id).second) {
                        if (record.type == "IFCSTAIR") admitted_stairs.emplace(record.id, std::move(*candidate));
                        else admitted_rails.emplace(record.id, std::move(*candidate));
                        recovered = true;
                    }
                }
            }
        } catch (const std::exception& error) {
            if (std::string_view(error.what()) == "ifc_native_reconstruction_budget_exceeded" ||
                std::string_view(error.what()) == "ifc_mesh_budget_exceeded")
                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                    "native_stair_or_railing_reconstruction_budget_exceeded");
        }
        if (!recovered) add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
            native_project_units ? "native_stair_or_railing_geometry_metadata_inconsistent" :
                "native_project_length_units_not_reconstructed");
    }
    std::map<std::string, std::string> stair_identity_map;
    for (auto& [id, stair] : admitted_stairs) {
        stair_identity_map.emplace(stair.id, "ifc-" + std::to_string(id));
        stair.id = "ifc-" + std::to_string(id);
    }
    for (auto& [id, rail] : admitted_rails) {
        rail.id = "ifc-" + std::to_string(id);
        if (rail.properties.contains("host")) {
            auto& h = rail.properties["host"];
            const auto original = h.at("stair_id").get<std::string>();
            // v1 flights use the stair identity; v2 child identities stay exact.
            if (h.contains("flight_id") && h.at("flight_id") == original)
                h["flight_id"] = stair_identity_map.at(original);
            h["stair_id"] = stair_identity_map.at(original);
        }
    }
#endif
    for (const auto& record : parsed.records) {
        if (is_product(record.type)) {
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
            if (admitted_stairs.contains(record.id) || admitted_rails.contains(record.id)) {
                auto candidate = admitted_stairs.contains(record.id) ? admitted_stairs.at(record.id) : admitted_rails.at(record.id);
                auto context = metadata_by_id.at(record.id);
                if (detach_native_context(context) || context.erase("vertical_placement") != 0 ||
                    context.at("_vertex_ifc_entity").at("required").get<bool>())
                    add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                        "native_context_retained_not_reconstructed");
                result.entities.push_back(std::move(candidate));
                continue;
            }
            if (record.type == "IFCROOF" || record.type == "IFCSPACE") {
                bool reconstructed = false;
                if (!native_project_units && metadata_by_id.contains(record.id) &&
                    native_mesh_role(metadata_by_id.at(record.id), record.type == "IFCROOF" ? "roof" : "room"))
                    add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                        "native_project_length_units_not_reconstructed");
                if (native_project_units && meshes_by_id.contains(record.id) && metadata_by_id.contains(record.id)) {
                    try {
                        if (auto candidate = reconstructed_native_roof_or_room(parsed, record,
                            metadata_by_id.at(record.id), meshes_by_id.at(record.id), argument_count, limits, native_ledger)) {
                            const auto& native = metadata_by_id.at(record.id);
                            auto context = native;
                            if (detach_native_context(context) ||
                                native.at("_vertex_ifc_entity").at("required").get<bool>())
                                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                                    "native_context_retained_not_reconstructed");
                            result.entities.push_back(std::move(*candidate));
                            reconstructed = true;
                        }
                    } catch (const std::exception& error) {
                        if (std::string_view(error.what()) == "ifc_native_reconstruction_budget_exceeded" ||
                            std::string_view(error.what()) == "ifc_mesh_budget_exceeded")
                            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                                "native_roof_or_room_reconstruction_budget_exceeded");
                    }
                }
                if (reconstructed) continue;
                if (metadata_by_id.contains(record.id) &&
                    native_mesh_role(metadata_by_id.at(record.id), record.type == "IFCROOF" ? "roof" : "room"))
                    add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                        "native_roof_or_room_geometry_metadata_inconsistent");
            }
            if (supported_units && record.type == "IFCWALL" && meshes_by_id.contains(record.id) &&
                metadata_by_id.contains(record.id) && native_mesh_role(metadata_by_id.at(record.id), "wall")) {
                const auto& metadata = metadata_by_id.at(record.id);
                try {
                    Entity candidate{"ifc-" + std::to_string(record.id), "wall", metadata};
                    const auto wall = native_wall(candidate);
                    if (matching_meshes(meshes_by_id.at(record.id), ifc_native_wall_mesh(wall,
                        limits.max_mesh_vertices, limits.max_mesh_triangles))) {
                        Json active{{"baseline", metadata.at("baseline")}, {"thickness_m", wall.thickness},
                            {"height_m", wall.height}, {"elevation_m", wall.elevation},
                            {"classification", "ifc_wall_axis"}, {"ifc_type", record.type},
                            {"ifc_name", product_string(record, 2, argument_count, limits)}};
                        if (wall.slope_rise) active["slope_rise_m"] = *wall.slope_rise;
                        if (wall.top_gradient_m_per_m)
                            active["top_plane"] = wall_top_plane_json(*wall.top_gradient_m_per_m);
                        if (metadata.contains("layers")) {
                            auto layers = parse_wall_layers(metadata.at("layers"), wall.thickness);
                            for (auto& layer : layers) layer.material.reset();
                            active["layers"] = wall_layers_json(layers);
                        }
                        candidate.properties = std::move(active);
                        candidate.extensions = {{"ifc_source", {{"record_id", record.id},
                            {"record_type", record.type}, {"arguments", record.args}}},
                            {"ifc_vertex_properties", metadata}};
                        result.entities.push_back(std::move(candidate));
                        continue;
                    }
                } catch (const std::exception&) { }
                add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                    "native_mesh_metadata_inconsistent");
            }
#endif
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
                    // An extrusion cannot establish a nonflat retained top.
                    // Activate only strictly decoded, geometrically flat plane
                    // metadata; keep incompatible native metadata as evidence.
                    bool supported_top = true;
                    try {
                        Wall decoded{"ifc-" + std::to_string(record.id), *axis, *thickness,
                                     *height, geometry.elevation};
                        const Entity metadata_source{decoded.id,"wall",metadata};
                        std::string top_error;
                        if (!read_document_wall_top_profile(metadata_source,decoded,top_error))
                            throw std::invalid_argument(top_error);
                        validate_wall_semantics(decoded);
                        const auto gradient = wall_top_gradient(decoded);
                        supported_top = gradient.x == 0.0 && gradient.y == 0.0;
                        if (supported_top) {
                            if (decoded.slope_rise) properties["slope_rise_m"] = *decoded.slope_rise;
                            if (decoded.top_gradient_m_per_m)
                                properties["top_plane"] = wall_top_plane_json(*decoded.top_gradient_m_per_m);
                        }
                    } catch (const std::exception&) { supported_top = false; }
                    if (supported_top) entity_type = "wall";
                    else add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                                        "wall_top_plane_not_reconstructed");
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
        if (is_relationship(record.type) && record.type != "IFCRELVOIDSELEMENT" &&
            record.type != "IFCRELFILLSELEMENT") {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "relationship_not_reconstructed");
        } else if (record.type == "IFCPROPERTYSET" || record.type == "IFCPROPERTYSINGLEVALUE" ||
                   record.type == "IFCMATERIAL" || record.type == "IFCMATERIALLAYER" ||
                   record.type == "IFCMATERIALLAYERSET" || record.type == "IFCINDEXEDPOLYCURVE") {
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                          "property_or_geometry_not_reconstructed");
        } else if (!is_structural(record.type) && record.type != "IFCRELVOIDSELEMENT" &&
                   record.type != "IFCRELFILLSELEMENT") {
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
        require(find_record(parsed, *opening)->type == "IFCOPENINGELEMENT");
        const auto host_type = find_record(parsed, *host)->type;
        if (host_type != "IFCWALL" && host_type != "IFCWALLSTANDARDCASE") {
            // IFC voids may legally cut slabs and other elements. Retain that
            // relation for the foreign geometry path without inventing a
            // hosted-wall opening or rejecting the entire valid document.
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                           "non_wall_void_relation_retained");
            continue;
        }
        hosts["ifc-" + std::to_string(*opening)].push_back({"ifc-" + std::to_string(*host), record.id});
    }
    std::set<int> reconstructed_relations;
    for (auto& opening : result.entities) {
        if (opening.properties.value("classification", "") != "ifc_opening" &&
            opening.properties.value("ifc_type", "") != "IFCOPENINGELEMENT") continue;
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
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
        if (host && opening.properties.value("ifc_type", "") == "IFCOPENINGELEMENT" &&
            opening.extensions.contains("ifc_vertex_properties")) {
            const auto& metadata = opening.extensions.at("ifc_vertex_properties");
            const auto source_id = opening.extensions.at("ifc_source").at("record_id").get<int>();
            if (native_mesh_role(metadata, "void") && meshes_by_id.contains(source_id)) {
                try {
                    auto candidate = opening; candidate.properties = metadata;
                    const auto wall = native_wall(*host);
                    const auto cut = native_opening(candidate);
                    if (metadata.contains("_vertex_ifc_host") &&
                        same_native_host(*host, metadata.at("_vertex_ifc_host")) &&
                        metadata.value("wall_id", "") == host->properties.value("ifc_name", "") &&
                        matching_meshes(meshes_by_id.at(source_id), ifc_native_void_mesh(wall, cut,
                            limits.max_mesh_vertices, limits.max_mesh_triangles))) {
                        opening.type = "opening";
                        opening.properties["wall_id"] = host->id;
                        opening.properties["offset_m"] = cut.offset;
                        opening.properties["width_m"] = cut.width;
                        opening.properties["sill_m"] = cut.sill;
                        opening.properties["height_m"] = cut.height;
                        opening.properties["opening_kind"] = "opening";
                        reconstructed_relations.insert(relation->second.front().second);
                        recovered = true;
                    }
                } catch (const std::exception&) { }
            }
        }
#endif
        if (!recovered && baseline && std::abs(baseline->sweep_radians) <= kTolerance &&
            footprint && footprint->size() == 4 && !rotated &&
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
                    if (!metadata.contains("opening_assembly") &&
                        metadata.contains("opening_kind") && metadata["opening_kind"].is_string() &&
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
    std::map<int, std::vector<std::pair<int, int>>> fills;
    std::map<int, std::size_t> fill_use;
    for (const auto& record : parsed.records) {
        if (record.type != "IFCRELFILLSELEMENT") continue;
        const auto fields = split_top_level(record.args, argument_count, limits);
        require(fields.size() == 6);
        const auto void_id = reference(fields[4]), fill_id = reference(fields[5]);
        require(void_id && fill_id && find_record(parsed, *void_id) && find_record(parsed, *fill_id));
        require(find_record(parsed, *void_id)->type == "IFCOPENINGELEMENT" &&
            (find_record(parsed, *fill_id)->type == "IFCDOOR" || find_record(parsed, *fill_id)->type == "IFCWINDOW"));
        fills[*void_id].push_back({*fill_id, record.id});
        ++fill_use[*fill_id];
    }
    std::set<int> reconstructed_fills;
    std::set<int> reconstructed_fill_relations;
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    for (auto& opening : result.entities) {
        if (opening.type != "opening") continue;
        const auto void_id = opening.extensions.at("ifc_source").at("record_id").get<int>();
        const auto links = fills.find(void_id);
        if (links == fills.end() || links->second.size() != 1) continue;
        const auto [fill_id, relation_id] = links->second.front();
        if (fill_use.at(fill_id) != 1 || !metadata_by_id.contains(fill_id) ||
            !metadata_by_id.contains(void_id) || !meshes_by_id.contains(fill_id)) continue;
        const auto& metadata = metadata_by_id.at(fill_id);
        const auto& void_metadata = metadata_by_id.at(void_id);
        try {
            const auto host = std::find_if(result.entities.begin(), result.entities.end(), [&](const auto& candidate) {
                return candidate.id == opening.properties.at("wall_id").get<std::string>() && candidate.type == "wall";
            });
            if (host == result.entities.end() || !native_mesh_role(metadata, "fill") ||
                !metadata.contains("_vertex_ifc_host") || !same_native_host(*host, metadata.at("_vertex_ifc_host")) ||
                metadata.value("wall_id", "") != host->properties.value("ifc_name", "") ||
                void_metadata.value("wall_id", "") != host->properties.value("ifc_name", "") ||
                !same_native_opening(opening, metadata) || !same_native_opening(opening, void_metadata) ||
                !metadata.contains("opening_assembly") || !void_metadata.contains("opening_assembly") ||
                metadata.at("opening_assembly") != void_metadata.at("opening_assembly") ||
                metadata.contains("door_operation") != void_metadata.contains("door_operation") ||
                (metadata.contains("door_operation") && metadata.at("door_operation") != void_metadata.at("door_operation")))
                continue;
            const auto profile = parse_opening_assembly(metadata.at("opening_assembly"));
            Entity candidate = opening; candidate.properties = metadata;
            const auto operation = native_operation(candidate);
            // Swept-void projections on oblique hosts introduce roundoff in
            // station/sill values. After agreement with that independent void
            // geometry is proven above, regenerate using exact source values;
            // even sub-ULP changes can alter native triangulation ordering.
            const auto checked_opening = native_opening(candidate);
            const bool door = profile.kind == OpeningAssemblyKind::door;
            const auto* fill_record = find_record(parsed, fill_id);
            const auto fields = split_top_level(fill_record->args, argument_count, limits);
            const auto frame = product_frame(parsed, fields, argument_count, limits);
            const bool legacy_fixed_window = !door && profile.window_layout == WindowLayoutKind::fixed &&
                metadata.at("opening_assembly").value("version", 0) == 1;
            if (fields.size() != 13 || (door ? fill_record->type != "IFCDOOR" : fill_record->type != "IFCWINDOW") ||
                metadata.value("opening_kind", "") != opening_assembly_kind_name(profile.kind) ||
                void_metadata.value("opening_kind", "") != opening_assembly_kind_name(profile.kind) ||
                (!door && operation) || fields[10] != (door ? ".DOOR." : ".WINDOW.") ||
                (fields[11] != (door ? door_operation_enum(operation) : window_partition_enum(profile)) &&
                 !(legacy_fixed_window && fields[11] == ".NOTDEFINED.")) ||
                fields[12] != (door ? door_operation_label(operation, limits) : window_partition_label(profile, limits)) ||
                std::abs(number<double>(fields[8]) - opening.properties.at("height_m").get<double>()) > kTolerance ||
                !frame || !same_frame(*frame, fill_frame(native_wall(*host), checked_opening, operation)) ||
                std::abs(number<double>(fields[9]) - fill_overall_width(native_wall(*host), checked_opening, *frame)) > kTolerance ||
                !matching_meshes(meshes_by_id.at(fill_id), ifc_native_fill_mesh(native_wall(*host),
                    checked_opening, profile, operation, limits.max_mesh_vertices, limits.max_mesh_triangles)))
                continue;
            for (const auto* key : {"offset_m", "width_m", "sill_m", "height_m"})
                opening.properties[key] = metadata.at(key);
            opening.properties["opening_kind"] = opening_assembly_kind_name(profile.kind);
            opening.properties["opening_assembly"] = metadata.at("opening_assembly");
            if (operation) opening.properties["door_operation"] = metadata.at("door_operation");
            opening.extensions["ifc_fill_source"] = {{"record_id", fill_id}, {"record_type", fill_record->type},
                {"arguments", fill_record->args}, {"relationship_id", relation_id}, {"properties", metadata}};
            reconstructed_fills.insert(fill_id);
            reconstructed_fill_relations.insert(relation_id);
        } catch (const std::exception&) { }
    }
#endif
    std::erase_if(result.entities, [&](const auto& entity) {
        return reconstructed_fills.contains(entity.extensions.at("ifc_source").at("record_id").template get<int>());
    });
    std::erase_if(result.diagnostics, [&](const auto& diagnostic) {
        return std::any_of(reconstructed_fills.begin(), reconstructed_fills.end(), [&](int id) {
            return diagnostic.source_id == "#" + std::to_string(id);
        });
    });
    for (const auto& record : parsed.records)
        if (record.type == "IFCRELFILLSELEMENT" && !reconstructed_fill_relations.contains(record.id))
            add_diagnostic(result.diagnostics, "#" + std::to_string(record.id), record.type,
                "fill_semantics_not_reconstructed");
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    // Reserve the existing candidates' cost first, then admit native additions
    // against one file-wide budget. A declined native carrier stays retained;
    // it cannot make an otherwise usable worker response exceed admission cost.
    project_import_detail::GeometryBudget candidate_budget;
    for (const auto& entity : result.entities) {
        if (entity.type != "slab" && entity.type != "boundary") continue;
        // Reserve analytical work by count, without changing admission of
        // existing conservative generic projections (for example an unbound
        // retraced opening). The isolated-response validator still checks all
        // geometry before transport; native roof/room admission remains strict.
        auto topology_segments = entity.properties.at("boundary").size();
        candidate_budget.charge(topology_segments);
        if (entity.type == "slab") {
            for (const auto& hole : entity.properties.at("holes")) {
                candidate_budget.charge(hole.size());
                topology_segments += hole.size();
            }
            candidate_budget.charge_cross(topology_segments);
        }
    }
    for (const bool rail_pass : {false,true}) for (auto& entity : result.entities) {
        if (rail_pass ? entity.type != "railing" : entity.type != "roof" && entity.type != "room" && entity.type != "stair") continue;
        auto proposed = candidate_budget;
        try {
            if (entity.type == "roof") project_import_detail::validate_roof(entity, proposed);
            else if (entity.type == "room") project_import_detail::validate_room(entity, proposed);
            else {
                const Entity* host = nullptr;
                if (entity.type == "railing" && entity.properties.contains("host")) {
                    const auto host_id = entity.properties.at("host").at("stair_id").get<std::string>();
                    const auto found = std::find_if(result.entities.begin(),result.entities.end(),
                        [&](const auto& e){return e.id == host_id && e.type == "stair";});
                    require(found != result.entities.end()); host = &*found;
                }
                proposed.charge(stair_railing_work(entity,host));
            }
            candidate_budget = proposed;
        } catch (const std::exception&) {
            const auto source = entity.extensions.at("ifc_source");
            const auto metadata = entity.extensions.at("ifc_vertex_properties");
            const auto record_id = source.at("record_id").get<int>();
            const auto kind = source.at("record_type").get<std::string>();
            entity.type = "ifc_reference";
            entity.properties = {{"ifc_name",entity.id},{"ifc_type",kind}};
            entity.extensions = {{"ifc_source",source},{"ifc_vertex_properties",metadata}};
            add_diagnostic(result.diagnostics, "#" + std::to_string(record_id), kind,
                (kind == "IFCSTAIR" || kind == "IFCRAILING") ?
                    "native_stair_or_railing_candidate_budget_exceeded" : "native_roof_or_room_candidate_budget_exceeded");
        }
    }
    try {
        if (!Document::create(project_import_detail::detached_ifc_validation_entities(result.entities)).snapshot().is_editable())
            throw std::invalid_argument("ifc_native_detached_graph_inconsistent");
    } catch (const std::exception&) {
        // A cross-product child/host collision must not escape as an active
        // native graph. Preserve every implicated native carrier as source.
        for (auto& entity : result.entities) if (entity.type == "stair" || entity.type == "railing") {
            const auto source = entity.extensions.at("ifc_source");
            const auto metadata = entity.extensions.at("ifc_vertex_properties");
            entity.type = "ifc_reference";
            entity.properties = {{"ifc_name",entity.id},{"ifc_type",source.at("record_type")}};
            entity.extensions = {{"ifc_source",source},{"ifc_vertex_properties",metadata}};
            add_diagnostic(result.diagnostics,"#"+std::to_string(source.at("record_id").get<int>()),
                source.at("record_type").get<std::string>(),"native_stair_or_railing_detached_graph_inconsistent");
        }
    }
#endif
    result.source_retention_required = !result.diagnostics.empty();
    return result;
}

} // namespace sketch
