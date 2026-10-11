#include "sketch/assembly_phase_membership.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/document.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace sketch {
namespace {
constexpr std::size_t max_roots = 10000;
constexpr std::size_t max_alternatives = 1000;
constexpr std::size_t max_json_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_json_values = 100000;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void document_id(const std::string& id) {
    require(!id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "assembly phase document identity must be a valid document identifier");
}
void local_id(const std::string& id) {
    // Match existing local identity admission, including spaces and Unicode.
    require(!id.empty() && id.size() <= 4096 &&
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }),
        "assembly phase local identity must be nonblank and at most 4096 bytes");
}
void charge(std::size_t& consumed, std::size_t limit, std::size_t amount) {
    require(amount <= limit - consumed, "assembly phase resource budget exceeded");
    consumed += amount;
}
struct JsonBudget {
    std::size_t bytes{};
    std::size_t values{};
    std::size_t byte_limit{max_json_bytes};
    std::size_t value_limit{max_json_values};
};
void string_bytes(JsonBudget& budget, const std::string& text) {
    // Charge UTF-8 source bytes and JSON escaping without allocating a dump.
    // Admit length before scanning; control bytes reserve the longest escape.
    charge(budget.bytes, budget.byte_limit, text.size());
    charge(budget.bytes, budget.byte_limit, 2);
    for (const unsigned char c : text) {
        if (c < 0x20) charge(budget.bytes, budget.byte_limit, 5);
        else if (c == '"' || c == '\\') charge(budget.bytes, budget.byte_limit, 1);
    }
}
void admit_json(const nlohmann::json& value, JsonBudget& budget, std::size_t depth = 0) {
    require(depth <= 64, "assembly phase JSON depth budget exceeded");
    charge(budget.values, budget.value_limit, 1);
    charge(budget.bytes, budget.byte_limit, 32);
    require(!value.is_binary() && !value.is_discarded(), "assembly phase requires ordinary JSON values");
    if (value.is_string()) string_bytes(budget, value.get_ref<const std::string&>());
    else if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            string_bytes(budget, item.key());
            admit_json(item.value(), budget, depth + 1);
        }
    } else if (value.is_array()) {
        for (const auto& child : value) admit_json(child, budget, depth + 1);
    } else if (value.is_number_float()) {
        require(std::isfinite(value.get<double>()), "assembly phase JSON numbers must be finite");
    }
}
void fields(const nlohmann::json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "invalid assembly phase JSON fields");
    for (const auto* key : keys) require(value.contains(key), "missing assembly phase JSON field");
}
AssemblyPhaseRoot decode_root(const nlohmann::json& row) {
    fields(row, {"catalog_id", "instance_id"});
    return {row.at("catalog_id").get<std::string>(), row.at("instance_id").get<std::string>()};
}
std::vector<AssemblyPhaseRoot> decode_roots(const nlohmann::json& rows) {
    require(rows.is_array() && rows.size() <= max_roots, "invalid assembly phase root collection");
    std::vector<AssemblyPhaseRoot> result;
    result.reserve(rows.size());
    for (const auto& row : rows) result.push_back(decode_root(row));
    return result;
}
nlohmann::json encode_roots(const std::vector<AssemblyPhaseRoot>& roots) {
    auto result = nlohmann::json::array();
    for (const auto& [catalog, instance] : roots)
        result.push_back({{"catalog_id", catalog}, {"instance_id", instance}});
    return result;
}
void canonical_roots(std::vector<AssemblyPhaseRoot>& roots) {
    require(roots.size() <= max_roots, "assembly phase root collection budget exceeded");
    for (const auto& [catalog, instance] : roots) { document_id(catalog); local_id(instance); }
    std::sort(roots.begin(), roots.end());
    require(std::adjacent_find(roots.begin(), roots.end()) == roots.end(), "duplicate assembly phase root");
}
// Typed construction gets the same aggregate admission before sorting, sets or
// JSON allocation. Include the fixed envelope/field overhead conservatively.
void admit_typed(const std::string& registry_id, const std::vector<AssemblyPhaseRoot>& roots,
    const std::vector<AssemblyPhaseRoot>& baseline, const std::vector<AssemblyPhaseAlternative>& alternatives) {
    require(alternatives.size() <= max_alternatives, "assembly phase alternative budget exceeded");
    JsonBudget budget;
    charge(budget.bytes, budget.byte_limit, 512);
    charge(budget.values, budget.value_limit, 8);
    document_id(registry_id);
    string_bytes(budget, registry_id);
    const auto admit_roots = [&](const auto& rows) {
        require(rows.size() <= max_roots, "assembly phase root collection budget exceeded");
        charge(budget.bytes, budget.byte_limit, 32);
        charge(budget.values, budget.value_limit, 1);
        for (const auto& [catalog, instance] : rows) {
            document_id(catalog); local_id(instance);
            charge(budget.bytes, budget.byte_limit, 128);
            charge(budget.values, budget.value_limit, 3);
            string_bytes(budget, catalog); string_bytes(budget, instance);
        }
    };
    admit_roots(roots); admit_roots(baseline);
    for (const auto& alternative : alternatives) {
        local_id(alternative.alternative_id);
        charge(budget.bytes, budget.byte_limit, 384);
        charge(budget.values, budget.value_limit, 4);
        string_bytes(budget, alternative.alternative_id);
        admit_roots(alternative.demolished_roots); admit_roots(alternative.proposed_roots);
    }
}
bool has_alternative(const ModelPhases& registry, const std::string& id) {
    const auto& alternatives = registry.alternatives();
    const auto found = std::lower_bound(alternatives.begin(), alternatives.end(), id,
        [](const auto& candidate, const auto& key) { return candidate.id < key; });
    return found != alternatives.end() && found->id == id;
}
nlohmann::json retain_roots(const nlohmann::json& saved, const nlohmann::json& proposed) {
    std::map<AssemblyPhaseRoot, const nlohmann::json*> remaining;
    for (const auto& row : proposed) remaining.emplace(decode_root(row), &row);
    auto result = nlohmann::json::array();
    for (const auto& row : saved)
        if (remaining.erase(decode_root(row))) result.push_back(row);
    for (const auto& [key, row] : remaining) { (void)key; result.push_back(*row); }
    return result;
}

