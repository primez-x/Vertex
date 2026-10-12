#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/phase_assembly_instance_property_edit.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "assembly_instance_property_carry.hpp"

#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_catalog_phase_edit.hpp"
#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/opening_host_geometry.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_store.hpp"
#include "sketch/site_frame.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Operation = AssemblyInstancePropertyPatchOperation;
using Target = AssemblyInstancePropertyEditTarget;
using Intent = AssemblyInstancePropertyEditIntent;
using Path = std::vector<std::string>;
using RootKey = std::tuple<std::string, std::string, std::optional<std::string>>;
constexpr std::size_t target_limit = 1000;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;

[[noreturn]] void reject(const char* message) {
    throw std::invalid_argument(std::string("Assembly instance Properties: ") + message);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
// Bound borrowed wire data before copying, dumping or decoding a source model.
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
        if (depth > 64 || ++nodes > 100000) reject("wire complexity budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON");
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
std::string text(const Json& value, std::size_t limit, bool nonblank) {
    if (!value.is_string()) reject("typed string required");
    const auto& result = value.get_ref<const std::string&>();
    if (result.size() > limit || result.find('\0') != std::string::npos ||
        (nonblank && (result.empty() || std::all_of(result.begin(), result.end(),
            [](unsigned char c) { return std::isspace(c); })))) reject("invalid or oversized typed string");
    return result;
}
std::string identity(const Json& value) {
    const auto id = text(value, 128, true);
    if (!std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid document identity");
    return id;
}
double number(const Json& value, bool positive = false) {
    if (!value.is_number()) reject("numeric SI field required");
    const auto result = value.get<double>();
    if (!std::isfinite(result) || (positive && result <= 0)) reject("invalid finite SI field or positive scale");
    return result;
}
const char* operation_name(Operation operation) {
    switch (operation) {
    case Operation::unchanged: return "unchanged";
    case Operation::set: return "set";
    case Operation::clear: return "clear";
    }
    reject("unknown patch operation");
}
Operation operation(const Json& value) {
    if (value == "unchanged") return Operation::unchanged;
    if (value == "set") return Operation::set;
    if (value == "clear") return Operation::clear;
    reject("unknown patch operation");
}
const char* unit_name(AssemblyQuantityUnit unit) {
    switch (unit) {
    case AssemblyQuantityUnit::count: return "count";
    case AssemblyQuantityUnit::metre: return "m";
    case AssemblyQuantityUnit::square_metre: return "m2";
    case AssemblyQuantityUnit::cubic_metre: return "m3";
    case AssemblyQuantityUnit::kilogram: return "kg";
    }
    reject("unknown quantity unit");
}
Json encode_quantity(const AssemblyQuantityProperty& value) {
    if (!std::isfinite(value.value) || value.value < 0 ||
        (value.unit == AssemblyQuantityUnit::count && std::floor(value.value) != value.value))
        reject("quantity must be nonnegative and counts integral");
    return {{"value", value.value}, {"unit", unit_name(value.unit)}};
}
AssemblyQuantityProperty decode_quantity(const Json& value) {
    fields(value, {"value", "unit"});
    for (const auto unit : {AssemblyQuantityUnit::count, AssemblyQuantityUnit::metre,
        AssemblyQuantityUnit::square_metre, AssemblyQuantityUnit::cubic_metre, AssemblyQuantityUnit::kilogram}) {
        if (value.at("unit") == unit_name(unit)) {
            AssemblyQuantityProperty result{number(value.at("value")), unit};
            (void)encode_quantity(result); return result;
        }
    }
    reject("unknown quantity dimension");
}
template<class T> Json encode_patches(const std::map<std::string, AssemblyInstancePropertyPatch<T>>& patches,
    auto encode) {
    if (patches.size() > 4096) reject("override patch budget exceeded");
    auto result = Json::object();
    for (const auto& [key, patch] : patches) {
        (void)text(Json(key), 1024, true);
        if ((patch.operation == Operation::set) != patch.value.has_value()) reject("only set may carry a value");
        result[key] = {{"operation", operation_name(patch.operation)},
            {"value", patch.value ? encode(*patch.value) : Json(nullptr)}};
    }
    return result;
}
template<class T> std::map<std::string, AssemblyInstancePropertyPatch<T>> decode_patches(const Json& value, auto decode) {
    if (!value.is_object() || value.size() > 4096) reject("invalid override patch budget");
    std::map<std::string, AssemblyInstancePropertyPatch<T>> result;
    for (const auto& [key, row] : value.items()) {
        (void)text(Json(key), 1024, true);
        fields(row, {"operation", "value"});
        AssemblyInstancePropertyPatch<T> patch;
        patch.operation = operation(row.at("operation"));
        if (patch.operation == Operation::set) patch.value = decode(row.at("value"));
        else if (!row.at("value").is_null()) reject("unchanged/clear must carry null");
        result.emplace(key, std::move(patch));
    }
    return result;
}
Json optional(auto value) { return value ? Json(*value) : Json(nullptr); }
bool has_identified_profiles(const std::vector<AssemblyProfile>& profiles) {
    return std::any_of(profiles.begin(), profiles.end(), [](const auto& profile) { return profile.topology.has_value(); });
}
bool has_mapped_profiles(const std::vector<AssemblyProfile>& profiles) {
    return std::any_of(profiles.begin(), profiles.end(), [](const auto& profile) { return profile.planar_affine.has_value(); });
}
bool topology_preserving_geometry(const Intent& intent) {
    return !intent.initialize_profile_topology && !intent.review_profile_topology &&
        !intent.initialize_deformed_profile_topology && !intent.review_initial_profile_geometry &&
        intent.profile_geometry.operation == Operation::set && intent.profile_geometry.value &&
        has_identified_profiles(*intent.profile_geometry.value);
}
void preflight_intent(const Intent& intent, Budget& budget) {
    const auto bounded = [&](const std::string& value, std::size_t limit, bool nonblank) {
        if (value.size() > limit || value.find('\0') != std::string::npos ||
            (nonblank && (value.empty() || std::all_of(value.begin(), value.end(),
                [](unsigned char c) { return std::isspace(c); })))) reject("invalid or oversized typed string");
        budget.quoted(value);
    };
    const auto& target = intent.target;
    bounded(target.catalog_id, 128, true); bounded(target.instance_id, 1024, true);
    if (target.document_entity_id) bounded(*target.document_entity_id, 128, true);
    if (target.part_path.size() >= 32) reject("invalid stable part path");
    for (const auto& id : target.part_path) bounded(id, 1024, true);
    // Conservatively reserve fixed field names, delimiters and pose numbers
    // before constructing JSON. The canonical wire receives the exact bound too.
    budget.add(1024);
    const auto patches = [&](const auto& values, auto inspect) {
        if (values.size() > 4096) reject("override patch budget exceeded");
        for (const auto& [key, patch] : values) {
            bounded(key, 1024, true); budget.add(128);
            if (++budget.nodes > 100000) reject("typed patch complexity budget exceeded");
            if ((patch.operation == Operation::set) != patch.value.has_value()) reject("only set may carry a value");
            (void)operation_name(patch.operation);
            if (patch.value) inspect(*patch.value);
        }
    };
    patches(intent.properties, [&](const std::string& value) { bounded(value, 4096, false); });
    patches(intent.materials, [&](const std::string& value) { bounded(value, 1024, true); });
    patches(intent.quantities, [&](const AssemblyQuantityProperty& value) { (void)encode_quantity(value); });
    const auto& geometry = intent.profile_geometry;
    (void)operation_name(geometry.operation);
    if ((geometry.operation == Operation::set) != geometry.value.has_value()) reject("only profile set may carry a value");
    if (intent.initialize_profile_topology && intent.review_profile_topology)
        reject("initial and reviewed profile topology authority are exclusive");
    if (intent.initialize_deformed_profile_topology &&
        (intent.initialize_profile_topology || intent.review_profile_topology))
        reject("initial deformed profile topology authority is exclusive");
    if (intent.review_initial_profile_geometry &&
        (intent.initialize_profile_topology || intent.review_profile_topology || intent.initialize_deformed_profile_topology))
        reject("reviewed initial profile geometry authority is exclusive");
    if (intent.review_initial_profile_geometry && geometry.operation == Operation::unchanged)
        reject("reviewed initial geometry requires profile Set or Clear");
    if (intent.review_profile_topology && geometry.operation == Operation::unchanged)
        reject("reviewed topology requires profile Set or Clear");
    if (intent.initialize_profile_topology &&
        (geometry.operation != Operation::set || !geometry.value || !has_identified_profiles(*geometry.value)))
        reject("initial topology authoring requires identified profile Set");
    if (intent.initialize_deformed_profile_topology &&
        (geometry.operation != Operation::set || !geometry.value || !has_identified_profiles(*geometry.value)))
        reject("initial deformed topology authoring requires identified profile Set");
    if (geometry.value) {
        // Charge borrowed strings and segments before helper validation or any
        // profile JSON/string copy. Retain the source profile ID grammar; the
        // wire budget and UTF-8 admission bound these local identifiers.
        const auto boundary = [&](const Boundary& ring) {
            budget.add(2 + (ring.empty() ? 0 : ring.size() - 1));
            for (const auto& edge : ring) {
                (void)edge;
                budget.add(208); // Closed segment fields and five numbers.
                if (8 > 100000 - budget.nodes) reject("typed profile complexity budget exceeded");
                budget.nodes += 8;
            }
        };
        for (const auto& profile : *geometry.value) {
            budget.add(256); budget.quoted(profile.id);
            if (profile.material_slot) budget.quoted(*profile.material_slot);
            if (8 > 100000 - budget.nodes) reject("typed profile complexity budget exceeded");
            budget.nodes += 8;
            if (profile.planar_affine) {
                budget.add(512);
                if (16 > 100000 - budget.nodes) reject("typed profile map complexity budget exceeded");
                budget.nodes += 16;
            }
            boundary(profile.outer);
            for (const auto& hole : profile.holes) {
                if (++budget.nodes > 100000) reject("typed profile complexity budget exceeded");
                boundary(hole);
            }
            if (profile.topology) {
                // Admit borrowed identity arrays before topology validation,
                // which builds identity sets, or profile encoding copies them.
                const auto& topology = *profile.topology;
                if (topology.holes.size() != profile.holes.size()) reject("profile topology hole count mismatch");
                const auto ring = [&](const AssemblyProfileRingIdentity& ids, const Boundary& edges) {
                    if (ids.segment_ids.size() != edges.size() || ids.vertex_ids.size() != edges.size())
                        reject("profile topology segment/vertex count mismatch");
                    budget.add(128);
                    if (8 > 100000 - budget.nodes) reject("typed topology complexity budget exceeded");
                    budget.nodes += 8;
                    bounded(ids.ring_id, 128, true);
                    for (const auto* values : {&ids.segment_ids, &ids.vertex_ids}) for (const auto& id : *values) {
                        if (++budget.nodes > 100000) reject("typed topology complexity budget exceeded");
                        budget.add(1); bounded(id, 128, true);
                    }
                };
                ring(topology.outer, profile.outer);
                for (std::size_t i = 0; i < topology.holes.size(); ++i) ring(topology.holes[i], profile.holes[i]);
            }
        }
        (void)validate_assembly_profiles(*geometry.value);
    }
    (void)operation_name(intent.pose.operation);
    for (const auto value : {intent.pose.x_metres, intent.pose.y_metres, intent.pose.z_metres, intent.pose.rotation_radians})
        if (value && !std::isfinite(*value)) reject("pose components must be finite");
    for (const auto value : {intent.pose.scale, intent.pose.vertical_scale})
        if (value && (!std::isfinite(*value) || *value <= 0.0)) reject("pose scales must be finite and positive");
}
Json encode_intent(const Intent& intent) {
    const auto& t = intent.target;
    const auto& p = intent.pose;
    const auto string_value = [](const std::string& value) { return Json(text(Json(value), 4096, false)); };
    const auto material = [](const std::string& value) { return Json(text(Json(value), 1024, true)); };
    Json result{{"version", intent.review_initial_profile_geometry ? 7 : intent.initialize_deformed_profile_topology ? 6 : intent.review_profile_topology ? 5 : intent.initialize_profile_topology ? 4 : intent.profile_geometry.operation == Operation::unchanged ? 1 :
            topology_preserving_geometry(intent) ? 3 : 2},
        {"target", {{"catalog_id", t.catalog_id}, {"instance_id", t.instance_id},
            {"document_entity_id", optional(t.document_entity_id)}, {"part_path", t.part_path}}},
        {"properties", encode_patches(intent.properties, string_value)},
        {"materials", encode_patches(intent.materials, material)},
        {"quantities", encode_patches(intent.quantities, encode_quantity)},
        {"pose", {{"operation", operation_name(p.operation)}, {"x_metres", optional(p.x_metres)},
            {"y_metres", optional(p.y_metres)}, {"z_metres", optional(p.z_metres)},
            {"rotation_radians", optional(p.rotation_radians)}, {"scale", optional(p.scale)},
            {"vertical_scale", optional(p.vertical_scale)}, {"mirrored_y", optional(p.mirrored_y)}}}};
    if (intent.profile_geometry.operation != Operation::unchanged)
        result["profile_geometry"] = {{"operation", operation_name(intent.profile_geometry.operation)},
            {"value", intent.profile_geometry.value ? encode_assembly_profiles(*intent.profile_geometry.value) : Json(nullptr)}};
    if (intent.initialize_profile_topology) result["initialize_profile_topology"] = true;
    if (intent.review_profile_topology) result["review_profile_topology"] = true;
    if (intent.initialize_deformed_profile_topology) result["initialize_deformed_profile_topology"] = true;
    if (intent.review_initial_profile_geometry) result["review_initial_profile_geometry"] = true;
    return result;
}
Intent decode_intent(const Json& value) {
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 && value.at("version") != 4 &&
            value.at("version") != 5 && value.at("version") != 6 && value.at("version") != 7))
        reject("unsupported intent version");
    const bool review_initial = value.at("version") == 7;
    const bool initialize_deformed = value.at("version") == 6;
    const bool review = value.at("version") == 5;
    const bool initialize = value.at("version") == 4;
    const bool topology = value.at("version") == 3 || initialize || initialize_deformed;
    const bool geometry = value.at("version") != 1;
    if (review_initial) fields(value, {"version", "target", "properties", "materials", "quantities", "pose", "profile_geometry",
        "review_initial_profile_geometry"});
    else if (initialize_deformed) fields(value, {"version", "target", "properties", "materials", "quantities", "pose", "profile_geometry",
        "initialize_deformed_profile_topology"});
    else if (review) fields(value, {"version", "target", "properties", "materials", "quantities", "pose", "profile_geometry",
        "review_profile_topology"});
    else if (initialize) fields(value, {"version", "target", "properties", "materials", "quantities", "pose", "profile_geometry",
        "initialize_profile_topology"});
    else if (geometry) fields(value, {"version", "target", "properties", "materials", "quantities", "pose", "profile_geometry"});
    else fields(value, {"version", "target", "properties", "materials", "quantities", "pose"});
    Intent result;
    if (review_initial) {
        if (!value.at("review_initial_profile_geometry").is_boolean() ||
            !value.at("review_initial_profile_geometry").get<bool>())
            reject("v7 requires literal review_initial_profile_geometry:true");
        result.review_initial_profile_geometry = true;
    }
    if (initialize_deformed) {
        if (!value.at("initialize_deformed_profile_topology").is_boolean() ||
            !value.at("initialize_deformed_profile_topology").get<bool>())
            reject("v6 requires literal initialize_deformed_profile_topology:true");
        result.initialize_deformed_profile_topology = true;
    }
    if (review) {
        if (!value.at("review_profile_topology").is_boolean() || !value.at("review_profile_topology").get<bool>())
            reject("v5 requires literal review_profile_topology:true");
        result.review_profile_topology = true;
    }
    if (initialize) {
        if (!value.at("initialize_profile_topology").is_boolean() || !value.at("initialize_profile_topology").get<bool>())
            reject("v4 requires literal initialize_profile_topology:true");
        result.initialize_profile_topology = true;
    }
    const auto& t = value.at("target");
    fields(t, {"catalog_id", "instance_id", "document_entity_id", "part_path"});
    result.target.catalog_id = identity(t.at("catalog_id"));
    result.target.instance_id = text(t.at("instance_id"), 1024, true);
    if (!t.at("document_entity_id").is_null()) result.target.document_entity_id = identity(t.at("document_entity_id"));
    const auto& path = t.at("part_path");
    if (!path.is_array() || path.size() >= 32) reject("invalid stable part path");
    for (const auto& id : path) result.target.part_path.push_back(text(id, 1024, true));
    result.properties = decode_patches<std::string>(value.at("properties"),
        [](const Json& v) { return text(v, 4096, false); });
    result.materials = decode_patches<std::string>(value.at("materials"),
        [](const Json& v) { return text(v, 1024, true); });
    result.quantities = decode_patches<AssemblyQuantityProperty>(value.at("quantities"), decode_quantity);
    if (geometry) {
        const auto& patch = value.at("profile_geometry");
        fields(patch, {"operation", "value"});
        result.profile_geometry.operation = operation(patch.at("operation"));
        if (result.profile_geometry.operation == Operation::unchanged) reject("geometry intent requires a profile operation");
        if (topology && result.profile_geometry.operation != Operation::set)
            reject("identified geometry intent requires profile Set");
        if (result.profile_geometry.operation == Operation::set) {
            // The shared profile codec admits both dialects. Historical intent
            // authority remains anonymous regardless of that decoder's growth.
            if (!topology && !review && !review_initial && patch.at("value").is_array()) for (const auto& profile : patch.at("value"))
                if (profile.is_object() && (profile.contains("topology") || profile.contains("version") ||
                    profile.contains("profile_version") || profile.contains("planar_affine")))
                    reject("v2 profile Set requires anonymous historical geometry");
            result.profile_geometry.value = decode_assembly_profiles(patch.at("value"));
            if (topology && !has_identified_profiles(*result.profile_geometry.value))
                reject("identified geometry intent requires identified profiles");
        }
        else if (!patch.at("value").is_null()) reject("profile clear must carry null");
    }
    const auto& p = value.at("pose");
    fields(p, {"operation", "x_metres", "y_metres", "z_metres", "rotation_radians", "scale", "vertical_scale", "mirrored_y"});
    result.pose.operation = operation(p.at("operation"));
    auto read_number = [&](const char* key, auto& out, bool positive = false) {
        if (!p.at(key).is_null()) out = number(p.at(key), positive);
    };
    read_number("x_metres", result.pose.x_metres); read_number("y_metres", result.pose.y_metres);
    read_number("z_metres", result.pose.z_metres); read_number("rotation_radians", result.pose.rotation_radians);
    read_number("scale", result.pose.scale, true); read_number("vertical_scale", result.pose.vertical_scale, true);
    if (!p.at("mirrored_y").is_null()) {
        if (!p.at("mirrored_y").is_boolean()) reject("absolute mirror parity must be boolean");
        result.pose.mirrored_y = p.at("mirrored_y").get<bool>();
    }
    const bool supplied = result.pose.x_metres || result.pose.y_metres || result.pose.z_metres ||
        result.pose.rotation_radians || result.pose.scale || result.pose.vertical_scale || result.pose.mirrored_y;
    if (result.pose.operation != Operation::set && supplied) reject("only pose set may carry component fields");
    if (result.pose.operation == Operation::clear && result.target.part_path.empty()) reject("root pose cannot be cleared");
    if (encode_intent(result).dump() != value.dump()) reject("intent must use its canonical typed codec");
    return result;
}
Json encode_intents(std::span<const Intent> intents) {
    if (intents.empty() || intents.size() > target_limit) reject("requires 1..1000 qualified targets");
    auto result = Json::array();
    const Target* previous = nullptr;
    Budget typed_budget;
    for (const auto& intent : intents) preflight_intent(intent, typed_budget);
    Budget wire_budget;
    wire_budget.add(2 + intents.size() - 1);
    for (const auto& intent : intents) {
        if (previous && !(*previous < intent.target)) reject("targets must be sorted and unique");
        previous = &intent.target;
        auto row = encode_intent(intent);
        wire_budget.visit(row);
        result.push_back(std::move(row));
    }
    for (const auto& row : result) (void)decode_intent(row);
    return result;
}
std::vector<Intent> decode_intents(const Json& rows) {
    if (!rows.is_array() || rows.empty() || rows.size() > target_limit) reject("requires 1..1000 qualified targets");
    std::vector<Intent> result;
    for (const auto& row : rows) {
        auto intent = decode_intent(row);
        if (!result.empty() && !(result.back().target < intent.target)) reject("targets must be sorted and unique");
        result.push_back(std::move(intent));
    }
    return result;
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
AssemblyTransform root_pose(const AssemblyInstance& source) {
    if (source.placement) {
        const auto& p = *source.placement;
        return {{p.translation_m.x, p.translation_m.y, p.translation_z_m}, p.rotation_radians,
            p.scale, p.mirrored_y, p.vertical_scale};
    }
    return source.root_transform.value_or(AssemblyTransform{});
}
AssemblyTransform changed_pose(AssemblyTransform pose, const AssemblyInstancePosePropertyPatch& patch) {
    if (patch.x_metres) pose.translation_m.x = *patch.x_metres;
    if (patch.y_metres) pose.translation_m.y = *patch.y_metres;
    if (patch.z_metres) pose.translation_m.z = *patch.z_metres;
    if (patch.rotation_radians) pose.rotation_radians = *patch.rotation_radians;
    if (patch.scale) pose.scale = *patch.scale;
    if (patch.vertical_scale) pose.vertical_scale = *patch.vertical_scale;
    if (patch.mirrored_y) pose.mirrored_y = *patch.mirrored_y;
    (void)encode_assembly_transform(pose); // Includes combined Z overflow/underflow.
    return pose;
}
template<class T> void change_overrides(std::map<std::string, T>& overrides,
    const std::map<std::string, T>& resolved, const std::map<std::string, T>& declared,
    const std::map<std::string, AssemblyInstancePropertyPatch<T>>& patches, auto admit) {
    for (const auto& [key, patch] : patches) {
        if (!declared.contains(key) || !resolved.contains(key)) reject("patch references an undeclared source slot");
        if (patch.operation == Operation::clear) overrides.erase(key);
        else if (patch.operation == Operation::set) {
            admit(key, *patch.value);
            // Equal effective values never materialize redundant provenance.
            if (resolved.at(key) != *patch.value) overrides.insert_or_assign(key, *patch.value);
        }
    }
}
bool same_boundary(const Boundary& first, const Boundary& second) {
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(),
        [](const auto& a, const auto& b) {
            return a.start.x == b.start.x && a.start.y == b.start.y &&
                a.end.x == b.end.x && a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
        });
}
void require_preserved_profile_topology(const std::vector<AssemblyProfile>& source,
    const std::vector<AssemblyProfile>& proposed) {
    if (!has_identified_profiles(source)) reject("topology-preserving Set requires actual identified source profiles");
    if (source.size() != proposed.size()) reject("topology-preserving Set changed the source profile roster");
    for (const auto& profile : source) {
        const auto found = std::find_if(proposed.begin(), proposed.end(),
            [&](const auto& row) { return row.id == profile.id; });
        if (found == proposed.end() || found->topology != profile.topology ||
            found->planar_affine != profile.planar_affine ||
            !same_boundary(found->outer, profile.outer) || found->holes.size() != profile.holes.size() ||
            !std::equal(found->holes.begin(), found->holes.end(), profile.holes.begin(), same_boundary))
            reject("topology-preserving Set changed source identity, map, rings or edges; reviewed mapping is required");
    }
}
void require_initial_profile_topology(const std::vector<AssemblyProfile>& source,
    const std::vector<AssemblyProfile>& proposed, const std::vector<AssemblyProfile>& type_profiles) {
    if (source.size() != proposed.size()) reject("initial topology authoring changed the source profile roster");
    bool assigned = false;
    for (const auto& profile : source) {
        const auto found = std::find_if(proposed.begin(), proposed.end(),
            [&](const auto& row) { return row.id == profile.id; });
        if (found == proposed.end() || !same_boundary(found->outer, profile.outer) ||
            found->planar_affine != profile.planar_affine ||
            found->holes.size() != profile.holes.size() ||
            !std::equal(found->holes.begin(), found->holes.end(), profile.holes.begin(), same_boundary) ||
            found->elevation_m != profile.elevation_m || found->height_m != profile.height_m ||
            found->material_slot != profile.material_slot)
            reject("initial topology authoring must retain exact source profiles, geometry, scalars and materials");
        if (profile.topology) {
            if (found->topology != profile.topology)
                reject("initial topology authoring changed an existing identified source profile");
            continue;
        }
        if (!found->topology) continue;
        assigned = true;
        // The shared validator supplies bounded portable IDs, exact counts and
        // independent profile-local ring/segment/vertex uniqueness. Do not
        // invent a root-wide namespace or reserve the profile's own spelling.
        validate_assembly_profile_topology(*found);
        const auto inherited = std::find_if(type_profiles.begin(), type_profiles.end(),
            [&](const auto& row) { return row.id == profile.id; });
        if (inherited == type_profiles.end() || !inherited->topology) continue;
        // An anonymous own override can shadow identified shared geometry. Its
        // newly authored IDs cannot claim those actual same-profile identities,
        // even if the two geometries happen to have equal segment counts.
        std::set<std::string_view> rings, segments, vertices;
        const auto reserve = [&](const AssemblyProfileRingIdentity& ring) {
            rings.insert(ring.ring_id);
            for (const auto& id : ring.segment_ids) segments.insert(id);
            for (const auto& id : ring.vertex_ids) vertices.insert(id);
        };
        reserve(inherited->topology->outer);
        for (const auto& ring : inherited->topology->holes) reserve(ring);
        const auto fresh = [&](const AssemblyProfileRingIdentity& ring) {
            if (rings.contains(ring.ring_id)) reject("initial topology ring identity borrows actual shared source topology");
            for (const auto& id : ring.segment_ids) if (segments.contains(id))
                reject("initial topology segment identity borrows actual shared source topology");
            for (const auto& id : ring.vertex_ids) if (vertices.contains(id))
                reject("initial topology vertex identity borrows actual shared source topology");
        };
        fresh(found->topology->outer);
        for (const auto& ring : found->topology->holes) fresh(ring);
    }
    if (!assigned) reject("initial topology authoring requires an actual anonymous profile identity assignment");
}
boost::multiprecision::cpp_rational affine_binary64(double value) {
    using Integer = boost::multiprecision::cpp_int;
    using Exact = boost::multiprecision::cpp_rational;
    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559 &&
        std::numeric_limits<double>::radix == 2 && std::numeric_limits<double>::digits == 53);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<unsigned>((bits >> 52) & 0x7ff);
    if (exponent == 0x7ff) reject("affine operator comparison requires finite operands");
    Integer significand = bits & ((std::uint64_t{1} << 52) - 1);
    if (exponent) significand += Integer{1} << 52;
    if (bits >> 63) significand = -significand;
    const int shift = exponent ? static_cast<int>(exponent) - 1075 : -1074;
    if (shift >= 0) return Exact{significand << shift};
    Exact result{significand}; result /= Integer{1} << -shift; return result;
}
bool same_profile_affine_operator(const std::optional<AssemblyProfilePlanarAffine>& first,
    const std::optional<AssemblyProfilePlanarAffine>& second) {
    const auto a = first.value_or(AssemblyProfilePlanarAffine{});
    const auto b = second.value_or(AssemblyProfilePlanarAffine{});
    if (a.xx != b.xx || a.xy != b.xy || a.yx != b.yx || a.yy != b.yy) return false;
    // Compare the exact constant terms of q = M*p + t. These fixed expressions
    // use only finite binary64 operands, bounding integer/exponent work. Do not
    // sample rounded points or depend on wider long double on Windows.
    const auto translation = [](const AssemblyProfilePlanarAffine& map, bool y)
        -> boost::multiprecision::cpp_rational {
        return affine_binary64(y ? map.target_origin.y : map.target_origin.x) -
            affine_binary64(y ? map.yx : map.xx) * affine_binary64(map.source_origin.x) -
            affine_binary64(y ? map.yy : map.xy) * affine_binary64(map.source_origin.y);
    };
    return translation(a, false) == translation(b, false) && translation(a, true) == translation(b, true);
}
void require_initial_deformed_profile_topology(const std::vector<AssemblyProfile>& source,
    const std::vector<AssemblyProfile>& proposed, const std::vector<AssemblyProfile>& type_profiles) {
    if (source.size() != proposed.size()) reject("initial deformed topology changed the source profile roster");
    bool assigned_deformation = false;
    for (const auto& profile : source) {
        const auto found = std::find_if(proposed.begin(), proposed.end(),
            [&](const auto& row) { return row.id == profile.id; });
        if (found == proposed.end() || !same_boundary(found->outer, profile.outer) ||
            found->holes.size() != profile.holes.size() ||
            !std::equal(found->holes.begin(), found->holes.end(), profile.holes.begin(), same_boundary) ||
            found->elevation_m != profile.elevation_m || found->height_m != profile.height_m ||
            found->material_slot != profile.material_slot)
            reject("initial deformed topology must retain exact source profiles, rings, scalars and materials");
        if (found->planar_affine == profile.planar_affine) {
            if (found->topology != profile.topology)
                reject("initial deformed unchanged maps must retain exact source topology");
            continue;
        }
        if (same_profile_affine_operator(profile.planar_affine, found->planar_affine))
            reject("initial deformed topology requires actual affine changes, not map reanchoring");
        if (!found->topology) reject("initial deformed profile must have fresh topology");
        validate_assembly_profile_topology(*found);
        std::set<std::string_view> rings, segments, vertices;
        const auto reserve = [&](const AssemblyProfile& reserved) {
            if (!reserved.topology) return;
            const auto ring = [&](const AssemblyProfileRingIdentity& ids) {
                rings.insert(ids.ring_id);
                for (const auto& id : ids.segment_ids) segments.insert(id);
                for (const auto& id : ids.vertex_ids) vertices.insert(id);
            };
            ring(reserved.topology->outer);
            for (const auto& hole : reserved.topology->holes) ring(hole);
        };
        reserve(profile);
        const auto inherited = std::find_if(type_profiles.begin(), type_profiles.end(),
            [&](const auto& row) { return row.id == profile.id; });
        if (inherited != type_profiles.end()) reserve(*inherited);
        // Source and hidden shared-type identities remain reserved in each
        // actual profile's separate ring/segment/vertex namespaces.
        const auto fresh = [&](const AssemblyProfileRingIdentity& ids) {
            if (rings.contains(ids.ring_id)) reject("initial deformed ring identity borrows actual same-profile topology");
            for (const auto& id : ids.segment_ids) if (segments.contains(id))
                reject("initial deformed segment identity borrows actual same-profile topology");
            for (const auto& id : ids.vertex_ids) if (vertices.contains(id))
                reject("initial deformed vertex identity borrows actual same-profile topology");
        };
        fresh(found->topology->outer);
        for (const auto& hole : found->topology->holes) fresh(hole);
        if (!profile.topology) assigned_deformation = true;
    }
    if (!assigned_deformation)
        reject("initial deformed topology requires actual anonymous profile deformation and initial identity assignment");
}
void require_reviewed_profile_roster(const std::vector<AssemblyProfile>& source,
    const std::vector<AssemblyProfile>& proposed, const std::vector<AssemblyProfile>& type_profiles,
    bool compare_affine_operator) {
    for (const auto& next : proposed) {
        const auto old = std::find_if(source.begin(), source.end(),
            [&](const auto& profile) { return profile.id == next.id; });
        if (old != source.end() &&
            same_boundary(old->outer, next.outer) && old->holes.size() == next.holes.size() &&
            std::equal(old->holes.begin(), old->holes.end(), next.holes.begin(), same_boundary) &&
            (compare_affine_operator ? same_profile_affine_operator(old->planar_affine, next.planar_affine) :
                old->planar_affine == next.planar_affine)) {
            // A mixed source can retain anonymous rows, but review is not an
            // alternative route for assigning identity to unchanged geometry.
            if (old->topology != next.topology)
                reject("reviewed unchanged geometry must retain exact source topology");
            continue;
        }
        if (!next.topology) reject("reviewed new or redrawn profile must be identified");
        validate_assembly_profile_topology(next);
        std::set<std::string_view> rings, segments, vertices;
        const auto reserve = [&](const AssemblyProfile& profile) {
            if (!profile.topology) return;
            const auto ring = [&](const AssemblyProfileRingIdentity& ids) {
                rings.insert(ids.ring_id);
                for (const auto& id : ids.segment_ids) segments.insert(id);
                for (const auto& id : ids.vertex_ids) vertices.insert(id);
            };
            ring(profile.topology->outer);
            for (const auto& hole : profile.topology->holes) ring(hole);
        };
        if (old != source.end()) reserve(*old);
        const auto inherited = std::find_if(type_profiles.begin(), type_profiles.end(),
            [&](const auto& profile) { return profile.id == next.id; });
        if (inherited != type_profiles.end()) reserve(*inherited);
        // IDs belong to separate namespaces within each actual profile. Even
        // equal-count redraws replace every ring, segment and vertex identity.
        const auto fresh = [&](const AssemblyProfileRingIdentity& ids) {
            if (rings.contains(ids.ring_id)) reject("reviewed ring identity borrows actual same-profile topology");
            for (const auto& id : ids.segment_ids) if (segments.contains(id))
                reject("reviewed segment identity borrows actual same-profile topology");
            for (const auto& id : ids.vertex_ids) if (vertices.contains(id))
                reject("reviewed vertex identity borrows actual same-profile topology");
        };
        fresh(next.topology->outer);
        for (const auto& hole : next.topology->holes) fresh(hole);
    }
}
void require_reviewed_profile_topology(const std::vector<AssemblyProfile>& source,
    const std::vector<AssemblyProfile>& proposed, const std::vector<AssemblyProfile>& type_profiles) {
    if (!has_identified_profiles(source)) reject("reviewed geometry requires actual identified own-source profiles");
    require_reviewed_profile_roster(source, proposed, type_profiles, false);
}
void require_reviewed_initial_profile_geometry(const std::vector<AssemblyProfile>& source,
    const std::vector<AssemblyProfile>& proposed, const std::vector<AssemblyProfile>& type_profiles) {
    if (has_identified_profiles(source)) reject("reviewed initial geometry requires anonymous own-source profiles");
    // Equal source rings and equal exact affine operators are unchanged
    // geometry. Reanchoring cannot smuggle initial IDs onto anonymous rows;
    // scalar/material edits and an empty final roster require no assignment.
    require_reviewed_profile_roster(source, proposed, type_profiles, true);
}
void change_maps(auto& source, const AssemblyExpandedNode& node, const AssemblyType& type,
    const AssemblyModel& model, const Intent& intent) {
    const auto& geometry = intent.profile_geometry;
    // Own local profiles only. A path follows its actual path override or its
    // resolved type; descendant profiles and world transforms grant no authority.
    const auto& resolved = source.profile_overrides ? *source.profile_overrides : type.profiles;
    if (intent.review_profile_topology && !has_identified_profiles(resolved) &&
        !(geometry.operation == Operation::clear && has_identified_profiles(type.profiles)))
        reject("reviewed geometry requires actual identified own-source profiles");
    if (intent.review_initial_profile_geometry) {
        if (has_identified_profiles(resolved))
            reject("reviewed initial geometry requires anonymous own-source profiles");
        if (geometry.operation == Operation::clear && !source.profile_overrides)
            reject("reviewed initial geometry Clear requires an actual own override");
    }
    if (!intent.review_profile_topology && !intent.review_initial_profile_geometry && geometry.operation == Operation::clear &&
        (has_identified_profiles(resolved) || has_mapped_profiles(resolved) || (source.profile_overrides &&
            (has_identified_profiles(type.profiles) || has_mapped_profiles(type.profiles)))))
        reject("clearing identified or mapped source profiles requires reviewed identity mapping/removal");
    if (geometry.operation == Operation::set) {
        if (intent.review_profile_topology) require_reviewed_profile_topology(resolved, *geometry.value, type.profiles);
        else if (intent.review_initial_profile_geometry)
            require_reviewed_initial_profile_geometry(resolved, *geometry.value, type.profiles);
        else if (intent.initialize_deformed_profile_topology)
            require_initial_deformed_profile_topology(resolved, *geometry.value, type.profiles);
        else if (intent.initialize_profile_topology) require_initial_profile_topology(resolved, *geometry.value, type.profiles);
        else if (topology_preserving_geometry(intent)) require_preserved_profile_topology(resolved, *geometry.value);
        else if (has_identified_profiles(resolved) || has_mapped_profiles(resolved))
            reject("historical profile Set cannot redraw identified or mapped source profiles");
    }
    change_overrides(source.property_overrides, node.properties, type.properties, intent.properties,
        [](const auto&, const auto&) {});
    change_overrides(source.material_overrides, node.materials, type.materials, intent.materials,
        [&](const auto&, const std::string& id) {
            if (std::none_of(model.materials().begin(), model.materials().end(),
                [&](const auto& material) { return material.id == id; })) reject("material is absent from the actual catalog");
        });
    change_overrides(source.quantity_overrides, node.quantities, type.quantities, intent.quantities,
        [&](const std::string& key, const AssemblyQuantityProperty& q) {
            if (q.unit != type.quantities.at(key).unit) reject("quantity override changes its declared dimension");
        });
    if (geometry.operation == Operation::clear) source.profile_overrides.reset();
    else if (geometry.operation == Operation::set) {
        auto changed = *geometry.value;
        std::sort(changed.begin(), changed.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        // Compare canonical values to preserve even equal explicit provenance,
        // while retaining the original raw row and its numeric representations.
        if (encode_assembly_profiles(resolved) != encode_assembly_profiles(changed))
            source.profile_overrides = std::move(changed);
    }
}
AssemblyInstance change_instance(const AssemblyInstance& source, const AssemblyExpansion& expansion,
    const AssemblyModel& model, const std::vector<const Intent*>& intents) {
    auto result = source;
    for (const auto* intent : intents) {
        const auto& path = intent->target.part_path;
        const auto node = std::find_if(expansion.nodes.begin(), expansion.nodes.end(),
            [&](const auto& value) { return value.part_path == path; });
        if (node == expansion.nodes.end()) reject("stable part path is absent from the actual source expansion");
        const auto type = std::find_if(model.types().begin(), model.types().end(),
            [&](const auto& value) { return value.id == node->type_id; });
        if (type == model.types().end()) reject("actual expanded type is missing");
        if (path.empty()) {
            change_maps(result, *node, *type, model, *intent);
            if (intent->pose.operation == Operation::set) {
                const auto before = root_pose(source);
                const auto after = changed_pose(before, intent->pose);
                if (after == before) continue;
                if (result.placement) {
                    auto& p = *result.placement;
                    p.translation_m = {after.translation_m.x, after.translation_m.y};
                    p.translation_z_m = after.translation_m.z; p.rotation_radians = after.rotation_radians;
                    p.scale = after.scale; p.mirrored_y = after.mirrored_y; p.vertical_scale = after.vertical_scale;
                } else result.root_transform = after;
            }
        } else {
            if (!node->source_part) reject("path node has no authoritative part identity");
            const auto existing = std::find_if(result.nested_overrides.begin(), result.nested_overrides.end(),
                [&](const auto& row) { return row.part_path == path; });
            AssemblyPathOverride changed;
            changed.part_path = path;
            if (existing != result.nested_overrides.end()) changed = *existing;
            const auto before = changed;
            change_maps(changed, *node, *type, model, *intent);
            if (intent->pose.operation == Operation::clear) changed.transform.reset();
            else if (intent->pose.operation == Operation::set) {
                const auto local = before.transform.value_or(node->source_part->transform);
                const auto after = changed_pose(local, intent->pose);
                if (after != local) changed.transform = after;
            }
            if (changed == before) continue;
            if (existing == result.nested_overrides.end()) result.nested_overrides.push_back(std::move(changed));
            else *existing = std::move(changed);
        }
    }
    return result;
}
// Retain raw unchanged components independently; JSON numeric equality alone
// cannot distinguish saved 0 from 0.0 or different authored quantity encodings.
void patch_pose(Json& raw, const AssemblyTransform& before, const AssemblyTransform& after, bool placement) {
    if (before == after) return;
    auto& coordinates = raw.at("translation_m");
    if (before.translation_m.x != after.translation_m.x) coordinates[0] = after.translation_m.x;
    if (before.translation_m.y != after.translation_m.y) coordinates[1] = after.translation_m.y;
    if (before.translation_m.z != after.translation_m.z) {
        if (placement && coordinates.size() == 2) coordinates.push_back(after.translation_m.z);
        else coordinates[2] = after.translation_m.z;
    }
    if (before.rotation_radians != after.rotation_radians) raw["rotation_radians"] = after.rotation_radians;
    if (before.scale != after.scale) raw["scale"] = after.scale;
    if (before.mirrored_y != after.mirrored_y) raw["mirrored_y"] = after.mirrored_y;
    if (before.vertical_scale != after.vertical_scale) raw["vertical_scale"] = after.vertical_scale;
}
template<class T> void patch_map(Json& raw, const std::map<std::string, T>& before,
    const std::map<std::string, T>& after, auto encode) {
    for (const auto& [key, value] : before) { (void)value; if (!after.contains(key)) raw.erase(key); }
    for (const auto& [key, value] : after) {
        const auto old = before.find(key);
        if (old == before.end() || old->second != value) raw[key] = encode(value);
    }
}
void patch_maps(Json& raw, const auto& before, const auto& after, const Json* inherited_profiles = nullptr,
    bool reviewed_profiles = false) {
    patch_map(raw.at("property_overrides"), before.property_overrides, after.property_overrides,
        [](const std::string& value) { return Json(value); });
    patch_map(raw.at("material_overrides"), before.material_overrides, after.material_overrides,
        [](const std::string& value) { return Json(value); });
    patch_map(raw.at("quantity_overrides"), before.quantity_overrides, after.quantity_overrides, encode_quantity);
    if (before.profile_overrides != after.profile_overrides) {
        if (!after.profile_overrides) { raw.erase("profile_overrides"); return; }
        auto encoded = encode_assembly_profiles(*after.profile_overrides);
        std::vector<AssemblyProfile> inherited;
        const auto* previous_profiles = before.profile_overrides ? &*before.profile_overrides : nullptr;
        const Json* previous_raw = before.profile_overrides ? &raw.at("profile_overrides") : nullptr;
        if (!previous_profiles && (reviewed_profiles || has_identified_profiles(*after.profile_overrides))) {
            if (!inherited_profiles) reject("identified override materialization has no actual source profiles");
            inherited = decode_assembly_profiles(*inherited_profiles);
            previous_profiles = &inherited;
            previous_raw = inherited_profiles;
        }
        if (previous_profiles) {
            std::map<std::string, const AssemblyProfile*, std::less<>> previous;
            std::map<std::string, const AssemblyProfile*, std::less<>> updated;
            std::map<std::string, const Json*, std::less<>> original;
            for (const auto& profile : *previous_profiles) previous.emplace(profile.id, &profile);
            for (const auto& profile : *after.profile_overrides) updated.emplace(profile.id, &profile);
            for (const auto& profile : *previous_raw)
                original.emplace(profile.at("id").template get<std::string>(), &profile);
            for (auto& profile : encoded) {
                const auto& next = *updated.at(profile.at("id").template get<std::string>());
                const auto old = previous.find(next.id);
                if (old == previous.end()) continue;
                const auto source = original.find(next.id);
                if (source == original.end()) reject("saved profile override is missing");
                const auto& retained = *source->second;
                if (*old->second == next) { profile = retained; continue; }
                if (same_boundary(old->second->outer, next.outer)) profile["outer"] = retained.at("outer");
                if (old->second->holes.size() == next.holes.size() &&
                    std::equal(old->second->holes.begin(), old->second->holes.end(), next.holes.begin(), same_boundary))
                    profile["holes"] = retained.at("holes");
                if (old->second->elevation_m == next.elevation_m) profile["elevation_m"] = retained.at("elevation_m");
                if (old->second->height_m == next.height_m) profile["height_m"] = retained.at("height_m");
                if (old->second->material_slot == next.material_slot) profile["material_slot"] = retained.at("material_slot");
                if (old->second->topology == next.topology && next.topology) {
                    // Scalar changes never regenerate the authored topology
                    // representation or widen/downgrade its profile envelope.
                    if (profile.contains("version") && retained.contains("version"))
                        profile["version"] = retained.at("version");
                    profile["topology"] = retained.at("topology");
                }
                if (old->second->planar_affine && next.planar_affine) {
                    profile["profile_version"] = retained.at("profile_version");
                    auto& affine=profile.at("planar_affine");
                    const auto& saved_affine=retained.at("planar_affine");
                    if (old->second->planar_affine==next.planar_affine) affine=saved_affine;
                    else {
                        affine["version"]=saved_affine.at("version");
                        const auto& before=*old->second->planar_affine;
                        const auto& after=*next.planar_affine;
                        if (before.source_origin.x==after.source_origin.x) affine["source_origin_m"][0]=saved_affine.at("source_origin_m")[0];
                        if (before.source_origin.y==after.source_origin.y) affine["source_origin_m"][1]=saved_affine.at("source_origin_m")[1];
                        if (before.target_origin.x==after.target_origin.x) affine["target_origin_m"][0]=saved_affine.at("target_origin_m")[0];
                        if (before.target_origin.y==after.target_origin.y) affine["target_origin_m"][1]=saved_affine.at("target_origin_m")[1];
                        const double before_matrix[]{before.xx,before.xy,before.yx,before.yy};
                        const double after_matrix[]{after.xx,after.xy,after.yx,after.yy};
                        for (std::size_t i=0;i<4;++i)
                            if (before_matrix[i]==after_matrix[i]) affine["matrix"][i]=saved_affine.at("matrix")[i];
                    }
                }
            }
            if (reviewed_profiles || has_identified_profiles(*after.profile_overrides)) {
                // Historical identified edits preserve the complete roster;
                // reviewed edits preserve the source order of surviving rows.
                auto retained_order = Json::array();
                for (const auto& profile : *previous_raw) {
                    const auto found = std::find_if(encoded.begin(), encoded.end(),
                        [&](const Json& row) { return row.at("id") == profile.at("id"); });
                    if (found == encoded.end()) {
                        if (reviewed_profiles) continue; // Explicit retirement.
                        reject("identified raw profile roster changed");
                    }
                    retained_order.push_back(*found);
                }
                // The reviewed roster retains actual raw source order first,
                // then adds every genuinely new row in canonical ID order.
                if (reviewed_profiles) for (const auto& profile : encoded)
                    if (!previous.contains(profile.at("id").template get<std::string>()))
                        retained_order.push_back(profile);
                encoded = std::move(retained_order);
            }
        }
        raw["profile_overrides"] = std::move(encoded);
    }
}
Json encode_path(const AssemblyPathOverride& path) {
    auto properties = Json(path.property_overrides), materials = Json(path.material_overrides), quantities = Json::object();
    for (const auto& [key, value] : path.quantity_overrides) quantities[key] = encode_quantity(value);
    Json result{{"part_path", path.part_path}, {"transform", path.transform ? encode_assembly_transform(*path.transform) : Json(nullptr)},
        {"property_overrides", std::move(properties)}, {"material_overrides", std::move(materials)},
        {"quantity_overrides", std::move(quantities)}};
    if (path.profile_overrides) result["profile_overrides"] = encode_assembly_profiles(*path.profile_overrides);
    return result;
}
const Json* source_type_profiles(const Json& catalog, const std::string& root_type, const Path& path) {
    std::string type_id = root_type;
    for (const auto& part_id : path) {
        const auto& types = catalog.at("types");
        const auto type = std::find_if(types.begin(), types.end(),
            [&](const Json& row) { return row.at("id") == type_id; });
        if (type == types.end() || !type->contains("parts")) reject("actual path type definition is missing");
        const auto& parts = type->at("parts");
        const auto part = std::find_if(parts.begin(), parts.end(),
            [&](const Json& row) { return row.at("id") == part_id; });
        if (part == parts.end()) reject("actual path part definition is missing");
        type_id = part->at("type_id").get<std::string>();
    }
    const auto& types = catalog.at("types");
    const auto type = std::find_if(types.begin(), types.end(),
        [&](const Json& row) { return row.at("id") == type_id; });
    if (type == types.end()) reject("actual source profile type is missing");
    // An authenticated empty type may omit its profiles field. Preserve that
    // distinction from a missing type while supplying the actual empty roster
    // for reviewed override materialization.
    static const Json empty_profiles = Json::array();
    return type->contains("profiles") ? &type->at("profiles") : &empty_profiles;
}
const Json& source_part_pose(const Json& catalog, const std::string& root_type, const Path& path) {
    std::string type_id = root_type;
    const Json* pose = nullptr;
    for (const auto& part_id : path) {
        const auto& types = catalog.at("types");
        const auto type = std::find_if(types.begin(), types.end(),
            [&](const Json& row) { return row.at("id") == type_id; });
        if (type == types.end() || !type->contains("parts")) reject("actual path type definition is missing");
        const auto& parts = type->at("parts");
        const auto part = std::find_if(parts.begin(), parts.end(),
            [&](const Json& row) { return row.at("id") == part_id; });
        if (part == parts.end()) reject("actual path part definition is missing");
        pose = &part->at("transform");
        type_id = part->at("type_id").get<std::string>();
    }
    if (!pose) reject("local part pose requires a nonempty source path");
    return *pose;
}
Json new_path_pose(const Json& catalog, const AssemblyInstance& source, const AssemblyPathOverride& changed) {
    // Materializing a partial local pose starts with its actual shared part's
    // raw transform. This copies its representation, never edits the definition.
    auto pose = source_part_pose(catalog, source.type_id, changed.part_path);
    const auto before = decode_assembly_transform(pose);
    patch_pose(pose, before, *changed.transform, false);
    return pose;
}
void patch_instance(Json& raw, const AssemblyInstance& before, const AssemblyInstance& after, const Json& catalog,
    const std::set<Path>& reviewed_paths = {}) {
    const auto inherited_profiles = [&](const Path& path, const auto& old, const auto& changed) -> const Json* {
        if (old.profile_overrides || !changed.profile_overrides ||
            (!reviewed_paths.contains(path) && !has_identified_profiles(*changed.profile_overrides)))
            return nullptr;
        return source_type_profiles(catalog, before.type_id, path);
    };
    patch_maps(raw, before, after, inherited_profiles({}, before, after), reviewed_paths.contains(Path{}));
    if (before.placement) patch_pose(raw.at("placement"), root_pose(before), root_pose(after), true);
    else if (before.root_transform && after.root_transform)
        patch_pose(raw.at("root_transform"), *before.root_transform, *after.root_transform, false);
    else if (before.root_transform != after.root_transform)
        raw["root_transform"] = after.root_transform ? encode_assembly_transform(*after.root_transform) : Json(nullptr);
    if (before.nested_overrides != after.nested_overrides) {
        if (!raw.contains("nested_overrides")) raw["nested_overrides"] = Json::array();
        auto& rows = raw.at("nested_overrides");
        for (const auto& changed : after.nested_overrides) {
            const auto old = std::find_if(before.nested_overrides.begin(), before.nested_overrides.end(),
                [&](const auto& row) { return row.part_path == changed.part_path; });
            if (old != before.nested_overrides.end() && *old == changed) continue;
            const auto row = std::find_if(rows.begin(), rows.end(),
                [&](const Json& value) { return value.at("part_path").get<Path>() == changed.part_path; });
            if (old == before.nested_overrides.end()) {
                AssemblyPathOverride empty;
                empty.part_path = changed.part_path;
                auto added = encode_path(empty);
                patch_maps(added, empty, changed, inherited_profiles(changed.part_path, empty, changed),
                    reviewed_paths.contains(changed.part_path));
                if (changed.transform) added["transform"] = new_path_pose(catalog, before, changed);
                rows.push_back(std::move(added));
            }
            else {
                if (row == rows.end()) reject("saved path override is missing");
                patch_maps(*row, *old, changed, inherited_profiles(changed.part_path, *old, changed),
                    reviewed_paths.contains(changed.part_path));
                if (old->transform && changed.transform) patch_pose(row->at("transform"), *old->transform, *changed.transform, false);
                else if (old->transform != changed.transform)
                    (*row)["transform"] = changed.transform ? new_path_pose(catalog, before, changed) : Json(nullptr);
            }
        }
    }
}
bool needs_embedded_promotion(const Json& model, const Json& row) {
    const auto schema = model.at("schema").get<std::string>();
    if (schema == "sketch.assemblies.v7" || schema == "sketch.assemblies.v8" || schema == "sketch.assemblies.v9") return false;
    const bool nested = schema == "sketch.assemblies.v4" || schema == "sketch.assemblies.v5" || schema == "sketch.assemblies.v6";
    if (!nested && (row.contains("root_transform") || row.contains("nested_overrides"))) return true;
    const auto vertical = [](const Json& pose) { return pose.is_object() && pose.contains("vertical_scale"); };
    if (row.contains("placement")) {
        const auto& p = row.at("placement");
        if (p.at("translation_m").size() == 3 && schema != "sketch.assemblies.v5" && schema != "sketch.assemblies.v6") return true;
        if (vertical(p) && schema != "sketch.assemblies.v6") return true;
    }
    if (schema != "sketch.assemblies.v6") {
        if (row.contains("root_transform") && vertical(row.at("root_transform"))) return true;
        if (row.contains("nested_overrides")) for (const auto& path : row.at("nested_overrides"))
            if (vertical(path.at("transform"))) return true;
    }
    return false;
}
bool has_profile_overrides(const Json& row) {
    return row.contains("profile_overrides") || (row.contains("nested_overrides") &&
        std::any_of(row.at("nested_overrides").begin(), row.at("nested_overrides").end(),
            [](const Json& path) { return path.contains("profile_overrides"); }));
}
bool has_mapped_profile_overrides(const Json& row) {
    const auto mapped = [](const Json& profiles) {
        return std::any_of(profiles.begin(), profiles.end(), [](const Json& profile) {
            return profile.contains("planar_affine");
        });
    };
    return (row.contains("profile_overrides") && mapped(row.at("profile_overrides"))) ||
        (row.contains("nested_overrides") && std::any_of(row.at("nested_overrides").begin(),
            row.at("nested_overrides").end(), [&](const Json& path) {
                return path.contains("profile_overrides") && mapped(path.at("profile_overrides"));
            }));
}
void promote_embedded(Json& model, const Json& row) {
    if (model.at("schema") == "sketch.assemblies.v9") return;
    if (has_mapped_profile_overrides(row)) model["schema"] = "sketch.assemblies.v9";
    else if (has_profile_overrides(row)) model["schema"] = "sketch.assemblies.v8";
    else if (needs_embedded_promotion(model, row)) model["schema"] = "sketch.assemblies.v7";
}
void promote_independent(Json& raw) {
    if (raw.at("schema") == "sketch.assembly-instance.v4") return;
    if (has_mapped_profile_overrides(raw)) raw["schema"] = "sketch.assembly-instance.v4";
    else if (has_profile_overrides(raw)) raw["schema"] = "sketch.assembly-instance.v3";
    else if (raw.at("schema") != "sketch.assembly-instance.v3") {
        const auto vertical = [](const Json& pose) { return pose.is_object() && pose.contains("vertical_scale"); };
        if (vertical(raw.at("root_transform")) || std::any_of(raw.at("nested_overrides").begin(),
            raw.at("nested_overrides").end(), [&](const Json& row) { return vertical(row.at("transform")); }))
            raw["schema"] = "sketch.assembly-instance.v2";
    }
}
void complete_row_envelope(Json& row) {
    if (row.contains("root_transform") != row.contains("nested_overrides")) {
        if (!row.contains("root_transform")) row["root_transform"] = nullptr;
        if (!row.contains("nested_overrides")) row["nested_overrides"] = Json::array();
    }
}
// Use the same actual host-copy builders as native preparation. A declaration
// without profiles and without a real supported host has no geometric fallback.
TopoDS_Shape host_shape(const AssemblyDocumentEntities& actual, const std::string& id,
    std::size_t& opening_work) {
    const auto found = actual.find(id);
    if (found == actual.end() || found->second.id != id || !is_complete_assembly_catalog_host_type(found->second.type))
        reject("legacy placement requires its actual geometry-bearing host");
    const auto& source = found->second;
    if (source.type == "opening") return make_document_opening_host_shape(actual, id, &opening_work);
    const auto resolved = resolve_vertical_placement(actual, source);
    if (can_recognize_building_entity_type(source.type)) return make_building_shape(decode_building_entity(resolved), actual);
    if (source.type == "wall") {
        std::vector<const Entity*> openings;
        const auto scope = constraint_phase_scope(actual);
        for (const auto& [owner_id, entity] : actual)
            if (entity.type == "opening" && !scope.inactive_owner_ids.contains(owner_id) &&
                entity.properties.value("wall_id", std::string{}) == id) openings.push_back(&entity);
        Wall wall; std::string error;
        if (!read_document_wall(resolved, openings, wall, error)) throw std::invalid_argument(error);
        return make_wall(wall);
    }
    if (source.type == "slab") {
        Slab slab; std::string error;
        if (!read_document_slab(resolved, slab, error)) throw std::invalid_argument(error);
        return make_slab(slab);
    }
    reject("legacy host-copy has no supported native solid builder for this actual host type");
}
void admit_geometry(const AssemblyDocumentEntities& actual, const Target& target,
    const AssemblyExpansion& expansion, std::size_t& opening_work,
    std::map<std::string, TopoDS_Shape, std::less<>>& host_cache) {
    if (target.document_entity_id) {
        const auto& owner = actual.at(*target.document_entity_id);
        // The actual vertical-placement contract refuses unsupported assembly
        // modes; do not turn a floor/context reference into a fabricated pose.
        (void)resolve_vertical_placement(actual, owner);
        const auto placement = resolve_site_presentation(actual, owner.id);
        const auto geometry = make_assembly_geometry(expansion);
        if (geometry.shape.IsNull()) reject("independent assembly has no native source profiles");
        const auto& p = placement.forward;
        (void)transform_assembly_shape(geometry.shape,
            {{p.translation_m.x, p.translation_m.y, p.translation_m.z}, p.rotation_radians, 1.0});
        return;
    }
    // Embedded profile expansions have the native source's world convention;
    // the catalog's drawing organization never enrolls a synthetic child frame.
    if (!expansion.profiles.empty()) {
        if (make_assembly_geometry(expansion).shape.IsNull()) reject("embedded assembly has no native source profiles");
        return;
    }
    if (expansion.source_instance.profile_overrides || std::any_of(expansion.source_instance.nested_overrides.begin(),
            expansion.source_instance.nested_overrides.end(),[](const auto& node) { return node.profile_overrides.has_value(); }))
        reject("explicit component geometry has no native source profiles");
    if (!expansion.source_instance.placement) reject("metadata-only embedded assembly has no editable geometric presentation");
    const auto& host_id = expansion.source_instance.placement->host_entity_id;
    (void)resolve_site_presentation(actual, host_id); // Validate actual host/context dependencies.
    auto found = host_cache.find(host_id);
    if (found == host_cache.end()) found = host_cache.emplace(host_id, host_shape(actual, host_id, opening_work)).first;
    (void)transform_assembly_shape(found->second, root_pose(expansion.source_instance));
}
void require_active_dependencies(const AssemblyDocumentEntities& actual, const Target& target,
    const AssemblyInstance& instance, const ConstraintPhaseScope& scope, bool qualified_membership_priority = false) {
    const auto& owner_id = target.document_entity_id ? *target.document_entity_id : target.catalog_id;
    if (!target.document_entity_id && scope.assembly_memberships.inactive_roots.contains({target.catalog_id, target.instance_id}))
        reject("selected embedded root is inactive in the saved design");
    const bool declared_row = !target.document_entity_id &&
        scope.assembly_memberships.owners.contains({target.catalog_id, target.instance_id});
    if (!(qualified_membership_priority && declared_row) && scope.inactive_owner_ids.contains(owner_id))
        reject("selected source owner is inactive in the saved design");
    if (instance.placement) {
        const auto& host_id = instance.placement->host_entity_id;
        const auto host = actual.find(host_id);
        if (host == actual.end() || host->second.id != host_id || !is_complete_assembly_catalog_host_type(host->second.type))
            reject("embedded placement host is missing or has an unsupported source role");
        if (scope.inactive_owner_ids.contains(host_id) ||
            (qualified_membership_priority && assembly_host_inactive_in_phase(actual, scope.inactive_owner_ids, host_id)))
            reject("embedded assembly host is inactive in the saved design");
        (void)resolve_vertical_placement(actual, host->second);
        (void)resolve_site_presentation(actual, host_id);
    }
    if (target.document_entity_id) (void)resolve_site_presentation(actual, *target.document_entity_id);
}
struct ProtectedBaselines {
    std::set<std::string, std::less<>> owner_ids;
    std::map<std::string, std::vector<std::string>, std::less<>> registry_ids;
    std::map<std::string, std::vector<std::string>, std::less<>> registered_registry_ids;
    std::vector<std::string> registries_for(const std::string& owner_id) const {
        const auto found = registry_ids.find(owner_id);
        return found == registry_ids.end() ? std::vector<std::string>{} : found->second;
    }
    std::vector<std::string> registered_registries_for(const std::string& owner_id) const {
        const auto found = registered_registry_ids.find(owner_id);
        return found == registered_registry_ids.end() ? std::vector<std::string>{} : found->second;
    }
};
ProtectedBaselines protected_baselines(const AssemblyDocumentEntities& actual) {
    ProtectedBaselines result;
    for (const auto& [id, entity] : actual) if (entity.type == "model_phases") {
        const auto model = ModelPhases::from_json(entity.properties.at("model"));
        for (const auto& owner_id : model.entity_ids()) result.registered_registry_ids[owner_id].push_back(id);
        if (model.active_alternative()) for (const auto& owner_id : model.baseline_ids()) {
            result.owner_ids.insert(owner_id);
            result.registry_ids[owner_id].push_back(id);
        }
    }
    return result;
}
struct InspectedRoot {
    Target target;
    AssemblyInstance original;
    AssemblyInstance changed;
    AssemblyExpansion final_expansion;
    std::set<Path> reviewed_profile_paths;
};
struct PropertyEditSourceInspection;
std::optional<std::string> replacement_registry(const PropertyEditSourceInspection&, const InspectedRoot&);
// Shared actual-source inspection. It neither patches raw rows nor decides
// whether a protected change may publish; replay retains that separate guard.
struct PropertyEditSourceInspection {
    const AssemblyDocumentEntities& original;
    ConstraintPhaseScope scope;
    ProtectedBaselines baselines;
    std::map<std::string, AssemblyModel, std::less<>> models;
    std::map<RootKey, std::vector<const Intent*>> grouped;
    AssemblyExpansionBudget before_budget, after_budget;
    std::size_t opening_work{};
    std::map<std::string, TopoDS_Shape, std::less<>> host_cache;
    bool qualified_membership_priority{};

    PropertyEditSourceInspection(const AssemblyDocumentEntities& actual, std::span<const Intent> intents,
        bool qualified_priority = false)
        : original(actual), qualified_membership_priority(qualified_priority) {
        (void)encode_intents(intents);
        AssemblyCatalogTransferBudget catalog_budget;
        // Admit every source catalog before any expensive semantic model decode.
        for (const auto& [id, entity] : original) {
            if (id != entity.id) reject("actual map and source owner identity differ");
            if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalog_budget);
        }
        validate_document_assembly_instances(original);
        validate_document_site_frames(original);
        if (const auto diagnostic = validate_active_phase_constraint_integrity(original))
            throw std::invalid_argument(*diagnostic);
        scope = constraint_phase_scope(original);
        baselines = protected_baselines(original);
        for (const auto& intent : intents) grouped[{intent.target.catalog_id, intent.target.instance_id,
            intent.target.document_entity_id}].push_back(&intent);
    }

    InspectedRoot inspect(const std::vector<const Intent*>& rows) {
        auto target = rows.front()->target;
        target.part_path.clear();
        const auto catalog = original.find(target.catalog_id);
        if (catalog == original.end() || catalog->second.type != "assembly_model") reject("actual selected catalog is missing");
        if (!models.contains(target.catalog_id)) models.emplace(target.catalog_id,
            AssemblyModel::from_json(catalog->second.properties.at("model")));
        const auto& model = models.at(target.catalog_id);
        AssemblyInstance instance;
        if (target.document_entity_id) {
            const auto found = original.find(*target.document_entity_id);
            if (found == original.end() || found->second.type != "assembly_instance") reject("actual independent root is missing");
            const auto value = decode_document_assembly_instance(found->second, before_budget);
            if (value.assembly_catalog_id != target.catalog_id || value.instance.id != target.instance_id)
                reject("qualified independent root does not match actual source identity");
            instance = value.instance;
        } else {
            const auto found = std::find_if(model.instances().begin(), model.instances().end(),
                [&](const auto& value) { return value.id == target.instance_id; });
            if (found == model.instances().end()) reject("actual embedded catalog row is missing");
            instance = *found;
        }
        require_active_dependencies(original, target, instance, scope, qualified_membership_priority);
        const auto before = model.expand(instance, before_budget);
        admit_geometry(original, target, before, opening_work, host_cache);
        auto changed = change_instance(instance, before, model, rows);
        auto after = model.expand(changed, after_budget);
        if (after.profiles.empty() && std::any_of(rows.begin(), rows.end(), [&](const Intent* intent) {
            return intent->profile_geometry.operation == Operation::set &&
                std::any_of(before.profiles.begin(), before.profiles.end(), [&](const auto& profile) {
                    return profile.part_path == intent->target.part_path;
                });
        }))
            reject("profile Set suppresses the last actual solid; legacy host-copy is not replacement geometry");
        std::set<Path> reviewed_paths;
        for (const auto* intent : rows) if (intent->review_profile_topology || intent->initialize_deformed_profile_topology ||
            intent->review_initial_profile_geometry)
            reviewed_paths.insert(intent->target.part_path);
        return {std::move(target), std::move(instance), std::move(changed), std::move(after), std::move(reviewed_paths)};
    }

    void admit_final_geometry(const InspectedRoot& root) {
        admit_geometry(original, root.target, root.final_expansion, opening_work, host_cache);
    }
};
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid captured revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source digest must be a string");
    const auto& s = value.get_ref<const std::string&>();
    if (s.size() != 64 || !std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("invalid source digest");
}
std::vector<Intent> decode_request(const Json& request) {
    Budget{}.visit(request);
    fields(request, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "intents"});
    if (!request.at("version").is_number_integer() ||
        (request.at("version") != 1 && request.at("version") != 2 && request.at("version") != 3 && request.at("version") != 4 &&
            request.at("version") != 5 && request.at("version") != 6 && request.at("version") != 7))
        reject("unsupported request version");
    (void)revision(request.at("expected_revision"));
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(request.at(name));
    if (!request.at("source_saved_revision").is_null()) (void)revision(request.at("source_saved_revision"));
    auto intents = decode_intents(request.at("intents"));
    const bool geometry = std::any_of(intents.begin(), intents.end(), [](const Intent& intent) {
        return intent.profile_geometry.operation != Operation::unchanged;
    });
    if (request.at("version") == 1 && geometry) reject("v1 request cannot carry profile geometry authority");
    const bool review_initial = std::any_of(intents.begin(), intents.end(), [](const Intent& intent) {
        return intent.review_initial_profile_geometry;
    });
    if (request.at("version") != 7 && review_initial)
        reject("reviewed initial profile geometry requires request v7");
    if (request.at("version") == 7 && !review_initial)
        reject("v7 request requires reviewed initial profile geometry");
    const bool initialize_deformed = std::any_of(intents.begin(), intents.end(), [](const Intent& intent) {
        return intent.initialize_deformed_profile_topology;
    });
    if (request.at("version") != 6 && request.at("version") != 7 && initialize_deformed)
        reject("initial deformed topology authoring requires request v6");
    if (request.at("version") == 6 && !initialize_deformed)
        reject("v6 request requires initial deformed topology authoring");
    const bool review = std::any_of(intents.begin(), intents.end(), [](const Intent& intent) {
        return intent.review_profile_topology;
    });
    if (request.at("version") != 5 && request.at("version") != 6 && request.at("version") != 7 && review)
        reject("reviewed profile topology requires request v5");
    if (request.at("version") == 5 && !review) reject("v5 request requires reviewed profile topology");
    const bool initialize = std::any_of(intents.begin(), intents.end(), [](const Intent& intent) {
        return intent.initialize_profile_topology;
    });
    if (request.at("version") != 4 && request.at("version") != 5 && request.at("version") != 6 &&
        request.at("version") != 7 && initialize)
        reject("initial topology authoring requires request v4/v5");
    if (request.at("version") == 4 && !initialize) reject("v4 request requires initial topology authoring");
    const bool topology = std::any_of(intents.begin(), intents.end(), topology_preserving_geometry);
    if (request.at("version") != 3 && request.at("version") != 4 && request.at("version") != 5 &&
        request.at("version") != 6 && request.at("version") != 7 && topology)
        reject("identified profile Set requires request v3/v4/v5");
    if (request.at("version") == 3 && !topology) reject("v3 request requires topology-preserving profile Set");
    return intents;
}
void authenticate(const DocumentSnapshot& source, const Json& request) {
    if (!source.is_editable()) reject("captured source is read-only");
    if (source.history().empty() || source.history().size() > ProjectStore::maximum_revision_count ||
        source.revision() >= source.history().size() || source.history()[source.revision()].revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size()))
        reject("invalid source history/save metadata");
    const auto saved = request.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(request.at("source_saved_revision"))};
    if (source.revision() != revision(request.at("expected_revision")) || source.saved_revision_optional() != saved ||
        document_snapshot_digest(source) != request.at("source_snapshot_digest") ||
        document_authoring_source_digest_v2(source) != request.at("source_authoring_digest") ||
        entity_map_digest(source.entities()) != request.at("source_entities_digest")) reject("complete captured source changed");
}
} // namespace

