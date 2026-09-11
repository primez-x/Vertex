#include "sketch/interchange_profile.hpp"

#include <algorithm>
#include <stdexcept>

namespace sketch {
namespace {
const char* format_name(InterchangeFormat format) {
    switch (format) {
    case InterchangeFormat::ifc: return "ifc";
    case InterchangeFormat::dxf: return "dxf";
    case InterchangeFormat::pdf: return "pdf";
    case InterchangeFormat::proj: return "proj";
    }
    return "invalid";
}
const char* policy_name(UnsupportedEntityPolicy policy) {
    switch (policy) {
    case UnsupportedEntityPolicy::reject: return "reject";
    case UnsupportedEntityPolicy::preserve_reference_and_report: return "preserve_reference_and_report";
    }
    return "invalid";
}
bool text(const std::string& value) {
    return !value.empty() && value.size() <= 256 &&
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c > 32; }) &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool identifier(const std::string& value) {
    return text(value) && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    }) && value != "." && value != "..";
}
std::vector<std::string> sorted(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    return values;
}
bool valid_list(const std::vector<std::string>& values, bool allow_empty = false) {
    if ((!allow_empty && values.empty()) || values.size() > 256) return false;
    const auto ordered = sorted(values);
    return std::all_of(values.begin(), values.end(), identifier) &&
        std::adjacent_find(ordered.begin(), ordered.end()) == ordered.end();
}
bool contains_all(const std::vector<std::string>& offered, const std::vector<std::string>& required) {
    return std::all_of(required.begin(), required.end(), [&](const auto& item) {
        return std::find(offered.begin(), offered.end(), item) != offered.end();
    });
}
}

InterchangeProfile declared_interchange_profile(InterchangeFormat format) {
    InterchangeProfile result;
    result.format = format;
    switch (format) {
    case InterchangeFormat::ifc:
        result.adapter_id = "local.ifc.worker";
        result.format_target = "IFC4 ADD2 TC1 Reference View 1.2";
        result.capabilities = {"geometry", "types", "properties", "materials", "relationships", "reference-preservation", "fidelity-report"};
        break;
    case InterchangeFormat::dxf:
        result.adapter_id = "local.dxf.worker";
        result.format_target = "DXF R2013";
        result.capabilities = {"LINE", "ARC", "LWPOLYLINE", "POLYLINE", "TEXT", "MTEXT", "DIMENSION", "HATCH", "BLOCK", "INSERT", "fidelity-report"};
        break;
    case InterchangeFormat::pdf:
        result.adapter_id = "local.qt-pdf.worker";
        result.format_target = "PDF calibrated reference and vector scene output";
        result.capabilities = {"calibrated-reference", "source-provenance", "traceable", "image-only", "vector-scene-output", "fidelity-report"};
        result.module_allowlist = {"Qt6Core", "Qt6Gui", "Qt6Pdf", "Qt6PrintSupport"};
        break;
    case InterchangeFormat::proj:
        result.adapter_id = "local.proj.worker";
        result.format_target = "PROJ bundled-resource coordinate operations";
        result.capabilities = {"bundled-resource-transform", "no-network", "no-network-callbacks"};
        result.required_resources = {"proj.db"};
        result.unsupported_entities = UnsupportedEntityPolicy::reject;
        break;
    default: throw std::invalid_argument("invalid interchange format");
    }
    return result;
}

nlohmann::json InterchangeProfile::to_json() const {
    return {{"schema_version", 1}, {"format", format_name(format)}, {"adapter_id", adapter_id},
        {"adapter_version", adapter_version}, {"format_target", format_target},
        {"capabilities", sorted(capabilities)}, {"module_allowlist", sorted(module_allowlist)},
        {"required_resources", sorted(required_resources)}, {"license_expression", license_expression},
        {"license_review_id", license_review_id}, {"unsupported_entities", policy_name(unsupported_entities)},
        {"max_input_bytes", max_input_bytes}, {"max_output_bytes", max_output_bytes},
        {"timeout_ms", timeout_ms}, {"hosted_service_required", false}};
}

nlohmann::json InterchangeProfileDecision::to_json() const {
    return {{"schema_version", 1}, {"allowed", allowed}, {"diagnostics", sorted(diagnostics)},
        {"network_requests_permitted", false}};
}

InterchangeProfileDecision validate_interchange_profile(
    const InterchangeProfile& profile, const InterchangeProfileAttestation& attestation) {
    InterchangeProfileDecision result;
    auto fail = [&](const char* code) { result.diagnostics.emplace_back(code); };
    if (std::string(format_name(profile.format)) == "invalid") {
        fail("invalid_format");
    } else {
        const auto baseline = declared_interchange_profile(profile.format);
        if (profile.format_target != baseline.format_target) fail("unsupported_format_target");
        // This version advertises only the reviewed subset, never arbitrary extras.
        if (sorted(profile.capabilities) != sorted(baseline.capabilities)) fail("unsupported_capability_subset");
        if (!contains_all(profile.required_resources, baseline.required_resources)) fail("missing_baseline_resource");
        if (profile.format == InterchangeFormat::pdf &&
            sorted(profile.module_allowlist) != sorted(baseline.module_allowlist)) fail("unsupported_pdf_module_allowlist");
        if (profile.format == InterchangeFormat::proj &&
            (!attestation.proj_network_disabled || !attestation.proj_network_callbacks_disabled)) fail("proj_offline_unverified");
        if (profile.format == InterchangeFormat::proj && profile.unsupported_entities != UnsupportedEntityPolicy::reject)
            fail("unsupported_proj_fallback");
    }
    if (!identifier(profile.adapter_id) || !identifier(profile.adapter_version) ||
        !text(profile.license_expression) || !identifier(profile.license_review_id) ||
        !valid_list(profile.capabilities) || !valid_list(profile.module_allowlist) ||
        !valid_list(profile.required_resources, true)) fail("invalid_manifest");
    if (std::string(policy_name(profile.unsupported_entities)) == "invalid") fail("invalid_unsupported_entity_policy");
    if (profile.max_input_bytes == 0 || profile.max_input_bytes > 64ULL * 1024 * 1024 ||
        profile.max_output_bytes == 0 || profile.max_output_bytes > 256ULL * 1024 * 1024 ||
        profile.timeout_ms == 0 || profile.timeout_ms > 30'000) fail("invalid_resource_limits");
    if (attestation.adapter_id != profile.adapter_id || attestation.adapter_version != profile.adapter_version)
        fail("adapter_identity_mismatch");
    if (!attestation.license_review_verified || attestation.license_review_id != profile.license_review_id)
        fail("license_review_unverified");
    if (!attestation.offline_build_verified) fail("offline_build_unverified");
    if (!attestation.network_denial_verified) fail("network_denial_unverified");
    if (!attestation.isolated_worker_verified) fail("worker_isolation_unverified");
    if (!attestation.limits_enforced) fail("resource_limits_unverified");
    if (!attestation.failure_preserves_project_verified) fail("failure_preservation_unverified");
    if (!valid_list(attestation.loaded_modules) ||
        sorted(attestation.loaded_modules) != sorted(profile.module_allowlist)) fail("module_attestation_mismatch");
    if (!valid_list(attestation.verified_local_resources, true) ||
        !contains_all(attestation.verified_local_resources, profile.required_resources)) fail("local_resources_unverified");
    result.diagnostics = sorted(result.diagnostics);
    result.allowed = result.diagnostics.empty();
    return result;
}

} // namespace sketch
