#include "sketch/phase_assembly_catalog_edit.hpp"

#include "sketch/phase_assembly_instance_property_edit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = AssemblyDocumentEntities;
using Target = AssemblyInstancePropertyEditTarget;
using Root = PhaseAssemblyCatalogIsolationRoot;
using Models = std::map<std::string, AssemblyModel, std::less<>>;
using DefinitionKind = PhaseAssemblyCatalogDefinitionKind;
using Disposition = PhaseAssemblyCatalogDefinitionDisposition;
using DefinitionKey = std::tuple<std::string, DefinitionKind, std::string>;
constexpr std::size_t root_limit = 1000;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(std::string("Phase assembly catalog edit: ") + message);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    require(value.is_object() && value.size() == names.size(), "unsupported closed fields");
    for (const auto* name : names) require(value.contains(name), "missing closed field");
}
void document_id(std::string_view text) {
    require(!text.empty() && text.size() <= 128 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    }), "invalid bounded document identity");
}
std::string id(const Json& value) {
    require(value.is_string(), "document identity must be text");
    const auto& text = value.get_ref<const std::string&>(); document_id(text); return text;
}
void local_id(std::string_view text) {
    require(!text.empty() && text.size() <= 4096 && text.find('\0') == std::string_view::npos &&
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
        "invalid bounded local identity");
}
// Admission precedes dumps, source copies, model codecs and identity inventories.
struct Budget {
    std::size_t bytes{}, nodes{};
    std::size_t byte_limit{64 * 1024 * 1024}, node_limit{4 * 1024 * 1024};
    void charge(std::size_t amount) {
        require(amount <= byte_limit - bytes, "raw byte budget exceeded"); bytes += amount;
    }
    void text(std::string_view value) {
        require(value.size() <= (byte_limit - bytes) / 6, "raw string budget exceeded"); charge(6 * value.size());
    }
    void read(const Json& value, std::size_t depth = 0) {
        require(depth <= 64 && ++nodes <= node_limit, "raw node/depth budget exceeded"); charge(32);
        require(!value.is_binary() && !value.is_discarded() &&
            (!value.is_number_float() || std::isfinite(value.get<double>())), "nonportable raw JSON");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
    }
    void entity(const Entity& value) {
        text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
    void map(const Entities& values) {
        require(values.size() <= 100000, "entity count exceeds bound");
        for (const auto& [key, value] : values) {
            document_id(key);
            require(value.id == key && value.properties.is_object() && value.extensions.is_object(), "invalid actual entity envelope");
            entity(value);
        }
    }
};
Budget wire_budget() { return Budget{0, 0, 16 * 1024 * 1024, 262144}; }
bool exact(const Json& a, const Json& b) {
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_object()) {
        for (const auto& [key, child] : a.items()) {
            const auto other = b.find(key); if (other == b.end() || !exact(child, *other)) return false;
        }
        return true;
    }
    if (a.is_array()) {
        for (std::size_t i = 0; i < a.size(); ++i) if (!exact(a.at(i), b.at(i))) return false;
        return true;
    }
    return a == b;
}
bool exact(const Entity& a, const Entity& b) {
    return a.id == b.id && a.type == b.type && a.required == b.required &&
        exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
Json optional(const std::optional<std::string>& value) { return value ? Json(*value) : Json(nullptr); }
Json encode_root(const Target& root) {
    document_id(root.catalog_id); local_id(root.instance_id);
    if (root.document_entity_id) document_id(*root.document_entity_id);
    require(root.part_path.empty(), "allocation must name an exact root");
    return { {"catalog_id", root.catalog_id}, {"instance_id", root.instance_id},
        {"document_entity_id", optional(root.document_entity_id)} };
}
Target decode_root(const Json& value) {
    fields(value, {"catalog_id", "instance_id", "document_entity_id"});
    require(value.at("instance_id").is_string(), "root instance identity must be text");
    const auto& instance = value.at("instance_id").get_ref<const std::string&>(); local_id(instance);
    return {id(value.at("catalog_id")), instance, value.at("document_entity_id").is_null() ? std::nullopt :
        std::optional{id(value.at("document_entity_id"))}, {}};
}
void admit_catalogs(const Entities& source, AssemblyCatalogTransferBudget& budget) {
    for (const auto& [key, value] : source) {
        (void)key; if (value.type == "assembly_model") admit_existing_assembly_catalog_work(value, budget);
    }
}
Models decode_models(const Entities& source) {
    Models result;
    for (const auto& [key, value] : source) if (value.type == "assembly_model")
        result.emplace(key, AssemblyModel::from_json(value.properties.at("model")));
    return result;
}
template<class T> const T& definition(const std::vector<T>& values, const std::string& key) {
    const auto found = std::find_if(values.begin(), values.end(), [&](const T& value) { return value.id == key; });
    require(found != values.end(), "actual reached definition is missing"); return *found;
}
const Json& raw_row(const Json& model, const char* collection, const std::string& key) {
    const auto& rows = model.at(collection);
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const Json& row) { return row.at("id") == key; });
    require(found != rows.end(), "actual source row is missing"); return *found;
}
bool changed(const AssemblyCatalogPhaseEditRootImpact& impact) {
    return impact.expansion_changed || impact.definitions_changed;
}

