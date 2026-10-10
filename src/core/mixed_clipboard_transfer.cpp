#include "sketch/mixed_clipboard_transfer.hpp"

#include "sketch/roof_entity_codec.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
constexpr std::size_t value_limit = 100000;
constexpr std::size_t depth_limit = 64;
constexpr std::size_t member_limit = 1000;
constexpr std::size_t row_limit = 4096;
constexpr std::size_t legacy_limit = 128;
constexpr std::string_view format = "vertex-mixed-clipboard";
constexpr std::string_view ordinary_format = "sketch.document.clipboard";

[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Mixed clipboard: ") + reason);
}
void identity(std::string_view value) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) reject("invalid identity");
}
const std::string& text(const Json& value) {
    if (!value.is_string()) reject("expected an identity string");
    const auto& result = value.get_ref<const std::string&>();
    identity(result);
    return result;
}
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) reject("unsupported closed fields");
    for (const auto* key : expected) if (!value.contains(key)) reject("missing required field");
}
void version_one(const Json& value) {
    if (!value.is_number_integer() || value != 1) reject("unsupported version");
}

// Account the complete compact wire envelope without allocating a serialization
// or copying any source subtree. 32 bytes bounds every finite JSON number.
struct Budget {
    std::size_t bytes{}, values{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("encoded byte budget exceeded");
        bytes += count;
    }
    void node(std::size_t depth) {
        if (depth > depth_limit || ++values > value_limit) reject("JSON complexity budget exceeded");
    }
    void quoted(std::string_view value) {
        add(2);
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c >= 0x80) {
                const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                if (!tail || tail >= value.size() - i) reject("invalid UTF-8 string");
                const auto first = static_cast<unsigned char>(value[i + 1]);
                if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
                    (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) reject("invalid UTF-8 string");
                for (std::size_t j = 1; j <= tail; ++j) {
                    const auto byte = static_cast<unsigned char>(value[i + j]);
                    if (byte < 0x80 || byte > 0xbf) reject("invalid UTF-8 string");
                }
                add(tail + 1); i += tail;
                continue;
            }
            if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') add(2);
            else add(c < 0x20 ? 6 : 1);
        }
    }
    void string(std::string_view value, std::size_t depth) { node(depth); quoted(value); }
    void object(std::initializer_list<const char*> names, std::size_t depth) {
        node(depth); add(2 + (names.size() ? names.size() - 1 : 0));
        for (const auto* name : names) { quoted(name); add(1); }
    }
    void array(std::size_t count, std::size_t depth) {
        node(depth); add(2 + (count ? count - 1 : 0));
    }
    void json(const Json& value, std::size_t depth) {
        node(depth);
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON value");
        if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else if (value.is_object()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); json(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.size() ? value.size() - 1 : 0));
            for (const auto& child : value) json(child, depth + 1);
        } else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
    void entity(const Entity& value, std::size_t depth) {
        object({"id", "type", "properties", "required", "extensions"}, depth);
        string(value.id, depth + 1); string(value.type, depth + 1);
        json(value.properties, depth + 1); node(depth + 1); add(5);
        json(value.extensions, depth + 1);
    }
};

void shape_counts(const MixedClipboardTransfer& value) {
    if (value.corners.size() > 128 || value.catalogs.size() > legacy_limit || value.skylights.size() > member_limit)
        reject("family roster limit exceeded");
    if (value.corners.empty() && !value.catalogs.empty()) reject("corner material pool has no corner members");
    std::size_t dimensions = 0, rows = value.catalogs.size() + value.skylights.size();
    for (const auto& corner : value.corners) {
        if (corner.dimensions.size() > 2048 - dimensions) reject("corner dimension limit exceeded");
        dimensions += corner.dimensions.size(); rows += 5 + corner.dimensions.size();
    }
    if (value.ordinary && value.ordinary->is_object() && value.ordinary->contains("entities") &&
        value.ordinary->at("entities").is_array()) rows += value.ordinary->at("entities").size();
    if (rows > row_limit) reject("aggregate entity row limit exceeded");
}
void preflight(const MixedClipboardTransfer& value) {
    shape_counts(value);
    Budget budget;
    budget.object({"format", "version", "ordinary", "corners", "catalogs", "skylights"}, 0);
    budget.string(format, 1); budget.node(1); budget.add(32);
    if (value.ordinary) budget.json(*value.ordinary, 1);
    else { budget.node(1); budget.add(4); }
    budget.array(value.corners.size(), 1);
    for (const auto& corner : value.corners) {
        budget.object({"owner", "walls", "cuts", "dimensions"}, 2);
        budget.entity(corner.owner, 3);
        budget.array(2, 3); for (const auto& wall : corner.walls) budget.entity(wall, 4);
        budget.array(2, 3); for (const auto& cut : corner.cuts) budget.entity(cut, 4);
        budget.array(corner.dimensions.size(), 3);
        for (const auto& dimension : corner.dimensions) budget.entity(dimension, 4);
    }
    budget.array(value.catalogs.size(), 1);
    for (const auto& catalog : value.catalogs) budget.entity(catalog, 2);
    budget.array(value.skylights.size(), 1);
    for (const auto& source : value.skylights) {
        budget.object({"roof", "opening_id"}, 2);
        budget.entity(source.roof, 3); budget.string(source.opening_id, 3);
    }
}