Json encode_assembly_instance_property_edit_intent(const Intent& intent) {
    Budget typed_budget; preflight_intent(intent, typed_budget);
    auto result = encode_intent(intent);
    Budget{}.visit(result); (void)decode_intent(result); return result;
}
Intent decode_assembly_instance_property_edit_intent(const Json& value) {
    Budget{}.visit(value); return decode_intent(value);
}
std::vector<AssemblyInstancePropertyEditImpact> inspect_assembly_instance_property_edits(
    const AssemblyDocumentEntities& original, std::span<const Intent> intents) {
    PropertyEditSourceInspection source(original, intents, true);
    std::vector<AssemblyInstancePropertyEditImpact> result;
    result.reserve(source.grouped.size());
    for (const auto& [key, rows] : source.grouped) {
        (void)key;
        const auto root = source.inspect(rows);
        source.admit_final_geometry(root);
        const auto& owner_id = root.target.document_entity_id ? *root.target.document_entity_id : root.target.catalog_id;
        const bool declared = !root.target.document_entity_id && source.scope.assembly_memberships.owners.contains(
            {root.target.catalog_id, root.target.instance_id});
        if (declared) {
            const AssemblyPhaseRoot qualified{root.target.catalog_id,root.target.instance_id};
            const auto& owner=source.scope.assembly_memberships.owners.at(qualified);
            const auto membership=AssemblyPhaseMembership::from_json(original.at(owner.membership_entity_id).properties.at("model"));
            const auto registry=ModelPhases::from_json(original.at(owner.registry_id).properties.at("model"));
            const bool baseline=std::binary_search(membership.baseline_roots().begin(),membership.baseline_roots().end(),qualified);
            result.push_back({root.target, root.changed != root.original,
                baseline && registry.active_alternative() ? std::vector<std::string>{owner.registry_id} : std::vector<std::string>{},
                baseline && root.original.placement ? source.baselines.registries_for(root.original.placement->host_entity_id) :
                    std::vector<std::string>{}});
        } else result.push_back({root.target, root.changed != root.original, source.baselines.registries_for(owner_id),
                root.original.placement ? source.baselines.registries_for(root.original.placement->host_entity_id) :
                    std::vector<std::string>{}});
    }
    return result;
}

