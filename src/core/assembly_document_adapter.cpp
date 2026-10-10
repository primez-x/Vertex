#include "sketch/assembly_document_adapter.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/wall_semantics.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <iterator>
#include <numbers>
#include <stdexcept>
#include <set>

namespace sketch {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void identifier(const std::string& value) {
    require(!value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "assembly catalog/entity identity must be a valid document identifier");
}
void local_identifier(const std::string& value) {
    // Match AssemblyModel's local identity rules without imposing document-ID
    // characters/length on authored part, type, profile or embedded instance IDs.
    require(!value.empty() && !std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); }), "assembly identifier/name must not be blank");
}
void retain_assembly_transform_source(nlohmann::json& encoded, const nlohmann::json& raw,
    const AssemblyTransform& before, const AssemblyTransform& after) {
    auto& translation = encoded.at("translation_m");
    const auto& original = raw.at("translation_m");
    if (before.translation_m.x == after.translation_m.x) translation[0] = original[0];
    if (before.translation_m.y == after.translation_m.y) translation[1] = original[1];
    if (before.translation_m.z == after.translation_m.z) translation[2] = original[2];
    if (before.rotation_radians == after.rotation_radians)
        encoded["rotation_radians"] = raw.at("rotation_radians");
    if (before.scale == after.scale) encoded["scale"] = raw.at("scale");
    // Start with the canonical target so changed optional defaults disappear.
    // Only unchanged typed values retain the original presence/representation.
    if (before.mirrored_y == after.mirrored_y) {
        if (raw.contains("mirrored_y")) encoded["mirrored_y"] = raw.at("mirrored_y");
        else encoded.erase("mirrored_y");
    }
    if (before.vertical_scale == after.vertical_scale) {
        if (raw.contains("vertical_scale")) encoded["vertical_scale"] = raw.at("vertical_scale");
        else encoded.erase("vertical_scale");
    }
}
template<class T> void retain_assembly_override_source(nlohmann::json& encoded,
    const nlohmann::json& raw, const T& before, const T& after) {
    if (before.property_overrides == after.property_overrides)
        encoded["property_overrides"] = raw.at("property_overrides");
    if (before.material_overrides == after.material_overrides)
        encoded["material_overrides"] = raw.at("material_overrides");
    for (const auto& [key, quantity] : after.quantity_overrides) {
        const auto previous = before.quantity_overrides.find(key);
        if (previous == before.quantity_overrides.end()) continue;
        const auto& original = raw.at("quantity_overrides").at(key);
        if (previous->second == quantity) encoded.at("quantity_overrides").at(key) = original;
        else if (previous->second.value == quantity.value)
            encoded.at("quantity_overrides").at(key).at("value") = original.at("value");
    }
}
nlohmann::json retain_independent_assembly_instance_source(const nlohmann::json& raw,
    const AssemblyInstance& before, nlohmann::json encoded) {
    // Both sides have passed the closed independent codec. JSON numeric equality
    // cannot distinguish authored integer/float forms; compare decoded values.
    const auto after = decode_assembly_instance(encoded);
    if (before.id == after.id) encoded["id"] = raw.at("id");
    if (before.type_id == after.type_id) encoded["type_id"] = raw.at("type_id");
    if (raw.at("schema") == "sketch.assembly-instance.v2") encoded["schema"] = raw.at("schema");
    retain_assembly_transform_source(encoded.at("root_transform"), raw.at("root_transform"),
        *before.root_transform, *after.root_transform);
    retain_assembly_override_source(encoded, raw, before, after);

    using Path = std::vector<std::string>;
    std::map<Path, const AssemblyPathOverride*> previous, current;
    std::map<Path, const nlohmann::json*> generated;
    for (const auto& row : before.nested_overrides) previous.emplace(row.part_path, &row);
    for (const auto& row : after.nested_overrides) current.emplace(row.part_path, &row);
    for (const auto& row : encoded.at("nested_overrides"))
        generated.emplace(row.at("part_path").get<Path>(), &row);
    auto retained = nlohmann::json::array();
    // The decoder sorts by path, so positions are not identities. Surviving rows
    // retain authored source order; new paths follow the canonical target order.
    for (const auto& original : raw.at("nested_overrides")) {
        const auto path = original.at("part_path").get<Path>();
        const auto found = current.find(path);
        if (found == current.end()) continue;
        const auto& old = *previous.at(path);
        const auto& replacement = *found->second;
        auto row = *generated.at(path);
        row["part_path"] = original.at("part_path");
        retain_assembly_override_source(row, original, old, replacement);
        if (old.transform == replacement.transform) row["transform"] = original.at("transform");
        else if (old.transform && replacement.transform)
            retain_assembly_transform_source(row.at("transform"), original.at("transform"),
                *old.transform, *replacement.transform);
        retained.push_back(std::move(row));
    }
    for (const auto& row : encoded.at("nested_overrides"))
        if (!previous.contains(row.at("part_path").get<Path>())) retained.push_back(row);
    encoded["nested_overrides"] = std::move(retained);
    return encoded;
}
struct ArchitecturalMaterialReferenceSite {
    // Absence identifies the root properties.material_assignment envelope.
    std::optional<std::size_t> layer_index;
    ArchitecturalMaterialSourceReference reference;
};
std::vector<ArchitecturalMaterialReferenceSite> architectural_material_reference_sites(const Entity& source) {
    const auto& properties = source.properties;
    require(properties.is_object(), "architectural material source properties must be an object");
    std::vector<ArchitecturalMaterialReferenceSite> result;
    if (properties.contains("material_assignment")) {
        static constexpr std::array<std::string_view, 12> roles{
            "wall", "opening", "room", "room_boundary", "slab", "roof", "stair", "railing", "column", "beam", "roof_join", "corner_window"};
        require(std::find(roles.begin(), roles.end(), source.type) != roles.end(),
            "material assignment requires an architectural object");
        const auto& assignment = properties.at("material_assignment");
        require(assignment.is_object() && assignment.contains("version") &&
            assignment.at("version").is_number_integer() && assignment.at("version") == 1 &&
            assignment.contains("catalog_id") && assignment.at("catalog_id").is_string() &&
            assignment.contains("material_id") && assignment.at("material_id").is_string(),
            "architectural material assignment must be version 1 with string identities");
        ArchitecturalMaterialSourceReference reference{assignment.at("catalog_id").get<std::string>(),
            assignment.at("material_id").get<std::string>()};
        identifier(reference.catalog_id);
        local_identifier(reference.material_id);
        result.push_back({std::nullopt, std::move(reference)});
    }
    if ((source.type == "wall" || source.type == "slab") && properties.contains("layers")) {
        const auto& raw_layers = properties.at("layers");
        // The codecs enforce the same cap after decoding; charge it before their
        // reserve/iteration as this inventory may inspect detached input.
        require(raw_layers.is_array() && raw_layers.size() <= 1024,
            "architectural material source layer count exceeds the supported limit or is not an array");
        const auto canonical = properties.find("thickness_m");
        const auto legacy = properties.find("thickness");
        const auto* thickness = canonical != properties.end() ? &canonical.value() :
            (legacy != properties.end() ? &legacy.value() : nullptr);
        require(thickness != nullptr && thickness->is_number(),
            "architectural material source layers require a numeric thickness_m");
        const auto append = [&](const auto& layers) {
            for (std::size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].material) {
                    const auto& material = *layers[i].material;
                    result.push_back({i, {material.catalog_id, material.material_id}});
                }
            }
        };
        if (source.type == "wall") append(parse_wall_layers(raw_layers, thickness->get<double>()));
        else append(parse_slab_layers(raw_layers, thickness->get<double>()));
    }
    return result;
}
void validate_profile_presentation_identity(const AssemblyProfilePresentationIdentity& identity) {
    identifier(identity.catalog_id);
    local_identifier(identity.instance_id);
    if (identity.document_entity_id) {
        identifier(*identity.document_entity_id);
        require(identity.instance_id == *identity.document_entity_id,
            "assembly profile instance identity differs from its document root");
    }
    require(identity.part_path.size() < 32, "assembly graph depth budget exceeded");
    for (const auto& part : identity.part_path) local_identifier(part);
    local_identifier(identity.type_id);
    local_identifier(identity.profile_id);
}
AssemblyModel catalog(const Entity& entity) {
    require(entity.type == "assembly_model", "assembly catalog reference has the wrong entity type");
    require(entity.properties.is_object() && entity.properties.contains("model") &&
        entity.properties.at("model").is_object(), "assembly catalog requires a model object");
    return AssemblyModel::from_json(entity.properties.at("model"));
}
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> catalog_context_slots{{
    {"property_id", "property"}, {"building_id", "building"},
    {"floor_id", "floor"}, {"layer_id", "layer"}}};
