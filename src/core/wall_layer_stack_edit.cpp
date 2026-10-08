#include "sketch/wall_layer_stack_edit.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/architecture.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/phase_wall_profile_capture.hpp"
#include "sketch/phase_wall_profile_edit.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t layer_limit = 1024;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t expression_limit = 4096;
constexpr const char* retirement_key = "wall_layer_stack_retirement";

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Wall layer stack edit: " + reason);
}
const Json* field(const Json& value, const std::string& name) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
}
void keys(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) invalid("invalid intent fields");
    for (const auto* name : names) if (!value.contains(name)) invalid("missing intent field");
}
const std::string& identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("identity must contain 1..128 supported ASCII characters");
    return id;
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("identity must be a string");
    return identity(value.get_ref<const std::string&>());
}
bool version_one(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 1;
}
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
// Check allocation/recursion budgets before serialization or native codecs.
// Opaque strings and keys reserve identities but never establish edit authority.
struct Budget {
    std::size_t nodes{}, bytes{};
    std::size_t byte_limit{64 * 1024 * 1024};
    Ids strings;
    void charge(std::size_t size) {
        if (size > byte_limit - bytes) invalid("JSON byte budget exceeded");
        bytes += size;
    }
    void text(const std::string& value) {
        charge(2);
        // Bound the serialized escaping before dump() or recursive codecs.
        for (const unsigned char c : value)
            charge(c < 0x20 ? 6 : (c == '"' || c == '\\' ? 2 : 1));
        strings.insert(value);
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) invalid("JSON node/nesting budget exceeded");
        charge(2);
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) {
            charge(1); read(child, depth + 1);
        } else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            charge(2); text(key); read(child, depth + 1);
        } else if (value.is_number()) {
            if (!std::isfinite(value.get<double>())) invalid("JSON number must be finite");
            charge(32);
        } else if (value.is_boolean() || value.is_null()) charge(5);
        else invalid("unsupported JSON value");
    }
};
void proof_budget(const Json& value) {
    Budget budget; budget.byte_limit = proof_limit; budget.read(value);
    if (value.dump().size() > proof_limit) invalid("proof byte budget exceeded");
}
void receipt_budget(const Json& value) {
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() ||
        expression->get_ref<const std::string&>().size() > expression_limit)
        invalid("quantity expression budget exceeded");
    proof_budget(value);
}
Quantity quantity(const Json& value, bool strict) {
    if (strict) {
        keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
        keys(value.at("exact_metres"), {"numerator", "denominator"});
    }
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (!std::isfinite(result.metres) || result.metres <= default_geometry_tolerance_metres)
        invalid("thickness must be finite and positive");
    return result;
}
Json quantity(const Quantity& value) {
    if (value.original_expression.size() > expression_limit) invalid("quantity expression budget exceeded");
    const auto result = encode_constraint_quantity_receipt(value);
    const auto parsed = quantity(result, true);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        invalid("quantity is internally inconsistent");
    return result;
}
bool known_receipt(const Json& value) {
    const auto version = field(value, "version");
    return version && version_one(*version);
}
Quantity admitted_receipt(const Json& raw, double metres) {
    if (!known_receipt(raw)) invalid("affected receipt version is unsupported");
    const auto result = quantity(raw, false);
    if (result.metres != metres) invalid("affected quantity receipt is stale");
    return result;
}
Json merge_receipt(Json raw, const Quantity& entered, double original) {
    (void)admitted_receipt(raw, original);
    const auto encoded = quantity(entered);
    for (const auto* key : {"version", "original_expression", "entered_unit"}) raw[key] = encoded.at(key);
    for (const auto* key : {"numerator", "denominator"})
        raw["exact_metres"][key] = encoded.at("exact_metres").at(key);
    proof_budget(raw);
    return raw;
}
double number(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) invalid("scalar must be finite");
    return value.get<double>();
}
double scalar(const Entity& entity, const char* canonical, const char* alias) {
    const auto a = field(entity.properties, canonical), b = field(entity.properties, alias);
    if (!a && !b) invalid("required scalar is missing");
    const auto value = number(a ? *a : *b);
    if (b && number(*b) != value) invalid("scalar aliases disagree");
    return value;
}
Wall native_wall(const Entity& entity, bool local_aliases = true, bool profile_receipts = true) {
    if (entity.type != "wall" || !entity.properties.is_object() || !entity.extensions.is_object())
        invalid("source must be an actual wall entity");
    (void)identity(entity.id);
    Budget budget; budget.read(entity.properties); budget.read(entity.extensions);
    const auto layers = field(entity.properties, "layers");
    if (layers && (!layers->is_array() || layers->size() > layer_limit)) invalid("layer inventory budget exceeded");
    // Admit before the shared profile helper decodes or serializes receipts.
    if (const auto values = field(entity.properties, "quantity_entries")) {
        if (!values->is_object() || values->size() > collection_limit)
            invalid("quantity_entries must be a bounded object");
        for (const auto& [pointer, receipt] : values->items()) {
            (void)pointer; proof_budget(receipt);
        }
    }
    if (const auto archive = field(entity.extensions, retirement_key))
        validate_wall_layer_stack_retirement(*archive);
    if (local_aliases) {
        (void)scalar(entity, "thickness_m", "thickness");
        (void)scalar(entity, "height_m", "height");
        (void)scalar(entity, "elevation_m", "elevation");
    }
    // Intermediate replay has not yet moved/updated indexed receipts. Its
    // geometry is admitted now; full profile authority is admitted afterward.
    if (profile_receipts) validate_wall_profile_source_entity(entity);
    Wall result; std::string diagnostic;
    if (!read_document_wall(entity, {}, result, diagnostic)) invalid("native wall " + entity.id + ": " + diagnostic);
    validate_wall_semantics(result);
    (void)make_wall(result);
    return result;
}
Json assignment(const WallLayerMaterial& value) {
    (void)identity(value.catalog_id); (void)identity(value.material_id);
    return {{"version", 1}, {"catalog_id", value.catalog_id}, {"material_id", value.material_id}};
}
WallLayerMaterial material(const Json& value) {
    keys(value, {"version", "catalog_id", "material_id"});
    if (!version_one(value.at("version"))) invalid("material assignment version is unsupported");
    return {identity(value.at("catalog_id")), identity(value.at("material_id"))};
}
const char* mode_name(WallLayerMaterialEditMode value) {
    switch (value) {
    case WallLayerMaterialEditMode::retain: return "retain";
    case WallLayerMaterialEditMode::clear: return "clear";
    case WallLayerMaterialEditMode::set: return "set";
    }
    invalid("material mode is unsupported");
}
const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && (!result->is_object() || result->size() > collection_limit))
        invalid("quantity_entries must be a bounded object");
    return result;
}
std::map<std::string, std::size_t, std::less<>> positions(const std::vector<WallLayer>& layers) {
    std::map<std::string, std::size_t, std::less<>> result;
    for (std::size_t i = 0; i < layers.size(); ++i) result.emplace(layers[i].id, i);
    return result;
}
bool retired_pointer(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    constexpr std::string_view prefix = "/layers/", suffix = "/thickness_m";
    const std::string_view path(text);
    if (!path.starts_with(prefix) || !path.ends_with(suffix) ||
        path.size() <= prefix.size() + suffix.size()) return false;
    const auto token = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    std::size_t index = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
    return (token.size() == 1 || token.front() != '0') && parsed.ec == std::errc{} &&
        parsed.ptr == token.data() + token.size() && index < layer_limit;
}
void retire(Entity& result, const std::string& id, const std::string& pointer, const Json& receipt) {
    auto existing = field(result.extensions, retirement_key);
    if (!existing) result.extensions[retirement_key] = {{"version", 1}, {"receipts", Json::array()}};
    auto& archive = result.extensions.at(retirement_key);
    validate_wall_layer_stack_retirement(archive);
    if (archive.at("receipts").size() >= collection_limit) invalid("retirement archive is full");
    Budget appended; appended.byte_limit = proof_limit;
    appended.read(archive); appended.charge(128); appended.text(id); appended.text(pointer); appended.read(receipt);
    archive.at("receipts").push_back({{"layer_id", id}, {"pointer", pointer}, {"receipt", receipt}});
    proof_budget(archive);
}
void total_receipts(const Entity& source, Entity& result, const Quantity& entered, double original) {
    const auto values = entries(source);
    if (values) for (const auto& [pointer, unused] : values->items()) {
        (void)unused;
        if (std::string_view(pointer).starts_with("/thickness_m/") ||
            std::string_view(pointer).starts_with("/thickness/"))
            invalid("total edit affects an unknown quantity binding");
    }
    const auto* primary = source.properties.contains("thickness_m") ? "thickness_m" : "thickness";
    for (const auto* name : {"thickness_m", "thickness"}) {
        const auto pointer = "/" + std::string(name);
        const auto previous = values ? field(*values, pointer) : nullptr;
        if (!source.properties.contains(name)) {
            if (previous) invalid("affected total receipt is dangling");
            continue;
        }
        result.properties[name] = entered.metres;
        if (previous || std::string_view(name) == primary) {
            if (!result.properties.contains("quantity_entries")) result.properties["quantity_entries"] = Json::object();
            result.properties.at("quantity_entries")[pointer] = previous ? merge_receipt(*previous, entered, original) : quantity(entered);
        }
    }
}
void layer_receipts(const Entity& source, Entity& result, const WallLayerStackEditIntent& intent,
    const Wall& before, const Wall& after) {
    const auto values = entries(source);
    const auto remaining = positions(after.layers);
    const auto old_positions = positions(before.layers);
    const auto before_json = field(source.properties, "layers"), after_json = field(result.properties, "layers");
    const Json empty = Json::array();
    const auto& old_rows = before_json ? *before_json : empty;
    const auto& new_rows = after_json ? *after_json : empty;
    const auto updated = entries(result);
    auto rebuilt = updated ? *updated : Json::object();
    // Remove all source indexed keys first so a swapped destination cannot
    // collide with the source row that has not yet moved.
    constexpr std::string_view prefix = "/layers/";
    if (values) for (const auto& [pointer, unused] : values->items()) {
        (void)unused;
        if (std::string_view(pointer).starts_with(prefix)) rebuilt.erase(pointer);
    }
    if (values) for (const auto& [pointer, receipt] : values->items()) {
        const std::string_view path(pointer);
        if (path == "/layers" && !exact(old_rows, new_rows))
            invalid("edit affects an unknown stack quantity binding");
        if (!path.starts_with(prefix)) continue;
        const auto tail = path.substr(prefix.size());
        const auto slash = tail.find('/');
        const auto token = tail.substr(0, slash);
        std::size_t index = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
        if (token.empty() || (token.size() > 1 && token.front() == '0') || parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size() || index >= before.layers.size()) {
            if (!exact(old_rows, new_rows)) invalid("edit affects a dangling or opaque indexed receipt");
            rebuilt[pointer] = receipt; continue;
        }
        const auto& layer = before.layers[index];
        const auto found = remaining.find(layer.id);
        const bool removed = found == remaining.end();
        const auto suffix = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash + 1);
        if (suffix != "thickness_m") {
            if (removed || found->second != index || !exact(old_rows.at(index), new_rows.at(found->second)))
                invalid("edit affects an unknown indexed quantity binding");
            rebuilt[pointer] = receipt; continue;
        }
        const bool changed = !removed && layer.thickness != after.layers[found->second].thickness;
        if (!removed && found->second == index && !changed) { rebuilt[pointer] = receipt; continue; }
        (void)admitted_receipt(receipt, layer.thickness);
        if (removed) { retire(result, layer.id, pointer, receipt); continue; }
        const auto destination = std::string(prefix) + std::to_string(found->second) + "/thickness_m";
        if (rebuilt.contains(destination)) invalid("indexed quantity remap collision");
        const auto& entered = intent.layers.at(found->second).thickness;
        if (changed && !entered) invalid("changed layer lacks entered thickness");
        rebuilt[destination] = changed ? merge_receipt(receipt, *entered, layer.thickness) : receipt;
    }
    for (std::size_t i = 0; i < after.layers.size(); ++i) {
        const auto& layer = after.layers[i];
        const auto previous = old_positions.find(layer.id);
        const bool changed = previous == old_positions.end() || before.layers[previous->second].thickness != layer.thickness;
        if (!changed) continue;
        const auto pointer = std::string(prefix) + std::to_string(i) + "/thickness_m";
        if (!rebuilt.contains(pointer)) {
            if (!intent.layers[i].thickness) invalid("new or changed layer lacks entered thickness");
            rebuilt[pointer] = quantity(*intent.layers[i].thickness);
        }
    }
    if (rebuilt.size() > collection_limit) invalid("quantity_entries budget exceeded");
    if (values || !rebuilt.empty()) result.properties["quantity_entries"] = std::move(rebuilt);
}
void admit_map(const Entities& source, const Ids& targets) {
    const auto organization = organize_project(source);
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    const auto bind = [&](const WallLayerMaterial& reference) {
        (void)assignment(reference);
        const auto found = source.find(reference.catalog_id);
        if (found == source.end() || found->second.id != found->first || found->second.type != "assembly_model" ||
            !found->second.properties.is_object() || !found->second.properties.contains("model"))
            invalid("material requires its actual assembly catalog");
        if (!catalogs.contains(reference.catalog_id)) catalogs.emplace(reference.catalog_id,
            AssemblyModel::from_json(found->second.properties.at("model")));
        const auto& materials = catalogs.at(reference.catalog_id).materials();
        if (std::none_of(materials.begin(), materials.end(), [&](const auto& m) { return m.id == reference.material_id; }))
            invalid("material is absent from its actual catalog");
    };
    for (const auto& id : targets) {
        const auto& entity = source.at(id);
        (void)native_wall(entity);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            invalid("target has unresolved actual drawing context");
        const auto wall = native_wall(resolve_vertical_placement(source, entity), false);
        if (const auto reference = field(entity.properties, "material_assignment")) {
            const auto version = field(*reference, "version"), catalog = field(*reference, "catalog_id"), item = field(*reference, "material_id");
            if (!version || !version_one(*version) || !catalog || !item) invalid("unsupported wall material assignment");
            bind({identity(*catalog), identity(*item)});
        }
        for (const auto& layer : wall.layers) if (layer.material) bind(*layer.material);
    }
}
Ids physical_members(const Entities& source, const Ids& targets, const ConstraintPhaseScope& scope) {
    Ids result = targets;
    // The shared physical helper parses all saved-active joins. Bound that
    // codec before its reserve(), then admit every member of affected joins.
    for (const auto& [id, entity] : source) {
        if (entity.type != "wall_join" || scope.inactive_owner_ids.contains(id)) continue;
        const auto rows = field(entity.properties, "wall_ids");
        if (!rows || !rows->is_array() || rows->size() > 32)
            invalid("wall join member inventory is unsupported");
        for (const auto& row : *rows) (void)identity(row);
        const auto join = parse_wall_join(entity.properties, id);
        if (std::any_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& member) {
            return scope.inactive_owner_ids.contains(member);
        }) || std::none_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& member) {
            return targets.contains(member);
        })) continue;
        result.insert(join.wall_ids.begin(), join.wall_ids.end());
    }
    std::map<std::string, std::size_t, std::less<>> hosted_counts;
    for (const auto& [id, entity] : source) {
        if (entity.type != "opening" || scope.inactive_owner_ids.contains(id)) continue;
        const auto host = field(entity.properties, "wall_id");
        if (!host || !host->is_string()) continue;
        const auto& wall_id = host->get_ref<const std::string&>();
        if (!result.contains(wall_id)) continue;
        if (++hosted_counts[wall_id] > collection_limit) invalid("hosted cut inventory budget exceeded");
        if (const auto values = entries(entity)) for (const auto& [pointer, receipt] : values->items()) {
            (void)pointer; proof_budget(receipt);
        }
    }
    return result;
}
Quantity captured(const Entity& before, const Entity& after, const std::vector<std::string>& pointers,
    double metres) {
    const auto a = entries(before), b = entries(after);
    for (const auto& pointer : pointers) {
        const auto old = a ? field(*a, pointer) : nullptr, receipt = b ? field(*b, pointer) : nullptr;
        if (!receipt || (old && exact(*old, *receipt))) continue;
        return admitted_receipt(*receipt, metres);
    }
    invalid("candidate lacks actual changed/new entered thickness receipt");
}
} // namespace

