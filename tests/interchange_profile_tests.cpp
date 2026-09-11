#include "sketch/interchange_profile.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
sketch::InterchangeProfileAttestation attest(const sketch::InterchangeProfile& p) {
    return {p.adapter_id, p.adapter_version, p.license_review_id, p.module_allowlist,
        p.required_resources, true, true, true, true, true, true, true, true};
}
void run() {
    using namespace sketch;
    for (const auto format : {InterchangeFormat::ifc, InterchangeFormat::dxf, InterchangeFormat::pdf, InterchangeFormat::proj}) {
        auto p = declared_interchange_profile(format);
        check(!validate_interchange_profile(p, {}).allowed, "declaration claimed a ready runtime");
        p.adapter_version = "1.0.0";
        p.license_expression = "LicenseRef-test-only";
        p.license_review_id = "review-test-only";
        if (p.module_allowlist.empty()) p.module_allowlist = {"test-adapter"};
        auto a = attest(p);
        check(validate_interchange_profile(p, a).allowed, "valid attested profile rejected");
        for (auto field : {&InterchangeProfileAttestation::license_review_verified,
            &InterchangeProfileAttestation::offline_build_verified, &InterchangeProfileAttestation::network_denial_verified,
            &InterchangeProfileAttestation::isolated_worker_verified, &InterchangeProfileAttestation::limits_enforced,
            &InterchangeProfileAttestation::failure_preserves_project_verified}) {
            a.*field = false;
            check(!validate_interchange_profile(p, a).allowed, "missing runtime control accepted");
            a.*field = true;
        }
        a.adapter_version = "wrong";
        check(!validate_interchange_profile(p, a).allowed, "wrong adapter version accepted");
        a = attest(p);
        a.license_review_id = "wrong";
        check(!validate_interchange_profile(p, a).allowed, "wrong license review accepted");
        a = attest(p);
        a.loaded_modules.push_back("unreviewed");
        check(!validate_interchange_profile(p, a).allowed, "unreviewed module accepted");
        a = attest(p);
        p.required_resources.push_back("local-grid.tif");
        check(!validate_interchange_profile(p, a).allowed, "missing grid accepted");
        a = attest(p);
        check(validate_interchange_profile(p, a).allowed, "verified grid rejected");
        p.required_resources.push_back("../escape");
        a = attest(p);
        check(!validate_interchange_profile(p, a).allowed, "resource path accepted as logical id");
        p.required_resources.pop_back();
        a = attest(p);
        const auto json = p.to_json().dump();
        std::reverse(p.capabilities.begin(), p.capabilities.end());
        std::reverse(p.required_resources.begin(), p.required_resources.end());
        check(p.to_json().dump() == json, "manifest serialization order unstable");
        check(nlohmann::json::parse(json).at("format_target") == p.format_target, "manifest JSON round trip failed");
        p.capabilities.push_back("editable-extraction");
        check(!validate_interchange_profile(p, a).allowed, "unsupported extraction claim accepted");
        p.capabilities.pop_back();
        p.timeout_ms = 0;
        const auto first = validate_interchange_profile(p, a).to_json().dump();
        check(!validate_interchange_profile(p, a).allowed, "unbounded timeout accepted");
        check(first == validate_interchange_profile(p, a).to_json().dump(), "decision JSON unstable");
        p.timeout_ms = 30'001;
        check(!validate_interchange_profile(p, a).allowed, "oversized timeout accepted");
        p.timeout_ms = 30'000;
        p.max_output_bytes = 0;
        check(!validate_interchange_profile(p, a).allowed, "unbounded output accepted");
        p.max_output_bytes = 256ULL * 1024 * 1024;
        p.unsupported_entities = static_cast<UnsupportedEntityPolicy>(999);
        check(!validate_interchange_profile(p, a).allowed, "invalid entity policy accepted");
    }
    auto proj = declared_interchange_profile(InterchangeFormat::proj);
    proj.adapter_version = "1"; proj.license_expression = "test"; proj.license_review_id = "test";
    proj.module_allowlist = {"proj"};
    auto a = attest(proj);
    a.proj_network_callbacks_disabled = false;
    check(!validate_interchange_profile(proj, a).allowed, "PROJ remote callbacks accepted");
    a = attest(proj); a.proj_network_disabled = false;
    check(!validate_interchange_profile(proj, a).allowed, "PROJ network accepted");
    proj.required_resources.clear(); a = attest(proj);
    check(!validate_interchange_profile(proj, a).allowed, "PROJ baseline database omitted");
    proj.format = static_cast<InterchangeFormat>(999);
    check(!validate_interchange_profile(proj, a).allowed, "invalid format accepted");
    bool threw = false;
    try { (void)declared_interchange_profile(proj.format); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "invalid baseline format accepted");
}
}
int main() {
    try { run(); std::cout << "interchange profile tests passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