void validate_catalog_transfer_budget(const AssemblyCatalogTransferBudget& budget) {
    require(budget.max_json_bytes <= 16777216 && budget.max_json_nodes <= 262144 &&
        budget.max_validation_work <= 67108864 &&
        budget.consumed_json_bytes <= budget.max_json_bytes &&
        budget.consumed_json_nodes <= budget.max_json_nodes &&
        budget.consumed_validation_work <= budget.max_validation_work &&
        budget.max_asset_payload_bytes <= 256ULL * 1024ULL * 1024ULL &&
        budget.max_asset_work_bytes <= 8ULL * 1024ULL * 1024ULL * 1024ULL &&
        budget.consumed_asset_payload_bytes <= budget.max_asset_payload_bytes &&
        budget.consumed_asset_work_bytes <= budget.max_asset_work_bytes,
        "invalid complete assembly catalog transfer budget");
}
void charge_catalog_work(std::size_t& consumed, std::size_t limit,
    std::size_t amount, const char* message) {
    if (amount > limit - consumed) {
        // Rejected attempts cannot retry the same expensive work for free.
        consumed = limit;
        throw std::invalid_argument(message);
    }
    consumed += amount;
}
void charge_catalog_product(AssemblyCatalogTransferBudget& budget,
    std::size_t first, std::size_t second) {
    const auto available = budget.max_validation_work - budget.consumed_validation_work;
    if (first != 0 && second > available / first) {
        budget.consumed_validation_work = budget.max_validation_work;
        throw std::invalid_argument("complete assembly catalog validation work budget exceeded");
    }
    charge_catalog_work(budget.consumed_validation_work, budget.max_validation_work,
        first * second, "complete assembly catalog validation work budget exceeded");
}
void charge_catalog_asset_bytes(std::uint64_t& consumed, std::uint64_t limit,
    std::uint64_t amount, const char* message) {
    if (amount > limit - consumed) {
        consumed = limit;
        throw std::invalid_argument(message);
    }
    consumed += amount;
}
void admit_catalog_json(const nlohmann::json& value, std::size_t depth,
    AssemblyCatalogTransferBudget& budget) {
    charge_catalog_work(budget.consumed_json_nodes, budget.max_json_nodes, 1,
        "complete assembly catalog JSON node budget exceeded");
    charge_catalog_product(budget, 1, 1);
    require(depth <= 64, "complete assembly catalog JSON depth budget exceeded");
    const auto charge_string = [&](const std::string& text) {
        // Six bytes per source byte covers JSON escaping without allocating a
        // dump of untrusted metadata. Include quotes and punctuation separately.
        const auto remaining = budget.max_json_bytes - budget.consumed_json_bytes;
        if (text.size() > remaining / 6) {
            budget.consumed_json_bytes = budget.max_json_bytes;
            throw std::invalid_argument("complete assembly catalog JSON byte budget exceeded");
        }
        charge_catalog_work(budget.consumed_json_bytes, budget.max_json_bytes, text.size() * 6,
            "complete assembly catalog JSON byte budget exceeded");
    };
    charge_catalog_work(budget.consumed_json_bytes, budget.max_json_bytes, 32,
        "complete assembly catalog JSON byte budget exceeded");
    require(!value.is_binary() && !value.is_discarded(),
        "complete assembly catalog requires ordinary JSON values");
    if (value.is_string()) charge_string(value.get_ref<const std::string&>());
    else if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            charge_string(item.key());
            admit_catalog_json(item.value(), depth + 1, budget);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) admit_catalog_json(item, depth + 1, budget);
    } else if (value.is_number_float()) {
        require(std::isfinite(value.get<double>()), "complete assembly catalog JSON numbers must be finite");
    }
}
void charge_catalog_string(AssemblyCatalogTransferBudget& budget, const std::string& value) {
    charge_catalog_product(budget, value.size(), 1);
    charge_catalog_product(budget, 1, 1);
}
// Keep the exact Document::reference_type_for_key contract: an engaged empty
// role is untyped; phase/material metadata is not a document owner reference.
std::optional<std::string_view> catalog_document_reference_role(std::string_view key) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 19> typed{{
        {"assembly_catalog_id", "assembly_model"}, {"property_id", "property"},
        {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"},
        {"boundary_id", "boundary"}, {"wall_id", "wall"}, {"opening_id", "opening"},
        {"room_id", "room"}, {"slab_id", "slab"}, {"roof_id", "roof"},
        {"stair_id", "stair"}, {"sheet_id", "sheet"}, {"view_id", "view"},
        {"constraint_id", "constraint"}, {"label_id", "label"}, {"column_id", "column"},
        {"beam_id", "beam"}, {"railing_id", "railing"}}};
    for (const auto& [candidate, role] : typed)
        if (key == candidate || (key.size() == candidate.size() + 1 && key.back() == 's' &&
            key.substr(0, candidate.size()) == candidate)) return role;
    for (const auto candidate : {"parent_id", "host_id", "target_id", "entity_id", "source_entity_id",
        "parent_ids", "host_ids", "target_ids", "entity_ids", "source_entity_ids", "refs", "references"})
        if (key == candidate) return std::string_view{};
    return std::nullopt;
}
bool catalog_context_slot(std::string_view key) {
    return std::any_of(catalog_context_slots.begin(), catalog_context_slots.end(),
        [&](const auto& slot) { return slot.first == key; });
}
bool catalog_asset_slot(std::string_view key) {
    return key == "asset_id" || key == "asset_ids" || key == "render_asset_id" || key == "render_asset_ids";
}
bool catalog_reference_collection(std::string_view key) {
    return key == "refs" || key == "references" || key.ends_with("_ids");
}
AssemblyCatalogSourceReferences catalog_document_references(const Entity& source,
    AssemblyCatalogTransferBudget& budget) {
    AssemblyCatalogSourceReferences result;
    std::set<std::string, std::less<>> assets;
    const auto owner = [&](const nlohmann::json& value, std::string_view role) {
        require(value.is_string(), "complete assembly catalog document reference must be a string");
        const auto& id = value.get_ref<const std::string&>();
        charge_catalog_string(budget, id);
        identifier(id);
        charge_catalog_product(budget, result.document_owner_roles.size() + 1, 1);
        const auto found = result.document_owner_roles.find(id);
        if (found == result.document_owner_roles.end()) result.document_owner_roles.emplace(id, role);
        else if (!role.empty()) {
            require(found->second.empty() || found->second == role,
                "complete assembly catalog document owner has conflicting roles");
            found->second = role;
        }
    };
    for (const auto& [key, value] : source.properties.items()) {
        require(key != "vertical_level_binding",
            "complete assembly catalog cannot contain a floor-only vertical level binding");
        if (catalog_context_slot(key)) continue;
        const auto role = catalog_document_reference_role(key);
        const bool asset = catalog_asset_slot(key);
        if (!role && !asset) continue;
        const auto append = [&](const nlohmann::json& reference) {
            if (!asset) { owner(reference, *role); return; }
            require(reference.is_string(), "complete assembly catalog asset reference must be a string");
            const auto& id = reference.get_ref<const std::string&>();
            charge_catalog_string(budget, id);
            identifier(id);
            charge_catalog_product(budget, assets.size() + 1, 1);
            assets.insert(id);
        };
        if (catalog_reference_collection(key)) {
            require(value.is_array(), "complete assembly catalog reference collection must be an array");
            for (const auto& reference : value) append(reference);
        } else append(value);
    }
    for (const auto& [key, role] : catalog_context_slots) {
        const auto reference = source.properties.find(std::string(key));
        if (reference == source.properties.end()) continue;
        require(reference->is_string(), "complete assembly catalog context owner must be a string");
        const auto& id = reference->get_ref<const std::string&>();
        charge_catalog_string(budget, id);
        identifier(id);
        const auto found = result.document_owner_roles.find(id);
        if (found != result.document_owner_roles.end()) {
            require(found->second.empty() || found->second == role,
                "complete assembly catalog context/document owner has conflicting roles");
            found->second = role;
        }
    }
    charge_catalog_product(budget, assets.size(), 1);
    result.asset_ids.assign(assets.begin(), assets.end());
    return result;
}
void validate_catalog_source_asset(const Asset& asset, AssemblyCatalogTransferBudget& budget) {
    // Match Document's native asset contract without manufacturing a document,
    // regenerating a hash, copying the payload or claiming import authority.
    charge_catalog_string(budget, asset.id);
    charge_catalog_string(budget, asset.media_type);
    charge_catalog_string(budget, asset.sha256);
    identifier(asset.id);
    require(!asset.media_type.empty() && asset.media_type.size() <= 256 &&
        asset.media_type.find_first_of("\r\n") == std::string::npos &&
        asset.media_type.find('\0') == std::string::npos,
        "complete assembly catalog source asset media type is invalid");
    require(asset.bytes.size() <= 256ULL * 1024ULL * 1024ULL,
        "complete assembly catalog source asset exceeds the native byte limit");
    charge_catalog_asset_bytes(budget.consumed_asset_payload_bytes, budget.max_asset_payload_bytes,
        static_cast<std::uint64_t>(asset.bytes.size()), "complete assembly catalog asset payload budget exceeded");
    require(asset.metadata.is_object(), "complete assembly catalog source asset metadata must be an object");
    const auto first_node = budget.consumed_json_nodes;
    const auto first_byte = budget.consumed_json_bytes;
    admit_catalog_json(asset.metadata, 0, budget);
    require(budget.consumed_json_nodes - first_node <= 100000,
        "complete assembly catalog source asset metadata exceeds the native node limit");
    charge_catalog_product(budget, budget.consumed_json_nodes - first_node +
        (budget.consumed_json_bytes - first_byte) / 8, 2);
    const auto validate_keys = [&](const auto& self, const nlohmann::json& value) -> void {
        if (value.is_object()) {
            for (auto item = value.begin(); item != value.end(); ++item) {
                require(item.key().size() <= 128,
                    "complete assembly catalog source asset metadata has an oversized key");
                self(self, item.value());
            }
        } else if (value.is_array()) for (const auto& child : value) self(self, child);
    };
    validate_keys(validate_keys, asset.metadata);
    // dump enforces strict UTF-8 exactly as native metadata admission does.
    (void)nlohmann::json(asset.media_type).dump();
    require(asset.metadata.dump().size() <= 1024 * 1024,
        "complete assembly catalog source asset metadata exceeds the native encoded size limit");
    require(asset.sha256.size() == 64 && std::all_of(asset.sha256.begin(), asset.sha256.end(),
        [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
        "complete assembly catalog source asset SHA-256 is invalid");
    // The hash implementation consumes the actual retained payload. Reserve its
    // linear work before hashing; a failure stays charged like catalog decoding.
    charge_catalog_asset_bytes(budget.consumed_asset_work_bytes, budget.max_asset_work_bytes,
        static_cast<std::uint64_t>(asset.bytes.size()), "complete assembly catalog asset hash work budget exceeded");
    require(sha256_hex(asset.bytes) == asset.sha256,
        "complete assembly catalog source asset SHA-256 does not match its bytes");
}
bool unsupported_catalog_owner_slot(std::string_view key) {
    // This is the canonical top-level reference contract, not a suffix scan.
    // Nested properties and extensions remain opaque, including local IDs.
    static constexpr std::string_view singles[]{
        "assembly_catalog_id", "property_id", "building_id", "floor_id", "layer_id",
        "boundary_id", "wall_id", "opening_id", "room_id", "slab_id", "roof_id",
        "stair_id", "sheet_id", "view_id", "constraint_id", "label_id", "column_id",
        "beam_id", "railing_id", "parent_id", "host_id", "target_id", "entity_id",
        "source_entity_id", "phase_id", "asset_id", "render_asset_id"};
    for (const auto candidate : singles) {
        if (key == candidate) {
            return std::none_of(catalog_context_slots.begin(), catalog_context_slots.end(),
                [&](const auto& slot) { return slot.first == key; });
        }
        if (key.size() == candidate.size() + 1 && key.back() == 's' &&
            key.substr(0, candidate.size()) == candidate) return true;
    }
    return key == "refs" || key == "references" || key == "vertical_level_binding" ||
        key == "material_assignment";
}
AssemblyCatalogSourceReferences preflight_complete_catalog(const Entity& source, AssemblyCatalogTransferBudget& budget,
    bool transport_owner_references = true,
    AssemblyCatalogSourceReferencePolicy policy = AssemblyCatalogSourceReferencePolicy::legacy_physical) {
    validate_catalog_transfer_budget(budget);
    require(policy == AssemblyCatalogSourceReferencePolicy::legacy_physical ||
        policy == AssemblyCatalogSourceReferencePolicy::document_authoring,
        "invalid complete assembly catalog reference policy");
    require(source.type == "assembly_model" && source.properties.is_object() && source.extensions.is_object(),
        "complete assembly catalog requires an actual assembly_model owner");
    if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) charge_catalog_string(budget, source.id);
    identifier(source.id);
    const auto first_node = budget.consumed_json_nodes;
    const auto first_byte = budget.consumed_json_bytes;
    charge_catalog_work(budget.consumed_json_nodes, budget.max_json_nodes, 4,
        "complete assembly catalog JSON node budget exceeded");
    charge_catalog_work(budget.consumed_json_bytes, budget.max_json_bytes,
        128 + 6 * (source.id.size() + source.type.size()),
        "complete assembly catalog JSON byte budget exceeded");
    charge_catalog_product(budget, 4, 1);
    admit_catalog_json(source.properties, 2, budget);
    admit_catalog_json(source.extensions, 2, budget);
    if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
        // Reserve raw string scanning and reference/binding traversal before
        // graph/map validation or any semantic decoder allocates authored data.
        charge_catalog_product(budget, budget.consumed_json_nodes - first_node +
            budget.consumed_json_bytes - first_byte, 1);
    }
    for (const auto& [key, value] : source.properties.items()) {
        (void)value;
        require(!transport_owner_references || policy == AssemblyCatalogSourceReferencePolicy::document_authoring ||
            !unsupported_catalog_owner_slot(key),
            "complete assembly catalog contains an unsupported canonical owner reference");
    }
    auto references = policy == AssemblyCatalogSourceReferencePolicy::document_authoring
        ? catalog_document_references(source, budget) : AssemblyCatalogSourceReferences{};
    const auto& model = source.properties.at("model");
    require(model.is_object(), "complete assembly catalog requires a model object");
    const auto& schema = model.at("schema");
    require(schema.is_string() && (schema == "sketch.assemblies.v1" || schema == "sketch.assemblies.v2" ||
        schema == "sketch.assemblies.v3" || schema == "sketch.assemblies.v4" ||
        schema == "sketch.assemblies.v5" || schema == "sketch.assemblies.v6" || schema == "sketch.assemblies.v7"),
        "unsupported complete assembly catalog schema");
    for (const auto* key : {"materials", "types", "instances"}) {
        require(model.at(key).is_array() && model.at(key).size() <= 4096,
            "complete assembly catalog collection budget exceeded");
    }
    struct GraphSize { std::size_t nodes{1}, depth{1}, segments{}; };
    struct RawType {
        const nlohmann::json* row;
        std::size_t own_segments{};
        int color{};
        GraphSize size;
    };
    std::map<std::string, RawType, std::less<>> types;
    for (const auto& row : model.at("types")) {
        require(row.is_object() && row.at("id").is_string(), "invalid complete assembly catalog type");
        const auto& id = row.at("id").get_ref<const std::string&>();
        if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
            charge_catalog_string(budget, id);
            charge_catalog_product(budget, types.size() + 1, 1);
        }
        local_identifier(id);
        auto [entry, inserted] = types.emplace(id, RawType{&row, 0, 0, {}});
        require(inserted, "duplicate complete assembly catalog type identity");
        if (row.contains("parts")) {
            require(row.at("parts").is_array() && row.at("parts").size() <= 4096,
                "complete assembly catalog part budget exceeded");
        }
        if (!row.contains("profiles")) continue;
        require(row.at("profiles").is_array() && row.at("profiles").size() <= 4096,
            "complete assembly catalog profile budget exceeded");
        for (const auto& profile : row.at("profiles")) {
            require(profile.is_object() && profile.at("outer").is_array() &&
                profile.at("outer").size() <= 1024 && profile.at("holes").is_array() &&
                profile.at("holes").size() <= 1024, "invalid complete assembly catalog profile collections");
            auto segments = profile.at("outer").size();
            for (const auto& hole : profile.at("holes")) {
                require(hole.is_array() && hole.size() <= 1024 - segments,
                    "complete assembly catalog profile segment budget exceeded");
                segments += hole.size();
            }
            std::size_t arcs{};
            const auto count_arcs = [&](const auto& boundary) {
                for (const auto& edge : boundary) {
                    require(edge.is_object() && edge.at("sweep_radians").is_number(),
                        "invalid complete assembly catalog boundary edge");
                    if (edge.at("sweep_radians") != 0) ++arcs;
                }
            };
            count_arcs(profile.at("outer"));
            for (const auto& hole : profile.at("holes")) count_arcs(hole);
            // Admission precedes create()'s topology pass. Squared total edges
            // covers outer/hole self checks, mutual contacts and containment;
            // the factor also reserves repeated analytical clearance work.
            charge_catalog_product(budget, segments + profile.at("holes").size() + 1,
                8 * (segments + profile.at("holes").size() + 1));
            // Contact allows three fresh 20,000-operation contexts. Clearance
            // allows three outer contexts plus three nested contact contexts;
            // cross-boundary validation can run clearance a second time. The
            // resulting 300,000-operation bound is charged for every possible
            // arc/edge pair before topology invokes certified arithmetic.
            charge_catalog_product(budget, arcs, 300000 * segments);
            require(segments <= 262144 - entry->second.own_segments,
                "complete assembly catalog type profile segment budget exceeded");
            entry->second.own_segments += segments;
        }
    }
    std::function<GraphSize(RawType&, std::size_t)> visit;
    visit = [&](RawType& type, std::size_t depth) -> GraphSize {
        charge_catalog_product(budget, 1, 1);
        require(depth <= 32 && type.color != 1, "complete assembly catalog graph cycle or depth limit");
        if (type.color == 2) return type.size;
        type.color = 1;
        GraphSize size{1, 1, type.own_segments};
        if (type.row->contains("parts")) {
            for (const auto& part : type.row->at("parts")) {
                require(part.is_object() && part.at("type_id").is_string(), "invalid complete assembly catalog part");
                const auto child = types.find(part.at("type_id").get_ref<const std::string&>());
                require(child != types.end(), "complete assembly catalog part type is missing");
                const auto nested = visit(child->second, depth + 1);
                require(nested.nodes <= 4096 - size.nodes && nested.segments <= 262144 - size.segments,
                    "complete assembly catalog expansion budget exceeded");
                size.nodes += nested.nodes;
                size.segments += nested.segments;
                size.depth = std::max(size.depth, nested.depth + 1);
                require(size.depth <= 32, "complete assembly catalog graph depth budget exceeded");
            }
        }
        type.color = 2;
        type.size = size;
        return size;
    };
    std::size_t validation_nodes{}, validation_segments{};
    for (auto& [id, type] : types) {
        (void)id;
        const auto size = visit(type, 1);
        require(size.nodes <= 16384 - validation_nodes && size.segments <= 1048576 - validation_segments,
            "complete assembly catalog aggregate validation budget exceeded");
        validation_nodes += size.nodes;
        validation_segments += size.segments;
    }
    std::size_t instance_nodes{}, instance_segments{};
    for (const auto& instance : model.at("instances")) {
        require(instance.is_object() && instance.at("type_id").is_string(), "invalid complete assembly catalog instance");
        if (instance.contains("nested_overrides")) {
            const auto& overrides = instance.at("nested_overrides");
            require(overrides.is_array() && overrides.size() <= 4096,
                "complete assembly catalog nested override budget exceeded");
            for (const auto& change : overrides) {
                require(change.is_object() && change.at("part_path").is_array() &&
                    !change.at("part_path").empty() && change.at("part_path").size() < 32,
                    "invalid complete assembly catalog nested override path");
            }
        }
        const auto type = types.find(instance.at("type_id").get_ref<const std::string&>());
        require(type != types.end(), "complete assembly catalog instance type is missing");
        require(type->second.size.nodes <= 4096 - instance_nodes &&
            type->second.size.segments <= 262144 - instance_segments,
            "complete assembly catalog aggregate instance budget exceeded");
        instance_nodes += type->second.size.nodes;
        instance_segments += type->second.size.segments;
    }
    // Conservatively reserve repeated linear lookups, overrides, maps and raw
    // profile copying for all unused-type probes and all embedded instances.
    // This deliberately refuses some large valid catalogs rather than letting
    // a small DAG multiply expensive authored data before charged admission.
    const auto weighted_source = budget.consumed_json_nodes - first_node +
        (budget.consumed_json_bytes - first_byte) / 8;
    charge_catalog_product(budget, weighted_source,
        4 * (validation_nodes + instance_nodes + types.size() + model.at("materials").size() + 1));
    charge_catalog_product(budget, validation_segments + instance_segments, 4);
    return references;
}
void revision(const DocumentSnapshot& source, Revision expected) {
    if (source.revision() != expected)
        throw DocumentError(DocumentErrorCode::stale_revision, "assembly command revision is stale");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, "assembly command source is read only");
}
const ArchitecturalGroupTransform& embedded_group_transform(const EmbeddedAssemblyGroupTarget& target,
    const ArchitecturalGroupTransform& shared) {
    const auto& transform = target.transform ? *target.transform : shared;
    const auto finite_point = [](const auto& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    require(finite_point(transform.pivot) && finite_point(transform.offset) &&
        std::isfinite(transform.rotation_z_radians) && std::isfinite(transform.scale) &&
        transform.scale > 0.0, "assembly group transform must be finite with a positive scale");
    return transform;
}
ApplyEntityChanges upsert(const DocumentSnapshot& source, Entity entity,
    Revision expected, const char* label) {
    auto candidate = source.entities();
    candidate.insert_or_assign(entity.id, entity);
    validate_document_assembly_instances(candidate);
    return {expected, {EntityChange::upsert(std::move(entity))}, {}, label};
}
}