struct Phases {
    std::map<std::string, ModelPhases, std::less<>> registries;
    std::map<std::string, AssemblyPhaseMembership, std::less<>> companions;
    std::map<std::string, std::string, std::less<>> owners;
    AssemblyPhaseMembershipScope rows;
    explicit Phases(const Entities& source) {
        for (const auto& [key, value] : source) if (value.type == "model_phases") {
            auto registry = ModelPhases::from_json(value.properties.at("model"));
            for (const auto& owner : registry.entity_ids()) {
                const auto actual = source.find(owner);
                require(actual != source.end() && is_model_phase_entity_type(actual->second.type), "registry has no actual supported owner");
                require(owners.emplace(owner, key).second, "actual owner has overlapping registries");
            }
            registries.emplace(key, std::move(registry));
        }
        rows = resolve_assembly_phase_memberships(source);
        for (const auto& [key, value] : source) if (value.type == assembly_phase_membership_entity_type)
            companions.emplace(key, AssemblyPhaseMembership::from_json(value.properties.at("model")));
    }
};
void account_definition_effects(const Entities&, const Json&, PhaseAssemblyCatalogEditPlan&);
PhaseAssemblyCatalogEditPlan isolation_plan(const Entities& source, const Json& request) {
    PhaseAssemblyCatalogEditPlan plan;
    // This authenticates the bound original and admits BOTH complete catalog
    // work sides before any expansion or semantic replacement authority here.
    plan.source = inspect_assembly_catalog_phase_edit(source, request);
    if (plan.source.ordinary_replay_safe) {
        account_definition_effects(source, request, plan); return plan;
    }
    const Phases phases(source);
    std::set<std::string, std::less<>> needed;
    for (const auto& impact : plan.source.roots) {
        if (!changed(impact) || impact.cohort == AssemblyCatalogPhaseEditCohort::inactive_retained) continue;
        require(plan.isolation_roots.size() < root_limit, "active isolated root count exceeds bound");
        Root root{impact.root, impact.cohort == AssemblyCatalogPhaseEditCohort::baseline_protected,
            std::nullopt, std::nullopt, std::nullopt};
        const auto declared = impact.root.document_entity_id ? phases.rows.owners.end() :
            phases.rows.owners.find({impact.root.catalog_id, impact.root.instance_id});
        const auto owner = phases.owners.find(impact.root.document_entity_id ? *impact.root.document_entity_id : impact.root.catalog_id);
        if (declared != phases.rows.owners.end()) {
            root.registry_id = declared->second.registry_id;
            root.membership_entity_id = declared->second.membership_entity_id;
        } else if (owner != phases.owners.end()) root.registry_id = owner->second;
        // A protected inherited host supplies a real saved registry choice.
        if (root.baseline_replacement) {
            for (const auto& binding : impact.phase_bindings) {
                if (!root.registry_id && binding.baseline && binding.active_alternative) root.registry_id = binding.registry_id;
            }
            require(root.registry_id.has_value(), "protected root has no actual replacement registry");
            // One companion cannot encode the conjunction of unrelated saved
            // registry choices. Never infer enrollment from phase-like strings.
            for (const auto& binding : impact.phase_bindings)
                require(binding.registry_id == *root.registry_id, "baseline replacement spans independent catalog/host registries; coordinated cohort authoring is required");
            const auto& registry = phases.registries.at(*root.registry_id);
            require(registry.active_alternative().has_value(), "baseline replacement needs the actual active alternative");
            root.alternative_id = registry.active_alternative();
            if (declared != phases.rows.owners.end()) {
                const auto& membership = phases.companions.at(*root.membership_entity_id);
                const AssemblyPhaseRoot pair{root.root.catalog_id, root.root.instance_id};
                require(std::find(membership.baseline_roots().begin(), membership.baseline_roots().end(), pair) != membership.baseline_roots().end(),
                    "proposed row on a protected host requires coordinated host replacement");
            } else if (owner != phases.owners.end()) {
                require(std::find(registry.baseline_ids().begin(), registry.baseline_ids().end(), owner->first) != registry.baseline_ids().end(),
                    "proposed owner on a protected host requires coordinated host replacement");
            }
            if (!root.root.document_entity_id && !root.membership_entity_id) {
                // Reuse one actual companion in canonical document order. A
                // registry can validly have several disjoint companions.
                for (const auto& [key, membership] : phases.companions) if (membership.registry_id() == *root.registry_id) {
                    root.membership_entity_id = key; break;
                }
                if (!root.membership_entity_id) needed.insert(*root.registry_id);
            }
        } else if (root.registry_id) root.alternative_id = phases.registries.at(*root.registry_id).active_alternative();
        plan.isolation_roots.push_back(std::move(root));
    }
    plan.required_membership_registry_ids.assign(needed.begin(), needed.end());
    account_definition_effects(source, request, plan);
    return plan;
}