namespace {
std::optional<std::string> replacement_registry(const PropertyEditSourceInspection& source, const InspectedRoot& inspected) {
    const auto& root = inspected.target;
    const auto hosts = inspected.original.placement ?
        source.baselines.registries_for(inspected.original.placement->host_entity_id) : std::vector<std::string>{};
    const auto require_replacement_cohort = [&](const std::string& registry_id) {
        // Enrollment must not erase a legacy dependency on another registry's
        // choice, including a catalog/host currently admitted as proposed.
        const auto catalogs = source.baselines.registered_registries_for(root.catalog_id);
        if (catalogs.size() > 1) reject("selected catalog has overlapping actual phase registries");
        if (!catalogs.empty() && catalogs.front() != registry_id)
            reject("selected catalog and replacement root require different actual phase registries");
        if (inspected.original.placement) {
            const auto registered_hosts = source.baselines.registered_registries_for(inspected.original.placement->host_entity_id);
            if (registered_hosts.size() > 1) reject("selected placement host has overlapping actual phase registries");
            if (!registered_hosts.empty() && registered_hosts.front() != registry_id)
                reject("selected placement host and replacement root require different actual phase registries");
        }
    };
    if (!root.document_entity_id) {
        const AssemblyPhaseRoot qualified{root.catalog_id, root.instance_id};
        const auto owner = source.scope.assembly_memberships.owners.find(qualified);
        if (owner != source.scope.assembly_memberships.owners.end()) {
            const auto& registry_id = owner->second.registry_id;
            const auto registry = ModelPhases::from_json(source.original.at(registry_id).properties.at("model"));
            const auto membership = AssemblyPhaseMembership::from_json(
                source.original.at(owner->second.membership_entity_id).properties.at("model"));
            // An explicit proposed row retains its own authority even on a
            // protected host; legacy catalog/host classification cannot turn it
            // into a second baseline. A declared baseline cannot be replaced
            // across independent host choices by this single-registry producer.
            if (std::find(membership.baseline_roots().begin(), membership.baseline_roots().end(), qualified) ==
                membership.baseline_roots().end()) return std::nullopt;
            if (hosts.size() > 1) reject("selected placement host has overlapping active baseline registries");
            if (!hosts.empty() && hosts.front() != registry_id)
                reject("declared baseline root and protected placement host require different replacement registries");
            if (registry.active_alternative()) {
                require_replacement_cohort(registry_id);
                return registry_id;
            }
            return std::nullopt;
        }
    }
    const auto owners = source.baselines.registries_for(root.document_entity_id ? *root.document_entity_id : root.catalog_id);
    if (owners.size() > 1) reject("selected root has overlapping active baseline registries");
    if (hosts.size() > 1) reject("selected placement host has overlapping active baseline registries");
    if (!owners.empty() && !hosts.empty() && owners.front() != hosts.front())
        reject("selected root and protected placement host require different replacement registries");
    // The actual unchanged host supplies phase authority, not copy authority.
    // Only the selected row is enrolled/demolished and privately replaced.
    const auto registry_id = !owners.empty() ? std::optional<std::string>{owners.front()} :
        (hosts.empty() ? std::nullopt : std::optional<std::string>{hosts.front()});
    if (registry_id) require_replacement_cohort(*registry_id);
    return registry_id;
}
PhaseAssemblyInstancePropertyEditPlan phase_plan(PropertyEditSourceInspection& source) {
    PhaseAssemblyInstancePropertyEditPlan result;
    std::set<std::string, std::less<>> needs_membership;
    for (const auto& [key, rows] : source.grouped) {
        (void)key;
        const auto root = source.inspect(rows);
        source.admit_final_geometry(root);
        if (root.changed == root.original) { result.unchanged_roots.push_back(root.target); continue; }
        const auto registry_id = replacement_registry(source, root);
        if (!registry_id) { result.ordinary_roots.push_back(root.target); continue; }
        const auto registry = ModelPhases::from_json(source.original.at(*registry_id).properties.at("model"));
        PhaseAssemblyInstanceReplacementRoot replacement{root.target, *registry_id, *registry.active_alternative(), std::nullopt};
        if (!root.target.document_entity_id) {
            const auto declared = source.scope.assembly_memberships.owners.find({root.target.catalog_id, root.target.instance_id});
            if (declared != source.scope.assembly_memberships.owners.end())
                replacement.membership_entity_id = declared->second.membership_entity_id;
            else {
                // Enroll an undeclared legacy baseline in an existing companion
                // of this actual registry, or allocate exactly one new companion.
                for (const auto& [id, entity] : source.original) if (entity.type == assembly_phase_membership_entity_type &&
                    AssemblyPhaseMembership::from_json(entity.properties.at("model")).registry_id() == *registry_id) {
                    replacement.membership_entity_id = id; break;
                }
                if (!replacement.membership_entity_id) needs_membership.insert(*registry_id);
            }
        }
        result.replacement_roots.push_back(std::move(replacement));
    }
    result.required_membership_registry_ids.assign(needs_membership.begin(), needs_membership.end());
    return result;
}
void fresh_id(const std::string& value) {
    if (value.empty() || value.size() > 128 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("fresh allocation requires a bounded document identity");
}
void patch_root(AssemblyDocumentEntities& result, const AssemblyDocumentEntities& original,
    const InspectedRoot& root, const Target& destination) {
    const auto& source_catalog = original.at(root.target.catalog_id).properties.at("model");
    if (destination.document_entity_id) {
        auto& raw = result.at(*destination.document_entity_id).properties.at("instance");
        patch_instance(raw, root.original, root.changed, source_catalog, root.reviewed_profile_paths);
        promote_independent(raw);
    } else {
        auto& model = result.at(destination.catalog_id).properties.at("model");
        auto& rows = model.at("instances");
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const Json& value) { return value.at("id") == destination.instance_id; });
        if (row == rows.end()) reject("replayed actual destination row is missing");
        patch_instance(*row, root.original, root.changed, source_catalog, root.reviewed_profile_paths);
        complete_row_envelope(*row);
        promote_embedded(model, *row);
    }
}
void require_embedded_context_carry(const AssemblyDocumentEntities& original,
    const AssemblyDocumentEntities& result, const std::map<Target, Target>& destinations) {
    std::vector<std::pair<std::string, std::string>> before, after;
    for (const auto& [source, destination] : destinations) {
        if (source.document_entity_id) continue;
        if (!original.at(source.catalog_id).properties.contains("embedded_instance_drawing_contexts") &&
            !result.at(destination.catalog_id).properties.contains("embedded_instance_drawing_contexts")) continue;
        before.emplace_back(source.catalog_id, source.instance_id);
        after.emplace_back(destination.catalog_id, destination.instance_id);
    }
    if (before.empty()) return; // Historical absence retains its existing admission.
    const auto source_contexts = resolve_embedded_assembly_drawing_contexts(original, before);
    const auto destination_contexts = resolve_embedded_assembly_drawing_contexts(result, after);
    for (std::size_t i = 0; i < before.size(); ++i)
        if (source_contexts.at(before[i]) != destination_contexts.at(after[i]))
            reject("carried embedded root changed its actual drawing context");
}
} // namespace