std::vector<ArchitecturalMaterialSourceReference> architectural_material_source_refs(const Entity& source) {
    std::set<std::pair<std::string, std::string>> unique;
    for (const auto& site : architectural_material_reference_sites(source))
        unique.emplace(site.reference.catalog_id, site.reference.material_id);
    std::vector<ArchitecturalMaterialSourceReference> result;
    result.reserve(unique.size());
    for (const auto& [catalog_id, material_id] : unique) result.push_back({catalog_id, material_id});
    return result;
}
Entity remap_architectural_material_source_refs(const Entity& source,
    const std::map<std::string, std::string, std::less<>>& catalog_mapping) {
    const auto sites = architectural_material_reference_sites(source);
    // Validate all reached mappings before copying or patching any raw data.
    for (const auto& site : sites) {
        const auto destination = catalog_mapping.find(site.reference.catalog_id);
        require(destination != catalog_mapping.end(), "architectural material catalog mapping is missing");
        identifier(destination->second);
    }
    Entity result = source;
    for (const auto& site : sites) {
        auto& assignment = site.layer_index
            ? result.properties.at("layers").at(*site.layer_index).at("material_assignment")
            : result.properties.at("material_assignment");
        assignment.at("catalog_id") = catalog_mapping.at(site.reference.catalog_id);
    }
    return result;
}

