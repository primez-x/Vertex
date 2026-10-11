#include "sketch/assembly_catalog_phase_edit.hpp"

#include "sketch/corner_window.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/stair_semantics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = AssemblyDocumentEntities;
using Target = AssemblyInstancePropertyEditTarget;
using Models = std::map<std::string, AssemblyModel, std::less<>>;
constexpr std::size_t definition_limit = 4096;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    require(value.is_object() && value.size() == names.size(), "catalog edit requires closed fields");
    for (const auto* name : names) require(value.contains(name), "catalog edit field is missing");
}
void document_id(std::string_view value) {
    require(!value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    }), "catalog edit needs an actual document identity");
}
std::string local_id(const Json& value) {
    require(value.is_string(), "catalog edit local identity must be a string");
    const auto& id = value.get_ref<const std::string&>();
    require(!id.empty() && id.size() <= 4096 && id.find('\0') == std::string::npos &&
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }),
        "catalog edit local identity exceeds bound");
    return id;
}
void digest(const Json& value) {
    require(value.is_string(), "catalog edit source digest must be text");
    const auto& id = value.get_ref<const std::string&>();
    require(id.size() == 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "catalog edit source digest is invalid");
}
// Match transport's complete-map ceilings, including opaque envelopes. Charge
// a conservative escaped encoding bound before any copying, digest or decode.
struct RawBudget {
    std::size_t bytes{}, nodes{};
    void reserve_nodes(std::size_t amount) {
        require(amount <= 4 * 1024 * 1024 - nodes, "catalog edit raw node budget exceeded");
        require(amount <= (64 * 1024 * 1024 - bytes) / 32, "catalog edit raw byte budget exceeded");
        nodes += amount; bytes += 32 * amount;
    }
    void text(std::string_view value) {
        require(value.size() <= (64 * 1024 * 1024 - bytes) / 6, "catalog edit raw byte budget exceeded");
        bytes += 6 * value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        require(depth <= 64, "catalog edit raw depth budget exceeded");
        reserve_nodes(1);
        require(!value.is_binary() && !value.is_discarded() &&
            (!value.is_number_float() || std::isfinite(value.get<double>())), "catalog edit requires finite ordinary JSON");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
    }
    void map(const Entities& values) {
        require(values.size() <= 100000, "catalog edit entity count exceeds bound");
        for (const auto& [id, entity] : values) {
            document_id(id);
            require(entity.id == id && entity.properties.is_object() && entity.extensions.is_object(),
                "catalog edit requires actual identified entity envelopes");
            text(id); text(entity.type); read(entity.properties); read(entity.extensions);
        }
    }
};
// Public typed input is bounded before the encoder allocates its JSON mirror.
// Semantic geometry/graph admission still belongs to the real candidate ledger.
void preflight_type(const AssemblyType& value, RawBudget& budget) {
    require(value.profiles.size() <= definition_limit && value.parts.size() <= definition_limit,
        "catalog edit typed collection exceeds bound");
    const auto strings = [&](const auto& values) {
        for (const auto& [key, text] : values) { budget.reserve_nodes(2); budget.text(key); budget.text(text); }
    };
    const auto quantities = [&](const auto& values) {
        for (const auto& [key, quantity] : values) { (void)quantity; budget.reserve_nodes(4); budget.text(key); }
    };
    budget.reserve_nodes(8); budget.text(value.id); budget.text(value.name);
    strings(value.properties); strings(value.materials); quantities(value.quantities);
    for (const auto& profile : value.profiles) {
        require(profile.outer.size() <= 1024 && profile.holes.size() <= 1024, "catalog edit typed profile exceeds bound");
        budget.reserve_nodes(8); budget.text(profile.id);
        if (profile.material_slot) budget.text(*profile.material_slot);
        std::size_t segments = profile.outer.size();
        for (const auto& hole : profile.holes) {
            require(hole.size() <= 1024 - segments, "catalog edit typed profile segments exceed bound");
            segments += hole.size(); budget.reserve_nodes(1);
        }
        budget.reserve_nodes(10 * segments);
    }
    for (const auto& part : value.parts) {
        budget.reserve_nodes(16); budget.text(part.id); budget.text(part.type_id);
        strings(part.property_overrides); strings(part.material_overrides); quantities(part.quantity_overrides);
    }
}
bool exact(const Json& a, const Json& b) {
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_object()) {
        for (const auto& [key, value] : a.items()) {
            const auto other = b.find(key);
            if (other == b.end() || !exact(value, *other)) return false;
        }
        return true;
    }
    if (a.is_array()) {
        for (std::size_t i = 0; i < a.size(); ++i) if (!exact(a.at(i), b.at(i))) return false;
        return true;
    }
    return a == b;
}
const Json& raw_model(const Entity& entity) {
    require(entity.type == "assembly_model" && entity.properties.contains("model") &&
        entity.properties.at("model").is_object(), "catalog edit actual catalog model is missing");
    return entity.properties.at("model");
}
const Json& row(const Json& model, const char* collection, const std::string& id) {
    require(model.contains(collection) && model.at(collection).is_array(), "catalog edit actual collection is missing");
    const Json* result = nullptr;
    for (const auto& value : model.at(collection)) {
        require(value.is_object() && value.contains("id") && value.at("id").is_string(),
            "catalog edit actual definition identity is malformed");
        if (value.at("id") == id) {
            require(!result, "catalog edit actual definition identity is duplicated"); result = &value;
        }
    }
    require(result, "catalog edit cannot add or remove a definition");
    return *result;
}
template<class T> const T& typed_row(const std::vector<T>& rows, const std::string& id) {
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const T& value) { return value.id == id; });
    require(found != rows.end(), "catalog edit reached definition is missing"); return *found;
}
const char* unit_name(AssemblyQuantityUnit unit) {
    switch (unit) {
    case AssemblyQuantityUnit::count: return "count";
    case AssemblyQuantityUnit::metre: return "m";
    case AssemblyQuantityUnit::square_metre: return "m2";
    case AssemblyQuantityUnit::cubic_metre: return "m3";
    case AssemblyQuantityUnit::kilogram: return "kg";
    }
    throw std::invalid_argument("catalog edit quantity unit is invalid");
}
Json quantities(const std::map<std::string, AssemblyQuantityProperty>& values) {
    require(values.size() <= definition_limit, "catalog edit quantity count exceeds bound");
    Json result = Json::object();
    for (const auto& [key, value] : values) {
        require(std::isfinite(value.value) && value.value >= 0 &&
            (value.unit != AssemblyQuantityUnit::count || std::floor(value.value) == value.value),
            "catalog edit quantity is invalid");
        result[key] = {{"value", value.value}, {"unit", unit_name(value.unit)}};
    }
    return result;
}
Json boundary(const Boundary& value) {
    require(value.size() <= 1024, "catalog edit profile segment count exceeds bound");
    Json result = Json::array();
    for (const auto& edge : value) result.push_back({{"start", {edge.start.x, edge.start.y}},
        {"end", {edge.end.x, edge.end.y}}, {"sweep_radians", edge.sweep_radians}});
    return result;
}
Json encode_type(const AssemblyType& value) {
    require(value.profiles.size() <= definition_limit && value.parts.size() <= definition_limit,
        "catalog edit type collection exceeds bound");
    Json profiles = Json::array(), parts = Json::array();
    for (const auto& profile : value.profiles) {
        require(profile.holes.size() <= 1024, "catalog edit profile hole count exceeds bound");
        Json holes = Json::array();
        std::size_t segments = profile.outer.size();
        for (const auto& hole : profile.holes) {
            require(segments <= 1024 && hole.size() <= 1024 - segments, "catalog edit profile segments exceed bound");
            segments += hole.size(); holes.push_back(boundary(hole));
        }
        profiles.push_back({{"id", profile.id}, {"outer", boundary(profile.outer)}, {"holes", std::move(holes)},
            {"elevation_m", profile.elevation_m}, {"height_m", profile.height_m},
            {"material_slot", profile.material_slot ? Json(*profile.material_slot) : Json(nullptr)}});
    }
    for (const auto& part : value.parts) parts.push_back({{"id", part.id}, {"type_id", part.type_id},
        {"transform", encode_assembly_transform(part.transform)}, {"property_overrides", part.property_overrides},
        {"material_overrides", part.material_overrides}, {"quantity_overrides", quantities(part.quantity_overrides)}});
    return {{"id", value.id}, {"name", value.name}, {"properties", value.properties}, {"materials", value.materials},
        {"quantities", quantities(value.quantities)}, {"profiles", std::move(profiles)}, {"parts", std::move(parts)}};
}
Json encode_material(const AssemblyMaterial& value) {
    Json result{{"id", value.id}, {"name", value.name}};
    if (value.color_srgb) result["color_srgb"] = *value.color_srgb;
    return result;
}
void closed_type(const Json& value) {
    fields(value, {"id", "name", "properties", "materials", "quantities", "profiles", "parts"});
    (void)local_id(value.at("id"));
    require(value.at("profiles").is_array() && value.at("parts").is_array() &&
        value.at("profiles").size() <= definition_limit && value.at("parts").size() <= definition_limit,
        "catalog edit replacement collections exceed bound");
    for (const auto& profile : value.at("profiles")) {
        fields(profile, {"id", "outer", "holes", "elevation_m", "height_m", "material_slot"});
        require(profile.at("outer").is_array() && profile.at("holes").is_array(), "catalog edit replacement profile is invalid");
        const auto edges = [](const Json& edges) {
            require(edges.is_array() && edges.size() <= 1024, "catalog edit replacement edges exceed bound");
            for (const auto& edge : edges) fields(edge, {"start", "end", "sweep_radians"});
        };
        edges(profile.at("outer"));
        for (const auto& hole : profile.at("holes")) edges(hole);
    }
    for (const auto& part : value.at("parts")) {
        fields(part, {"id", "type_id", "transform", "property_overrides", "material_overrides", "quantity_overrides"});
        const auto& pose=part.at("transform");
        require(pose.is_object() && pose.contains("translation_m") && pose.contains("rotation_radians") &&
            pose.contains("scale") && pose.size()==3u+static_cast<unsigned>(pose.contains("mirrored_y"))+
                static_cast<unsigned>(pose.contains("vertical_scale")),"catalog edit transform fields are invalid");
        require(pose.at("translation_m").is_array() && pose.at("translation_m").size()==3 &&
            std::all_of(pose.at("translation_m").begin(),pose.at("translation_m").end(),[](const auto& number) {
                return number.is_number();
            }) && pose.at("rotation_radians").is_number() && pose.at("scale").is_number() &&
            (!pose.contains("mirrored_y") || pose.at("mirrored_y").is_boolean()) &&
            (!pose.contains("vertical_scale") || pose.at("vertical_scale").is_number()),
            "catalog edit transform framing is invalid");
        // Typed pose/positive-scale validation belongs to the candidate model
        // after both complete catalog work ledgers have been admitted.
    }
}
void closed_material(const Json& value) {
    if (value.contains("color_srgb")) fields(value, {"id", "name", "color_srgb"});
    else fields(value, {"id", "name"});
    (void)local_id(value.at("id"));
}
void admit_catalogs(const Entities& values, AssemblyCatalogTransferBudget& budget) {
    for (const auto& [id, entity] : values) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, budget);
    }
}
Models models(const Entities& values) {
    Models result;
    for (const auto& [id, entity] : values)
        if (entity.type == "assembly_model") result.emplace(id, AssemblyModel::from_json(raw_model(entity)));
    return result;
}
std::string catalog_digest(const Entity& value) { return entity_map_digest(Entities{{value.id, value}}); }

