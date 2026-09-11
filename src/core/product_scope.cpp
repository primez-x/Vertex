#include "sketch/product_scope.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
void invalid(const char* message) { throw std::invalid_argument(message); }

void exact_keys(const nlohmann::json& value, const std::set<std::string>& keys) {
    if (!value.is_object() || value.size() != keys.size()) invalid("invalid product scope JSON fields");
    for (const auto& key : keys) {
        if (!value.contains(key)) throw std::invalid_argument("missing product scope field: " + key);
    }
}

template <typename T, typename Name>
void validate_names(const std::vector<T>& values, Name name, const char* description) {
    if (values.empty()) throw std::invalid_argument(std::string(description) + " cannot be empty");
    std::vector<std::string> names;
    names.reserve(values.size());
    for (const auto value : values) names.push_back(name(value));
    std::sort(names.begin(), names.end());
    if (std::adjacent_find(names.begin(), names.end()) != names.end())
        throw std::invalid_argument(std::string("duplicate ") + description);
}

template <typename T>
bool has_value(const std::vector<T>& values, T expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

ScopeUnitSystem parse_unit(const nlohmann::json& value) {
    if (!value.is_string()) invalid("product scope unit must be a string");
    const auto name = value.get<std::string>();
    if (name == "imperial") return ScopeUnitSystem::imperial;
    if (name == "metric") return ScopeUnitSystem::metric;
    invalid("unsupported product scope unit");
    return ScopeUnitSystem::imperial;
}

ScopeMarket parse_market(const nlohmann::json& value) {
    if (!value.is_string()) invalid("product scope market must be a string");
    const auto name = value.get<std::string>();
    if (name == "residential") return ScopeMarket::residential;
    if (name == "light_commercial") return ScopeMarket::light_commercial;
    invalid("unsupported product scope market");
    return ScopeMarket::residential;
}

ScopeWorkspace parse_workspace(const nlohmann::json& value) {
    if (!value.is_string()) invalid("product scope workspace must be a string");
    const auto name = value.get<std::string>();
    if (name == "measurement") return ScopeWorkspace::measurement;
    if (name == "architectural") return ScopeWorkspace::architectural;
    invalid("unsupported product scope workspace");
    return ScopeWorkspace::measurement;
}

template <typename T, typename Parse>
std::vector<T> parse_array(const nlohmann::json& value, Parse parse, const char* description) {
    if (!value.is_array()) throw std::invalid_argument(std::string("product scope ") + description + " must be an array");
    std::vector<T> result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(parse(item));
    return result;
}
}  // namespace

std::string scope_unit_name(ScopeUnitSystem value) {
    switch (value) {
    case ScopeUnitSystem::imperial: return "imperial";
    case ScopeUnitSystem::metric: return "metric";
    }
    invalid("unknown product scope unit");
    return {};
}

std::string scope_market_name(ScopeMarket value) {
    switch (value) {
    case ScopeMarket::residential: return "residential";
    case ScopeMarket::light_commercial: return "light_commercial";
    }
    invalid("unknown product scope market");
    return {};
}

std::string scope_workspace_name(ScopeWorkspace value) {
    switch (value) {
    case ScopeWorkspace::measurement: return "measurement";
    case ScopeWorkspace::architectural: return "architectural";
    }
    invalid("unknown product scope workspace");
    return {};
}

ProductScopeProfile ProductScopeProfile::production_scope() {
    return {"Windows 11 x64", "en", {ScopeUnitSystem::imperial, ScopeUnitSystem::metric},
            {ScopeMarket::residential, ScopeMarket::light_commercial},
            {ScopeWorkspace::measurement, ScopeWorkspace::architectural}, true, false, false, false};
}

void ProductScopeProfile::validate() const {
    if (platform != "Windows 11 x64") invalid("product scope platform must be Windows 11 x64");
    if (interface_language.empty() || interface_language.size() > 16)
        invalid("product scope interface language is invalid");
    validate_names(units, scope_unit_name, "unit profiles");
    validate_names(markets, scope_market_name, "markets");
    validate_names(workspaces, scope_workspace_name, "workspaces");
    if (!has_value(units, ScopeUnitSystem::imperial) || !has_value(units, ScopeUnitSystem::metric))
        invalid("production scope requires imperial and metric units");
    if (!has_value(markets, ScopeMarket::residential) || !has_value(markets, ScopeMarket::light_commercial))
        invalid("production scope requires residential and light-commercial markets");
    if (!has_value(workspaces, ScopeWorkspace::measurement) || !has_value(workspaces, ScopeWorkspace::architectural))
        invalid("production scope requires measurement and architectural workspaces");
    if (!offline_core_required || account_required || activation_server_required || subscription_required)
        invalid("production scope cannot require account, activation, subscription, or network access");
}

nlohmann::json ProductScopeProfile::to_json() const {
    validate();
    nlohmann::json unit_values = nlohmann::json::array();
    for (const auto value : units) unit_values.push_back(scope_unit_name(value));
    nlohmann::json market_values = nlohmann::json::array();
    for (const auto value : markets) market_values.push_back(scope_market_name(value));
    nlohmann::json workspace_values = nlohmann::json::array();
    for (const auto value : workspaces) workspace_values.push_back(scope_workspace_name(value));
    return {{"schema", "sketch.product_scope"}, {"version", 1}, {"platform", platform},
            {"interface_language", interface_language}, {"units", unit_values},
            {"markets", market_values}, {"workspaces", workspace_values},
            {"offline_core_required", offline_core_required}, {"account_required", account_required},
            {"activation_server_required", activation_server_required},
            {"subscription_required", subscription_required}};
}

ProductScopeProfile ProductScopeProfile::from_json(const nlohmann::json& value) {
    try {
        exact_keys(value, {"schema", "version", "platform", "interface_language", "units", "markets",
                           "workspaces", "offline_core_required", "account_required",
                           "activation_server_required", "subscription_required"});
        if (value.at("schema") != "sketch.product_scope" || value.at("version") != 1 ||
            !value.at("version").is_number_integer()) invalid("unsupported product scope schema");
        ProductScopeProfile result;
        result.platform = value.at("platform").get<std::string>();
        result.interface_language = value.at("interface_language").get<std::string>();
        result.units = parse_array<ScopeUnitSystem>(value.at("units"), parse_unit, "units");
        result.markets = parse_array<ScopeMarket>(value.at("markets"), parse_market, "markets");
        result.workspaces = parse_array<ScopeWorkspace>(value.at("workspaces"), parse_workspace, "workspaces");
        result.offline_core_required = value.at("offline_core_required").get<bool>();
        result.account_required = value.at("account_required").get<bool>();
        result.activation_server_required = value.at("activation_server_required").get<bool>();
        result.subscription_required = value.at("subscription_required").get<bool>();
        result.validate();
        return result;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid product scope JSON: ") + error.what());
    }
}

}  // namespace sketch