void admit_complete_assembly_catalog_source(const Entity& source, AssemblyCatalogTransferBudget& budget,
    AssemblyCatalogSourceReferencePolicy policy) {
    try {
        (void)preflight_complete_catalog(source, budget, true, policy);
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid complete assembly catalog source: ") + error.what());
    }
}
void admit_existing_assembly_catalog_work(const Entity& existing, AssemblyCatalogTransferBudget& budget) {
    try {
        preflight_complete_catalog(existing, budget, false);
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid existing assembly catalog work: ") + error.what());
    }
}
nlohmann::json parse_assembly_catalog_transport_json(std::string_view bytes) {
    require(bytes.size() <= assembly_catalog_transport_byte_limit,
        "complete assembly catalog transport byte limit exceeded");
    std::size_t nodes = 0;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
        require(depth <= assembly_catalog_transport_depth_limit && ++nodes <= assembly_catalog_transport_node_limit,
            "complete assembly catalog transport JSON limit exceeded");
        if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
        else if (event == nlohmann::json::parse_event_t::object_end) keys.pop_back();
        else if (event == nlohmann::json::parse_event_t::key)
            require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,
                "duplicate complete assembly catalog transport key");
        if (value.is_string())
            require(value.get_ref<const std::string&>().size() <= assembly_catalog_transport_byte_limit,
                "complete assembly catalog transport string limit exceeded");
        if (value.is_number_float())
            require(std::isfinite(value.get<double>()), "complete assembly catalog transport number must be finite");
        return true;
    };
    return nlohmann::json::parse(bytes, callback);
}
AssemblyCatalogSourceReferences complete_assembly_catalog_source_refs(
    const Entity& source, AssemblyCatalogTransferBudget& budget, AssemblyCatalogSourceReferencePolicy policy) {
    try {
        auto result = preflight_complete_catalog(source, budget, true, policy);
        // Full semantic validation never becomes the representation: the raw
        // entity, including unused rows and authored ordering, stays untouched.
        (void)catalog(source);
        std::set<std::string, std::less<>> hosts, context;
        for (const auto& instance : source.properties.at("model").at("instances")) {
            if (!instance.contains("placement")) continue;
            const auto& id = instance.at("placement").at("host_entity_id").get_ref<const std::string&>();
            if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
                charge_catalog_string(budget, id);
                charge_catalog_product(budget, hosts.size() + 1, 1);
            }
            identifier(id);
            hosts.insert(id);
        }
        for (const auto& [key, role] : catalog_context_slots) {
            (void)role;
            const auto found = source.properties.find(std::string(key));
            if (found == source.properties.end()) continue;
            require(found->is_string(), "complete assembly catalog context owner must be a string");
            const auto& id = found->get_ref<const std::string&>();
            if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
                charge_catalog_string(budget, id);
                charge_catalog_product(budget, context.size() + 1, 1);
            }
            identifier(id);
            context.insert(id);
        }
        if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring)
            charge_catalog_product(budget, hosts.size() + context.size(), 1);
        result.hosted_entity_ids.assign(hosts.begin(), hosts.end());
        result.context_owner_ids.assign(context.begin(), context.end());
        return result;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid complete assembly catalog source: ") + error.what());
    }
}
Entity remap_complete_assembly_catalog_source_refs(const Entity& source,
    const std::map<std::string, std::string, std::less<>>& catalog_owner_mapping,
    const std::map<std::string, std::string, std::less<>>& host_owner_mapping,
    const std::map<std::string, std::string, std::less<>>& context_owner_mapping,
    AssemblyCatalogTransferBudget& budget, AssemblyCatalogSourceReferencePolicy policy,
    const std::map<std::string, std::string, std::less<>>& document_owner_mapping,
    const std::map<std::string, std::string, std::less<>>& asset_mapping) {
    const auto first_node = budget.consumed_json_nodes;
    const auto references = complete_assembly_catalog_source_refs(source, budget, policy);
    if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
        for (const auto* mapping : {&catalog_owner_mapping, &host_owner_mapping, &context_owner_mapping,
            &document_owner_mapping, &asset_mapping}) {
            charge_catalog_product(budget, mapping->size(), 1);
            for (const auto& [from, to] : *mapping) {
                charge_catalog_string(budget, from);
                charge_catalog_string(budget, to);
            }
        }
    }
    const auto mapped_id = [](const auto& mapping, const std::string& id) -> const std::string& {
        const auto found = mapping.find(id);
        require(found != mapping.end(), "complete assembly catalog owner mapping is missing");
        identifier(found->second);
        return found->second;
    };
    const auto& owner_id = mapped_id(catalog_owner_mapping, source.id);
    for (const auto& id : references.hosted_entity_ids) (void)mapped_id(host_owner_mapping, id);
    for (const auto& id : references.context_owner_ids) (void)mapped_id(context_owner_mapping, id);
    for (const auto& [id, role] : references.document_owner_roles) {
        (void)role;
        (void)mapped_id(document_owner_mapping, id);
    }
    for (const auto& id : references.asset_ids) (void)mapped_id(asset_mapping, id);
    if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
        // A reached global owner must retain one destination identity even when
        // it appears in different reference categories, including self refs.
        std::map<std::string, std::string_view, std::less<>> destinations;
        const auto consistent = [&](const auto& mapping, const std::string& id) {
            charge_catalog_string(budget, id);
            charge_catalog_product(budget, destinations.size() + 1, 1);
            const std::string_view destination = mapped_id(mapping, id);
            const auto [found, inserted] = destinations.emplace(id, destination);
            require(inserted || found->second == destination,
                "complete assembly catalog owner mappings disagree");
        };
        consistent(catalog_owner_mapping, source.id);
        for (const auto& id : references.hosted_entity_ids) consistent(host_owner_mapping, id);
        for (const auto& id : references.context_owner_ids) consistent(context_owner_mapping, id);
        for (const auto& [id, role] : references.document_owner_roles) {
            (void)role;
            consistent(document_owner_mapping, id);
        }
    }
    charge_catalog_product(budget, budget.consumed_json_nodes - first_node, 1);
    Entity result = source;
    result.id = owner_id;
    for (auto& instance : result.properties.at("model").at("instances")) {
        if (!instance.contains("placement")) continue;
        auto& host = instance.at("placement").at("host_entity_id");
        host = host_owner_mapping.at(host.get<std::string>());
    }
    for (const auto& [key, role] : catalog_context_slots) {
        (void)role;
        const auto found = result.properties.find(std::string(key));
        if (found != result.properties.end()) *found = context_owner_mapping.at(found->get<std::string>());
    }
    if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
        for (auto& [key, value] : result.properties.items()) {
            if (catalog_context_slot(key)) continue;
            const bool asset = catalog_asset_slot(key);
            if (!asset && !catalog_document_reference_role(key)) continue;
            const auto& mapping = asset ? asset_mapping : document_owner_mapping;
            const auto patch = [&](nlohmann::json& reference) {
                reference = mapping.at(reference.get_ref<const std::string&>());
            };
            if (catalog_reference_collection(key)) for (auto& reference : value) patch(reference);
            else patch(value);
        }
    }
    (void)complete_assembly_catalog_source_refs(result, budget, policy);
    return result;
}
bool is_complete_assembly_catalog_host_type(std::string_view type) noexcept {
    static constexpr std::array<std::string_view, 12> host_roles{
        "boundary", "measurement_boundary", "room_boundary", "wall", "opening", "slab", "roof",
        "stair", "railing", "column", "beam", "terrain_surface"};
    return std::find(host_roles.begin(), host_roles.end(), type) != host_roles.end();
}
AssemblyDocumentEntities capture_complete_assembly_catalog_sources(const DocumentSnapshot& source,
    const std::vector<std::string>& catalog_ids, AssemblyCatalogTransferBudget& budget,
    AssemblyCatalogSourceReferencePolicy policy) {
    validate_catalog_transfer_budget(budget);
    require(policy == AssemblyCatalogSourceReferencePolicy::legacy_physical ||
        policy == AssemblyCatalogSourceReferencePolicy::document_authoring,
        "invalid complete assembly catalog reference policy");
    require(catalog_ids.size() <= 4096, "complete assembly catalog capture count budget exceeded");
    std::set<std::string, std::less<>> selected;
    for (const auto& id : catalog_ids) {
        charge_catalog_product(budget, 1, 1);
        if (policy == AssemblyCatalogSourceReferencePolicy::document_authoring) {
            charge_catalog_string(budget, id);
            charge_catalog_product(budget, selected.size() + 1, 1);
        }
        identifier(id);
        require(selected.insert(id).second, "duplicate complete assembly catalog capture owner");
    }
    AssemblyDocumentEntities result;
    std::set<std::string, std::less<>> captured_assets;
    for (const auto& id : selected) {
        const auto owner = source.entities().find(id);
        require(owner != source.entities().end() && owner->second.id == id && owner->second.type == "assembly_model",
            "complete assembly catalog capture owner is missing or has inconsistent role/identity");
        const auto first_node = budget.consumed_json_nodes;
        const auto references = complete_assembly_catalog_source_refs(owner->second, budget, policy);
        for (const auto& host_id : references.hosted_entity_ids) {
            const auto host = source.entities().find(host_id);
            require(host != source.entities().end() && host->second.id == host_id &&
                is_complete_assembly_catalog_host_type(host->second.type),
                "complete assembly catalog source host is missing or has inconsistent role/identity");
        }
        for (const auto& [key, role] : catalog_context_slots) {
            const auto reference = owner->second.properties.find(std::string(key));
            if (reference == owner->second.properties.end()) continue;
            const auto& context_id = reference->get_ref<const std::string&>();
            const auto context = source.entities().find(context_id);
            require(context != source.entities().end() && context->second.id == context_id && context->second.type == role,
                "complete assembly catalog source context is missing or has inconsistent role/identity");
        }
        for (const auto& [reference_id, role] : references.document_owner_roles) {
            charge_catalog_string(budget, reference_id);
            const auto target = source.entities().find(reference_id);
            require(target != source.entities().end() && target->second.id == reference_id &&
                (role.empty() || target->second.type == role),
                "complete assembly catalog source document owner is missing or has inconsistent role/identity");
        }
        for (const auto& asset_id : references.asset_ids) {
            charge_catalog_string(budget, asset_id);
            const auto asset = source.assets().find(asset_id);
            require(asset != source.assets().end() && asset->second.id == asset_id,
                "complete assembly catalog source asset is missing or has inconsistent identity");
            charge_catalog_product(budget, captured_assets.size() + 1, 1);
            if (!captured_assets.insert(asset_id).second) continue;
            try {
                validate_catalog_source_asset(asset->second, budget);
            } catch (const nlohmann::json::exception& error) {
                throw std::invalid_argument(std::string("invalid complete assembly catalog source asset: ") + error.what());
            }
        }
        charge_catalog_product(budget, budget.consumed_json_nodes - first_node, 1);
        result.emplace(id, owner->second);
    }
    return result;
}