// Patch only requested existing definition rows. Retained unrelated rows never
// pass through a semantic serializer. A union v7 promotion is used only when
// the old dialect cannot express a changed row's new envelope.
Entities derive_raw(const Entities& actual, const Json& request) {
    Entities result = actual;
    for (const auto& edit : request.at("edits")) {
        const auto id = edit.at("catalog_id").get<std::string>();
        const auto found = actual.find(id);
        require(found != actual.end(), "catalog edit actual catalog is missing");
        const auto& original = raw_model(found->second);
        require(catalog_digest(found->second) == edit.at("source_catalog_digest"), "catalog edit original catalog binding changed");
        auto& candidate = result.at(id).properties.at("model");
        const auto schema = original.at("schema").get<std::string>();
        bool promote = false;
        for (const auto* collection : {"types", "materials"}) for (const auto& change : edit.at(collection)) {
            const auto key = local_id(change.at("replacement").at("id"));
            require(exact(row(original, collection, key), change.at("original")), "catalog edit original definition binding changed");
            auto replacement = change.at("replacement");
            if (std::string_view(collection) == "types") {
                const bool old_nested = schema == "sketch.assemblies.v4" || schema == "sketch.assemblies.v5" ||
                    schema == "sketch.assemblies.v6";
                if (!old_nested && !row(original, collection, key).contains("profiles") &&
                    replacement.at("profiles").empty() && replacement.at("parts").empty()) {
                    replacement.erase("profiles"); replacement.erase("parts");
                } else if (!old_nested && schema != "sketch.assemblies.v7" && schema != "sketch.assemblies.v8") promote = true;
                if (schema != "sketch.assemblies.v6" && schema != "sketch.assemblies.v7" && schema != "sketch.assemblies.v8" && replacement.contains("parts"))
                    for (const auto& part : replacement.at("parts"))
                        if (part.at("transform").contains("vertical_scale")) promote = true;
            } else if (schema == "sketch.assemblies.v1" && replacement.contains("color_srgb")) promote = true;
            for (auto& saved : candidate.at(collection)) if (saved.at("id") == key) { saved = std::move(replacement); break; }
        }
        if (promote) candidate.at("schema") = schema == "sketch.assemblies.v8" ? "sketch.assemblies.v8" : "sketch.assemblies.v7";
    }
    return result;
}
void retain_equal_rows(const Entities& actual, Entities& candidate, const Models& before, const Models& after) {
    for (auto& [id, entity] : candidate) if (entity.type == "assembly_model") {
        auto& value = entity.properties.at("model");
        const auto& original = raw_model(actual.at(id));
        bool changed = false;
        for (auto& type : value.at("types")) {
            const auto key = type.at("id").get<std::string>();
            if (typed_row(before.at(id).types(), key) == typed_row(after.at(id).types(), key)) type = row(original, "types", key);
            else changed = true;
        }
        for (auto& material : value.at("materials")) {
            const auto key = material.at("id").get<std::string>();
            if (typed_row(before.at(id).materials(), key) == typed_row(after.at(id).materials(), key)) material = row(original, "materials", key);
            else changed = true;
        }
        if (!changed) value = original;
    }
}
struct PhaseInventory {
    std::map<std::string, ModelPhases, std::less<>> registries;
    std::map<std::string, std::string, std::less<>> owners;
    std::map<std::string, ModelPhase, std::less<>> states;
    std::set<std::string, std::less<>> baseline_owners;
    AssemblyPhaseMembershipScope rows;
    std::map<std::string, AssemblyPhaseMembership, std::less<>> companions;
    explicit PhaseInventory(const Entities& actual) {
        for (const auto& [id, entity] : actual) if (entity.type == "model_phases") {
            require(entity.properties.contains("model"), "catalog edit actual registry model is missing");
            auto registry = ModelPhases::from_json(entity.properties.at("model"));
            const auto active = registry.active_state();
            for (const auto& owner : registry.entity_ids()) {
                const auto found = actual.find(owner);
                require(found != actual.end() && is_model_phase_entity_type(found->second.type),
                    "catalog edit registry member is not an actual model owner");
                require(owners.emplace(owner, id).second, "catalog edit actual owner belongs to overlapping registries");
                if (const auto state = active.find(owner); state != active.end()) states.emplace(owner, state->second);
            }
            baseline_owners.insert(registry.baseline_ids().begin(), registry.baseline_ids().end());
            registries.emplace(id, std::move(registry));
        }
        rows = resolve_assembly_phase_memberships(actual);
        for (const auto& [id, entity] : actual) if (entity.type == assembly_phase_membership_entity_type)
            companions.emplace(id, AssemblyPhaseMembership::from_json(entity.properties.at("model")));
    }
    std::optional<AssemblyCatalogPhaseEditBinding> binding(const std::string& owner) const {
        const auto found = owners.find(owner);
        if (found == owners.end()) return std::nullopt;
        const auto& registry = registries.at(found->second);
        const auto state = states.find(owner);
        return AssemblyCatalogPhaseEditBinding{owner, found->second, registry.active_alternative(),
            state == states.end() ? std::nullopt : std::optional{state->second},
            baseline_owners.contains(owner),
            std::nullopt};
    }
};
std::vector<std::string> physical_host_closure(const Entities& actual, const std::string& host_id) {
    std::set<std::string, std::less<>> result;
    const auto reach = [&](const auto& self, const std::string& id) -> void {
        if (result.contains(id)) return;
        const auto found = actual.find(id);
        require(found != actual.end() && (is_complete_assembly_catalog_host_type(found->second.type) ||
            found->second.type == "corner_window"), "catalog edit placement host closure is unresolved");
        require(result.size() < 10000, "catalog edit placement host closure exceeds bound");
        result.insert(id);
        const auto& entity = found->second;
        if (entity.type == "opening") {
            require(entity.properties.contains("wall_id") && entity.properties.at("wall_id").is_string(),
                "catalog edit opening host has no actual wall");
            const auto wall = entity.properties.at("wall_id").get<std::string>();
            require(actual.contains(wall) && actual.at(wall).type == "wall", "catalog edit opening host wall is missing");
            self(self, wall);
            if (entity.properties.contains("corner_window_id")) {
                require(entity.properties.at("corner_window_id").is_string(), "catalog edit corner backlink is invalid");
                const auto owner = entity.properties.at("corner_window_id").get<std::string>();
                require(actual.contains(owner) && actual.at(owner).type == "corner_window", "catalog edit corner owner is missing");
                const auto corner = parse_corner_window(actual.at(owner));
                const auto leg = std::find(corner.opening_ids.begin(), corner.opening_ids.end(), id);
                require(leg != corner.opening_ids.end(), "catalog edit corner cut is not owned by its actual corner");
                self(self, owner);
            }
        } else if (entity.type == "corner_window") {
            const auto corner = parse_corner_window(entity);
            for (std::size_t leg = 0; leg < corner.opening_ids.size(); ++leg) {
                const auto cut = actual.find(corner.opening_ids[leg]);
                require(cut != actual.end() && cut->second.type == "opening" &&
                    cut->second.properties.contains("corner_window_id") && cut->second.properties.at("corner_window_id") == id &&
                    cut->second.properties.contains("corner_leg") && cut->second.properties.at("corner_leg").is_number_integer() &&
                    cut->second.properties.at("corner_leg") == leg && cut->second.properties.contains("wall_id") &&
                    cut->second.properties.at("wall_id") == corner.wall_ids[leg], "catalog edit corner host cut closure is inconsistent");
                self(self, cut->first); self(self, corner.wall_ids[leg]);
            }
        }
    };
    reach(reach, host_id);
    return {result.begin(), result.end()};
}
std::vector<std::string> host_closure(const Entities& actual, const AssemblyInstance& instance) {
    return instance.placement ? physical_host_closure(actual, instance.placement->host_entity_id) : std::vector<std::string>{};
}
bool equal_expansion(const AssemblyExpansion& a, const AssemblyExpansion& b) {
    if (!(a.source_instance == b.source_instance) || a.nodes.size() != b.nodes.size() || a.profiles.size() != b.profiles.size() ||
        a.declared_quantities != b.declared_quantities || a.material_volumes_m3 != b.material_volumes_m3 || a.volume_m3 != b.volume_m3) return false;
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        const auto& x = a.nodes[i]; const auto& y = b.nodes[i];
        if (x.part_path != y.part_path || x.type_id != y.type_id || x.transform != y.transform || x.properties != y.properties ||
            x.materials != y.materials || x.quantities != y.quantities || x.source_part != y.source_part || x.path_override != y.path_override) return false;
    }
    for (std::size_t i = 0; i < a.profiles.size(); ++i) {
        const auto& x = a.profiles[i]; const auto& y = b.profiles[i];
        if (x.part_path != y.part_path || x.type_id != y.type_id || !(x.profile == y.profile) || x.transform != y.transform ||
            x.material_id != y.material_id || x.volume_m3 != y.volume_m3) return false;
    }
    return true;
}
void closure(const AssemblyExpansion& expansion, std::vector<std::string>& types, std::vector<std::string>& materials) {
    std::set<std::string, std::less<>> type_ids, material_ids;
    for (const auto& node : expansion.nodes) {
        type_ids.insert(node.type_id);
        for (const auto& [slot, id] : node.materials) { (void)slot; material_ids.insert(id); }
    }
    types.assign(type_ids.begin(), type_ids.end()); materials.assign(material_ids.begin(), material_ids.end());
}
bool reached_definitions_changed(const Models& before, const Models& after, const AssemblyCatalogPhaseEditRootImpact& impact) {
    const auto& a = before.at(impact.root.catalog_id); const auto& b = after.at(impact.root.catalog_id);
    std::set<std::string, std::less<>> types(impact.before_type_ids.begin(), impact.before_type_ids.end());
    types.insert(impact.after_type_ids.begin(), impact.after_type_ids.end());
    for (const auto& id : types) if (!(typed_row(a.types(), id) == typed_row(b.types(), id))) return true;
    std::set<std::string, std::less<>> materials(impact.before_material_ids.begin(), impact.before_material_ids.end());
    materials.insert(impact.after_material_ids.begin(), impact.after_material_ids.end());
    for (const auto& id : materials) if (!(typed_row(a.materials(), id) == typed_row(b.materials(), id))) return true;
    return false;
}
void classify(const PhaseInventory& phases, AssemblyCatalogPhaseEditRootImpact& impact) {
    std::vector<AssemblyCatalogPhaseEditBinding> decisive;
    bool declared_proposed = false;
    const auto add = [&](const std::string& id, bool decides) {
        if (auto binding = phases.binding(id)) {
            impact.phase_bindings.push_back(*binding);
            if (decides) decisive.push_back(*binding);
        }
    };
    const AssemblyPhaseRoot pair{impact.root.catalog_id, impact.root.instance_id};
    const auto declared = phases.rows.owners.find(pair);
    if (!impact.root.document_entity_id && declared != phases.rows.owners.end()) {
        const auto& owner = declared->second;
        const auto& registry = phases.registries.at(owner.registry_id);
        const auto& membership = phases.companions.at(owner.membership_entity_id);
        const auto state = phases.rows.states.find(pair);
        AssemblyCatalogPhaseEditBinding binding{impact.root.catalog_id, owner.registry_id, registry.active_alternative(),
            state == phases.rows.states.end() ? std::nullopt : std::optional{state->second},
            std::binary_search(membership.baseline_roots().begin(), membership.baseline_roots().end(), pair),
            owner.membership_entity_id};
        impact.phase_bindings.push_back(binding); decisive.push_back(binding);
        declared_proposed = !binding.baseline && binding.state == ModelPhase::proposed;
        // The qualified row supplies membership, not its shared carrier. Retain
        // that carrier's actual registry binding for later private-catalog work.
        add(impact.root.catalog_id, false);
    } else {
        add(impact.root.document_entity_id ? *impact.root.document_entity_id : impact.root.catalog_id, true);
        if (impact.root.document_entity_id) add(impact.root.catalog_id, false);
    }
    for (const auto& host : impact.host_closure) add(host, true);
    const bool inactive = std::any_of(decisive.begin(), decisive.end(), [](const auto& binding) {
        return !binding.state || *binding.state == ModelPhase::demolished;
    });
    // An actual declared proposal keeps its row authority on a protected host.
    // Every host still decides inactivity; undeclared/independent roots retain
    // their legacy catalog/owner and host baseline protection.
    const bool protected_baseline = !declared_proposed && std::any_of(decisive.begin(), decisive.end(), [](const auto& binding) {
        return binding.baseline && binding.active_alternative.has_value();
    });
    impact.cohort = inactive ? AssemblyCatalogPhaseEditCohort::inactive_retained : protected_baseline ?
        AssemblyCatalogPhaseEditCohort::baseline_protected : AssemblyCatalogPhaseEditCohort::active_ordinary;
}

