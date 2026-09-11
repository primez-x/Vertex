#include "sketch/offline_policy.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
RuntimeCapabilities independent() {
    RuntimeCapabilities value;
    value.account = value.activation = value.subscription = value.network = RuntimeRequirement::not_required;
    return value;
}
void run() {
    require(!evaluate_offline_policy({}).startup_allowed, "unknown runtime admitted");
    require(evaluate_offline_policy({}).diagnostics.size() == 4, "missing unknown diagnostics");
    auto runtime = independent();
    require(evaluate_offline_policy(runtime).startup_allowed, "independent runtime blocked");
    const std::array members{&RuntimeCapabilities::account, &RuntimeCapabilities::activation,
        &RuntimeCapabilities::subscription, &RuntimeCapabilities::network};
    for (auto member : members) {
        for (auto state : {RuntimeRequirement::unknown, RuntimeRequirement::required,
                           static_cast<RuntimeRequirement>(99)}) {
            auto changed = runtime;
            changed.*member = state;
            auto report = evaluate_offline_policy(changed);
            require(!report.startup_allowed && report.diagnostics.size() == 1, "dependency did not fail closed");
        }
    }
    const std::array flags{&OfflinePolicy::require_no_account, &OfflinePolicy::require_no_activation,
        &OfflinePolicy::require_no_subscription, &OfflinePolicy::require_no_network};
    for (auto flag : flags) {
        OfflinePolicy policy;
        policy.*flag = false;
        auto report = evaluate_offline_policy(runtime, policy);
        require(!report.startup_allowed && report.diagnostics.front().code == "policy_weakened", "weak policy admitted");
    }
    runtime.optional_local_resources = {{"cad-engine", LocalResourceStatus::available},
        {"local-ai", LocalResourceStatus::unavailable}, {"material-library", LocalResourceStatus::unknown}};
    const auto optional = evaluate_offline_policy(runtime);
    require(optional.startup_allowed && optional.diagnostics.size() == 2, "optional resources blocked startup");
    const auto json = offline_policy_json(runtime);
    require(json == nlohmann::json::parse(json.dump()), "JSON envelope does not roundtrip");
    require(json.dump() == offline_policy_json(runtime).dump(), "JSON output not deterministic");
    require(json.at("schema_version") == 1 && json.at("startup_allowed") == true, "JSON verdict incorrect");
    require(json.at("requirements").at("network") == "not_required", "JSON declaration lost");
    runtime.optional_local_resources.push_back({"local-ai", LocalResourceStatus::available});
    require(!evaluate_offline_policy(runtime).startup_allowed, "duplicate resource id admitted");
    runtime.optional_local_resources = {{"secret/path", LocalResourceStatus::available}};
    require(!evaluate_offline_policy(runtime).startup_allowed, "invalid resource id admitted");
    require(offline_policy_json(runtime).dump().find("secret/path") == std::string::npos, "invalid identifier leaked");
    runtime.optional_local_resources = {{"valid", static_cast<LocalResourceStatus>(99)}};
    require(!evaluate_offline_policy(runtime).startup_allowed, "invalid resource status admitted");
}
}
int main() {
    try { run(); std::cout << "Offline policy tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