namespace {
Target root_target(Target target) {
    target.part_path.clear();
    return target;
}
const Json& actual_instance_row(const AssemblyDocumentEntities& actual, const Target& target) {
    if (target.document_entity_id) {
        const auto owner = actual.find(*target.document_entity_id);
        if (owner == actual.end() || owner->second.type != "assembly_instance")
            reject("carried independent owner is missing");
        return owner->second.properties.at("instance");
    }
    const auto catalog = actual.find(target.catalog_id);
    if (catalog == actual.end() || catalog->second.type != "assembly_model") reject("carried catalog is missing");
    const auto& rows = catalog->second.properties.at("model").at("instances");
    const auto row = std::find_if(rows.begin(), rows.end(),
        [&](const Json& value) { return value.at("id") == target.instance_id; });
    if (row == rows.end()) reject("carried embedded row is missing");
    return *row;
}
AssemblyInstance actual_carried_instance(const AssemblyDocumentEntities& actual, const Target& target,
    const AssemblyModel& model, AssemblyExpansionBudget& budget) {
    if (target.document_entity_id) {
        const auto value = decode_document_assembly_instance(actual.at(*target.document_entity_id), budget);
        if (value.assembly_catalog_id != target.catalog_id || value.instance.id != target.instance_id)
            reject("carried qualified owner does not match actual catalog and row");
        return value.instance;
    }
    const auto row = std::find_if(model.instances().begin(), model.instances().end(),
        [&](const auto& value) { return value.id == target.instance_id; });
    if (row == model.instances().end()) reject("carried actual embedded instance is missing");
    return *row;
}
// The known envelope promotions add only absent empty collections. They do
// not authorize canonicalizing definitions, quantities, part poses or metadata.
void remove_empty_type_promotion(const Json& before, Json& after) {
    for (const auto* key : {"profiles", "parts"})
        if (!before.contains(key) && after.contains(key) && after.at(key).is_array() && after.at(key).empty())
            after.erase(key);
}
void require_carry_catalog(const Entity& before, const Entity& carried,
    const std::map<std::string, std::string, std::less<>>& instance_mapping) {
    const auto& saved = before.properties.at("model");
    const auto& model = carried.properties.at("model");
    // Decoding separately admits each actual supported dialect. A carry may
    // retain its dialect or promote it; it may never silently downgrade it.
    const auto schema_rank = [](const Json& schema) {
        for (int version = 1; version <= 9; ++version)
            if (schema == "sketch.assemblies.v" + std::to_string(version)) return version;
        reject("unsupported carried catalog dialect");
    };
    if (schema_rank(model.at("schema")) < schema_rank(saved.at("schema")))
        reject("carried catalog downgraded its saved dialect");
    const auto& types = model.at("types");
    const auto& saved_types = saved.at("types");
    if (types.size() != saved_types.size()) reject("carry changed catalog definitions");
    for (std::size_t i = 0; i < types.size(); ++i) {
        auto normalized = types[i];
        remove_empty_type_promotion(saved_types[i], normalized);
        if (normalized.dump() != saved_types[i].dump()) reject("carry changed raw catalog definitions");
    }
    if (model.at("materials").dump() != saved.at("materials").dump()) reject("carry changed raw catalog materials");
    // The caller derives the complete carried roster. Only this root's actual
    // selected row is checked below; other carried rows must survive untouched.
    // Compare borrowed fields without copying either complete instance roster.
    if (before.type != carried.type || before.required != carried.required ||
        before.extensions.dump() != carried.extensions.dump() || before.properties.size() != carried.properties.size())
        reject("carry changed raw catalog metadata");
    for (const auto& [key, value] : before.properties.items()) {
        if (key == "model") continue;
        const auto found = carried.properties.find(key);
        if (key == "embedded_instance_drawing_contexts") {
            auto proof = carried; proof.properties[key] = value;
            const auto expected = remap_embedded_assembly_drawing_contexts(before, proof, instance_mapping);
            if (found == carried.properties.end() || found->dump() != expected.properties.at(key).dump())
                reject("carry changed source-derived embedded drawing context records");
            continue;
        }
        if (found == carried.properties.end() || value.dump() != found->dump()) reject("carry changed raw catalog metadata");
    }
}
void require_carry_instance(const AssemblyDocumentEntities& original, const Target& source,
    const AssemblyInstance& before, const AssemblyDocumentEntities& carried, const Target& destination,
    const AssemblyInstance& after) {
    auto normalized = after;
    normalized.id = before.id;
    normalized.root_transform = before.root_transform;
    if (before.placement.has_value() != after.placement.has_value()) reject("carry changed root placement kind");
    normalized.placement = before.placement;
    if (normalized != before) reject("carry competes with type, overrides or nested part edits");
    const auto& raw_before = actual_instance_row(original, source);
    auto raw_after = actual_instance_row(carried, destination);
    raw_after["id"] = raw_before.at("id");
    if (raw_before.contains("schema")) {
        const auto rank = [](const Json& schema) {
            for (int version = 1; version <= 4; ++version)
                if (schema == "sketch.assembly-instance.v" + std::to_string(version)) return version;
            reject("unsupported carried independent dialect");
        };
        if (rank(raw_after.at("schema")) < rank(raw_before.at("schema")))
            reject("carry downgraded independent instance dialect");
        raw_after["schema"] = raw_before.at("schema");
    }
    if (raw_before.contains("root_transform")) raw_after["root_transform"] = raw_before.at("root_transform");
    else raw_after.erase("root_transform");
    if (before.placement) raw_after["placement"] = raw_before.at("placement");
    if (!raw_before.contains("nested_overrides") && raw_after.contains("nested_overrides") &&
        raw_after.at("nested_overrides").is_array() && raw_after.at("nested_overrides").empty())
        raw_after.erase("nested_overrides");
    if (raw_after.dump() != raw_before.dump()) reject("carry changed raw non-pose instance fields");
    if (source.document_entity_id) {
        const auto& owner = original.at(*source.document_entity_id);
        auto carried_owner = carried.at(*destination.document_entity_id);
        carried_owner.id = owner.id;
        carried_owner.properties["assembly_catalog_id"] = owner.properties.at("assembly_catalog_id");
        carried_owner.properties["instance"] = owner.properties.at("instance");
        if (!exact(owner, carried_owner)) reject("carry changed independent owner metadata");
    }
}
} // namespace