nlohmann::json encode_assembly_profile_presentation_identity(
    const AssemblyProfilePresentationIdentity& identity) {
    validate_profile_presentation_identity(identity);
    return {{"version", 1},
        {"origin", identity.document_entity_id ? "document_instance" : "embedded_catalog"},
        {"catalog_id", identity.catalog_id}, {"instance_id", identity.instance_id},
        {"document_entity_id", identity.document_entity_id
            ? nlohmann::json(*identity.document_entity_id) : nlohmann::json(nullptr)},
        {"part_path", identity.part_path}, {"type_id", identity.type_id},
        {"profile_id", identity.profile_id}};
}
AssemblyProfilePresentationIdentity decode_assembly_profile_presentation_identity(const nlohmann::json& value) {
    require(value.is_object(), "assembly profile presentation identity must be an object");
    require(value.contains("version") && value.at("version").is_number_integer() && value.at("version") == 1,
        "unsupported assembly profile presentation identity version");
    for (const auto* key : {"origin", "catalog_id", "instance_id", "type_id", "profile_id"})
        require(value.contains(key) && value.at(key).is_string(),
            "assembly profile presentation identity requires string identities and origin");
    require(value.contains("part_path") && value.at("part_path").is_array() &&
        value.at("part_path").size() < 32, "invalid assembly profile presentation path");
    for (const auto& part : value.at("part_path"))
        require(part.is_string(), "assembly profile presentation path identities must be strings");
    require(value.contains("document_entity_id"), "assembly profile presentation identity requires document root field");
    const auto& root = value.at("document_entity_id");
    const auto& origin = value.at("origin");
    require((origin == "document_instance" && root.is_string()) ||
        (origin == "embedded_catalog" && root.is_null()),
        "assembly profile presentation origin disagrees with its document root");
    AssemblyProfilePresentationIdentity result{value.at("catalog_id").get<std::string>(),
        value.at("instance_id").get<std::string>(), std::nullopt,
        value.at("part_path").get<std::vector<std::string>>(),
        value.at("type_id").get<std::string>(), value.at("profile_id").get<std::string>()};
    if (!root.is_null()) result.document_entity_id = root.get<std::string>();
    validate_profile_presentation_identity(result);
    return result;
}
nlohmann::json assembly_profile_presentation_key(const std::string& catalog_id,
    const AssemblyInstance& instance, const AssemblyExpandedProfile& profile,
    const std::optional<std::string>& document_entity_id) {
    return encode_assembly_profile_presentation_identity({catalog_id, instance.id,
        document_entity_id, profile.part_path, profile.type_id, profile.profile.id});
}

