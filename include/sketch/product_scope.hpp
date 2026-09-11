#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace sketch {

enum class ScopeUnitSystem { imperial, metric };
enum class ScopeMarket { residential, light_commercial };
enum class ScopeWorkspace { measurement, architectural };

[[nodiscard]] std::string scope_unit_name(ScopeUnitSystem value);
[[nodiscard]] std::string scope_market_name(ScopeMarket value);
[[nodiscard]] std::string scope_workspace_name(ScopeWorkspace value);

// Immutable product boundary declaration. This is a runtime scope contract,
// not evidence that every listed workflow has passed production qualification.
struct ProductScopeProfile {
    std::string platform;
    std::string interface_language;
    std::vector<ScopeUnitSystem> units;
    std::vector<ScopeMarket> markets;
    std::vector<ScopeWorkspace> workspaces;
    bool offline_core_required{true};
    bool account_required{false};
    bool activation_server_required{false};
    bool subscription_required{false};

    [[nodiscard]] static ProductScopeProfile production_scope();
    void validate() const;
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static ProductScopeProfile from_json(const nlohmann::json& value);
};

}  // namespace sketch