AssemblyDocumentEntities detail::calculate_carried_assembly_instance_property_entities(
    const AssemblyDocumentEntities& original, std::span<const Intent> intents,
    const AssemblyDocumentEntities& derived_carried, const std::map<Target, Target>& selected_destinations) {
    // This is a mathematical seam, deliberately separate from sealed authoring
    // replay. Raw carried maps never authenticate a source or grant publication.
    (void)encode_intents(intents);
    if (selected_destinations.size() != intents.size()) reject("carry map differs from exact selected roster");
    Budget target_budget;
    std::map<Target, Target> roots;
    std::set<Target> destinations;
    for (const auto& intent : intents) {
        const auto found = selected_destinations.find(intent.target);
        if (found == selected_destinations.end()) reject("carry map omits an original selected target");
        const auto& destination = found->second;
        Intent bounded; bounded.target = destination;
        preflight_intent(bounded, target_budget);
        (void)encode_assembly_instance_property_edit_intent(bounded);
        if (destination.part_path != intent.target.part_path ||
            destination.document_entity_id.has_value() != intent.target.document_entity_id.has_value())
            reject("carry map changes exact part paths or root kind");
        const auto [row, inserted] = roots.emplace(root_target(intent.target), root_target(destination));
        if (!inserted && row->second != root_target(destination)) reject("carry map splits one actual root");
        if (inserted && !destinations.insert(row->second).second) reject("carry map roots are not injective");
        if (destination.document_entity_id && *destination.document_entity_id != destination.instance_id)
            reject("carried independent owner and root identities differ");
    }
    AssemblyCatalogTransferBudget catalog_budget;
    const auto admit_catalogs = [&](const AssemblyDocumentEntities& entities) {
        for (const auto& [id, entity] : entities) {
            if (id != entity.id) reject("carry map and actual owner identity differ");
            if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, catalog_budget);
        }
    };
    // Charge both complete graphs cumulatively before decoding, dumping raw
    // envelopes or copying the carried map. No per-catalog budget reset.
    admit_catalogs(original);
    admit_catalogs(derived_carried);
    PropertyEditSourceInspection source(original, intents, true);
    validate_document_assembly_instances(derived_carried);
    std::map<std::string, AssemblyModel, std::less<>> carried_models;
    std::set<std::pair<std::string, std::string>> checked_catalogs;
    // Fixed bounded passes each accumulate across every selected root, as the
    // source inspector does. Repeated validation must not multiply authored
    // quantities or consume one graph's node allowance several times.
    AssemblyExpansionBudget carried_before_budget, authority_budget, carried_after_budget, final_budget;
    std::map<Target, InspectedRoot> expected;
    auto result = derived_carried;
    for (const auto& [key, rows] : source.grouped) {
        (void)key;
        auto root = source.inspect(rows); // Original slot/path/operation authority.
        const auto& destination = roots.at(root.target);
        const auto catalog = derived_carried.find(destination.catalog_id);
        if (catalog == derived_carried.end() || catalog->second.type != "assembly_model") reject("actual carried catalog is missing");
        if (!carried_models.contains(destination.catalog_id)) carried_models.emplace(destination.catalog_id,
            AssemblyModel::from_json(catalog->second.properties.at("model")));
        const auto& model = carried_models.at(destination.catalog_id);
        const auto carried = actual_carried_instance(derived_carried, destination, model, carried_before_budget);
        if (checked_catalogs.emplace(root.target.catalog_id, destination.catalog_id).second) {
            std::map<std::string, std::string, std::less<>> instance_mapping;
            std::set<std::string, std::less<>> mapped;
            for (const auto& [source_root, destination_root] : roots)
                if (!source_root.document_entity_id && source_root.catalog_id == root.target.catalog_id &&
                    destination_root.catalog_id == destination.catalog_id) {
                    instance_mapping.emplace(source_root.instance_id, destination_root.instance_id);
                    mapped.insert(destination_root.instance_id);
                }
            for (const auto& row : model.instances()) if (!mapped.contains(row.id))
                instance_mapping.emplace(row.id, row.id);
            require_carry_catalog(original.at(root.target.catalog_id), catalog->second, instance_mapping);
        }
        require_carry_instance(original, root.target, root.original, derived_carried, destination, carried);
        const auto before = model.expand(carried, carried_before_budget);
        // Part and declared-slot authority stays at the actual original source.
        const auto authority = source.models.at(root.target.catalog_id).expand(root.original, authority_budget);
        if (before.nodes.size() != authority.nodes.size()) reject("carry changed exact expansion paths");
        for (std::size_t i = 0; i < before.nodes.size(); ++i)
            if (before.nodes[i].part_path != authority.nodes[i].part_path || before.nodes[i].type_id != authority.nodes[i].type_id)
                reject("carry changed exact expansion paths or types");
        // Replay supplied operations, including source-equal explicit SETs,
        // onto the carried pose. Never translate original->changed deltas.
        auto changed = change_instance(carried, authority, source.models.at(root.target.catalog_id), rows);
        auto expansion = model.expand(changed, carried_after_budget);
        InspectedRoot carried_root{destination, carried, changed, std::move(expansion), root.reviewed_profile_paths};
        if (changed != carried) patch_root(result, derived_carried, carried_root, destination);
        expected.emplace(destination, std::move(carried_root));
    }
    // Final whole-graph admission uses current saved activity/constraints/Site
    // and current owner/host geometry. A fresh cache is essential even when a
    // host kept its ID but the independent host preparation changed its shape.
    admit_catalogs(result);
    validate_document_assembly_instances(result);
    validate_document_site_frames(result);
    require_embedded_context_carry(original, result, roots);
    if (const auto diagnostic = validate_active_phase_constraint_integrity(result)) throw std::invalid_argument(*diagnostic);
    const auto final_scope = constraint_phase_scope(result);
    std::map<std::string, AssemblyModel, std::less<>> final_models;
    std::map<std::string, TopoDS_Shape, std::less<>> final_host_cache;
    std::size_t final_opening_work{};
    for (const auto& [destination, root] : expected) {
        if (!final_models.contains(destination.catalog_id)) final_models.emplace(destination.catalog_id,
            AssemblyModel::from_json(result.at(destination.catalog_id).properties.at("model")));
        const auto& model = final_models.at(destination.catalog_id);
        auto actual = actual_carried_instance(result, destination, model, final_budget);
        auto intended = root.changed;
        const auto sort_paths = [](auto& instance) {
            std::sort(instance.nested_overrides.begin(), instance.nested_overrides.end(),
                [](const auto& a, const auto& b) { return a.part_path < b.part_path; });
        };
        sort_paths(actual); sort_paths(intended);
        if (actual != intended) reject("final carried row differs from supplied original operations");
        require_active_dependencies(result, destination, actual, final_scope, true);
        admit_geometry(result, destination, model.expand(actual, final_budget), final_opening_work, final_host_cache);
    }
    return result;
}

