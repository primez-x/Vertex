#pragma once
#include "sketch/workspace_recovery_budget.hpp"

namespace sketch {
// Version-one workspace_history record. Active input remains a separate archive
// record; immutable historical owners and retired references live here.
struct WorkspaceHistoryRecord {
    WorkspaceDocumentHistory document_history;
    std::vector<WorkspaceLifecycleEvent> events;
    WorkspaceNavigationState navigation;
    WorkspaceRetiredBoundaries retired;
    std::uint64_t workspace_epoch{}, edited_generation{}, checkpoint_generation{};
    nlohmann::json extensions = nlohmann::json::object();
};
struct WorkspaceHistoryDecodeResult {
    std::optional<WorkspaceHistoryRecord> record;
    std::optional<nlohmann::json> original_envelope;
    std::string diagnostic;
    [[nodiscard]] bool supported() const noexcept { return record.has_value(); }
    [[nodiscard]] bool opaque() const noexcept { return original_envelope.has_value() && !record; }
};
[[nodiscard]] WorkspaceHistoryRecord capture_workspace_history_record(const ProjectWorkspaceSnapshot&);
void validate_workspace_history_record(const DocumentSnapshot&, const WorkspaceHistoryRecord&,
    const std::optional<BoundaryActiveRecovery>& active,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy,
    const WorkspaceRecoveryLimits& = {});
[[nodiscard]] nlohmann::json encode_workspace_history_record(const DocumentSnapshot&, const WorkspaceHistoryRecord&,
    const std::optional<BoundaryActiveRecovery>& active,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy,
    const WorkspaceRecoveryLimits& = {});
// Bounded whole-envelope preflight precedes decoding. Unknown positive outer
// or nested input versions preserve the whole record opaquely. No load authority.
[[nodiscard]] WorkspaceHistoryDecodeResult decode_workspace_history_record(const DocumentSnapshot&, const nlohmann::json&,
    const std::optional<BoundaryActiveRecovery>& active,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy,
    const WorkspaceRecoveryLimits& = {});
}