std::vector<AssemblyCatalogPhaseEditMaterialConsumerImpact> material_consumers(
    const Entities& actual, const PhaseInventory& phases, const Models& before, const Models* after = nullptr) {
    std::vector<AssemblyCatalogPhaseEditMaterialConsumerImpact> result;
    // The shared collector validates every supported root assignment and wall/
    // slab layer stack, bounding parser reserves and deduplicating each owner's
    // actual catalog/material pairs. Complete raw-map admission precedes it.
    std::map<std::pair<std::string, std::string>, bool> definitions;
    for (const auto& [id, entity] : actual) {
        // Unknown entity envelopes remain opaque. The collector owns the
        // supported architectural assignment roles within the model inventory.
        if (!is_model_phase_entity_type(entity.type)) continue;
        const auto references = architectural_material_source_refs(entity);
        if (references.empty()) continue;
        std::vector<AssemblyCatalogPhaseEditBinding> bindings;
        bool inactive = false, protected_baseline = false;
        const auto owner = phases.binding(id);
        const bool proposed = owner && !owner->baseline && owner->state == ModelPhase::proposed;
        const auto add = [&](const std::string& host, bool baseline_decides) {
            if (auto binding = phases.binding(host)) {
                inactive = inactive || !binding->state || binding->state == ModelPhase::demolished;
                protected_baseline = protected_baseline || (baseline_decides && binding->baseline && binding->active_alternative.has_value());
                bindings.push_back(std::move(*binding));
            }
        };
        add(id, true);
        if (entity.type == "opening" || entity.type == "corner_window") {
            // Reuse the actual finite two-cut/two-wall closure. Backlinks and
            // child roles must be exact; opaque host-like metadata is ignored.
            for (const auto& host : physical_host_closure(actual, id))
                if (host != id) add(host, !proposed);
        } else if (entity.type == "railing") {
            const auto& p = entity.properties;
            const auto version = p.find("version"), form = p.find("form");
            const bool hosted = version != p.end() && version->is_number_integer() && form != p.end() &&
                ((*version == 2 && *form == "stair_flight_railing") || (*version == 3 && *form == "stair_landing_railing"));
            if (hosted) {
                const auto railing = decode_railing_properties(id, p);
                const auto& host = railing.host ? railing.host->stair_id : railing.landing_host->stair_id;
                require(actual.contains(host) && actual.at(host).type == "stair", "catalog edit railing has no actual stair host");
                add(host, !proposed);
            }
        } else if (entity.type == "roof_join") {
            // Join baseline protection belongs to the actual join owner.
            // Its members decide inactivity, not baseline protection: active
            // qualified proposals may intentionally use shared baseline roofs.
            const auto join = parse_roof_join(entity.properties, id);
            for (const auto& roof : join.roof_ids) {
                require(actual.contains(roof) && actual.at(roof).type == "roof", "catalog edit material scope has no actual roof member");
                add(roof, false);
            }
        }
        // Rooms and roof bodies use their own actual registry. Descriptor wall
        // references and inactive joins sharing a roof do not confer ownership.
        const auto cohort = inactive ? AssemblyCatalogPhaseEditCohort::inactive_retained : protected_baseline ?
            AssemblyCatalogPhaseEditCohort::baseline_protected : AssemblyCatalogPhaseEditCohort::active_ordinary;
        for (const auto& assignment : references) {
            const auto key = std::pair{assignment.catalog_id, assignment.material_id};
            auto found = definitions.find(key);
            if (found == definitions.end()) {
                require(before.contains(assignment.catalog_id), "catalog edit physical material catalog is missing");
                const auto& original = typed_row(before.at(assignment.catalog_id).materials(), assignment.material_id);
                bool changed = false;
                if (after) {
                    require(after->contains(assignment.catalog_id), "catalog edit physical material catalog disappeared");
                    changed = !(original == typed_row(after->at(assignment.catalog_id).materials(), assignment.material_id));
                }
                found = definitions.emplace(key, changed).first;
            }
            result.push_back({id, assignment.catalog_id, assignment.material_id, cohort, found->second, bindings});
        }
    }
    return result;
}