PhaseAssemblyInstancePropertyEditPlan inspect_phase_assembly_instance_property_edit_plan(
    const AssemblyDocumentEntities& original, std::span<const Intent> intents) {
    PropertyEditSourceInspection source(original, intents, true);
    return phase_plan(source);
}
void validate_phase_assembly_instance_property_edit_fresh_identities(
    const AssemblyDocumentEntities& retained, const std::vector<std::string>& identities) {
    const std::set<std::string, std::less<>> fresh(identities.begin(), identities.end());
    if (fresh.size() != identities.size()) reject("fresh identity inventory is not injective");
    validate_selection_edit_fresh_identity_tokens(retained, fresh);
    // Global children and actual render aliases use the common fence. Root row
    // IDs also reserve their local spelling for this new producer's allocations.
    // Model/source budget admission in the common fence precedes this scan.
    const auto local_ids = [&](const auto& self, const Json& value) -> void {
        if (value.is_object()) for (const auto& [key, child] : value.items()) {
            if (key == "id" && child.is_string() && fresh.contains(child.get_ref<const std::string&>()))
                reject("fresh identity reuses a retained catalog-local identity");
            self(self, child);
        } else if (value.is_array()) for (const auto& child : value) self(self, child);
    };
    for (const auto& [id, entity] : retained) {
        (void)id;
        if (entity.type == "assembly_model") local_ids(local_ids, entity.properties.at("model"));
        else if (entity.type == "assembly_instance" &&
            fresh.contains(entity.properties.at("instance").at("id").get<std::string>()))
            reject("fresh identity reuses a retained independent root identity");
    }
}
PhaseAssemblyInstancePropertyEditResult replay_phase_assembly_instance_property_edits(
    const AssemblyDocumentEntities& original, std::span<const Intent> intents,
    const PhaseAssemblyInstancePropertyEditAllocation& allocation) {
    // Admit the closed typed allocation before source model decoding or copies.
    if (intents.empty() || intents.size() > target_limit || allocation.roots.size() > target_limit ||
        allocation.membership_entity_ids.size() > target_limit) reject("typed replacement count exceeds bound");
    (void)encode_intents(intents); // Aggregate typed preflight precedes any intent copy.
    const bool geometry = std::any_of(intents.begin(), intents.end(), [](const Intent& intent) {
        return intent.profile_geometry.operation != Operation::unchanged;
    });
    // This is allocation admission only. Presentation correspondence is owned
    // by the enclosing sealed authoring request, never this mathematical replay.
    (void)encode_phase_assembly_instance_property_edit_authoring(
        {std::vector<Intent>(intents.begin(), intents.end()), allocation, geometry});
    PropertyEditSourceInspection inspection(original, intents, true);
    const auto plan = phase_plan(inspection);
    if (allocation.roots.size() != plan.replacement_roots.size() ||
        allocation.membership_entity_ids.size() != plan.required_membership_registry_ids.size())
        reject("allocation differs from actual changed replacement inventory");
    std::vector<std::string> fresh;
    std::set<std::string, std::less<>> reserved;
    const auto reserve = [&](const std::string& id) {
        fresh_id(id);
        if (!reserved.insert(id).second) reject("fresh replacement allocations collide");
        fresh.push_back(id);
    };
    for (const auto& root : plan.replacement_roots) {
        const auto found = allocation.roots.find(root.root);
        if (found == allocation.roots.end()) reject("missing exact qualified root allocation");
        const auto& ids = found->second;
        if (root.root.document_entity_id) {
            if (!ids.proposed_entity_id || ids.proposed_catalog_id) reject("independent allocation requires only fresh owner and instance");
            if (*ids.proposed_entity_id != ids.proposed_instance_id) reject("independent owner and instance require the same fresh identity");
            reserve(*ids.proposed_entity_id);
        } else {
            if (!ids.proposed_catalog_id || ids.proposed_entity_id) reject("embedded allocation requires only fresh private catalog and row");
            reserve(*ids.proposed_catalog_id);
            reserve(ids.proposed_instance_id);
        }
    }
    for (const auto& registry_id : plan.required_membership_registry_ids) {
        const auto found = allocation.membership_entity_ids.find(registry_id);
        if (found == allocation.membership_entity_ids.end()) reject("missing fresh required membership companion");
        reserve(found->second);
    }
    validate_phase_assembly_instance_property_edit_fresh_identities(original, fresh);
    // Bound the destination envelope and repeated decode work before the first
    // whole-map/private-catalog copy. A private catalog temporarily copies its
    // complete source envelope before narrowing the instance roster, so charge
    // that conservative upper bound for every replica, not once per source.
    AssemblyCatalogTransferBudget destination_budget;
    for (const auto& [id, entity] : original) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, destination_budget);
    }
    for (const auto& replacement : plan.replacement_roots)
        if (!replacement.root.document_entity_id)
            admit_existing_assembly_catalog_work(original.at(replacement.root.catalog_id), destination_budget);
    PhaseAssemblyInstancePropertyEditResult result{original, {}, {}};
    std::map<Target, Target> destinations;
    std::map<Target, InspectedRoot> inspected;
    std::map<std::string, ModelPhases, std::less<>> registries;
    std::map<std::string, AssemblyPhaseMembership, std::less<>> memberships;
    for (const auto& replacement : plan.replacement_roots) {
        const auto& source_root = replacement.root;
        const auto& ids = allocation.roots.at(source_root);
        Target destination{ids.proposed_catalog_id.value_or(source_root.catalog_id), ids.proposed_instance_id,
            ids.proposed_entity_id, {}};
        destinations.emplace(source_root, destination);
        if (source_root.document_entity_id) {
            auto copy = original.at(*source_root.document_entity_id);
            copy.id = *ids.proposed_entity_id;
            copy.properties.at("instance")["id"] = ids.proposed_instance_id;
            result.entities.emplace(copy.id, std::move(copy));
            if (!registries.contains(replacement.registry_id)) registries.emplace(replacement.registry_id,
                ModelPhases::from_json(original.at(replacement.registry_id).properties.at("model")));
            const auto& saved = registries.at(replacement.registry_id);
            auto owners = saved.entity_ids(); auto alternatives = saved.alternatives();
            owners.push_back(*ids.proposed_entity_id);
            auto active = std::find_if(alternatives.begin(), alternatives.end(),
                [&](const auto& row) { return row.id == replacement.alternative_id; });
            if (active == alternatives.end()) reject("replacement has no actual registry alternative");
            active->demolished_ids.push_back(*source_root.document_entity_id);
            active->proposed_ids.push_back(*ids.proposed_entity_id);
            registries.at(replacement.registry_id) = ModelPhases::create(std::move(owners), saved.baseline_ids(),
                std::move(alternatives), saved.active_alternative());
        } else {
            auto copy = original.at(source_root.catalog_id);
            copy.id = *ids.proposed_catalog_id;
            auto& model = copy.properties.at("model");
            const auto& rows = original.at(source_root.catalog_id).properties.at("model").at("instances");
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const Json& value) { return value.at("id") == source_root.instance_id; });
            if (row == rows.end()) reject("actual selected private catalog source row is missing");
            auto private_row = *row; private_row["id"] = ids.proposed_instance_id;
            // Keep every raw definition, material and metadata field needed by
            // the selected expansion. Unselected live instances stay at source.
            model["instances"] = Json::array({std::move(private_row)});
            copy = remap_embedded_assembly_drawing_contexts(original.at(source_root.catalog_id), copy,
                {{source_root.instance_id, ids.proposed_instance_id}});
            result.entities.emplace(copy.id, std::move(copy));
            const auto companion = replacement.membership_entity_id.value_or(
                allocation.membership_entity_ids.contains(replacement.registry_id) ?
                    allocation.membership_entity_ids.at(replacement.registry_id) : std::string{});
            if (!memberships.contains(companion)) {
                if (replacement.membership_entity_id) memberships.emplace(companion,
                    AssemblyPhaseMembership::from_json(original.at(companion).properties.at("model")));
                else memberships.emplace(companion, AssemblyPhaseMembership::create(replacement.registry_id, {}, {}, {}));
            }
            const auto& saved = memberships.at(companion);
            auto roots = saved.roots(); auto baselines = saved.baseline_roots(); auto alternatives = saved.alternatives();
            const AssemblyPhaseRoot old{source_root.catalog_id, source_root.instance_id};
            const AssemblyPhaseRoot proposed{destination.catalog_id, destination.instance_id};
            if (std::find(roots.begin(), roots.end(), old) == roots.end()) { roots.push_back(old); baselines.push_back(old); }
            roots.push_back(proposed);
            auto active = std::find_if(alternatives.begin(), alternatives.end(),
                [&](const auto& row) { return row.alternative_id == replacement.alternative_id; });
            if (active == alternatives.end()) {
                alternatives.push_back({replacement.alternative_id, {}, {}}); active = std::prev(alternatives.end());
            }
            active->demolished_roots.push_back(old); active->proposed_roots.push_back(proposed);
            memberships.at(companion) = AssemblyPhaseMembership::create(replacement.registry_id,
                std::move(roots), std::move(baselines), std::move(alternatives));
        }
    }
    // Reinspect using a fresh ledger rather than charging expansions repeatedly
    // against the inspection API's ledger. Every patch still derives from source.
    PropertyEditSourceInspection replay(original, intents, true);
    for (const auto& [key, rows] : replay.grouped) {
        (void)key;
        auto root = replay.inspect(rows);
        auto destination = destinations.contains(root.target) ? destinations.at(root.target) : root.target;
        if (root.changed != root.original) patch_root(result.entities, original, root, destination);
        for (const auto* intent : rows) {
            auto selected = destination; selected.part_path = intent->target.part_path;
            result.original_to_proposed.emplace(intent->target, std::move(selected));
        }
        inspected.emplace(std::move(destination), std::move(root));
    }
    for (const auto& [id, registry] : registries)
        result.entities.at(id).properties["model"] = retain_model_phase_source(original.at(id).properties.at("model"), registry);
    for (const auto& [id, membership] : memberships) {
        if (original.contains(id)) result.entities.at(id).properties["model"] =
            retain_assembly_phase_membership_source(original.at(id).properties.at("model"), membership);
        else result.entities.emplace(id, Entity{id, std::string(assembly_phase_membership_entity_type),
            Json{{"model", membership.to_json()}}, true});
    }
    validate_document_assembly_instances(result.entities);
    validate_document_site_frames(result.entities);
    require_embedded_context_carry(original, result.entities, destinations);
    if (const auto diagnostic = validate_active_phase_constraint_integrity(result.entities)) throw std::invalid_argument(*diagnostic);
    const auto final_scope = constraint_phase_scope(result.entities);
    AssemblyExpansionBudget final_budget;
    std::map<std::string, AssemblyModel, std::less<>> final_models;
    for (const auto& [destination, root] : inspected) {
        if (!final_models.contains(destination.catalog_id)) final_models.emplace(destination.catalog_id,
            AssemblyModel::from_json(result.entities.at(destination.catalog_id).properties.at("model")));
        const auto& model = final_models.at(destination.catalog_id);
        AssemblyInstance actual;
        if (destination.document_entity_id)
            actual = decode_document_assembly_instance(result.entities.at(*destination.document_entity_id), final_budget).instance;
        else {
            const auto row = std::find_if(model.instances().begin(), model.instances().end(),
                [&](const auto& value) { return value.id == destination.instance_id; });
            if (row == model.instances().end()) reject("final private/ordinary row is missing");
            actual = *row;
        }
        auto intended = root.changed; intended.id = destination.instance_id;
        const auto sort_paths = [](auto& instance) { std::sort(instance.nested_overrides.begin(), instance.nested_overrides.end(),
            [](const auto& a, const auto& b) { return a.part_path < b.part_path; }); };
        sort_paths(intended); sort_paths(actual);
        if (actual != intended) reject("actual destination differs from original-source typed intent");
        require_active_dependencies(result.entities, destination, actual, final_scope, true);
        admit_geometry(result.entities, destination, model.expand(actual, final_budget), replay.opening_work, replay.host_cache);
    }
    // Constructors and raw patches above never touch source replacement owners.
    // Check this explicitly, including an embedded source's exact selected row.
    for (const auto& replacement : plan.replacement_roots) {
        const auto& root = replacement.root;
        if (root.document_entity_id) {
            if (!exact(original.at(*root.document_entity_id), result.entities.at(*root.document_entity_id)))
                reject("replacement changed the protected independent original");
        } else {
            const auto row = [&](const AssemblyDocumentEntities& entities) -> const Json& {
                const auto& rows = entities.at(root.catalog_id).properties.at("model").at("instances");
                const auto found = std::find_if(rows.begin(), rows.end(), [&](const Json& value) { return value.at("id") == root.instance_id; });
                if (found == rows.end()) reject("protected embedded source row disappeared");
                return *found;
            };
            if (row(original).dump() != row(result.entities).dump()) reject("replacement changed protected embedded source row");
        }
    }
    const auto aliases = embedded_assembly_presentation_ids(result.entities);
    for (const auto& [binding, alias] : embedded_assembly_presentation_ids(original))
        if (!aliases.contains(binding) || aliases.at(binding) != alias)
            reject("private allocation changed a retained component presentation alias");
    for (const auto& replacement : plan.replacement_roots) if (!replacement.root.document_entity_id) {
        const auto& ids = allocation.roots.at(replacement.root);
        const auto& alias = aliases.at({*ids.proposed_catalog_id, ids.proposed_instance_id});
        if (!reserved.insert(alias).second) reject("fresh private presentation alias collides with fresh allocation");
        fresh.push_back(alias);
    }
    validate_phase_assembly_instance_property_edit_fresh_identities(original, fresh);
    std::sort(fresh.begin(), fresh.end()); result.fresh_identity_ids = std::move(fresh);
    return result;
}