struct Envelope {
    std::string_view id, type;
    const Json& properties;
    bool required;
    const Json& extensions;
};
Envelope envelope(const Entity& value) {
    return {value.id, value.type, value.properties, value.required, value.extensions};
}
Envelope envelope(const Json& value) {
    keys(value, {"id", "type", "properties", "required", "extensions"});
    if (!value.at("type").is_string() || !value.at("required").is_boolean()) reject("invalid entity envelope fields");
    return {text(value.at("id")), value.at("type").get_ref<const std::string&>(),
        value.at("properties"), value.at("required").get<bool>(), value.at("extensions")};
}
void validate_envelope(const Envelope& value) {
    identity(value.id); identity(value.type);
    if (!value.properties.is_object() || !value.extensions.is_object()) reject("entity properties/extensions must be objects");
    for (const auto* key : {"id", "type", "properties", "required"})
        if (value.extensions.contains(key)) reject("extension shadows an envelope field");
}
bool exact(const Json& a, const Json& b) {
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_object()) {
        for (const auto& [key, child] : a.items()) {
            const auto found = b.find(key);
            if (found == b.end() || !exact(child, *found)) return false;
        }
        return true;
    }
    if (a.is_array()) {
        for (std::size_t i = 0; i < a.size(); ++i) if (!exact(a[i], b[i])) return false;
        return true;
    }
    if (a.is_number_float() && std::signbit(a.get<double>()) != std::signbit(b.get<double>())) return false;
    return a == b;
}
bool exact(const Envelope& a, const Envelope& b) {
    return a.id == b.id && a.type == b.type && a.required == b.required &&
        exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
bool same_fields_except(const Json& a, const Json& b, std::initializer_list<std::string_view> ignored) {
    if (!a.is_object() || !b.is_object()) return false;
    const auto retained = [&](std::string_view key) {
        return std::find(ignored.begin(), ignored.end(), key) == ignored.end();
    };
    for (const auto& [key, value] : a.items()) {
        if (!retained(key)) continue;
        const auto found = b.find(key);
        if (found == b.end() || !exact(value, *found)) return false;
    }
    for (const auto& [key, value] : b.items()) {
        (void)value;
        if (retained(key) && !a.contains(key)) return false;
    }
    return true;
}
// Both producers can retain different referenced subsets from the same catalog.
// Their carrier context remains lane-local. Shared raw definitions must agree;
// a pool is material-only and its compatible catalog dialect grants no authority.
bool material_projection(const Envelope& pool, const Envelope& original) {
    if (pool.id != original.id || pool.type != "assembly_model" || original.type != "assembly_model" ||
        !pool.properties.contains("model") || !original.properties.contains("model")) return false;
    const auto& projected = pool.properties.at("model");
    const auto& source = original.properties.at("model");
    if (!same_fields_except(projected, source, {"schema", "materials", "types", "instances"})) return false;
    const auto dialect = [](const Json& model) {
        if (!model.contains("schema") || !model.at("schema").is_string()) return false;
        const auto& schema = model.at("schema").get_ref<const std::string&>();
        constexpr std::string_view prefix="sketch.assemblies.v";
        return schema.size() == prefix.size()+1 && schema.starts_with(prefix) &&
            schema.back() >= '1' && schema.back() <= '7';
    };
    if (!dialect(projected) || !dialect(source)) return false;
    for (const auto* collection : {"types", "instances"})
        if (!projected.contains(collection) || !projected.at(collection).is_array() ||
            !projected.at(collection).empty() || !source.contains(collection) ||
            !source.at(collection).is_array()) return false;
    if (!projected.contains("materials") || !projected.at("materials").is_array() ||
        !source.contains("materials") || !source.at("materials").is_array()) return false;
    std::map<std::string_view, const Json*, std::less<>> material_rows;
    for (const auto& row : source.at("materials")) {
        if (!row.is_object() || !row.contains("id") || !row.at("id").is_string()) return false;
        if (!material_rows.emplace(text(row.at("id")), &row).second) return false;
    }
    std::set<std::string_view, std::less<>> seen;
    for (const auto& row : projected.at("materials")) {
        if (!row.is_object() || !row.contains("id") || !row.at("id").is_string()) return false;
        const auto& id = text(row.at("id"));
        const auto found = material_rows.find(id);
        if (!seen.insert(id).second || (found != material_rows.end() && !exact(row, *found->second))) return false;
    }
    return true;
}
enum class Role { ordinary, corner_owned, host, material };
struct IdentityRoster {
    struct Entry { Role role; Envelope entity; };
    std::map<std::string_view, Entry, std::less<>> rows;
    std::set<std::string_view, std::less<>> children;
    std::set<std::pair<std::string_view, std::string_view>> selected_children;
    void add(const Envelope& value, Role role) {
        validate_envelope(value);
        if (children.contains(value.id)) reject("entity identity conflicts with a selected skylight");
        const auto [entry, inserted] = rows.emplace(value.id, Entry{role, value});
        if (inserted) return;
        if ((role == Role::host && entry->second.role == Role::ordinary) ||
            (role == Role::ordinary && entry->second.role == Role::host)) {
            if (!exact(entry->second.entity, value)) reject("transported host conflicts with its passive source");
            return;
        }
        if (role == Role::material && entry->second.role == Role::material &&
            (material_projection(value, entry->second.entity) || material_projection(entry->second.entity, value))) return;
        if (entry->second.role != role || (role != Role::host && role != Role::material) ||
            !exact(entry->second.entity, value)) reject("conflicting or duplicate source entity identity");
    }
    void child(std::string_view roof_id, std::string_view id) {
        identity(id);
        if (rows.contains(id) || !selected_children.emplace(roof_id, id).second)
            reject("conflicting or duplicate selected skylight identity");
        children.insert(id);
    }
};

std::size_t ordinary(const Json& value, IdentityRoster& identities) {
    if (!value.is_object() || value.size() < 4 || value.size() > 5 ||
        !value.contains("format") || !value.at("format").is_string() || value.at("format") != ordinary_format ||
        !value.contains("version") || !value.contains("root_id") || !value.contains("entities"))
        reject("unsupported ordinary clipboard contract");
    for (const auto& [key, child] : value.items()) {
        (void)child;
        if (key != "format" && key != "version" && key != "root_id" && key != "root_ids" && key != "entities")
            reject("unknown ordinary clipboard field");
    }
    version_one(value.at("version"));
    const auto& entities = value.at("entities");
    if (!entities.is_array() || entities.empty() || entities.size() > legacy_limit) reject("ordinary entity roster limit exceeded");
    std::set<std::string_view, std::less<>> ids;
    for (const auto& row : entities) {
        const auto entity = envelope(row);
        if (!is_known_entity_type(entity.type) || entity.type == "corner_window" ||
            (entity.type == "opening" && entity.properties.contains("corner_window_id")))
            reject("ordinary row belongs to an unsupported or coordinated family");
        if ((entity.required && entity.type != "measurement_linework") ||
            (entity.type == "measurement_linework" && !entity.required)) reject("ordinary required-entity policy differs from legacy paste");
        if (!ids.insert(entity.id).second) reject("duplicate ordinary entity identity");
        identities.add(entity, entity.type == "assembly_model" ? Role::material : Role::ordinary);
    }
    const auto& first = text(value.at("root_id"));
    if (!ids.contains(first)) reject("ordinary root is absent from the source graph");
    if (!value.contains("root_ids")) return 1;
    const auto& roots = value.at("root_ids");
    if (!roots.is_array() || roots.empty() || roots.size() > legacy_limit) reject("ordinary root roster limit exceeded");
    std::set<std::string_view, std::less<>> selected;
    for (const auto& root : roots) {
        const auto& id = text(root);
        if (!ids.contains(id) || !selected.insert(id).second) reject("missing or duplicate ordinary root");
    }
    return selected.size();
}

void skylight(const RoofOpeningCloneSource& source) {
    identity(source.opening_id);
    const auto object = decode_roof_entity(source.roof);
    std::visit([&](const auto& roof) {
        std::set<std::string_view, std::less<>> ids;
        const RoofOpening* selected = nullptr;
        for (const auto& opening : roof.openings) {
            if (!ids.insert(opening.id).second) reject("duplicate roof child identity");
            if (opening.id == source.opening_id) selected = &opening;
        }
        if (!selected || !selected->skylight) reject("selected child must be an actual profiled skylight");
        // Analytical facet/mouth admission only; never construct native shapes.
        const auto frame = roof_opening_plan_frame(roof, *selected);
        const auto& profile = *selected->skylight;
        if (profile.frame_width <= default_geometry_tolerance_metres ||
            profile.glazing_thickness <= default_geometry_tolerance_metres || profile.curb_height < 0.0 ||
            !std::isfinite(frame.width_surface_scale) || !std::isfinite(frame.depth_surface_scale))
            reject("skylight profile has invalid physical dimensions");
    }, object);
}

Json encode_entity(const Entity& value) {
    return Json{{"id", value.id}, {"type", value.type}, {"properties", value.properties},
        {"required", value.required}, {"extensions", value.extensions}};
}
Entity decode_entity(const Json& value) {
    const auto row = envelope(value); validate_envelope(row);
    return {std::string(row.id), std::string(row.type), row.properties, row.required, row.extensions};
}
void wire_shape(const Json& value) {
    keys(value, {"format", "version", "ordinary", "corners", "catalogs", "skylights"});
    if (!value.at("format").is_string() || value.at("format") != format) reject("unsupported format");
    version_one(value.at("version"));
    if ((!value.at("ordinary").is_null() && !value.at("ordinary").is_object()) ||
        !value.at("corners").is_array() || value.at("corners").size() > 128 ||
        !value.at("catalogs").is_array() || value.at("catalogs").size() > legacy_limit ||
        !value.at("skylights").is_array() || value.at("skylights").size() > member_limit)
        reject("invalid family roster shape");
    std::size_t dimensions = 0;
    std::size_t rows = value.at("catalogs").size() + value.at("skylights").size();
    for (const auto& row : value.at("corners")) {
        keys(row, {"owner", "walls", "cuts", "dimensions"});
        if (!row.at("walls").is_array() || row.at("walls").size() != 2 ||
            !row.at("cuts").is_array() || row.at("cuts").size() != 2 ||
            !row.at("dimensions").is_array() || row.at("dimensions").size() > 2048 - dimensions)
            reject("invalid corner member roster");
        dimensions += row.at("dimensions").size();
        rows += 5 + row.at("dimensions").size();
    }
    if (!value.at("ordinary").is_null()) {
        IdentityRoster identities;
        const auto members = ordinary(value.at("ordinary"), identities);
        if (members + value.at("corners").size() + value.at("skylights").size() > member_limit)
            reject("aggregate semantic member limit exceeded");
        rows += value.at("ordinary").at("entities").size();
    } else if (value.at("corners").size() + value.at("skylights").size() > member_limit)
        reject("aggregate semantic member limit exceeded");
    if (rows > row_limit) reject("aggregate entity row limit exceeded");
}
} // namespace