struct Inspection { Entities candidate; AssemblyCatalogPhaseEditPlan plan; };
Inspection inspect(const Entities& actual, const Json& request,
    AssemblyCatalogTransferBudget* source_admitted = nullptr) {
    RawBudget raw;
    raw.map(actual); raw.read(request);
    (void)validate_assembly_catalog_phase_edit_request(request);
    // All raw source catalogs are admitted before the first source digest,
    // copy, semantic model, phase or expansion operation.
    AssemblyCatalogTransferBudget local_catalog_work;
    auto& catalog_work = source_admitted ? *source_admitted : local_catalog_work;
    if (!source_admitted) admit_catalogs(actual, catalog_work);
    const auto source_digest = entity_map_digest(actual);
    require(source_digest == request.at("source_entities_digest"), "catalog edit complete source map changed");
    auto candidate = derive_raw(actual, request);
    raw.map(candidate);
    admit_catalogs(candidate, catalog_work);
    // Neither source nor candidate has reached semantic admission until both
    // complete sides of actual catalog/opaque/graph work were reserved.
    const auto before = models(actual), after = models(candidate);
    for (const auto& edit : request.at("edits")) {
        const auto id = edit.at("catalog_id").get<std::string>();
        for (const auto& change : edit.at("types")) {
            const auto key = local_id(change.at("replacement").at("id"));
            require(exact(encode_type(typed_row(after.at(id).types(), key)), change.at("replacement")),
                "catalog edit replacement type is not canonical typed data");
        }
        for (const auto& change : edit.at("materials")) {
            const auto key = local_id(change.at("replacement").at("id"));
            require(exact(encode_material(typed_row(after.at(id).materials(), key)), change.at("replacement")),
                "catalog edit replacement material is not canonical typed data");
        }
    }
    retain_equal_rows(actual, candidate, before, after);
    const PhaseInventory phases(actual);
    // Candidate companions, registries, hosts and instance rows are untouched;
    // resolving actual membership therefore supplies the exact candidate roles.
    std::map<Target, AssemblyInstance> roots;
    for (const auto& [id, model] : before) for (const auto& instance : model.instances())
        roots.emplace(Target{id, instance.id, std::nullopt, {}}, instance);
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_instance") {
        const auto value = decode_document_assembly_instance(entity);
        require(before.contains(value.assembly_catalog_id), "catalog edit independent root references a missing catalog");
        require(roots.emplace(Target{value.assembly_catalog_id, value.instance.id, id, {}}, value.instance).second,
            "catalog edit independent qualified root is duplicated");
    }
    AssemblyExpansionBudget before_budget, after_budget;
    AssemblyCatalogPhaseEditPlan plan;
    plan.source_entities_digest = source_digest;
    plan.ordinary_replay_safe = true;
    for (const auto& [target, instance] : roots) {
        AssemblyCatalogPhaseEditRootImpact impact;
        impact.root = target;
        impact.before = before.at(target.catalog_id).expand(instance, before_budget);
        impact.after = after.at(target.catalog_id).expand(instance, after_budget);
        closure(impact.before, impact.before_type_ids, impact.before_material_ids);
        closure(impact.after, impact.after_type_ids, impact.after_material_ids);
        impact.expansion_changed = !equal_expansion(impact.before, impact.after);
        impact.definitions_changed = reached_definitions_changed(before, after, impact);
        impact.dependency_changed = impact.definitions_changed || impact.before_type_ids != impact.after_type_ids ||
            impact.before_material_ids != impact.after_material_ids;
        impact.host_closure = host_closure(actual, instance);
        classify(phases, impact);
        if ((impact.expansion_changed || impact.definitions_changed) &&
            impact.cohort != AssemblyCatalogPhaseEditCohort::active_ordinary) {
            plan.replacement_required_roots.push_back(plan.roots.size()); plan.ordinary_replay_safe = false;
        }
        plan.roots.push_back(std::move(impact));
    }
    if (request.at("version") == 2) {
        plan.material_consumers = material_consumers(actual, phases, before, &after);
        for (const auto& consumer : plan.material_consumers)
            if (consumer.definitions_changed && consumer.cohort != AssemblyCatalogPhaseEditCohort::active_ordinary)
                plan.ordinary_replay_safe = false;
    }
    return {std::move(candidate), std::move(plan)};
}

} // namespace