// The request has already been authenticated and typed by the foundation.
// Derive only the actual requested rows, mirroring its retained dialect policy.
Entity edited_catalog(const Entity& original, const Json* edit) {
    auto result = original;
    if (!edit) return result;
    auto& model = result.properties.at("model");
    const auto& saved = original.properties.at("model");
    const auto schema = saved.at("schema").get<std::string>();
    bool promote = false;
    for (const auto* collection : {"types", "materials"}) for (const auto& change : edit->at(collection)) {
        const auto key = change.at("replacement").at("id").get<std::string>();
        require(exact(raw_row(saved, collection, key), change.at("original")), "actual original definition binding changed");
        auto replacement = change.at("replacement");
        if (std::string_view(collection) == "types") {
            // Foundation request3 has already proved exact source topology/XY.
            // Private carriers must retain the same raw source representations
            // as ordinary replay, including when type metadata alone changes.
            const auto& original_type=raw_row(saved,collection,key);
            if(original_type.contains("profiles"))for(auto& profile:replacement.at("profiles")) {
                const auto retained=std::find_if(original_type.at("profiles").begin(),original_type.at("profiles").end(),
                    [&](const auto& value){return value.at("id")==profile.at("id");});
                if(retained!=original_type.at("profiles").end() && retained->contains("topology")) {
                    for(const auto* field:{"version","topology","outer","holes"})profile[field]=retained->at(field);
                }
            }
            const bool nested = schema == "sketch.assemblies.v4" || schema == "sketch.assemblies.v5" || schema == "sketch.assemblies.v6";
            if (!nested && !raw_row(saved, collection, key).contains("profiles") &&
                replacement.at("profiles").empty() && replacement.at("parts").empty()) {
                replacement.erase("profiles"); replacement.erase("parts");
            } else if (!nested && schema != "sketch.assemblies.v7" && schema != "sketch.assemblies.v8") promote = true;
            if (schema != "sketch.assemblies.v6" && schema != "sketch.assemblies.v7" && schema != "sketch.assemblies.v8" && replacement.contains("parts"))
                for (const auto& part : replacement.at("parts")) if (part.at("transform").contains("vertical_scale")) promote = true;
        } else if (schema == "sketch.assemblies.v1" && replacement.contains("color_srgb")) promote = true;
        for (auto& row : model.at(collection)) if (row.at("id") == key) { row = std::move(replacement); break; }
    }
    if (promote) model.at("schema") = schema == "sketch.assemblies.v8" ? "sketch.assemblies.v8" : "sketch.assemblies.v7";
    return result;
}
void retain_equal_definitions(const Entity& original, Entity& edited, const AssemblyModel& before, const AssemblyModel& after) {
    const auto& saved = original.properties.at("model"); auto& model = edited.properties.at("model");
    bool different = false;
    for (auto& row : model.at("types")) {
        const auto key = row.at("id").get<std::string>();
        if (definition(before.types(), key) == definition(after.types(), key)) row = raw_row(saved, "types", key);
        else different = true;
    }
    for (auto& row : model.at("materials")) {
        const auto key = row.at("id").get<std::string>();
        if (definition(before.materials(), key) == definition(after.materials(), key)) row = raw_row(saved, "materials", key);
        else different = true;
    }
    if (!different) model = saved;
}
void account_definition_effects(const Entities& source, const Json& request, PhaseAssemblyCatalogEditPlan& plan) {
    // Full-request foundation admission has already completed. Reserve these
    // additional actual carrier copies and BOTH model work sides before decode.
    Budget copies; copies.map(source);
    AssemblyCatalogTransferBudget work; admit_catalogs(source, work);
    for (const auto& edit : request.at("edits")) copies.entity(source.at(edit.at("catalog_id").get<std::string>()));
    Entities edited;
    for (const auto& edit : request.at("edits")) {
        const auto catalog = edit.at("catalog_id").get<std::string>();
        edited.emplace(catalog, edited_catalog(source.at(catalog), &edit));
    }
    admit_catalogs(edited, work);
    const auto before = decode_models(source), after = decode_models(edited);
    std::set<DefinitionKey> protected_definitions;
    std::map<DefinitionKey, std::size_t> private_consumers;
    std::set<Target> isolated;
    for (const auto& root : plan.isolation_roots) isolated.insert(root.root);
    for (const auto& impact : plan.source.roots) {
        const auto inventory = [&](DefinitionKind kind, const auto& ids, auto& destination) {
            for (const auto& key : ids) destination.insert(DefinitionKey{impact.root.catalog_id, kind, key});
        };
        if (impact.cohort != AssemblyCatalogPhaseEditCohort::active_ordinary) {
            inventory(DefinitionKind::type, impact.before_type_ids, protected_definitions);
            inventory(DefinitionKind::material, impact.before_material_ids, protected_definitions);
        }
        if (isolated.contains(impact.root)) {
            for (const auto& key : impact.after_type_ids) ++private_consumers[{impact.root.catalog_id, DefinitionKind::type, key}];
            for (const auto& key : impact.after_material_ids) ++private_consumers[{impact.root.catalog_id, DefinitionKind::material, key}];
        }
    }
    for (const auto& consumer : plan.source.material_consumers)
        if (consumer.cohort != AssemblyCatalogPhaseEditCohort::active_ordinary)
            protected_definitions.insert({consumer.catalog_id, DefinitionKind::material, consumer.material_id});
    std::set<DefinitionKey> global_candidates;
    Json residual_edits = Json::array();
    for (const auto& edit : request.at("edits")) {
        const auto catalog = edit.at("catalog_id").get<std::string>();
        Json types = Json::array(), materials = Json::array();
        for (const auto* collection : {"types", "materials"}) {
            const auto kind = std::string_view(collection) == "types" ? DefinitionKind::type : DefinitionKind::material;
            for (const auto& change : edit.at(collection)) {
                const auto key = change.at("replacement").at("id").get<std::string>();
                const DefinitionKey qualified{catalog, kind, key};
                const bool different = kind == DefinitionKind::type ?
                    !(definition(before.at(catalog).types(), key) == definition(after.at(catalog).types(), key)) :
                    !(definition(before.at(catalog).materials(), key) == definition(after.at(catalog).materials(), key));
                const auto consumed = private_consumers.find(qualified);
                const auto count = consumed == private_consumers.end() ? 0 : consumed->second;
                const bool eligible = different && (plan.source.ordinary_replay_safe || !protected_definitions.contains(qualified));
                if (eligible) {
                    global_candidates.insert(qualified);
                    (kind == DefinitionKind::type ? types : materials).push_back(change);
                }
                plan.definition_effects.push_back({catalog, kind, key, different ?
                    (count != 0 ? Disposition::private_only : Disposition::not_applied) : Disposition::unchanged, count});
            }
        }
        if (!types.empty() || !materials.empty()) residual_edits.push_back({{"catalog_id", catalog},
            {"source_catalog_digest", edit.at("source_catalog_digest")}, {"types", std::move(types)}, {"materials", std::move(materials)}});
    }
    if (plan.source.ordinary_replay_safe) plan.admitted_global_request = request;
    else if (!residual_edits.empty()) {
        const auto diagnostic = [&](const std::string& catalog, std::string_view reason) {
            if (plan.global_scope_diagnostic.size() >= 4096) return;
            const auto prefix = plan.global_scope_diagnostic.empty() ? std::string{} : std::string("; ");
            const auto label = prefix + catalog + ": ";
            plan.global_scope_diagnostic.append(label.substr(0, 4096 - plan.global_scope_diagnostic.size()));
            plan.global_scope_diagnostic.append(reason.substr(0, 4096 - plan.global_scope_diagnostic.size()));
        };
        // Catalog-local type graphs are screened as WHOLE eligible subsets.
        // A broken residual graph in C1 must not discard an independent safe
        // unused edit in C2. Admit every raw candidate before semantic screening,
        // and share work/expansion ledgers across all groups, including failures.
        Budget residual_copies; residual_copies.map(source);
        for (const auto& edit : residual_edits) residual_copies.entity(source.at(edit.at("catalog_id").get<std::string>()));
        Entities residual_catalogs;
        for (const auto& edit : residual_edits) {
            const auto catalog = edit.at("catalog_id").get<std::string>();
            residual_catalogs.emplace(catalog, edited_catalog(source.at(catalog), &edit));
        }
        AssemblyCatalogTransferBudget residual_work; admit_catalogs(source, residual_work);
        std::set<std::string, std::less<>> admitted_catalogs;
        for (const auto& [catalog, value] : residual_catalogs) {
            try { admit_existing_assembly_catalog_work(value, residual_work); admitted_catalogs.insert(catalog); }
            catch (const std::invalid_argument& error) { diagnostic(catalog, error.what()); }
        }
        std::map<std::string, std::vector<const AssemblyCatalogPhaseEditRootImpact*>, std::less<>> catalog_roots;
        for (const auto& impact : plan.source.roots) catalog_roots[impact.root.catalog_id].push_back(&impact);
        AssemblyExpansionBudget residual_expansions;
        for (const auto& [catalog, value] : residual_catalogs) if (admitted_catalogs.contains(catalog)) {
            try {
                const auto model = AssemblyModel::from_json(value.properties.at("model"));
                const auto roots = catalog_roots.find(catalog);
                if (roots != catalog_roots.end()) for (const auto* impact : roots->second)
                    (void)model.expand(impact->before.source_instance, residual_expansions);
            } catch (const std::invalid_argument& error) {
                admitted_catalogs.erase(catalog); diagnostic(catalog, error.what());
            }
        }
        Json accepted = Json::array();
        for (const auto& edit : residual_edits)
            if (admitted_catalogs.contains(edit.at("catalog_id").get<std::string>())) accepted.push_back(edit);
        for (auto key = global_candidates.begin(); key != global_candidates.end(); ) {
            if (!admitted_catalogs.contains(std::get<0>(*key))) key = global_candidates.erase(key);
            else ++key;
        }
        if (!accepted.empty()) {
            Json residual{{"version", request.at("version")}, {"source_entities_digest", request.at("source_entities_digest")}, {"edits", std::move(accepted)}};
            try {
                const auto admitted = inspect_assembly_catalog_phase_edit(source, residual);
                require(admitted.ordinary_replay_safe, "whole residual changes protected or inactive consumers");
                plan.admitted_global_request = std::move(residual);
            } catch (const std::invalid_argument& error) {
                // The combined source-bound admission is the final authority;
                // local graph validity never grants global/phase permission.
                global_candidates.clear(); diagnostic("combined residual", error.what());
            }
        }
    }
    if (plan.admitted_global_request) for (auto& effect : plan.definition_effects)
        if (global_candidates.contains({effect.catalog_id, effect.kind, effect.definition_id})) effect.disposition = Disposition::global;
    const bool unapplied = std::any_of(plan.definition_effects.begin(), plan.definition_effects.end(),
        [](const auto& effect) { return effect.disposition == Disposition::not_applied; });
    if (unapplied && plan.global_scope_diagnostic.empty())
        plan.global_scope_diagnostic = "requested changed definition has no admitted global effect or actual active private consumer";
}
template<class T> void append_once(std::vector<T>& rows, const T& value) {
    if (std::find(rows.begin(), rows.end(), value) == rows.end()) rows.push_back(value);
}
void remap_pair(std::vector<AssemblyPhaseRoot>& rows, const AssemblyPhaseRoot& source, const AssemblyPhaseRoot& destination) {
    for (auto& row : rows) if (row == source) row = destination;
}
AssemblyPhaseMembership move_membership(const AssemblyPhaseMembership& saved,
    const AssemblyPhaseRoot& source, const AssemblyPhaseRoot& destination) {
    auto roots = saved.roots(), baseline = saved.baseline_roots(); auto alternatives = saved.alternatives();
    remap_pair(roots, source, destination); remap_pair(baseline, source, destination);
    for (auto& row : alternatives) {
        remap_pair(row.demolished_roots, source, destination); remap_pair(row.proposed_roots, source, destination);
    }
    return AssemblyPhaseMembership::create(saved.registry_id(), std::move(roots), std::move(baseline), std::move(alternatives));
}
AssemblyPhaseMembership replace_membership(const AssemblyPhaseMembership& saved, const std::string& alternative,
    const AssemblyPhaseRoot& source, const AssemblyPhaseRoot& destination) {
    auto roots = saved.roots(), baseline = saved.baseline_roots(); auto alternatives = saved.alternatives();
    if (std::find(roots.begin(), roots.end(), source) == roots.end()) { roots.push_back(source); baseline.push_back(source); }
    append_once(roots, destination);
    auto active = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& row) { return row.alternative_id == alternative; });
    if (active == alternatives.end()) { alternatives.push_back({alternative, {}, {}}); active = std::prev(alternatives.end()); }
    append_once(active->demolished_roots, source); append_once(active->proposed_roots, destination);
    return AssemblyPhaseMembership::create(saved.registry_id(), std::move(roots), std::move(baseline), std::move(alternatives));
}
ModelPhases replace_owner(const ModelPhases& saved, const std::string& source, const std::string& destination) {
    require(saved.active_alternative().has_value(), "replacement registry has no actual saved alternative");
    auto owners = saved.entity_ids(), baseline = saved.baseline_ids(); auto alternatives = saved.alternatives();
    if (std::find(owners.begin(), owners.end(), source) == owners.end()) { owners.push_back(source); baseline.push_back(source); }
    append_once(owners, destination);
    auto active = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& row) { return row.id == *saved.active_alternative(); });
    require(active != alternatives.end(), "actual saved alternative is missing");
    append_once(active->demolished_ids, source); append_once(active->proposed_ids, destination);
    return ModelPhases::create(std::move(owners), std::move(baseline), std::move(alternatives), saved.active_alternative());
}
// Undeclared ordinary rows keep their carrier's full actual membership in all
// saved designs after moving into a private carrier. The source stays registered
// because other rows/independent consumers still use it.
ModelPhases duplicate_owner_membership(const ModelPhases& saved, const std::string& source, const std::string& destination) {
    require(std::find(saved.entity_ids().begin(), saved.entity_ids().end(), source) != saved.entity_ids().end(), "ordinary carrier is not an actual registry member");
    auto owners = saved.entity_ids(), baseline = saved.baseline_ids(); auto alternatives = saved.alternatives();
    append_once(owners, destination);
    if (std::find(baseline.begin(), baseline.end(), source) != baseline.end()) append_once(baseline, destination);
    for (auto& row : alternatives) {
        if (std::find(row.demolished_ids.begin(), row.demolished_ids.end(), source) != row.demolished_ids.end()) append_once(row.demolished_ids, destination);
        if (std::find(row.proposed_ids.begin(), row.proposed_ids.end(), source) != row.proposed_ids.end()) append_once(row.proposed_ids, destination);
    }
    return ModelPhases::create(std::move(owners), std::move(baseline), std::move(alternatives), saved.active_alternative());
}
bool equal_expansion(AssemblyExpansion actual, const AssemblyExpansion& expected) {
    // Identity is the only expansion field changed by fresh-root allocation.
    actual.source_instance.id = expected.source_instance.id;
    if (!(actual.source_instance == expected.source_instance) || actual.nodes.size() != expected.nodes.size() ||
        actual.profiles.size() != expected.profiles.size() || actual.declared_quantities != expected.declared_quantities ||
        actual.material_volumes_m3 != expected.material_volumes_m3 || actual.volume_m3 != expected.volume_m3) return false;
    for (std::size_t i = 0; i < actual.nodes.size(); ++i) {
        const auto& a = actual.nodes[i]; const auto& b = expected.nodes[i];
        if (a.part_path != b.part_path || a.type_id != b.type_id || a.transform != b.transform || a.properties != b.properties ||
            a.materials != b.materials || a.quantities != b.quantities || a.source_part != b.source_part || a.path_override != b.path_override) return false;
    }
    for (std::size_t i = 0; i < actual.profiles.size(); ++i) {
        const auto& a = actual.profiles[i]; const auto& b = expected.profiles[i];
        if (a.part_path != b.part_path || a.type_id != b.type_id || !(a.profile == b.profile) || a.transform != b.transform ||
            a.material_id != b.material_id || a.volume_m3 != b.volume_m3) return false;
    }
    return true;
}
void equal_reached_definitions(const AssemblyModel& actual, const AssemblyModel& expected,
    const std::vector<std::string>& types, const std::vector<std::string>& materials) {
    for (const auto& key : types) require(definition(actual.types(), key) == definition(expected.types(), key), "destination reached type differs from source authority");
    for (const auto& key : materials) require(definition(actual.materials(), key) == definition(expected.materials(), key), "destination reached material differs from source authority");
}
void require_saved_active(const Phases& phases, const Root& root, const Target& destination) {
    if (!root.registry_id) return;
    const auto& registry = phases.registries.at(*root.registry_id);
    require(registry.active_alternative() == root.alternative_id, "actual saved choice changed during replay");
    if (destination.document_entity_id) {
        const auto states = registry.active_state();
        const auto found = states.find(*destination.document_entity_id);
        require(found != states.end() && found->second != ModelPhase::demolished, "private independent destination is not active in actual registry");
        if (root.baseline_replacement) {
            const auto old = states.find(*root.root.document_entity_id);
            require(found->second == ModelPhase::proposed && old != states.end() && old->second == ModelPhase::demolished,
                "independent baseline replacement lacks actual demolition/proposed enrollment");
        }
    } else if (root.membership_entity_id || root.baseline_replacement) {
        const AssemblyPhaseRoot pair{destination.catalog_id, destination.instance_id};
        const auto found = phases.rows.states.find(pair);
        require(found != phases.rows.states.end() && found->second != ModelPhase::demolished, "private embedded destination is not active in actual companion");
        require(phases.rows.owners.at(pair).registry_id == *root.registry_id, "private embedded companion uses a different actual registry");
        if (root.baseline_replacement) {
            const auto old = phases.rows.states.find({root.root.catalog_id, root.root.instance_id});
            require(found->second == ModelPhase::proposed && old != phases.rows.states.end() && old->second == ModelPhase::demolished,
                "embedded baseline replacement lacks actual demolition/proposed enrollment");
        }
    } else {
        const auto states = registry.active_state(); const auto found = states.find(destination.catalog_id);
        require(found != states.end() && found->second != ModelPhase::demolished, "private ordinary carrier is not active in actual registry");
    }
}
} // namespace