EmbeddedAssemblyPresentationIds embedded_assembly_presentation_ids(const AssemblyDocumentEntities& entities) {
    EmbeddedAssemblyPresentationIds aliases;
    std::map<std::string, std::size_t, std::less<>> alias_counts;
    std::set<std::string, std::less<>> occupied;
    for (const auto& [id, entity] : entities) {
        require(id == entity.id, "document map identity differs from entity identity");
        occupied.insert(id);
        if (entity.type == kAnnotationEntityType) {
            require(entity.properties.is_object() && entity.properties.contains("state") &&
                entity.properties.at("state").is_object(), "annotation requires a state object");
            const auto& state = entity.properties.at("state");
            for (const auto* key : {"labels", "symbols"}) {
                require(state.contains(key) && state.at(key).is_array(), "annotation requires child arrays");
                for (const auto& child : state.at(key)) {
                    require(child.is_object() && child.contains("id") && child.at("id").is_string(),
                        "annotation child requires an identity");
                    const auto child_id = child.at("id").get<std::string>();
                    local_identifier(child_id);
                    occupied.insert(child_id);
                }
            }
        } else if (entity.type == "assembly_model") {
            identifier(id);
            require(entity.properties.is_object() && entity.properties.contains("model") &&
                entity.properties.at("model").is_object(), "assembly catalog requires a model object");
            const auto& model = entity.properties.at("model");
            require(model.contains("instances") && model.at("instances").is_array(),
                "assembly catalog requires an instances array");
            for (const auto& instance : model.at("instances")) {
                require(instance.is_object() && instance.contains("id") && instance.at("id").is_string(),
                    "embedded assembly requires an instance identity");
                const auto instance_id = instance.at("id").get<std::string>();
                local_identifier(instance_id);
                const auto alias = id + ":instance:" + instance_id;
                require(aliases.emplace(std::pair{id, instance_id}, alias).second,
                    "duplicate embedded assembly instance identity");
                ++alias_counts[alias];
            }
        }
    }
    // Reserve even ambiguous raw aliases so escaped IDs cannot claim another
    // embedded instance's historical spelling. Framed payloads are injective;
    // leading '@' escapes also cover arbitrary authored annotation child IDs.
    auto reserved = occupied;
    for (const auto& [alias, count] : alias_counts) { (void)count; reserved.insert(alias); }
    for (auto& [binding, alias] : aliases) {
        if (alias_counts.at(alias) == 1 && !occupied.contains(alias)) continue;
        auto qualified = "@assembly-instance:" + nlohmann::json::array({binding.first, binding.second}).dump();
        while (reserved.contains(qualified)) qualified.insert(qualified.begin(), '@');
        reserved.insert(qualified);
        alias = std::move(qualified);
    }
    return aliases;
}
std::string embedded_assembly_presentation_id(const AssemblyDocumentEntities& entities,
    const std::string& catalog_id, const std::string& instance_id) {
    identifier(catalog_id);
    local_identifier(instance_id);
    const auto aliases = embedded_assembly_presentation_ids(entities);
    const auto found = aliases.find({catalog_id, instance_id});
    require(found != aliases.end(), "embedded assembly presentation references a missing instance");
    return found->second;
}
std::optional<AssemblyDocumentInstance> resolve_embedded_assembly_presentation(
    const AssemblyDocumentEntities& entities, std::string_view render_id) {
    if (render_id.empty() || entities.contains(render_id)) return std::nullopt;
    const auto aliases = embedded_assembly_presentation_ids(entities);
    const auto found = std::find_if(aliases.begin(), aliases.end(),
        [&](const auto& value) { return value.second == render_id; });
    if (found == aliases.end()) return std::nullopt;
    const auto model = catalog(entities.at(found->first.first));
    const auto instance = std::find_if(model.instances().begin(), model.instances().end(),
        [&](const auto& value) { return value.id == found->first.second; });
    require(instance != model.instances().end(), "embedded assembly presentation source instance is missing");
    return AssemblyDocumentInstance{found->first.first, *instance};
}