Json validate_assembly_catalog_phase_edit_request(const Json& request) {
    try {
        RawBudget budget; budget.read(request);
        require(budget.bytes <= assembly_catalog_transport_byte_limit && budget.nodes <= 262144,
            "catalog edit request exceeds transport bound");
        fields(request, {"version", "source_entities_digest", "edits"});
        require(request.at("version").is_number_integer() && (request.at("version") == 1 || request.at("version") == 2),
            "catalog edit request version is unsupported");
        digest(request.at("source_entities_digest"));
        const auto& edits = request.at("edits");
        require(edits.is_array() && !edits.empty() && edits.size() <= 1000, "catalog edit request catalog count is invalid");
        std::optional<std::string> previous;
        std::size_t count = 0;
        for (const auto& edit : edits) {
            fields(edit, {"catalog_id", "source_catalog_digest", "types", "materials"});
            require(edit.at("catalog_id").is_string(), "catalog edit request catalog identity is invalid");
            const auto id = edit.at("catalog_id").get<std::string>(); document_id(id);
            require(!previous || *previous < id, "catalog edit catalogs must be sorted unique"); previous = id;
            digest(edit.at("source_catalog_digest"));
            require(edit.at("types").is_array() && edit.at("materials").is_array() &&
                (!edit.at("types").empty() || !edit.at("materials").empty()), "catalog edit request has no definition edit");
            for (const auto* collection : {"types", "materials"}) {
                require(edit.at(collection).size() <= definition_limit - count, "catalog edit definition count exceeds bound");
                count += edit.at(collection).size();
                std::optional<std::string> previous_definition;
                for (const auto& change : edit.at(collection)) {
                    fields(change, {"original", "replacement"});
                    require(change.at("original").is_object() && change.at("original").contains("id"),
                        "catalog edit original definition is invalid");
                    if (std::string_view(collection) == "types") closed_type(change.at("replacement"));
                    else closed_material(change.at("replacement"));
                    const auto key = local_id(change.at("replacement").at("id"));
                    require(local_id(change.at("original").at("id")) == key &&
                        (!previous_definition || *previous_definition < key), "catalog edit definition bindings must be sorted unique and retain identity");
                    previous_definition = key;
                }
            }
        }
        // UTF-8 admission follows the raw bounds. This validator establishes
        // closed framing only; source-bound typed validation occurs on inspect.
        (void)request.dump();
        return request;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid catalog edit request: ") + error.what());
    }
}