struct ResolveBudget {
    JsonBudget json{0, 0, 16 * 1024 * 1024, 262144};
    std::size_t work{};
    void product(std::size_t a, std::size_t b) {
        constexpr std::size_t limit = 67108864;
        require(a == 0 || b <= (limit - work) / a, "assembly phase catalog validation work budget exceeded");
        charge(work, limit, a * b);
    }
    void admit(const nlohmann::json& value) {
        JsonBudget local;
        admit_json(value, local);
        charge(json.bytes, json.byte_limit, local.bytes);
        charge(json.values, json.value_limit, local.values);
        product(local.values + local.bytes / 8, 1);
    }
};
// Reserve topology and shared-graph multiplication before the real decoder.
// No adapter dependency is needed; raw rows grant no semantic authority.
void admit_catalog_work(const nlohmann::json& model, ResolveBudget& budget) {
    const auto& types = model.at("types");
    const auto& instances = model.at("instances");
    const auto& materials = model.at("materials");
    require(types.is_array() && types.size() <= 4096 && instances.is_array() && instances.size() <= 4096 &&
        materials.is_array(), "invalid assembly phase catalog collections");
    struct GraphSize { std::size_t nodes{1}, depth{1}, segments{}; };
    struct RawType {
        const nlohmann::json* row{};
        std::size_t own_segments{};
        int color{};
        GraphSize size;
    };
    std::map<std::string, RawType, std::less<>> raw_types;
    for (const auto& row : types) {
        require(row.is_object() && row.at("id").is_string(), "invalid assembly phase catalog type");
        const auto [entry, inserted] = raw_types.emplace(row.at("id").get<std::string>(), RawType{&row});
        require(inserted, "duplicate assembly phase catalog type");
        if (row.contains("parts"))
            require(row.at("parts").is_array() && row.at("parts").size() <= 4096,
                "invalid assembly phase catalog parts");
        if (!row.contains("profiles")) continue;
        require(row.at("profiles").is_array() && row.at("profiles").size() <= 4096,
            "invalid assembly phase catalog profiles");
        for (const auto& profile : row.at("profiles")) {
            require(profile.is_object() && profile.at("outer").is_array() && profile.at("outer").size() <= 1024 &&
                profile.at("holes").is_array() && profile.at("holes").size() <= 1024,
                "invalid assembly phase catalog profile boundaries");
            auto segments = profile.at("outer").size();
            for (const auto& hole : profile.at("holes")) {
                require(hole.is_array() && hole.size() <= 1024 - segments,
                    "assembly phase catalog profile segment budget exceeded");
                segments += hole.size();
            }
            std::size_t arcs{};
            const auto count_arcs = [&](const auto& boundary) {
                for (const auto& edge : boundary) {
                    require(edge.is_object() && edge.at("sweep_radians").is_number(),
                        "invalid assembly phase catalog boundary edge");
                    if (edge.at("sweep_radians") != 0) ++arcs;
                }
            };
            count_arcs(profile.at("outer"));
            for (const auto& hole : profile.at("holes")) count_arcs(hole);
            const auto topology = segments + profile.at("holes").size() + 1;
            budget.product(topology, 8 * topology);
            budget.product(arcs, 300000 * segments);
            require(segments <= 262144 - entry->second.own_segments,
                "assembly phase catalog type segment budget exceeded");
            entry->second.own_segments += segments;
        }
    }
    const auto visit = [&](const auto& self, RawType& type, std::size_t depth) -> GraphSize {
        budget.product(1, 1);
        require(depth <= 32 && type.color != 1, "assembly phase catalog graph cycle or depth limit");
        if (type.color == 2) return type.size;
        type.color = 1;
        GraphSize size{1, 1, type.own_segments};
        if (type.row->contains("parts")) for (const auto& part : type.row->at("parts")) {
            require(part.is_object() && part.at("type_id").is_string(), "invalid assembly phase catalog part");
            const auto child = raw_types.find(part.at("type_id").get_ref<const std::string&>());
            require(child != raw_types.end(), "assembly phase catalog part type is missing");
            const auto nested = self(self, child->second, depth + 1);
            require(nested.nodes <= 4096 - size.nodes && nested.segments <= 262144 - size.segments,
                "assembly phase catalog expansion budget exceeded");
            size.nodes += nested.nodes; size.segments += nested.segments;
            size.depth = std::max(size.depth, nested.depth + 1);
            require(size.depth <= 32, "assembly phase catalog graph depth budget exceeded");
        }
        type.color = 2; type.size = size;
        return size;
    };
    std::size_t validation_nodes{}, validation_segments{};
    for (auto& [id, type] : raw_types) {
        (void)id;
        const auto size = visit(visit, type, 1);
        require(size.nodes <= 16384 - validation_nodes && size.segments <= 1048576 - validation_segments,
            "assembly phase catalog aggregate validation budget exceeded");
        validation_nodes += size.nodes; validation_segments += size.segments;
    }
    std::size_t instance_nodes{}, instance_segments{};
    for (const auto& instance : instances) {
        require(instance.is_object() && instance.at("type_id").is_string(), "invalid assembly phase catalog instance");
        const auto type = raw_types.find(instance.at("type_id").get_ref<const std::string&>());
        require(type != raw_types.end(), "assembly phase catalog instance type is missing");
        require(type->second.size.nodes <= 4096 - instance_nodes &&
            type->second.size.segments <= 262144 - instance_segments,
            "assembly phase catalog aggregate instance budget exceeded");
        instance_nodes += type->second.size.nodes; instance_segments += type->second.size.segments;
    }
    JsonBudget source;
    admit_json(model, source);
    budget.product(source.values + source.bytes / 8,
        4 * (validation_nodes + instance_nodes + types.size() + materials.size() + 1));
    budget.product(validation_segments + instance_segments, 4);
}
const nlohmann::json& entity_model(const Entity& entity, const std::string& map_id, std::string_view type) {
    document_id(map_id);
    require(entity.id == map_id && entity.type == type, "assembly phase actual entity identity/type mismatch");
    require(entity.properties.is_object() && entity.properties.contains("model") &&
        entity.properties.at("model").is_object(), "assembly phase actual entity requires a model object");
    return entity.properties.at("model");
}
AssemblyPhaseMembershipScope resolve(const std::map<std::string, Entity, std::less<>>& entities,
    const std::optional<std::string>& comparison_registry, const std::optional<std::string>& alternative) {
    try {
        ResolveBudget budget;
        std::map<std::string, ModelPhases, std::less<>> registries;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> catalogs;
        const auto registry = [&](const std::string& id) -> const ModelPhases& {
            const auto cached = registries.find(id);
            if (cached != registries.end()) return cached->second;
            const auto actual = entities.find(id);
            require(actual != entities.end(), "assembly phase registry is missing");
            const auto& model = entity_model(actual->second, actual->first, "model_phases");
            budget.admit(model);
            return registries.emplace(id, ModelPhases::from_json(model)).first->second;
        };
        if (comparison_registry) {
            document_id(*comparison_registry);
            const auto& selected = registry(*comparison_registry);
            require(!alternative || has_alternative(selected, *alternative), "unknown assembly phase comparison alternative");
        }
        AssemblyPhaseMembershipScope result;
        for (const auto& [id, entity] : entities) {
            if (entity.type != assembly_phase_membership_entity_type) continue;
            require(entity.required, "assembly phase membership companion must be required");
            const auto& raw = entity_model(entity, id, assembly_phase_membership_entity_type);
            budget.admit(raw);
            const auto membership = AssemblyPhaseMembership::from_json(raw);
            const auto& selected_registry = registry(membership.registry_id());
            const auto states = comparison_registry && *comparison_registry == membership.registry_id() ?
                membership.state(selected_registry, alternative) : membership.state(selected_registry);
            for (const auto& root : membership.roots()) {
                require(result.owners.emplace(root, AssemblyPhaseMembershipOwner{id, membership.registry_id()}).second,
                    "qualified assembly phase root belongs to multiple companions");
                auto catalog = catalogs.find(root.first);
                if (catalog == catalogs.end()) {
                    const auto actual = entities.find(root.first);
                    require(actual != entities.end(), "assembly phase catalog is missing");
                    const auto& model = entity_model(actual->second, actual->first, "assembly_model");
                    budget.admit(model);
                    admit_catalog_work(model, budget);
                    const auto decoded = AssemblyModel::from_json(model);
                    std::set<std::string, std::less<>> instances;
                    for (const auto& row : decoded.instances()) instances.insert(row.id);
                    catalog = catalogs.emplace(root.first, std::move(instances)).first;
                }
                require(catalog->second.contains(root.second), "assembly phase root references a missing actual catalog instance");
                const auto phase = states.find(root);
                if (phase != states.end()) result.states.emplace(root, phase->second);
                if (phase == states.end() || phase->second == ModelPhase::demolished) result.inactive_roots.insert(root);
            }
        }
        return result;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly phase actual map: ") + error.what());
    }
}
} // namespace

