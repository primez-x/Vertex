#include "sketch/model_phases.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
void check_id(const std::string& id) {
    if (id.empty() || std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }))
        throw std::invalid_argument("phase entity and alternative IDs must not be blank");
}
void canonical_ids(std::vector<std::string>& ids) {
    for (const auto& id : ids) check_id(id);
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        throw std::invalid_argument("duplicate phase entity ID");
}
void exact_keys(const nlohmann::json& value, const std::set<std::string>& keys) {
    if (!value.is_object() || value.size() != keys.size())
        throw std::invalid_argument("invalid phases JSON fields");
    for (const auto& key : keys)
        if (!value.contains(key)) throw std::invalid_argument("missing phases JSON field: " + key);
}
}  // namespace

std::string phase_name(ModelPhase phase) {
    switch (phase) {
    case ModelPhase::existing: return "existing";
    case ModelPhase::demolished: return "demolished";
    case ModelPhase::proposed: return "proposed";
    }
    throw std::invalid_argument("unknown model phase");
}

ModelPhases ModelPhases::create(std::vector<std::string> model_entity_ids,
                               std::vector<std::string> baseline_ids,
                               std::vector<RemodelingAlternative> alternatives,
                               std::optional<std::string> active_alternative) {
    canonical_ids(model_entity_ids);
    canonical_ids(baseline_ids);
    const std::set<std::string> known(model_entity_ids.begin(), model_entity_ids.end());
    const std::set<std::string> baseline(baseline_ids.begin(), baseline_ids.end());
    std::set<std::string> assigned = baseline;
    for (const auto& id : baseline)
        if (!known.contains(id)) throw std::invalid_argument("baseline references unknown entity: " + id);
    std::set<std::string> alternative_ids;
    for (auto& alternative : alternatives) {
        check_id(alternative.id);
        if (!alternative_ids.insert(alternative.id).second)
            throw std::invalid_argument("duplicate alternative ID: " + alternative.id);
        canonical_ids(alternative.demolished_ids);
        canonical_ids(alternative.proposed_ids);
        for (const auto& id : alternative.demolished_ids)
            if (!baseline.contains(id))
                throw std::invalid_argument("demolition must reference shared baseline: " + id);
        for (const auto& id : alternative.proposed_ids) {
            if (!known.contains(id)) throw std::invalid_argument("proposal references unknown entity: " + id);
            if (!assigned.insert(id).second)
                throw std::invalid_argument("proposal conflicts with baseline or another alternative: " + id);
        }
    }
    if (assigned != known) throw std::invalid_argument("every model entity must have phase membership");
    std::sort(alternatives.begin(), alternatives.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    ModelPhases result;
    result.entity_ids_ = std::move(model_entity_ids);
    result.baseline_ids_ = std::move(baseline_ids);
    result.alternatives_ = std::move(alternatives);
    result.validate_selection(active_alternative);
    result.active_ = std::move(active_alternative);
    return result;
}

void ModelPhases::validate_selection(const std::optional<std::string>& alternative) const {
    if (alternative && std::none_of(alternatives_.begin(), alternatives_.end(),
        [&](const auto& candidate) { return candidate.id == *alternative; }))
        throw std::invalid_argument("unknown active or comparison alternative: " + *alternative);
}

ModelPhases ModelPhases::with_active(std::optional<std::string> alternative) const {
    validate_selection(alternative);
    auto result = *this;
    result.active_ = std::move(alternative);
    return result;
}

ModelPhases ModelPhases::with_alternative(RemodelingAlternative alternative) const {
    auto alternatives = alternatives_;
    alternatives.push_back(std::move(alternative));
    return create(entity_ids_, baseline_ids_, std::move(alternatives), active_);
}

const std::vector<std::string>& ModelPhases::entity_ids() const noexcept { return entity_ids_; }

const std::vector<std::string>& ModelPhases::baseline_ids() const noexcept { return baseline_ids_; }

const std::vector<RemodelingAlternative>& ModelPhases::alternatives() const noexcept {
    return alternatives_;
}

const std::optional<std::string>& ModelPhases::active_alternative() const noexcept { return active_; }

std::map<std::string, ModelPhase, std::less<>> ModelPhases::state(
    const std::optional<std::string>& alternative) const {
    validate_selection(alternative);
    std::map<std::string, ModelPhase, std::less<>> result;
    for (const auto& id : baseline_ids_) result.emplace(id, ModelPhase::existing);
    if (alternative) {
        const auto found = std::find_if(alternatives_.begin(), alternatives_.end(),
            [&](const auto& candidate) { return candidate.id == *alternative; });
        for (const auto& id : found->demolished_ids) result.at(id) = ModelPhase::demolished;
        for (const auto& id : found->proposed_ids) result.emplace(id, ModelPhase::proposed);
    }
    return result;
}

std::map<std::string, ModelPhase, std::less<>> ModelPhases::active_state() const { return state(active_); }

PhaseComparison ModelPhases::compare(std::optional<std::string> left,
                                     std::optional<std::string> right) const {
    const auto left_state = state(left);
    const auto right_state = state(right);
    PhaseComparison result{std::move(left), std::move(right), {}};
    for (const auto& id : entity_ids_) {
        const auto l = left_state.find(id);
        const auto r = right_state.find(id);
        const std::optional<ModelPhase> lp = l == left_state.end() ? std::nullopt : std::optional(l->second);
        const std::optional<ModelPhase> rp = r == right_state.end() ? std::nullopt : std::optional(r->second);
        if (lp != rp) result.differences.push_back({id, lp, rp});
    }
    return result;
}

nlohmann::json ModelPhases::to_json() const {
    auto alternatives = nlohmann::json::array();
    for (const auto& value : alternatives_)
        alternatives.push_back({{"id", value.id}, {"name", value.name},
            {"demolished_ids", value.demolished_ids}, {"proposed_ids", value.proposed_ids}});
    return {{"schema", "sketch.model_phases"}, {"version", 1}, {"entity_ids", entity_ids_},
            {"baseline_ids", baseline_ids_}, {"alternatives", alternatives},
            {"active_alternative", active_ ? nlohmann::json(*active_) : nlohmann::json(nullptr)}};
}

ModelPhases ModelPhases::from_json(const nlohmann::json& value) {
    try {
        exact_keys(value, {"schema", "version", "entity_ids", "baseline_ids", "alternatives", "active_alternative"});
        if (value.at("schema") != "sketch.model_phases" || !value.at("version").is_number_integer() ||
            value.at("version") != 1 || !value.at("alternatives").is_array())
            throw std::invalid_argument("unsupported model phases schema");
        std::vector<RemodelingAlternative> alternatives;
        for (const auto& entry : value.at("alternatives")) {
            exact_keys(entry, {"id", "name", "demolished_ids", "proposed_ids"});
            alternatives.push_back({entry.at("id").get<std::string>(), entry.at("name").get<std::string>(),
                entry.at("demolished_ids").get<std::vector<std::string>>(),
                entry.at("proposed_ids").get<std::vector<std::string>>()});
        }
        std::optional<std::string> active;
        if (!value.at("active_alternative").is_null()) active = value.at("active_alternative").get<std::string>();
        return create(value.at("entity_ids").get<std::vector<std::string>>(),
            value.at("baseline_ids").get<std::vector<std::string>>(), std::move(alternatives), std::move(active));
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid model phases JSON: ") + error.what());
    }
}

}  // namespace sketch