Json encode_phase_assembly_catalog_edit_allocation(const PhaseAssemblyCatalogEditAllocation& value) {
    require(value.roots.size() <= root_limit && value.membership_entity_ids.size() <= root_limit, "typed allocation count exceeds bound");
    auto budget = wire_budget(); Json roots = Json::array(), memberships = Json::array();
    for (const auto& [root, identity] : value.roots) {
        document_id(identity.private_catalog_id);
        if (identity.replacement_entity_id) document_id(*identity.replacement_entity_id);
        if (identity.replacement_instance_id) document_id(*identity.replacement_instance_id);
        Json row{{"root", encode_root(root)}, {"private_catalog_id", identity.private_catalog_id},
            {"replacement_entity_id", optional(identity.replacement_entity_id)}, {"replacement_instance_id", optional(identity.replacement_instance_id)}};
        budget.read(row); roots.push_back(std::move(row));
    }
    for (const auto& [registry, membership] : value.membership_entity_ids) {
        document_id(registry); document_id(membership);
        Json row{{"registry_id", registry}, {"membership_entity_id", membership}}; budget.read(row); memberships.push_back(std::move(row));
    }
    Json result{{"version", 1}, {"roots", std::move(roots)}, {"membership_entity_ids", std::move(memberships)}};
    (void)decode_phase_assembly_catalog_edit_allocation(result); return result;
}
PhaseAssemblyCatalogEditAllocation decode_phase_assembly_catalog_edit_allocation(const Json& value) {
    try {
        auto budget = wire_budget(); budget.read(value);
        fields(value, {"version", "roots", "membership_entity_ids"});
        require(value.at("version").is_number_integer() && value.at("version") == 1, "unsupported allocation version");
        const auto& roots = value.at("roots"); const auto& memberships = value.at("membership_entity_ids");
        require(roots.is_array() && roots.size() <= root_limit && memberships.is_array() && memberships.size() <= root_limit,
            "allocation count exceeds bound");
        PhaseAssemblyCatalogEditAllocation result; std::set<std::string, std::less<>> fresh;
        const auto reserve = [&](const Json& wire) {
            auto identity = id(wire); require(fresh.insert(identity).second, "fresh allocation identities must be injective"); return identity;
        };
        std::optional<Target> previous;
        for (const auto& row : roots) {
            fields(row, {"root", "private_catalog_id", "replacement_entity_id", "replacement_instance_id"});
            auto root = decode_root(row.at("root"));
            require(!previous || *previous < root, "allocation roots must be sorted unique"); previous = root;
            PhaseAssemblyCatalogRootIdentity identity{reserve(row.at("private_catalog_id")), std::nullopt, std::nullopt};
            if (!row.at("replacement_entity_id").is_null()) identity.replacement_entity_id = reserve(row.at("replacement_entity_id"));
            if (!row.at("replacement_instance_id").is_null()) {
                if (identity.replacement_entity_id) {
                    identity.replacement_instance_id = id(row.at("replacement_instance_id"));
                    require(identity.replacement_instance_id == identity.replacement_entity_id, "fresh independent entity and instance must agree");
                } else identity.replacement_instance_id = reserve(row.at("replacement_instance_id"));
            }
            require(root.document_entity_id ? identity.replacement_entity_id.has_value() == identity.replacement_instance_id.has_value() :
                (!identity.replacement_entity_id && identity.replacement_instance_id.has_value()), "allocation role differs from qualified root");
            result.roots.emplace(std::move(root), std::move(identity));
        }
        std::optional<std::string> previous_registry;
        for (const auto& row : memberships) {
            fields(row, {"registry_id", "membership_entity_id"}); auto registry = id(row.at("registry_id"));
            require(!previous_registry || *previous_registry < registry, "membership allocations must be sorted unique"); previous_registry = registry;
            result.membership_entity_ids.emplace(std::move(registry), reserve(row.at("membership_entity_id")));
        }
        (void)value.dump(); return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid phase catalog allocation: ") + error.what());
    }
}
Json encode_phase_assembly_catalog_edit_authoring(const PhaseAssemblyCatalogEditAuthoring& value) {
    auto budget = wire_budget(); budget.read(value.catalog_request);
    auto request = validate_assembly_catalog_phase_edit_request(value.catalog_request);
    auto allocation = encode_phase_assembly_catalog_edit_allocation(value.allocation); budget.read(allocation);
    Json result{{"version", 1}, {"catalog_request", std::move(request)}, {"allocation", std::move(allocation)}};
    (void)decode_phase_assembly_catalog_edit_authoring(result); return result;
}
PhaseAssemblyCatalogEditAuthoring decode_phase_assembly_catalog_edit_authoring(const Json& value) {
    try {
        auto budget = wire_budget(); budget.read(value);
        fields(value, {"version", "catalog_request", "allocation"});
        require(value.at("version").is_number_integer() && value.at("version") == 1, "unsupported authoring version");
        return {validate_assembly_catalog_phase_edit_request(value.at("catalog_request")),
            decode_phase_assembly_catalog_edit_allocation(value.at("allocation"))};
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid phase catalog authoring: ") + error.what());
    }
}
PhaseAssemblyCatalogEditPlan inspect_phase_assembly_catalog_edit_plan(const Entities& source, const Json& request) {
    try { return isolation_plan(source, request); }
    catch (const Json::exception& error) { throw std::invalid_argument(std::string("invalid phase catalog source: ") + error.what()); }
}