void validate_wall_layer_stack_retirement(const nlohmann::json& archive) {
    proof_budget(archive);
    keys(archive, {"version", "receipts"});
    if (!version_one(archive.at("version")) || !archive.at("receipts").is_array() ||
        archive.at("receipts").size() > collection_limit) invalid("retirement archive is unsupported");
    for (const auto& row : archive.at("receipts")) {
        keys(row, {"layer_id", "pointer", "receipt"});
        (void)identity(row.at("layer_id"));
        if (!retired_pointer(row.at("pointer"))) invalid("retirement pointer is invalid");
        (void)quantity(row.at("receipt"), false);
    }
}

nlohmann::json encode_wall_layer_stack_edit_intent(const WallLayerStackEditIntent& intent) {
    (void)identity(intent.wall_id);
    if (intent.layers.size() > layer_limit) invalid("layer inventory budget exceeded");
    // Bound the complete lexical inventory before quantity encoding or row
    // allocation. The fixed charge covers owned names, scalar proofs and JSON
    // punctuation; escaped strings are independently counted by Budget.
    Budget lexical; lexical.byte_limit = proof_limit; lexical.charge(256);
    lexical.text(intent.wall_id);
    const auto entered = [&](const std::optional<Quantity>& value) {
        if (!value) return;
        if (value->original_expression.size() > expression_limit)
            invalid("quantity expression budget exceeded");
        lexical.text(value->original_expression);
    };
    entered(intent.thickness);
    for (const auto& row : intent.layers) {
        lexical.charge(256); lexical.text(identity(row.layer_id)); entered(row.thickness);
        if (row.material) {
            lexical.text(identity(row.material->catalog_id));
            lexical.text(identity(row.material->material_id));
        }
    }
    Json rows = Json::array(); Ids ids;
    for (const auto& row : intent.layers) {
        if (!ids.insert(identity(row.layer_id)).second) invalid("duplicate layer identity");
        const auto mode = mode_name(row.material_mode);
        if ((row.material_mode == WallLayerMaterialEditMode::set) != row.material.has_value())
            invalid("set material requires a reference; retain/clear require null");
        rows.push_back({{"layer_id", row.layer_id}, {"thickness", row.thickness ? quantity(*row.thickness) : Json(nullptr)},
            {"material_mode", mode}, {"material", row.material ? assignment(*row.material) : Json(nullptr)}});
    }
    Json result{{"version", 1}, {"wall_id", intent.wall_id},
        {"thickness", intent.thickness ? quantity(*intent.thickness) : Json(nullptr)}, {"layers", std::move(rows)}};
    proof_budget(result);
    return result;
}
WallLayerStackEditIntent decode_wall_layer_stack_edit_intent(const nlohmann::json& value) {
    proof_budget(value); keys(value, {"version", "wall_id", "thickness", "layers"});
    if (!version_one(value.at("version"))) invalid("intent version is unsupported");
    WallLayerStackEditIntent result;
    result.wall_id = identity(value.at("wall_id"));
    if (!value.at("thickness").is_null()) result.thickness = quantity(value.at("thickness"), true);
    const auto& rows = value.at("layers");
    if (!rows.is_array() || rows.size() > layer_limit) invalid("layer inventory budget exceeded");
    for (const auto& raw : rows) {
        keys(raw, {"layer_id", "thickness", "material_mode", "material"});
        WallLayerStackRow row; row.layer_id = identity(raw.at("layer_id"));
        if (!raw.at("thickness").is_null()) row.thickness = quantity(raw.at("thickness"), true);
        const auto& mode = raw.at("material_mode");
        if (mode == "retain") row.material_mode = WallLayerMaterialEditMode::retain;
        else if (mode == "clear") row.material_mode = WallLayerMaterialEditMode::clear;
        else if (mode == "set") row.material_mode = WallLayerMaterialEditMode::set;
        else invalid("material mode is unsupported");
        if (!raw.at("material").is_null()) row.material = material(raw.at("material"));
        result.layers.push_back(std::move(row));
    }
    (void)encode_wall_layer_stack_edit_intent(result);
    return result;
}
Entity replay_wall_layer_stack_entity(const Entity& source, const WallLayerStackEditIntent& intent) {
    (void)encode_wall_layer_stack_edit_intent(intent);
    if (source.id != intent.wall_id) invalid("target differs from actual source identity");
    const auto before = native_wall(source);
    const auto old_positions = positions(before.layers);
    auto result = source;
    auto rows = Json::array();
    Budget occupied; occupied.text(source.id); occupied.text(source.type);
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) occupied.text(key);
    occupied.read(source.properties); occupied.read(source.extensions);
    for (const auto& row : intent.layers) {
        const auto old = old_positions.find(row.layer_id);
        Json raw;
        if (old == old_positions.end()) {
            if (!row.thickness || row.material_mode == WallLayerMaterialEditMode::retain)
                invalid("new layer requires entered thickness and explicit clear/set material");
            if (occupied.strings.contains(row.layer_id)) invalid("new layer identity collides with actual source data");
            raw = {{"id", row.layer_id}, {"thickness_m", row.thickness->metres}};
        } else {
            raw = source.properties.at("layers").at(old->second);
            if (row.thickness && row.thickness->metres != before.layers[old->second].thickness)
                raw.at("thickness_m") = row.thickness->metres;
        }
        if (row.material_mode == WallLayerMaterialEditMode::clear) raw.erase("material_assignment");
        else if (row.material_mode == WallLayerMaterialEditMode::set) {
            if (old == old_positions.end() || before.layers[old->second].material != row.material)
                raw["material_assignment"] = assignment(*row.material);
        }
        rows.push_back(std::move(raw));
    }
    if (source.properties.contains("layers") || !rows.empty()) result.properties["layers"] = std::move(rows);
    if (intent.thickness && intent.thickness->metres != before.thickness)
        total_receipts(source, result, *intent.thickness, before.thickness);
    const auto after = native_wall(result, true, false);
    layer_receipts(source, result, intent, before, after);
    (void)native_wall(result);
    return result;
}
std::map<std::string, Entity, std::less<>> replay_wall_layer_stack_entities(
    const std::map<std::string, Entity, std::less<>>& source, const std::vector<WallLayerStackEditIntent>& intents, bool validate_final_constraints) {
    if (intents.size() > collection_limit || source.size() > 65536) invalid("entity/target inventory budget exceeded");
    Budget occupied;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) occupied.text(key);
    for (const auto& [id, entity] : source) {
        (void)identity(id);
        if (entity.id != id) invalid("actual map contains inconsistent entity identity");
        occupied.text(id); occupied.text(entity.type); occupied.read(entity.properties); occupied.read(entity.extensions);
    }
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    Ids targets, fresh; std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto size = encode_wall_layer_stack_edit_intent(intent).dump().size();
        if (size > proof_limit - bytes) invalid("batch intent byte budget exceeded");
        bytes += size;
        if (!targets.insert(intent.wall_id).second) invalid("duplicate target");
        if (scope.inactive_owner_ids.contains(intent.wall_id)) invalid("target is inactive in the saved design");
        const auto found = source.find(intent.wall_id);
        if (found == source.end()) invalid("target is missing from actual source");
        const auto before = native_wall(found->second);
        const auto existing = positions(before.layers);
        for (const auto& row : intent.layers) if (!existing.contains(row.layer_id)) {
            if (occupied.strings.contains(row.layer_id) || !fresh.insert(row.layer_id).second)
                invalid("new layer identity collides with actual map/family/opaque data");
        }
    }
    const auto members = physical_members(source, targets, scope);
    admit_map(source, members);
    validate_active_wall_physical_dependencies(source, targets, true);
    auto result = source;
    bool depth_changed = false;
    for (const auto& intent : intents) {
        auto edited = replay_wall_layer_stack_entity(source.at(intent.wall_id), intent);
        depth_changed = depth_changed || native_wall(source.at(intent.wall_id)).thickness != native_wall(edited).thickness;
        result.at(intent.wall_id) = std::move(edited);
    }
    admit_map(result, members);
    validate_active_wall_physical_dependencies(result, targets, true);
    if (depth_changed) {
        const auto edits = exterior_wall_measurement_source_updates_active_phase(source, result, false);
        result = edited_boundary_entities_batch(result, edits);
    }
    // Measurement completion never grants authority over physical room review
    // or owners excluded by the saved phase selection.
    for (const auto& [id, entity] : source) {
        if (entity.type != "room" && !scope.inactive_owner_ids.contains(id)) continue;
        const auto retained = result.find(id);
        if (retained == result.end() || !exact(entity, retained->second))
            invalid("measurement completion changed a physical room or inactive owner");
    }
    if (validate_final_constraints) {
        if (const auto diagnostic = validate_active_phase_constraint_integrity(result))
            throw std::invalid_argument(*diagnostic);
    }
    return result;
}
std::optional<WallLayerStackEditIntent> capture_wall_layer_stack_edit(
    const Entity& original, const Entity& candidate, const std::optional<WallLayerStackEditIntent>& authored) {
    const auto before = native_wall(original), after = native_wall(candidate);
    if (exact(original, candidate)) return std::nullopt;
    if (authored) {
        const auto result = replay_wall_layer_stack_entity(original, *authored);
        if (!exact(result, candidate)) invalid("candidate differs from independent authored replay");
        return authored;
    }
    WallLayerStackEditIntent intent; intent.wall_id = original.id;
    if (before.thickness != after.thickness)
        intent.thickness = captured(original, candidate, {"/thickness_m", "/thickness"}, after.thickness);
    const auto existing = positions(before.layers);
    for (std::size_t i = 0; i < after.layers.size(); ++i) {
        const auto& layer = after.layers[i];
        const auto old = existing.find(layer.id);
        WallLayerStackRow row; row.layer_id = layer.id;
        const bool added = old == existing.end();
        if (added || before.layers[old->second].thickness != layer.thickness) {
            // Compare the candidate's qualified destination against the actual
            // source row receipt, rather than an unrelated old occupant there.
            const auto receipts = entries(candidate);
            const auto pointer = "/layers/" + std::to_string(i) + "/thickness_m";
            const auto receipt = receipts ? field(*receipts, pointer) : nullptr;
            if (!receipt) invalid("candidate layer lacks actual entered thickness receipt");
            if (!added) {
                const auto originals = entries(original);
                const auto previous = originals ? field(*originals, "/layers/" + std::to_string(old->second) + "/thickness_m") : nullptr;
                if (previous && exact(*previous, *receipt)) invalid("changed layer reuses its unchanged source receipt");
            }
            row.thickness = admitted_receipt(*receipt, layer.thickness);
        }
        if (added || before.layers[old->second].material != layer.material) {
            row.material_mode = layer.material ? WallLayerMaterialEditMode::set : WallLayerMaterialEditMode::clear;
            row.material = layer.material;
        }
        intent.layers.push_back(std::move(row));
    }
    const auto result = replay_wall_layer_stack_entity(original, intent);
    if (!exact(result, candidate)) invalid("candidate differs from independent stack replay");
    return intent;
}
} // namespace sketch