Json prepare_assembly_catalog_phase_edit_request(const Entities& actual,
    const std::vector<AssemblyCatalogPhaseEditIntent>& intents) {
    try {
        require(!intents.empty() && intents.size() <= 1000, "catalog edit intent count is invalid");
        RawBudget budget; budget.map(actual);
        AssemblyCatalogTransferBudget catalog_work; admit_catalogs(actual, catalog_work);
        Json edits = Json::array();
        std::optional<std::string> previous;
        std::size_t count = 0;
        for (const auto& intent : intents) {
            document_id(intent.catalog_id);
            require(!previous || *previous < intent.catalog_id, "catalog edit intents must be sorted unique");
            previous = intent.catalog_id;
            require(intent.replacement_types.size() <= definition_limit - count, "catalog edit type intent count exceeds bound");
            count += intent.replacement_types.size();
            require(intent.replacement_materials.size() <= definition_limit - count, "catalog edit material intent count exceeds bound");
            count += intent.replacement_materials.size();
            require(!intent.replacement_types.empty() || !intent.replacement_materials.empty(), "catalog edit intent has no replacement");
            const auto found = actual.find(intent.catalog_id);
            require(found != actual.end(), "catalog edit actual intent catalog is missing");
            const auto& model = raw_model(found->second);
            Json types = Json::array(), materials = Json::array();
            std::optional<std::string> previous_type, previous_material;
            for (const auto& value : intent.replacement_types) {
                require(!previous_type || *previous_type < value.id, "catalog edit replacement types must be sorted unique");
                previous_type = value.id;
                preflight_type(value, budget);
                auto replacement = encode_type(value); budget.read(replacement);
                types.push_back({{"original", row(model, "types", value.id)}, {"replacement", std::move(replacement)}});
            }
            for (const auto& value : intent.replacement_materials) {
                require(!previous_material || *previous_material < value.id, "catalog edit replacement materials must be sorted unique");
                previous_material = value.id;
                budget.reserve_nodes(4); budget.text(value.id); budget.text(value.name);
                if (value.color_srgb) budget.text(*value.color_srgb);
                auto replacement = encode_material(value); budget.read(replacement);
                materials.push_back({{"original", row(model, "materials", value.id)}, {"replacement", std::move(replacement)}});
            }
            edits.push_back({{"catalog_id", intent.catalog_id}, {"source_catalog_digest", catalog_digest(found->second)},
                {"types", std::move(types)}, {"materials", std::move(materials)}});
        }
        Json request{{"version", 2}, {"source_entities_digest", entity_map_digest(actual)}, {"edits", std::move(edits)}};
        (void)inspect(actual, request, &catalog_work);
        return request;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid catalog edit source: ") + error.what());
    }
}