PhaseAssemblyCatalogEditResult replay_phase_assembly_catalog_edit_entities(const Entities& source,
    const Json& request, const PhaseAssemblyCatalogEditAllocation& allocation) {
    try {
        const auto plan = isolation_plan(source, request);
        const auto unapplied = std::find_if(plan.definition_effects.begin(), plan.definition_effects.end(),
            [](const auto& effect) { return effect.disposition == Disposition::not_applied; });
        if (unapplied != plan.definition_effects.end()) throw std::invalid_argument(
            std::string("Phase assembly catalog edit: whole request refused for ") +
            (unapplied->kind == DefinitionKind::type ? "type " : "material ") + unapplied->catalog_id + "/" +
            unapplied->definition_id + ": " + plan.global_scope_diagnostic);
        // Source authority always precedes allocation encoding or collision scans.
        (void)encode_phase_assembly_catalog_edit_allocation(allocation);
        require(allocation.roots.size() == plan.isolation_roots.size() &&
            allocation.membership_entity_ids.size() == plan.required_membership_registry_ids.size(), "allocation differs from actual active isolation inventory");
        std::set<std::string, std::less<>> fresh;
        for (const auto& root : plan.isolation_roots) {
            const auto found = allocation.roots.find(root.root); require(found != allocation.roots.end(), "missing actual active root allocation");
            const auto& identity = found->second;
            if (root.root.document_entity_id) require(root.baseline_replacement ?
                (identity.replacement_entity_id && identity.replacement_instance_id && identity.replacement_entity_id == identity.replacement_instance_id) :
                (!identity.replacement_entity_id && !identity.replacement_instance_id), "independent allocation differs from actual baseline role");
            fresh.insert(identity.private_catalog_id);
            if (identity.replacement_entity_id) fresh.insert(*identity.replacement_entity_id);
            else if (identity.replacement_instance_id) fresh.insert(*identity.replacement_instance_id);
        }
        for (const auto& registry : plan.required_membership_registry_ids) {
            const auto found = allocation.membership_entity_ids.find(registry);
            require(found != allocation.membership_entity_ids.end(), "missing actual required companion allocation"); fresh.insert(found->second);
        }
        std::vector<std::string> fresh_ids(fresh.begin(), fresh.end());
        validate_phase_assembly_instance_property_edit_fresh_identities(source, fresh_ids);
        PhaseAssemblyCatalogEditResult result;
        result.definition_effects = plan.definition_effects;
        for (const auto& impact : plan.source.roots) result.original_to_proposed.emplace(impact.root, impact.root);
        if (plan.source.ordinary_replay_safe) {
            result.entities = replay_assembly_catalog_phase_edit_entities(source, request); return result;
        }

        // Admit the complete source plus every whole carrier replica before
        // copying envelopes. Changed destination catalogs receive a second work
        // ledger before any destination model decoding or final expansion.
        Budget copy_budget; copy_budget.map(source);
        AssemblyCatalogTransferBudget copied_work; admit_catalogs(source, copied_work);
        for (const auto& root : plan.isolation_roots) {
            copy_budget.entity(source.at(root.root.catalog_id));
            admit_existing_assembly_catalog_work(source.at(root.root.catalog_id), copied_work);
            if (root.baseline_replacement && root.root.document_entity_id) copy_budget.entity(source.at(*root.root.document_entity_id));
        }
        for (const auto& edit : request.at("edits")) copy_budget.entity(source.at(edit.at("catalog_id").get<std::string>()));
        std::map<std::string, const Json*, std::less<>> edits;
        for (const auto& edit : request.at("edits")) edits.emplace(edit.at("catalog_id").get<std::string>(), &edit);
        Entities edited;
        for (const auto& [catalog, edit] : edits) {
            edited.emplace(catalog, edited_catalog(source.at(catalog), edit));
        }
        AssemblyCatalogTransferBudget edited_work; admit_catalogs(source, edited_work); admit_catalogs(edited, edited_work);
        const auto before_models = decode_models(source), after_models = decode_models(edited);
        for (auto& [key, catalog] : edited) retain_equal_definitions(source.at(key), catalog, before_models.at(key), after_models.at(key));
        const Phases original_phases(source);
        auto registries = original_phases.registries;
        auto memberships = original_phases.companions;
        std::set<std::string, std::less<>> changed_registries, changed_memberships, row_changed_catalogs, changed_owners;
        Entities global_base;
        const Entities* global_authority = &source;
        std::optional<AssemblyCatalogPhaseEditPlan> global_plan;
        if (plan.admitted_global_request) {
            global_base = replay_assembly_catalog_phase_edit_entities(source, *plan.admitted_global_request);
            global_authority = &global_base;
            global_plan = inspect_assembly_catalog_phase_edit(source, *plan.admitted_global_request);
        }
        const auto global_models = decode_models(*global_authority);
        std::map<Target, const AssemblyCatalogPhaseEditRootImpact*> global_impacts;
        if (global_plan) for (const auto& impact : global_plan->roots) global_impacts.emplace(impact.root, &impact);
        result.entities = *global_authority;
        for (const auto& root : plan.isolation_roots) {
            const auto& identity = allocation.roots.at(root.root);
            const Target destination{identity.private_catalog_id,
                identity.replacement_instance_id.value_or(root.root.instance_id),
                identity.replacement_entity_id ? identity.replacement_entity_id : root.root.document_entity_id, {}};
            result.original_to_proposed.at(root.root) = destination;
            auto catalog = edited.at(root.root.catalog_id); catalog.id = identity.private_catalog_id;
            auto& private_rows = catalog.properties.at("model").at("instances");
            if (root.root.document_entity_id) private_rows = Json::array();
            else {
                auto row = raw_row(source.at(root.root.catalog_id).properties.at("model"), "instances", root.root.instance_id);
                row.at("id") = destination.instance_id; private_rows = Json::array({std::move(row)});
            }
            std::map<std::string, std::string, std::less<>> context_mapping;
            if (!root.root.document_entity_id) context_mapping.emplace(root.root.instance_id, destination.instance_id);
            catalog = remap_embedded_assembly_drawing_contexts(source.at(root.root.catalog_id), catalog, context_mapping);
            require(result.entities.emplace(destination.catalog_id, std::move(catalog)).second, "fresh private catalog collides");
            if (root.root.document_entity_id) {
                auto owner = source.at(*root.root.document_entity_id);
                if (root.baseline_replacement) owner.id = *destination.document_entity_id;
                owner.properties.at("assembly_catalog_id") = destination.catalog_id;
                owner.properties.at("instance").at("id") = destination.instance_id;
                if (root.baseline_replacement) {
                    require(result.entities.emplace(*destination.document_entity_id, std::move(owner)).second, "fresh independent owner collides");
                    registries.at(*root.registry_id) = replace_owner(registries.at(*root.registry_id), *root.root.document_entity_id, *destination.document_entity_id);
                    changed_registries.insert(*root.registry_id);
                } else {
                    changed_owners.insert(*destination.document_entity_id);
                    result.entities.at(*destination.document_entity_id) = std::move(owner);
                }
            } else {
                const AssemblyPhaseRoot old{root.root.catalog_id, root.root.instance_id}, proposed{destination.catalog_id, destination.instance_id};
                if (!root.baseline_replacement) {
                    auto& rows = result.entities.at(root.root.catalog_id).properties.at("model").at("instances");
                    const auto row = std::find_if(rows.begin(), rows.end(), [&](const Json& value) { return value.at("id") == root.root.instance_id; });
                    require(row != rows.end(), "ordinary actual row disappeared"); rows.erase(row); row_changed_catalogs.insert(root.root.catalog_id);
                    auto& source_catalog = result.entities.at(root.root.catalog_id);
                    source_catalog = prune_embedded_assembly_drawing_contexts(source_catalog);
                    if (root.membership_entity_id) {
                        memberships.at(*root.membership_entity_id) = move_membership(memberships.at(*root.membership_entity_id), old, proposed);
                        changed_memberships.insert(*root.membership_entity_id);
                    } else if (root.registry_id) {
                        registries.at(*root.registry_id) = duplicate_owner_membership(registries.at(*root.registry_id), root.root.catalog_id, destination.catalog_id);
                        changed_registries.insert(*root.registry_id);
                    }
                } else {
                    const auto companion = root.membership_entity_id.value_or(allocation.membership_entity_ids.contains(*root.registry_id) ?
                        allocation.membership_entity_ids.at(*root.registry_id) : std::string{});
                    if (!memberships.contains(companion)) memberships.emplace(companion, AssemblyPhaseMembership::create(*root.registry_id, {}, {}, {}));
                    memberships.at(companion) = replace_membership(memberships.at(companion), *root.alternative_id, old, proposed);
                    changed_memberships.insert(companion);
                }
            }
        }
        for (const auto& key : changed_registries) result.entities.at(key).properties.at("model") =
            retain_model_phase_source(source.at(key).properties.at("model"), registries.at(key));
        for (const auto& key : changed_memberships) {
            if (source.contains(key)) result.entities.at(key).properties.at("model") =
                retain_assembly_phase_membership_source(source.at(key).properties.at("model"), memberships.at(key));
            else require(result.entities.emplace(key, Entity{key, std::string(assembly_phase_membership_entity_type),
                Json{{"model", memberships.at(key).to_json()}}, true}).second, "fresh companion collides");
        }

        Budget final_raw; final_raw.map(result.entities);
        AssemblyCatalogTransferBudget final_work; admit_catalogs(result.entities, final_work);
        validate_embedded_assembly_drawing_contexts(result.entities);
        std::vector<std::pair<std::string, std::string>> context_sources, context_destinations;
        for (const auto& root : plan.isolation_roots) if (!root.root.document_entity_id) {
            const auto& destination = result.original_to_proposed.at(root.root);
            if (!source.at(root.root.catalog_id).properties.contains("embedded_instance_drawing_contexts") &&
                !result.entities.at(destination.catalog_id).properties.contains("embedded_instance_drawing_contexts")) continue;
            context_sources.emplace_back(root.root.catalog_id, root.root.instance_id);
            context_destinations.emplace_back(destination.catalog_id, destination.instance_id);
        }
        if (!context_sources.empty()) {
            const auto before = resolve_embedded_assembly_drawing_contexts(source, context_sources);
            const auto after = resolve_embedded_assembly_drawing_contexts(result.entities, context_destinations);
            for (std::size_t i = 0; i < context_sources.size(); ++i)
                require(before.at(context_sources[i]) == after.at(context_destinations[i]),
                    "private component changed its actual source drawing context");
        }
        const auto final_models = decode_models(result.entities);
        const Phases final_phases(result.entities);
        // Expand every actual root once under one shared bounded ledger, not
        // just roots selected for edits. Even inactive supporting rows count.
        std::map<Target, AssemblyExpansion> expansions;
        AssemblyExpansionBudget final_expansion;
        for (const auto& [key, model] : final_models) for (const auto& row : model.instances())
            expansions.emplace(Target{key, row.id, std::nullopt, {}}, model.expand(row, final_expansion));
        for (const auto& [key, owner] : result.entities) if (owner.type == "assembly_instance") {
            const auto value = decode_document_assembly_instance(owner);
            require(final_models.contains(value.assembly_catalog_id), "final independent catalog is missing");
            expansions.emplace(Target{value.assembly_catalog_id, value.instance.id, key, {}},
                final_models.at(value.assembly_catalog_id).expand(value.instance, final_expansion));
        }
        std::set<Target> expected_roots;
        for (const auto& impact : plan.source.roots) {
            const auto& destination = result.original_to_proposed.at(impact.root);
            const bool isolated = !(destination == impact.root);
            const auto* global_impact = global_impacts.contains(impact.root) ? global_impacts.at(impact.root) : nullptr;
            const auto& expected = isolated ? impact.after : global_impact ? global_impact->after : impact.before;
            require(expansions.contains(destination) && equal_expansion(expansions.at(destination), expected), "actual final expansion differs from bound source authority");
            expected_roots.insert(destination);
            equal_reached_definitions(final_models.at(destination.catalog_id), isolated ? after_models.at(impact.root.catalog_id) : global_models.at(impact.root.catalog_id),
                isolated ? impact.after_type_ids : global_impact ? global_impact->after_type_ids : impact.before_type_ids,
                isolated ? impact.after_material_ids : global_impact ? global_impact->after_material_ids : impact.before_material_ids);
            if (isolated && impact.cohort == AssemblyCatalogPhaseEditCohort::baseline_protected) {
                require(expansions.contains(impact.root) && equal_expansion(expansions.at(impact.root), global_impact ? global_impact->after : impact.before), "baseline original expansion changed");
                expected_roots.insert(impact.root);
                equal_reached_definitions(final_models.at(impact.root.catalog_id), before_models.at(impact.root.catalog_id), impact.before_type_ids, impact.before_material_ids);
            }
        }
        require(expansions.size() == expected_roots.size(), "final root inventory differs from actual source and typed allocations");
        for (const auto& root : plan.isolation_roots) require_saved_active(final_phases, root, result.original_to_proposed.at(root.root));
        // Account for every intended definition effect, independently of the
        // root expansion comparisons. Unused global rows count as real effects;
        // private effects count only genuine reached destination definitions.
        std::map<DefinitionKey, std::size_t> final_private_consumers;
        for (const auto& root : plan.isolation_roots) {
            const auto& destination = result.original_to_proposed.at(root.root);
            std::set<std::string, std::less<>> types, materials;
            for (const auto& node : expansions.at(destination).nodes) {
                types.insert(node.type_id);
                for (const auto& [slot, material] : node.materials) { (void)slot; materials.insert(material); }
            }
            for (const auto& key : types) ++final_private_consumers[{root.root.catalog_id, DefinitionKind::type, key}];
            for (const auto& key : materials) ++final_private_consumers[{root.root.catalog_id, DefinitionKind::material, key}];
        }
        for (const auto& effect : plan.definition_effects) {
            const DefinitionKey key{effect.catalog_id, effect.kind, effect.definition_id};
            const auto count = final_private_consumers.find(key);
            require((count == final_private_consumers.end() ? 0 : count->second) == effect.active_private_consumer_count,
                "actual private definition-consumer inventory differs from inspected scope");
            if (effect.disposition == Disposition::global || effect.disposition == Disposition::unchanged) {
                const auto& expected = effect.disposition == Disposition::global ? after_models.at(effect.catalog_id) : before_models.at(effect.catalog_id);
                const auto& actual = final_models.at(effect.catalog_id);
                require(effect.kind == DefinitionKind::type ?
                    definition(actual.types(), effect.definition_id) == definition(expected.types(), effect.definition_id) :
                    definition(actual.materials(), effect.definition_id) == definition(expected.materials(), effect.definition_id),
                    "actual global definition effect differs from the full requested typed replacement");
            } else require(effect.disposition == Disposition::private_only && effect.active_private_consumer_count != 0,
                "changed definition lacks a complete actual effect");
        }
        // Physical layers keep their actual source catalog references. Private
        // component copies cannot supply definition authority for those layers;
        // only the independently admitted global remainder may change them.
        std::set<std::pair<std::string, std::string>> physical_materials;
        std::set<std::pair<std::string, std::string>> protected_materials;
        for (const auto& consumer : plan.source.material_consumers) {
            if (physical_materials.emplace(consumer.catalog_id, consumer.material_id).second)
                require(exact(raw_row(result.entities.at(consumer.catalog_id).properties.at("model"), "materials", consumer.material_id),
                    raw_row(global_authority->at(consumer.catalog_id).properties.at("model"), "materials", consumer.material_id)),
                    "actual physical material differs from admitted global definition authority");
            if (consumer.cohort != AssemblyCatalogPhaseEditCohort::active_ordinary &&
                protected_materials.emplace(consumer.catalog_id, consumer.material_id).second)
                require(exact(raw_row(result.entities.at(consumer.catalog_id).properties.at("model"), "materials", consumer.material_id),
                    raw_row(source.at(consumer.catalog_id).properties.at("model"), "materials", consumer.material_id)),
                    "protected/inactive physical material definition changed during private component replacement");
        }
        // Existing envelopes are byte-form equivalent except the explicitly
        // derived roster/catalog references and actual registry/companion edits.
        for (const auto& [key, original] : *global_authority) {
            require(result.entities.contains(key), "an unrelated source owner disappeared");
            auto expected = original;
            if (row_changed_catalogs.contains(key)) {
                expected.properties.at("model").at("instances") = result.entities.at(key).properties.at("model").at("instances");
                expected = prune_embedded_assembly_drawing_contexts(expected);
            }
            if (changed_owners.contains(key)) expected.properties.at("assembly_catalog_id") = result.entities.at(key).properties.at("assembly_catalog_id");
            if (changed_registries.contains(key) || changed_memberships.contains(key)) expected.properties.at("model") = result.entities.at(key).properties.at("model");
            require(exact(expected, result.entities.at(key)), "an unrelated original envelope/definition/row changed");
        }
        const auto before_aliases = embedded_assembly_presentation_ids(source), after_aliases = embedded_assembly_presentation_ids(result.entities);
        for (const auto& [binding, alias] : before_aliases) if (after_aliases.contains(binding))
            require(after_aliases.at(binding) == alias, "private allocation changed a retained presentation alias");
        for (const auto& root : plan.isolation_roots) if (!root.root.document_entity_id) {
            const auto& destination = result.original_to_proposed.at(root.root);
            const auto& alias = after_aliases.at({destination.catalog_id, destination.instance_id});
            require(fresh.insert(alias).second, "fresh private presentation alias collides");
        }
        fresh_ids.assign(fresh.begin(), fresh.end());
        validate_phase_assembly_instance_property_edit_fresh_identities(source, fresh_ids);
        result.fresh_identity_ids = std::move(fresh_ids);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid phase catalog replay: ") + error.what());
    }
}
} // namespace sketch