AssemblyPhaseMembership AssemblyPhaseMembership::create(std::string registry_id,
    std::vector<AssemblyPhaseRoot> roots, std::vector<AssemblyPhaseRoot> baseline_roots,
    std::vector<AssemblyPhaseAlternative> alternatives) {
    admit_typed(registry_id, roots, baseline_roots, alternatives);
    canonical_roots(roots); canonical_roots(baseline_roots);
    const std::set<AssemblyPhaseRoot> known(roots.begin(), roots.end());
    const std::set<AssemblyPhaseRoot> baseline(baseline_roots.begin(), baseline_roots.end());
    auto assigned = baseline;
    for (const auto& root : baseline) require(known.contains(root), "assembly phase baseline references an undeclared root");
    std::set<std::string, std::less<>> ids;
    for (auto& alternative : alternatives) {
        require(ids.insert(alternative.alternative_id).second, "duplicate assembly phase alternative ID");
        canonical_roots(alternative.demolished_roots); canonical_roots(alternative.proposed_roots);
        for (const auto& root : alternative.demolished_roots)
            require(baseline.contains(root), "assembly phase demolition must reference baseline");
        for (const auto& root : alternative.proposed_roots) {
            require(known.contains(root), "assembly phase proposal references an undeclared root");
            require(assigned.insert(root).second, "assembly phase proposal overlaps baseline or another alternative");
        }
    }
    require(assigned == known, "every declared assembly phase root must have membership");
    std::sort(alternatives.begin(), alternatives.end(),
        [](const auto& a, const auto& b) { return a.alternative_id < b.alternative_id; });
    AssemblyPhaseMembership result;
    result.registry_id_ = std::move(registry_id); result.roots_ = std::move(roots);
    result.baseline_roots_ = std::move(baseline_roots); result.alternatives_ = std::move(alternatives);
    return result;
}
AssemblyPhaseMembership AssemblyPhaseMembership::from_json(const nlohmann::json& value) {
    try {
        JsonBudget budget;
        admit_json(value, budget);
        fields(value, {"schema", "version", "registry_id", "roots", "baseline_roots", "alternatives"});
        require(value.at("schema") == "sketch.assembly_phase_membership" &&
            value.at("version").is_number_integer() && value.at("version") == 1,
            "unsupported assembly phase membership schema");
        const auto& rows = value.at("alternatives");
        require(rows.is_array() && rows.size() <= max_alternatives, "invalid assembly phase alternative collection");
        std::vector<AssemblyPhaseAlternative> alternatives;
        alternatives.reserve(rows.size());
        for (const auto& row : rows) {
            fields(row, {"alternative_id", "demolished_roots", "proposed_roots"});
            alternatives.push_back({row.at("alternative_id").get<std::string>(),
                decode_roots(row.at("demolished_roots")), decode_roots(row.at("proposed_roots"))});
        }
        return create(value.at("registry_id").get<std::string>(), decode_roots(value.at("roots")),
            decode_roots(value.at("baseline_roots")), std::move(alternatives));
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly phase membership JSON: ") + error.what());
    }
}
nlohmann::json AssemblyPhaseMembership::to_json() const {
    auto alternatives = nlohmann::json::array();
    for (const auto& row : alternatives_)
        alternatives.push_back({{"alternative_id", row.alternative_id},
            {"demolished_roots", encode_roots(row.demolished_roots)}, {"proposed_roots", encode_roots(row.proposed_roots)}});
    return {{"schema", "sketch.assembly_phase_membership"}, {"version", 1}, {"registry_id", registry_id_},
        {"roots", encode_roots(roots_)}, {"baseline_roots", encode_roots(baseline_roots_)}, {"alternatives", alternatives}};
}
void AssemblyPhaseMembership::validate_registry(const ModelPhases& registry) const {
    for (const auto& row : alternatives_)
        require(has_alternative(registry, row.alternative_id), "assembly phase membership references an unknown registry alternative");
}
std::map<AssemblyPhaseRoot, ModelPhase> AssemblyPhaseMembership::state(const ModelPhases& registry) const {
    return state(registry, registry.active_alternative());
}
std::map<AssemblyPhaseRoot, ModelPhase> AssemblyPhaseMembership::state(const ModelPhases& registry,
    const std::optional<std::string>& alternative) const {
    validate_registry(registry);
    require(!alternative || has_alternative(registry, *alternative), "unknown assembly phase state alternative");
    std::map<AssemblyPhaseRoot, ModelPhase> result;
    for (const auto& root : baseline_roots_) result.emplace(root, ModelPhase::existing);
    if (alternative) {
        const auto found = std::lower_bound(alternatives_.begin(), alternatives_.end(), *alternative,
            [](const auto& row, const auto& id) { return row.alternative_id < id; });
        if (found != alternatives_.end() && found->alternative_id == *alternative) {
            for (const auto& root : found->demolished_roots) result.at(root) = ModelPhase::demolished;
            for (const auto& root : found->proposed_roots) result.emplace(root, ModelPhase::proposed);
        }
    }
    return result;
}
nlohmann::json retain_assembly_phase_membership_source(const nlohmann::json& saved,
    const AssemblyPhaseMembership& proposed) {
    (void)AssemblyPhaseMembership::from_json(saved);
    const auto canonical = proposed.to_json();
    auto result = saved;
    result.at("registry_id") = canonical.at("registry_id");
    result.at("roots") = retain_roots(saved.at("roots"), canonical.at("roots"));
    result.at("baseline_roots") = retain_roots(saved.at("baseline_roots"), canonical.at("baseline_roots"));
    std::map<std::string, const nlohmann::json*, std::less<>> remaining;
    for (const auto& row : canonical.at("alternatives"))
        remaining.emplace(row.at("alternative_id").get_ref<const std::string&>(), &row);
    auto alternatives = nlohmann::json::array();
    for (const auto& row : saved.at("alternatives")) {
        const auto found = remaining.find(row.at("alternative_id").get_ref<const std::string&>());
        if (found == remaining.end()) continue;
        auto retained = row;
        retained.at("demolished_roots") = retain_roots(row.at("demolished_roots"), found->second->at("demolished_roots"));
        retained.at("proposed_roots") = retain_roots(row.at("proposed_roots"), found->second->at("proposed_roots"));
        alternatives.push_back(std::move(retained));
        remaining.erase(found);
    }
    for (const auto& [id, row] : remaining) { (void)id; alternatives.push_back(*row); }
    result.at("alternatives") = std::move(alternatives);
    require(AssemblyPhaseMembership::from_json(result).to_json() == canonical,
        "retained assembly phase source differs from proposed model");
    return result;
}
AssemblyPhaseMembershipScope resolve_assembly_phase_memberships(
    const std::map<std::string, Entity, std::less<>>& entities) {
    return resolve(entities, std::nullopt, std::nullopt);
}
AssemblyPhaseMembershipScope resolve_assembly_phase_memberships(
    const std::map<std::string, Entity, std::less<>>& entities,
    std::string_view registry_id, const std::optional<std::string>& alternative) {
    return resolve(entities, std::string(registry_id), alternative);
}

} // namespace sketch