void validate_mixed_clipboard_transfer(const MixedClipboardTransfer& transfer) {
    preflight(transfer);
    const auto lanes = static_cast<unsigned>(transfer.ordinary.has_value()) +
        static_cast<unsigned>(!transfer.corners.empty()) + static_cast<unsigned>(!transfer.skylights.empty());
    if (lanes < 2) reject("at least two represented family lanes are required");
    IdentityRoster identities;
    const auto ordinary_members = transfer.ordinary ? ordinary(*transfer.ordinary, identities) : 0;
    if (ordinary_members + transfer.corners.size() + transfer.skylights.size() > member_limit)
        reject("aggregate semantic member limit exceeded");
    for (const auto& corner : transfer.corners) {
        identities.add(envelope(corner.owner), Role::corner_owned);
        for (const auto& cut : corner.cuts) identities.add(envelope(cut), Role::corner_owned);
        for (const auto& dimension : corner.dimensions) identities.add(envelope(dimension), Role::corner_owned);
        for (const auto& wall : corner.walls) identities.add(envelope(wall), Role::host);
    }
    std::set<std::string_view, std::less<>> pooled;
    for (const auto& catalog : transfer.catalogs) {
        if (!pooled.insert(catalog.id).second) reject("duplicate pooled material catalog");
        identities.add(envelope(catalog), Role::material);
    }
    for (const auto& source : transfer.skylights) {
        identities.add(envelope(source.roof), Role::host);
        identities.child(source.roof.id, source.opening_id);
        skylight(source);
    }
    if (!transfer.corners.empty()) validate_corner_window_transfer_group(transfer.corners, transfer.catalogs);
}