AssemblyCatalogPhaseEditPlan inspect_assembly_catalog_phase_edit(const Entities& actual, const Json& request) {
    try { return inspect(actual, request).plan; }
    catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid catalog edit actual source: ") + error.what());
    }
}
Entities replay_assembly_catalog_phase_edit_entities(const Entities& actual, const Json& request) {
    try {
        auto result = inspect(actual, request);
        require(result.plan.ordinary_replay_safe,
            "catalog edit affects protected/inactive consumers; inspect the complete impact plan for phase-isolated private replacements");
        return std::move(result.candidate);
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid catalog edit actual replay: ") + error.what());
    }
}

void validate_current_assembly_catalog_phase_preservation(const Entities& source, const Entities& candidate) {
    require(source.size()<=100000 && candidate.size()<=100000,"current catalog edit entity count exceeds bound");
    if (std::none_of(source.begin(),source.end(),[](const auto& entry) {
        return entry.second.type=="assembly_model" || entry.second.type=="assembly_instance";
    })) return;
    RawBudget raw; raw.map(source); raw.map(candidate);
    // Avoid repeating model work for an event that cannot change a component.
    // Host/phase producers retain their separate actual-source obligations.
    bool changed = false;
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model" && entity.type != "assembly_instance") continue;
        const auto after = candidate.find(id);
        if (after == candidate.end() || !exact(entity.properties, after->second.properties) ||
            !exact(entity.extensions, after->second.extensions) || entity.type != after->second.type ||
            entity.required != after->second.required) { changed = true; break; }
    }
    if (!changed) return;
    AssemblyCatalogTransferBudget work; admit_catalogs(source, work); admit_catalogs(candidate, work);
    const auto before = models(source), after = models(candidate);
    const PhaseInventory phases(source);
    std::map<Target, AssemblyInstance> roots;
    for (const auto& [id, model] : before) for (const auto& instance : model.instances())
        roots.emplace(Target{id,instance.id,std::nullopt,{}},instance);
    for (const auto& [id, entity] : source) if (entity.type == "assembly_instance") {
        const auto value = decode_document_assembly_instance(entity);
        roots.emplace(Target{value.assembly_catalog_id,value.instance.id,id,{}},value.instance);
    }
    AssemblyExpansionBudget before_budget, after_budget;
    for (const auto& [target, instance] : roots) {
        AssemblyCatalogPhaseEditRootImpact impact;
        impact.root=target; impact.host_closure=host_closure(source,instance); classify(phases,impact);
        if (impact.cohort == AssemblyCatalogPhaseEditCohort::active_ordinary) continue;
        require(candidate.contains(target.catalog_id) && after.contains(target.catalog_id),
            "current catalog editing removed a protected/inactive source catalog");
        const auto& original_catalog=source.at(target.catalog_id);
        const auto& candidate_catalog=candidate.at(target.catalog_id);
        auto original_envelope=original_catalog.properties, candidate_envelope=candidate_catalog.properties;
        original_envelope.erase("model"); candidate_envelope.erase("model");
        require(original_catalog.type==candidate_catalog.type && original_catalog.required==candidate_catalog.required &&
            exact(original_envelope,candidate_envelope) && exact(original_catalog.extensions,candidate_catalog.extensions),
            "current catalog editing changed a protected/inactive carrier binding");
        AssemblyInstance retained;
        if (target.document_entity_id) {
            require(candidate.contains(*target.document_entity_id), "current catalog editing removed a protected/inactive independent root");
            const auto& original=source.at(*target.document_entity_id);
            const auto& proposed=candidate.at(*target.document_entity_id);
            require(original.type==proposed.type && original.required==proposed.required &&
                exact(original.properties,proposed.properties) && exact(original.extensions,proposed.extensions),
                "current catalog editing changed a protected/inactive independent root");
            const auto decoded=decode_document_assembly_instance(proposed);
            require(decoded.assembly_catalog_id==target.catalog_id, "protected/inactive root changed catalog");
            retained=decoded.instance;
        } else {
            require(exact(row(raw_model(original_catalog),"instances",target.instance_id),
                row(raw_model(candidate_catalog),"instances",target.instance_id)),
                "current catalog editing changed a protected/inactive embedded row");
            retained=typed_row(after.at(target.catalog_id).instances(),target.instance_id);
        }
        impact.before=before.at(target.catalog_id).expand(instance,before_budget);
        impact.after=after.at(target.catalog_id).expand(retained,after_budget);
        closure(impact.before,impact.before_type_ids,impact.before_material_ids);
        closure(impact.after,impact.after_type_ids,impact.after_material_ids);
        require(equal_expansion(impact.before,impact.after) && !reached_definitions_changed(before,after,impact),
            "current catalog editing changed protected/inactive component definitions; create phase-isolated replacements");
        // Semantic equality is insufficient for retained source authority:
        // optional fields, numeric kinds and the original row dialect survive
        // even when decoding would produce the same type/material value.
        for (const auto& id : impact.before_type_ids)
            require(exact(row(raw_model(original_catalog),"types",id), row(raw_model(candidate_catalog),"types",id)),
                "current catalog editing changed a protected/inactive raw type definition");
        for (const auto& id : impact.before_material_ids)
            require(exact(row(raw_model(original_catalog),"materials",id), row(raw_model(candidate_catalog),"materials",id)),
                "current catalog editing changed a protected/inactive raw material definition");
    }
    // This live safeguard always uses actual physical consumers, even for raw
    // writers or a submitted historical v1 request. Historical replay calls the
    // source-bound request codec separately and retains its original meaning.
    std::set<std::pair<std::string, std::string>> retained_materials;
    for (const auto& consumer : material_consumers(source, phases, before)) {
        if (consumer.cohort == AssemblyCatalogPhaseEditCohort::active_ordinary ||
            !retained_materials.emplace(consumer.catalog_id, consumer.material_id).second) continue;
        require(candidate.contains(consumer.catalog_id) && after.contains(consumer.catalog_id),
            "current catalog editing removed a protected/inactive physical material catalog");
        const auto& original = source.at(consumer.catalog_id);
        const auto& proposed = candidate.at(consumer.catalog_id);
        auto original_envelope = original.properties, proposed_envelope = proposed.properties;
        original_envelope.erase("model"); proposed_envelope.erase("model");
        require(original.type == proposed.type && original.required == proposed.required &&
            exact(original_envelope, proposed_envelope) && exact(original.extensions, proposed.extensions),
            "current catalog editing changed a protected/inactive physical material carrier binding");
        require(exact(row(raw_model(original), "materials", consumer.material_id),
            row(raw_model(proposed), "materials", consumer.material_id)),
            "current catalog editing changed a protected/inactive physical material definition; isolate active component consumers");
    }
}

} // namespace sketch