AssemblyDocumentEntities replay_assembly_instance_property_entities(const AssemblyDocumentEntities& original,
    std::span<const Intent> intents) {
    PropertyEditSourceInspection source(original, intents, true);
    const auto& scope = source.scope;
    const auto& baselines = source.baselines.owner_ids;
    const auto& grouped = source.grouped;
    auto result = original;
    std::map<RootKey, AssemblyInstance> expected;
    std::set<RootKey> changed_roots;
    for (const auto& [key, rows] : grouped) {
        const auto root = source.inspect(rows);
        const auto& target = root.target;
        const auto& instance = root.original;
        const auto& changed = root.changed;
        const auto& catalog = original.at(target.catalog_id);
        expected.emplace(key, changed);
        if (changed == instance) continue;
        changed_roots.insert(key);
        const auto& owner_id = target.document_entity_id ? *target.document_entity_id : target.catalog_id;
        if (replacement_registry(source, root))
            reject("changed instance belongs to a protected active-alternative baseline; use full replacement");
        source.admit_final_geometry(root);
        if (target.document_entity_id) {
            auto& owner = result.at(owner_id);
            auto& raw = owner.properties.at("instance");
            patch_instance(raw, instance, changed, catalog.properties.at("model"), root.reviewed_profile_paths);
            promote_independent(raw);
        } else {
            auto& raw_model = result.at(owner_id).properties.at("model");
            auto& raw_rows = raw_model.at("instances");
            const auto row = std::find_if(raw_rows.begin(), raw_rows.end(),
                [&](const Json& value) { return value.at("id") == target.instance_id; });
            if (row == raw_rows.end()) reject("actual embedded raw row is missing");
            patch_instance(*row, instance, changed, catalog.properties.at("model"), root.reviewed_profile_paths);
            complete_row_envelope(*row);
            promote_embedded(raw_model, *row);
        }
    }
    // Actual final source admission is separate from typed temporary expansion.
    // Whole-map return retains every original owner and every untouched row.
    validate_document_assembly_instances(result);
    validate_document_site_frames(result);
    if (const auto diagnostic = validate_active_phase_constraint_integrity(result)) throw std::invalid_argument(*diagnostic);
    std::map<Target, Target> context_destinations;
    for (const auto& [key, rows] : grouped) {
        (void)key;
        auto root = rows.front()->target; root.part_path.clear();
        context_destinations.emplace(root, root);
    }
    require_embedded_context_carry(original, result, context_destinations);
    for (const auto& [id, entity] : original) if (entity.type == "assembly_model" &&
        entity.properties.contains("embedded_instance_drawing_contexts") &&
        entity.properties.at("embedded_instance_drawing_contexts").dump() !=
            result.at(id).properties.at("embedded_instance_drawing_contexts").dump())
        reject("same-row Properties changed raw embedded drawing context records");
    std::map<std::string, AssemblyModel, std::less<>> final_models;
    AssemblyExpansionBudget final_budget;
    for (const auto& [key, rows] : grouped) {
        const auto& target = rows.front()->target;
        AssemblyInstance actual;
        if (!final_models.contains(target.catalog_id)) final_models.emplace(target.catalog_id,
            AssemblyModel::from_json(result.at(target.catalog_id).properties.at("model")));
        const auto& model = final_models.at(target.catalog_id);
        if (target.document_entity_id)
            actual = decode_document_assembly_instance(result.at(*target.document_entity_id), final_budget).instance;
        else {
            const auto found = std::find_if(model.instances().begin(), model.instances().end(),
                [&](const auto& value) { return value.id == target.instance_id; });
            if (found == model.instances().end()) reject("final actual embedded row is missing");
            actual = *found;
        }
        auto intended = expected.at(key);
        // Decoders canonicalize path order; equality compares identities/content.
        const auto sort_paths = [](auto& instance) {
            std::sort(instance.nested_overrides.begin(), instance.nested_overrides.end(),
                [](const auto& a, const auto& b) { return a.part_path < b.part_path; });
        };
        sort_paths(intended); sort_paths(actual);
        if (actual != intended) reject("raw replay differs from original-source typed intent");
        require_active_dependencies(result, target, actual, scope, true);
        const auto expansion = model.expand(actual, final_budget);
        admit_geometry(result, target, expansion, source.opening_work, source.host_cache);
    }
    for (const auto& [id, entity] : original)
        if (entity.type != "assembly_model" &&
            (scope.inactive_owner_ids.contains(id) || baselines.contains(id)) && !exact(entity, result.at(id)))
            reject("replay changed an inactive or protected source owner");
    validate_current_assembly_catalog_phase_preservation(original, result);
    if (std::any_of(intents.begin(),intents.end(),[](const Intent& intent) {
            return intent.profile_geometry.operation!=Operation::unchanged;
        })) {
        std::vector<AssemblyDimensionTransformRequest> requests;
        for (const auto& [key,rows]:grouped) {
            if (!changed_roots.contains(key)) continue;
            auto root=rows.front()->target;root.part_path.clear();
            requests.push_back({root,root,AssemblyDimensionTransformMode::move,{}});
        }
        const auto dimensions=requests.empty() ? std::vector<Entity>{} :
            carry_assembly_dimension_transform_entities(original,result,requests);
        std::vector<std::string> changed;
        for (const auto& dimension:dimensions) changed.push_back(dimension.id);
        validate_ordinary_assembly_dimension_changes(original,changed);
        for (const auto& dimension:dimensions) result.at(dimension.id)=dimension;
    }
    return result;
}