nlohmann::json encode_mixed_clipboard_transfer(const MixedClipboardTransfer& transfer) {
    validate_mixed_clipboard_transfer(transfer);
    Json result{{"format", format}, {"version", 1}, {"ordinary", transfer.ordinary ? *transfer.ordinary : Json()},
        {"corners", Json::array()}, {"catalogs", Json::array()}, {"skylights", Json::array()}};
    for (const auto& corner : transfer.corners) {
        Json row{{"owner", encode_entity(corner.owner)}, {"walls", Json::array()},
            {"cuts", Json::array()}, {"dimensions", Json::array()}};
        for (const auto& wall : corner.walls) row["walls"].push_back(encode_entity(wall));
        for (const auto& cut : corner.cuts) row["cuts"].push_back(encode_entity(cut));
        for (const auto& dimension : corner.dimensions) row["dimensions"].push_back(encode_entity(dimension));
        result["corners"].push_back(std::move(row));
    }
    for (const auto& catalog : transfer.catalogs) result["catalogs"].push_back(encode_entity(catalog));
    for (const auto& source : transfer.skylights)
        result["skylights"].push_back(Json{{"roof", encode_entity(source.roof)}, {"opening_id", source.opening_id}});
    return result;
}

MixedClipboardTransfer decode_mixed_clipboard_transfer(const nlohmann::json& value) {
    Budget budget; budget.json(value, 0);
    wire_shape(value);
    MixedClipboardTransfer result;
    if (!value.at("ordinary").is_null()) result.ordinary = value.at("ordinary");
    std::size_t dimensions = 0;
    for (const auto& row : value.at("corners")) {
        keys(row, {"owner", "walls", "cuts", "dimensions"});
        if (!row.at("walls").is_array() || row.at("walls").size() != 2 ||
            !row.at("cuts").is_array() || row.at("cuts").size() != 2 ||
            !row.at("dimensions").is_array() || row.at("dimensions").size() > 2048 - dimensions)
            reject("invalid corner member roster");
        dimensions += row.at("dimensions").size();
        CornerWindowTransfer corner{decode_entity(row.at("owner")),
            {decode_entity(row.at("walls")[0]), decode_entity(row.at("walls")[1])},
            {decode_entity(row.at("cuts")[0]), decode_entity(row.at("cuts")[1])}, {}};
        for (const auto& dimension : row.at("dimensions")) corner.dimensions.push_back(decode_entity(dimension));
        result.corners.push_back(std::move(corner));
    }
    for (const auto& catalog : value.at("catalogs")) result.catalogs.push_back(decode_entity(catalog));
    for (const auto& row : value.at("skylights")) {
        keys(row, {"roof", "opening_id"});
        result.skylights.push_back({decode_entity(row.at("roof")), text(row.at("opening_id"))});
    }
    validate_mixed_clipboard_transfer(result);
    return result;
}

MixedClipboardTransfer decode_mixed_clipboard_transfer(std::string_view encoded) {
    if (encoded.empty() || encoded.size() > byte_limit) reject("wire byte budget exceeded");
    std::size_t values = 0;
    const auto callback = [&](int depth, Json::parse_event_t event, Json&) {
        if (depth < 0 || static_cast<std::size_t>(depth) > depth_limit) reject("parser depth budget exceeded");
        if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start ||
            event == Json::parse_event_t::value)
            if (++values > value_limit) reject("parser value budget exceeded");
        return true;
    };
    try { return decode_mixed_clipboard_transfer(Json::parse(encoded.begin(), encoded.end(), callback)); }
    catch (const Json::exception&) { reject("invalid encoded JSON"); }
}

} // namespace sketch
