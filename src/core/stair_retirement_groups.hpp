#pragma once

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sketch::stair_retirement_detail {
// Offer discovery shares the individual/cohort preflights' aggregate source
// allowance. Exhaustion can only withhold additional offers.
struct WorkBudget {
    static constexpr std::size_t limit = 2000000;
    std::size_t used{};
    bool take(std::size_t count) {
        if (used > limit || count > limit - used) return false;
        used += count; return true;
    }
};
struct Capacity {};
using Json = nlohmann::json;
using Members = std::set<std::size_t>;
using Names = std::map<std::string, Members, std::less<>>;
inline const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key); return found == value.end() ? nullptr : &*found;
}
template<class Visit> void walk(const Json& value, WorkBudget& budget, Visit&& visit) {
    if (!budget.take(1)) throw Capacity{};
    visit(value);
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        if (!budget.take(1)) throw Capacity{};
        visit(Json(key)); walk(child, budget, visit);
    }
    else if (value.is_array()) for (const auto& child : value) walk(child, budget, visit);
}
inline Members named(const Json* value, const Names& names) {
    if (!value || !value->is_string()) return {};
    const auto found = names.find(value->get_ref<const std::string&>());
    return found == names.end() ? Members{} : found->second;
}

// Weak components deliberately over-group directed dependencies. Only the
// affected, caller-qualified rails are vertices; external blockers never gain
// retirement authority. The caller must preflight each complete component.
inline std::vector<std::vector<std::string>> groups(const AssemblyDocumentEntities& actual,
    const std::vector<std::string>& cohort, bool phase_proposals, WorkBudget& budget) {
    Names names;
    for (std::size_t i = 0; i < cohort.size(); ++i) names[cohort[i]].insert(i);
    const auto rail_names = names;
    // The removal producers compute this same lightweight, collision-aware
    // alias inventory. Reserve its source and inventory passes before calling.
    if (!budget.take(actual.size() * 3)) throw Capacity{};
    const auto aliases = embedded_assembly_presentation_ids(actual);
    if (!budget.take(aliases.size() * 2)) throw Capacity{};
    std::map<std::pair<std::string, std::string>, std::size_t> instances;
    for (const auto& [id, entity] : actual) {
        if (!budget.take(1)) throw Capacity{};
        if (entity.type != "assembly_model") continue;
        const auto model = field(entity.properties, "model");
        const auto rows = model ? field(*model, "instances") : nullptr;
        if (!rows || !rows->is_array()) continue;
        for (const auto& row : *rows) {
            if (!budget.take(1)) throw Capacity{};
            const auto placement = field(row, "placement");
            const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
            const auto owners = named(host, rail_names);
            if (owners.empty()) continue;
            // Before dimensions/overlays are indexed, these are solely actual
            // physical railing host IDs, exactly as in the removal producers.
            const auto key = std::pair{id, row.at("id").get<std::string>()};
            instances.emplace(key, *owners.begin());
            names[aliases.at(key)].insert(owners.begin(), owners.end());
        }
    }
    if (!phase_proposals) for (const auto& [id, entity] : actual) {
        if (!budget.take(1)) throw Capacity{};
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        bool affected{};
        const auto visit = [&](const Json& value) { affected = affected || !named(&value, names).empty(); };
        walk(entity.properties, budget, visit); walk(entity.extensions, budget, visit);
        if (!affected) continue;
        try {
            const auto decoded = decode_boundary_dimension_entity(entity);
            if (!decoded.supported()) continue; // Preflight retains the refusal.
            const Json owner(decoded.dimension->boundary_id);
            const auto owners = named(&owner, names);
            names[id].insert(owners.begin(), owners.end());
        } catch (const std::exception&) {
            // An independently blocked dimension must not suppress discovery
            // for other groups. Its actual affected preflight still refuses it.
        }
    }
    using OverlayKey = std::tuple<std::string, std::string, std::string>;
    std::map<OverlayKey, Members> overlays;
    Names overlay_names;
    for (const auto& [id, entity] : actual) {
        if (!budget.take(1)) throw Capacity{};
        if (entity.type != kSheetViewEntityType) continue;
        const auto model = field(entity.properties, "model");
        const auto views = model ? field(*model, "views") : nullptr;
        if (!views || !views->is_array()) continue;
        for (const auto& view : *views) {
            if (!budget.take(1)) throw Capacity{};
            const auto rows = field(view, "overlays");
            if (!rows || !rows->is_array()) continue;
            const auto view_id = field(view, "id");
            if (!view_id || !view_id->is_string()) continue;
            for (const auto& row : *rows) {
                if (!budget.take(1)) throw Capacity{};
                auto owners = named(field(row, "object_id"), names);
                const auto binding = field(row, "dimension_binding");
                const auto bound = named(binding ? field(*binding, "object_id") : nullptr, names);
                owners.insert(bound.begin(), bound.end());
                if (owners.empty()) continue;
                const auto local_id = field(row, "id");
                if (!local_id || !local_id->is_string()) continue;
                const auto local = local_id->get<std::string>();
                overlays[{id, view_id->get<std::string>(), local}] = owners;
                overlay_names[local].insert(owners.begin(), owners.end());
            }
        }
    }
    // Ordinary removal keeps overlay IDs qualified to their sheet/view;
    // proposed-rail retirement's final reference scan also checks their raw
    // spellings. Match the actual producers' different namespaces.
    if (phase_proposals) for (const auto& [name, owners] : overlay_names)
        names[name].insert(owners.begin(), owners.end());
    std::vector<std::size_t> roots(cohort.size());
    for (std::size_t i = 0; i < roots.size(); ++i) roots[i] = i;
    const auto root = [&](std::size_t i) { while (roots[i] != i) i = roots[i]; return i; };
    for (std::size_t i = 0; i < cohort.size(); ++i) {
        const auto link = [&](const Members& owners) {
            for (const auto owner : owners) {
                if (!budget.take(1)) throw Capacity{};
                roots[root(owner)] = root(i);
            }
        };
        const auto visit = [&](const Json& value) {
            link(named(&value, names));
            if (!value.is_object()) return;
            const auto local = field(value, "instance_id");
            if (local && local->is_string()) for (const auto* key : {"catalog_id", "assembly_catalog_id"}) {
                const auto catalog = field(value, key);
                if (!catalog || !catalog->is_string()) continue;
                const auto found = instances.find({catalog->get<std::string>(), local->get<std::string>()});
                if (found != instances.end()) link(Members{found->second});
            }
            if (phase_proposals) return;
            const auto view = field(value, "view_id"), overlay = field(value, "overlay_id");
            if (!view || !view->is_string() || !overlay || !overlay->is_string()) return;
            std::string owner = cohort[i];
            for (const auto* key : {"sheet_view_entity_id", "sheet_view_id", "entity_id"}) {
                const auto explicit_owner = field(value, key);
                if (explicit_owner && explicit_owner->is_string()) { owner = explicit_owner->get<std::string>(); break; }
            }
            const auto found = overlays.find({owner, view->get<std::string>(), overlay->get<std::string>()});
            if (found != overlays.end()) link(found->second);
        };
        const auto& rail = actual.at(cohort[i]);
        walk(rail.properties, budget, visit); walk(rail.extensions, budget, visit);
    }
    std::map<std::size_t, std::vector<std::string>> partitions;
    for (std::size_t i = 0; i < cohort.size(); ++i) partitions[root(i)].push_back(cohort[i]);
    std::vector<std::vector<std::string>> result;
    for (auto& [key, group] : partitions) { (void)key; result.push_back(std::move(group)); }
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace sketch::stair_retirement_detail
