#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sketch {

enum class AssistanceKind { tracing, dimension_extraction, label_placement, natural_language };

struct AssistanceResource {
    std::string id;
    std::string relative_path; // Portable package-relative path; empty when explicitly omitted.
    std::string provenance;
    std::string license;
    bool included{};
    bool operator==(const AssistanceResource&) const = default;
};

struct AssistanceSource {
    std::string reference_id;
    std::string original_text;
    // Normalized source rectangle, also suitable for a text selection viewport.
    double x{}, y{}, width{}, height{};
    double confidence{};
    bool operator==(const AssistanceSource&) const = default;
};

// A preview envelope, not an executable document command. Command-specific typing,
// permissions, entity lookup and undo remain the normal command system's responsibility.
struct AssistanceCommandPreview {
    std::string command_type;
    std::vector<std::string> affected_entity_ids;
    nlohmann::json arguments = nlohmann::json::object();
    bool operator==(const AssistanceCommandPreview&) const = default;
};

struct AssistanceProposal {
    std::string id;
    AssistanceKind kind{AssistanceKind::tracing};
    std::string producer;
    std::vector<AssistanceResource> resources;
    AssistanceSource source;
    AssistanceCommandPreview preview;
    bool operator==(const AssistanceProposal&) const = default;
};

void validate_assistance_proposal(const AssistanceProposal&);
[[nodiscard]] nlohmann::json encode_assistance_proposal(const AssistanceProposal&);
[[nodiscard]] AssistanceProposal decode_assistance_proposal(const nlohmann::json&);

// Returns diagnostics only; existence/integrity is established by the package loader.
[[nodiscard]] std::vector<std::string> missing_assistance_resources(
    const AssistanceProposal&, const std::vector<std::string>& available_resource_ids);

struct AssistanceCommandRequest {
    AssistanceProposal proposal; // Retained provenance; acceptance does not verify measurements.
    bool requires_normal_command_validation{true};
    bool requires_permission_check{true};
    bool requires_undo_transaction{true};
};

class AssistanceSession {
public:
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    // Explicit user acceptance only creates a request. This class has no document reference
    // and cannot change geometry, measurements or classifications.
    [[nodiscard]] AssistanceCommandRequest request_acceptance(
        const AssistanceProposal&, bool explicitly_accepted,
        const std::vector<std::string>& available_resource_ids) const;
private:
    bool enabled_{}; // Optional assistance is off until explicitly enabled.
};

} // namespace sketch