AssemblyDocumentInstance decode_document_assembly_instance(const Entity& entity) {
    require(entity.type == "assembly_instance", "expected an assembly_instance entity");
    identifier(entity.id);
    const auto& p = entity.properties;
    require(p.is_object(), "assembly instance properties must be an object");
    require(p.contains("version") && p.at("version").is_number_integer() && p.at("version") == 1,
        "unsupported assembly instance entity version");
    require(p.contains("form") && p.at("form").is_string() && p.at("form") == "independent_assembly_instance",
        "unsupported assembly instance entity form");
    require(p.contains("assembly_catalog_id") && p.at("assembly_catalog_id").is_string(),
        "assembly instance requires assembly_catalog_id");
    require(p.contains("instance") && p.at("instance").is_object(), "assembly instance requires an instance object");
    // Historical placement fields are owned forbidden aliases, not opaque data.
    for (const auto* key : {"host", "host_entity_id", "placement", "root_transform", "type_id"})
        require(!p.contains(key), "independent assembly instance has a forbidden placement/type alias");
    AssemblyDocumentInstance result{p.at("assembly_catalog_id").get<std::string>(),
        decode_assembly_instance(p.at("instance"))};
    identifier(result.assembly_catalog_id);
    require(result.instance.id == entity.id, "assembly instance identity differs from its document entity");
    require(result.instance.root_transform.has_value() && !result.instance.placement,
        "independent assembly instance requires root_transform and forbids host placement");
    return result;
}
Entity encode_document_assembly_instance(const Entity& source, const AssemblyDocumentInstance& value) {
    identifier(source.id);
    require(source.type == "assembly_instance", "expected an assembly_instance entity");
    require(source.properties.is_object(), "assembly instance properties must be an object");
    identifier(value.assembly_catalog_id);
    require(value.instance.id == source.id, "assembly instance identity differs from its document entity");
    require(value.instance.root_transform.has_value() && !value.instance.placement,
        "independent assembly instance requires root_transform and forbids host placement");
    Entity result = source;
    auto encoded = encode_assembly_instance(value.instance);
    if (source.properties.contains("instance")) {
        const auto& raw = source.properties.at("instance");
        const auto before = decode_assembly_instance(raw);
        // Duplication callers have already changed Entity.id. Validate the saved
        // envelope against its original root before authoring the new identity.
        auto original = source;
        original.id = before.id;
        (void)decode_document_assembly_instance(original);
        encoded = retain_independent_assembly_instance_source(raw, before, std::move(encoded));
    } else {
        result.properties["version"] = 1;
        result.properties["form"] = "independent_assembly_instance";
    }
    result.properties["assembly_catalog_id"] = value.assembly_catalog_id;
    result.properties["instance"] = std::move(encoded);
    (void)decode_document_assembly_instance(result);
    return result;
}
AssemblyExpansion expand_document_assembly_instance(const Entity& entity,
    const AssemblyDocumentEntities& entities, AssemblyExpansionBudget& budget) {
    const auto value = decode_document_assembly_instance(entity);
    const auto found = entities.find(value.assembly_catalog_id);
    require(found != entities.end(), "assembly instance references a missing catalog");
    return catalog(found->second).expand(value.instance, budget);
}
std::map<std::string, AssemblyExpansion, std::less<>> expand_document_assembly_instances(
    const AssemblyDocumentEntities& entities, AssemblyExpansionBudget& budget) {
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    for (const auto& [id, entity] : entities) {
        require(id == entity.id, "document map identity differs from entity identity");
        if (entity.type == "assembly_model") catalogs.emplace(id, catalog(entity));
    }
    auto candidate_budget = budget;
    // Legacy catalog instances still participate in the document-wide limits
    // and declared totals, even when they carry only a host placement.
    for (const auto& [id, model] : catalogs) {
        (void)id;
        for (const auto& instance : model.instances()) (void)model.expand(instance, candidate_budget);
    }
    std::map<std::string, AssemblyExpansion, std::less<>> result;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "assembly_instance") continue;
        const auto value = decode_document_assembly_instance(entity);
        const auto found = catalogs.find(value.assembly_catalog_id);
        require(found != catalogs.end(), "assembly instance references a missing catalog");
        result.emplace(id, found->second.expand(value.instance, candidate_budget));
    }
    budget = std::move(candidate_budget);
    return result;
}
void validate_document_assembly_instances(const AssemblyDocumentEntities& entities) {
    AssemblyExpansionBudget budget;
    (void)expand_document_assembly_instances(entities, budget);
}
AssemblyDocumentEntities assembly_clipboard_dependencies(const DocumentSnapshot& source,
    const std::vector<std::string>& root_entity_ids) {
    AssemblyExpansionBudget source_budget;
    (void)expand_document_assembly_instances(source.entities(), source_budget);
    std::map<std::string, std::vector<AssemblyInstance>, std::less<>> roots;
    std::set<std::string> selected;
    AssemblyDocumentEntities closure;
    for (const auto& id : root_entity_ids) {
        require(selected.insert(id).second, "duplicate assembly clipboard root");
        const auto found = source.entities().find(id);
        require(found != source.entities().end(), "assembly clipboard root is missing");
        const auto value = decode_document_assembly_instance(found->second);
        roots[value.assembly_catalog_id].push_back(value.instance);
        closure.emplace(id, found->second);
    }
    AssemblyDocumentEntities result;
    for (const auto& [catalog_id, instances] : roots) {
        const auto& source_entity = source.entities().at(catalog_id);
        const auto model = catalog(source_entity);
        std::set<std::string> type_ids, material_ids;
        std::vector<std::string> pending;
        auto materials = [&](const auto& bindings) {
            for (const auto& [slot, id] : bindings) { (void)slot; material_ids.insert(id); }
        };
        for (const auto& instance : instances) {
            pending.push_back(instance.type_id);
            materials(instance.material_overrides);
            for (const auto& override : instance.nested_overrides) materials(override.material_overrides);
        }
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (!type_ids.insert(id).second) continue;
            const auto found = std::find_if(model.types().begin(), model.types().end(),
                [&](const auto& type) { return type.id == id; });
            require(found != model.types().end(), "assembly clipboard type is missing");
            materials(found->materials);
            for (const auto& part : found->parts) {
                pending.push_back(part.type_id);
                materials(part.material_overrides);
            }
        }
        std::vector<AssemblyType> types;
        std::vector<AssemblyMaterial> retained_materials;
        for (const auto& type : model.types()) if (type_ids.contains(type.id)) types.push_back(type);
        for (const auto& material : model.materials())
            if (material_ids.contains(material.id)) retained_materials.push_back(material);
        Entity entity = source_entity;
        entity.properties["model"] = retain_assembly_catalog_dialect(source_entity.properties.at("model"),
            AssemblyModel::create(std::move(retained_materials), std::move(types), {}).to_json());
        result.emplace(catalog_id, entity);
        closure.emplace(catalog_id, std::move(entity));
    }
    AssemblyExpansionBudget closure_budget;
    (void)expand_document_assembly_instances(closure, closure_budget);
    return result;
}
Entity remap_independent_assembly_instance(const Entity& source,
    const std::map<std::string, std::string>& identity_mapping) {
    const auto value = decode_document_assembly_instance(source);
    const auto root = identity_mapping.find(source.id);
    const auto catalog_id = identity_mapping.find(value.assembly_catalog_id);
    require(root != identity_mapping.end() && catalog_id != identity_mapping.end(),
        "assembly clipboard requires root and catalog identity mappings");
    identifier(root->second); identifier(catalog_id->second);
    require(root->second != catalog_id->second, "assembly root and catalog mapping collide");
    Entity result = source;
    result.id = root->second;
    result.properties.at("instance").at("id") = result.id;
    result.properties.at("assembly_catalog_id") = catalog_id->second;
    (void)decode_document_assembly_instance(result);
    return result;
}
std::vector<AssemblyDocumentTypeUpdateImpact> preview_document_assembly_type_update(
    const AssemblyDocumentEntities& entities, const std::string& catalog_id, AssemblyType replacement) {
    const auto found=entities.find(catalog_id);
    require(found!=entities.end(),"assembly catalog update target is missing");
    const auto type_id=replacement.id;
    auto candidate=entities;
    candidate.at(catalog_id).properties["model"]=retain_assembly_catalog_dialect(found->second.properties.at("model"),
        catalog(found->second).with_type(std::move(replacement)).to_json());
    AssemblyExpansionBudget before_budget,after_budget;
    const auto before=expand_document_assembly_instances(entities,before_budget);
    const auto after=expand_document_assembly_instances(candidate,after_budget);
    std::vector<AssemblyDocumentTypeUpdateImpact> result;
    for(const auto& [id,expansion]:before) {
        const auto value=decode_document_assembly_instance(entities.at(id));
        if(value.assembly_catalog_id==catalog_id && std::any_of(expansion.nodes.begin(),expansion.nodes.end(),
            [&](const auto& node){return node.type_id==type_id;}))
            result.push_back({id,expansion,after.at(id),value});
    }
    return result;
}
ApplyEntityChanges assembly_instance_create_command(const DocumentSnapshot& source, Entity entity,
    const AssemblyDocumentInstance& value, Revision expected_revision) {
    revision(source, expected_revision);
    require(!source.entities().contains(entity.id), "assembly instance identity already exists");
    return upsert(source, encode_document_assembly_instance(entity, value), expected_revision, "Create assembly instance");
}
ApplyEntityChanges assembly_instance_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const AssemblyDocumentInstance& value, Revision expected_revision) {
    revision(source, expected_revision);
    const auto found = source.entities().find(entity_id);
    require(found != source.entities().end(), "assembly instance update target is missing");
    (void)decode_document_assembly_instance(found->second);
    return upsert(source, encode_document_assembly_instance(found->second, value), expected_revision, "Update assembly instance");
}
ApplyEntityChanges assembly_instance_remove_command(const DocumentSnapshot& source,
    const std::string& entity_id, Revision expected_revision) {
    revision(source, expected_revision);
    const auto found = source.entities().find(entity_id);
    require(found != source.entities().end(), "assembly instance removal target is missing");
    (void)decode_document_assembly_instance(found->second);
    auto candidate = source.entities();
    candidate.erase(entity_id);
    validate_document_assembly_instances(candidate);
    return {expected_revision, {EntityChange::erase(entity_id)}, {}, "Remove assembly instance"};
}
ApplyEntityChanges independent_assembly_type_update_command(const DocumentSnapshot& source,
    const std::string& catalog_id, AssemblyType replacement, Revision expected_revision) {
    revision(source, expected_revision);
    const auto found = source.entities().find(catalog_id);
    require(found != source.entities().end(), "assembly catalog update target is missing");
    Entity entity = found->second;
    entity.properties["model"] = retain_assembly_catalog_dialect(found->second.properties.at("model"),
        catalog(entity).with_type(std::move(replacement)).to_json());
    return upsert(source, std::move(entity), expected_revision, "Update assembly type");
}
ApplyEntityChanges independent_assembly_type_remove_command(const DocumentSnapshot& source,
    const std::string& catalog_id, const std::string& type_id, Revision expected_revision) {
    revision(source,expected_revision);
    const auto found=source.entities().find(catalog_id);
    require(found!=source.entities().end(),"assembly catalog removal target is missing");
    Entity entity=found->second;
    entity.properties["model"]=retain_assembly_catalog_dialect(found->second.properties.at("model"),
        catalog(entity).without_type(type_id).to_json());
    return upsert(source,std::move(entity),expected_revision,"Remove assembly type");
}
ApplyEntityChanges embedded_assembly_group_transform_command(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& transform, Revision expected_revision) {
    revision(source, expected_revision);
    require(!targets.empty() && targets.size() <= maximum_architectural_group_targets,
        "an embedded assembly group requires between 1 and 1000 roots");
    const auto finite_point = [](const auto& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    using Identity = std::pair<std::string, std::string>;
    std::set<Identity> selected, copies;
    std::map<std::string, AssemblyModel, std::less<>> models;
    for (const auto& target : targets) {
        (void)embedded_group_transform(target, transform);
        identifier(target.catalog_id);
        identifier(target.instance_id);
        require(selected.emplace(target.catalog_id, target.instance_id).second,
            "duplicate embedded assembly group root");
        const auto found = source.entities().find(target.catalog_id);
        require(found != source.entities().end() && found->second.id == target.catalog_id,
            "embedded assembly group catalog is missing or has inconsistent identity");
        if (!models.contains(target.catalog_id))
            models.emplace(target.catalog_id, catalog(found->second));
        const auto& model = models.at(target.catalog_id);
        require(std::any_of(model.instances().begin(), model.instances().end(),
            [&](const auto& instance) { return instance.id == target.instance_id; }),
            "embedded assembly group instance is missing from its catalog");
        if (target.copy_instance_id) {
            identifier(*target.copy_instance_id);
            require(copies.emplace(target.catalog_id, *target.copy_instance_id).second &&
                std::none_of(model.instances().begin(), model.instances().end(),
                    [&](const auto& instance) { return instance.id == *target.copy_instance_id; }),
                "embedded assembly copy identity already exists");
        }
    }
    // Includes all embedded and independent roots, not just selected profiles.
    // Identity gestures must pass the same source budgets and descriptors.
    validate_document_assembly_instances(source.entities());
    struct Root {
        std::size_t raw_index;
        AssemblyTransform pose;
    };
    std::vector<Root> roots;
    roots.reserve(targets.size());
    AssemblyExpansionBudget selected_budget;
    for (const auto& target : targets) {
        const auto& model = models.at(target.catalog_id);
        const auto instance = std::find_if(model.instances().begin(), model.instances().end(),
            [&](const auto& value) { return value.id == target.instance_id; });
        const auto expansion = model.expand(*instance, selected_budget);
        require(!expansion.profiles.empty() && !expansion.nodes.empty(),
            "embedded assembly group requires genuine geometric profile expansion");
        const auto& raw_instances = source.entities().at(target.catalog_id).properties.at("model").at("instances");
        const auto raw = std::find_if(raw_instances.begin(), raw_instances.end(),
            [&](const auto& value) { return value.at("id") == target.instance_id; });
        require(raw != raw_instances.end(), "embedded assembly source record is missing");
        roots.push_back({static_cast<std::size_t>(std::distance(raw_instances.begin(), raw)),
            expansion.nodes.front().transform});
    }

    const auto whole_turn = 2.0 * std::numbers::pi;
    std::map<std::string, Entity, std::less<>> changed;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        const auto& operation = embedded_group_transform(target, transform);
        const bool reflected = operation.flip_horizontal != operation.flip_vertical;
        // Two flips are a proper half-turn. Normalize before addition so even a
        // finite, very large angle cannot overflow, and exact whole turns vanish.
        const auto angle = std::remainder(std::remainder(operation.rotation_z_radians, whole_turn) +
            (operation.flip_horizontal && operation.flip_vertical ? std::numbers::pi : 0.0), whole_turn);
        const bool identity = angle == 0.0 && !reflected && operation.scale == 1.0 &&
            operation.offset.x == 0.0 && operation.offset.y == 0.0 && operation.offset.z == 0.0;
        const auto cosine = std::abs(angle) == std::numbers::pi ? -1.0 : std::cos(angle);
        const auto sine = std::abs(angle) == std::numbers::pi ? 0.0 : std::sin(angle);
        const auto hx = reflected && operation.flip_horizontal ? -1.0 : 1.0;
        const auto hy = reflected && operation.flip_vertical ? -1.0 : 1.0;
        const auto moved_pose = [&](AssemblyTransform root) {
            if (angle == 0.0 && !reflected && operation.scale == 1.0) {
                root.translation_m.x += operation.offset.x;
                root.translation_m.y += operation.offset.y;
                root.translation_m.z += operation.offset.z;
            } else {
                // Subtract this root's captured pivot before scale/rotation;
                // no enormous affine translation is subtracted from source pose.
                const auto dx = root.translation_m.x - operation.pivot.x;
                const auto dy = root.translation_m.y - operation.pivot.y;
                const auto dz = root.translation_m.z - operation.pivot.z;
                root.translation_m = {
                    operation.pivot.x + hx * operation.scale * (cosine * dx - sine * dy) + operation.offset.x,
                    operation.pivot.y + hy * operation.scale * (sine * dx + cosine * dy) + operation.offset.y,
                    operation.pivot.z + operation.scale * dz + operation.offset.z};
            }
            if (angle != 0.0 || reflected) {
                auto heading = std::remainder(std::remainder(root.rotation_radians, whole_turn) + angle, whole_turn);
                if (reflected) heading = operation.flip_horizontal ? std::numbers::pi - heading : -heading;
                root.rotation_radians = std::remainder(heading, whole_turn);
            }
            root.scale *= operation.scale;
            root.mirrored_y = root.mirrored_y != reflected;
            require(finite_point(root.translation_m) && std::isfinite(root.rotation_radians) &&
                std::isfinite(root.scale) && root.scale > 0.0,
                "embedded assembly group pose exceeds the supported transform range");
            return root;
        };

        if (identity && !target.copy_instance_id) continue;
        const auto& before = source.entities().at(target.catalog_id);
        auto [entry, inserted] = changed.try_emplace(target.catalog_id, before);
        (void)inserted;
        auto& instances = entry->second.properties.at("model").at("instances");
        // Always clone the captured original, even if another selected record
        // in this same catalog has already been transformed in the candidate.
        auto raw = before.properties.at("model").at("instances").at(roots[i].raw_index);
        if (target.copy_instance_id) raw["id"] = *target.copy_instance_id;
        if (!identity) {
            const auto pose = moved_pose(roots[i].pose);
            raw.erase("placement");
            // Patch only owned pose fields, retaining an existing explicit
            // false parity field and all raw override/nested-override records.
            auto& encoded = raw["root_transform"];
            if (!encoded.is_object()) {
                encoded = encode_assembly_transform(pose);
            } else {
                const auto& previous = roots[i].pose;
                auto& translation = encoded.at("translation_m");
                if (pose.translation_m.x != previous.translation_m.x) translation[0] = pose.translation_m.x;
                if (pose.translation_m.y != previous.translation_m.y) translation[1] = pose.translation_m.y;
                if (pose.translation_m.z != previous.translation_m.z) translation[2] = pose.translation_m.z;
                if (pose.rotation_radians != previous.rotation_radians) encoded["rotation_radians"] = pose.rotation_radians;
                if (pose.scale != previous.scale) encoded["scale"] = pose.scale;
                if (pose.mirrored_y != previous.mirrored_y) encoded["mirrored_y"] = pose.mirrored_y;
                if (pose.vertical_scale != previous.vertical_scale) encoded["vertical_scale"] = pose.vertical_scale;
            }
            // A retained v7 row may omit both envelope slots. Materializing its
            // selected root must supply the pair without changing other rows.
            if (!raw.contains("nested_overrides")) raw["nested_overrides"] = nlohmann::json::array();
        }
        if (target.copy_instance_id) instances.push_back(std::move(raw));
        else instances.at(roots[i].raw_index) = std::move(raw);
    }
    ApplyEntityChanges command{expected_revision, {}, {},
        copies.empty() ? "Transform embedded assembly group" : "Copy/transform embedded assembly group"};
    for (auto& [id, entity] : changed) {
        (void)id;
        command.entity_changes.push_back(EntityChange::upsert(std::move(entity)));
    }
    if (command.entity_changes.empty()) (void)Document::fork(source);
    else (void)Document::preview_command(source, command);
    return command;
}
ApplyEntityChanges embedded_assembly_group_copy_command(const DocumentSnapshot& source,
    std::span<const EmbeddedAssemblyGroupTarget> targets,
    const ArchitecturalGroupTransform& transform, Revision expected_revision) {
    revision(source, expected_revision);
    require(!targets.empty() && targets.size() <= maximum_architectural_group_targets,
        "an embedded assembly copy requires between 1 and 1000 roots");
    std::set<std::string, std::less<>> fresh_ids;
    std::vector<EmbeddedAssemblyGroupTarget> moving;
    moving.reserve(targets.size());
    for (const auto& target : targets) {
        require(target.copy_instance_id.has_value(), "an embedded assembly copy requires a fresh document identity");
        identifier(*target.copy_instance_id);
        require(!source.entities().contains(*target.copy_instance_id) && fresh_ids.insert(*target.copy_instance_id).second,
            "embedded assembly copy document identity already exists");
        moving.push_back({target.catalog_id, target.instance_id, std::nullopt, target.transform});
    }
    // Derive exact poses through the same qualified per-root command. Its
    // existing-catalog upserts are detached input only, never published here.
    const auto movement = embedded_assembly_group_transform_command(source, moving, transform, expected_revision);
    std::map<std::string, const Entity*, std::less<>> moved_catalogs;
    for (const auto& change : movement.entity_changes) {
        require(change.kind == EntityChangeKind::upsert && change.entity.type == "assembly_model" &&
            moved_catalogs.emplace(change.entity.id, &change.entity).second,
            "embedded assembly copy has conflicting catalog consequences");
    }
    for (const auto& [id, entity] : source.entities()) {
        if (entity.type == kAnnotationEntityType) {
            const auto state = decode_annotation_entity(entity);
            for (const auto& child : state.labels)
                require(!fresh_ids.contains(child.id), "embedded assembly copy identity collides with an annotation");
            for (const auto& child : state.symbols)
                require(!fresh_ids.contains(child.id), "embedded assembly copy identity collides with a component");
        } else if (entity.type == "assembly_model") {
            for (const auto& instance : entity.properties.at("model").at("instances"))
                require(!fresh_ids.contains(id + ":instance:" + instance.at("id").get<std::string>()),
                    "embedded assembly copy identity collides with a catalog instance");
        }
    }
    ApplyEntityChanges command{expected_revision, {}, {}, "Copy embedded assemblies as independent objects"};
    AssemblyExpansionBudget budget;
    std::map<std::string, AssemblyModel, std::less<>> proposed_models;
    for (const auto& target : targets) {
        const auto& original = source.entities().at(target.catalog_id);
        const auto found_catalog = moved_catalogs.find(target.catalog_id);
        const auto& proposed = found_catalog == moved_catalogs.end() ? original : *found_catalog->second;
        if (!proposed_models.contains(target.catalog_id))
            proposed_models.emplace(target.catalog_id, catalog(proposed));
        const auto& model = proposed_models.at(target.catalog_id);
        const auto instance = std::find_if(model.instances().begin(), model.instances().end(),
            [&](const auto& value) { return value.id == target.instance_id; });
        require(instance != model.instances().end(), "embedded assembly copy lost its qualified source instance");
        const auto expansion = model.expand(*instance, budget);
        require(!expansion.profiles.empty() && !expansion.nodes.empty(),
            "embedded assembly copy requires genuine geometric profile expansion");
        const auto& rows = proposed.properties.at("model").at("instances");
        const auto row = std::find_if(rows.begin(), rows.end(),
            [&](const auto& value) { return value.at("id") == target.instance_id; });
        require(row != rows.end(), "embedded assembly copy lost its raw source record");
        auto raw = *row;
        const auto catalog_schema = proposed.properties.at("model").value("schema", nlohmann::json{});
        raw["schema"] = catalog_schema == "sketch.assemblies.v6" || catalog_schema == "sketch.assemblies.v7"
            ? "sketch.assembly-instance.v2" : "sketch.assembly-instance.v1";
        raw["id"] = *target.copy_instance_id;
        if (raw.contains("placement") || !raw.contains("root_transform") || !raw.at("root_transform").is_object())
            raw["root_transform"] = encode_assembly_transform(expansion.nodes.front().transform);
        if (!raw.contains("nested_overrides")) raw["nested_overrides"] = nlohmann::json::array();
        raw.erase("placement");
        Entity copy = original;
        copy.id = *target.copy_instance_id;
        copy.type = "assembly_instance";
        copy.properties.erase("model");
        copy.properties["version"] = 1;
        copy.properties["form"] = "independent_assembly_instance";
        copy.properties["assembly_catalog_id"] = target.catalog_id;
        copy.properties["instance"] = std::move(raw);
        // Embedded profile poses are already world-authored. Materializing a
        // root must not apply its catalog's optional Site/building frame again.
        copy.properties["presentation_frame"] = {{"version", 1}, {"mode", "world"}};
        (void)decode_document_assembly_instance(copy);
        command.entity_changes.push_back(EntityChange::upsert(std::move(copy)));
    }
    (void)Document::preview_command(source, command);
    return command;
}
} // namespace sketch