AssemblyInstancePropertyEditPreparation::AssemblyInstancePropertyEditPreparation(Json request, AssemblyDocumentEntities entities)
    : request_(std::move(request)), entities_(std::move(entities)),
      digest_(request_.at("source_snapshot_digest").get<std::string>()) {}
Json validate_assembly_instance_property_edit_request(const Json& request) {
    (void)decode_request(request); return request;
}
AssemblyInstancePropertyEditPreparation prepare_assembly_instance_property_edit(const DocumentSnapshot& source,
    std::span<const Intent> intents) {
    auto wire = encode_intents(intents);
    const bool review_initial = std::any_of(wire.begin(), wire.end(), [](const Json& intent) { return intent.at("version") == 7; });
    const bool initialize_deformed = std::any_of(wire.begin(), wire.end(), [](const Json& intent) { return intent.at("version") == 6; });
    const bool review = std::any_of(wire.begin(), wire.end(), [](const Json& intent) { return intent.at("version") == 5; });
    const bool initialize = std::any_of(wire.begin(), wire.end(), [](const Json& intent) { return intent.at("version") == 4; });
    const bool topology = std::any_of(wire.begin(), wire.end(), [](const Json& intent) { return intent.at("version") == 3; });
    const bool geometry = std::any_of(wire.begin(), wire.end(), [](const Json& intent) { return intent.at("version") != 1; });
    Json request{{"version", review_initial ? 7 : initialize_deformed ? 6 : review ? 5 : initialize ? 4 : topology ? 3 : geometry ? 2 : 1}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", optional(source.saved_revision_optional())}, {"intents", std::move(wire)}};
    return replay_assembly_instance_property_edit_preparation(source, request);
}
AssemblyInstancePropertyEditPreparation replay_assembly_instance_property_edit_preparation(const DocumentSnapshot& source,
    const Json& request) {
    const auto intents = decode_request(request);
    authenticate(source, request);
    // Complete retained history, assets, references and derived read-only state
    // admit independently of our detached contribution. No preview token escapes.
    (void)Document::fork(source);
    auto result = replay_assembly_instance_property_entities(source.entities(), intents);
    ApplyEntityChanges command{source.revision(), {}, {}, "Assembly instance Properties preview"};
    for (const auto& [id, entity] : result) if (!exact(source.entities().at(id), entity))
        command.entity_changes.push_back(EntityChange::upsert(entity));
    if (!command.entity_changes.empty()) (void)Document::preview_command(source, command);
    return {request, std::move(result)};
}
void require_assembly_instance_property_edit_source_current(const DocumentSnapshot& source,
    const AssemblyInstancePropertyEditPreparation& preparation) {
    (void)decode_request(preparation.request()); authenticate(source, preparation.request());
}

} // namespace sketch
