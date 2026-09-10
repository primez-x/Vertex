#pragma once
#include "sketch/workspace_history_record.hpp"
#include "sketch/recovery_copy_record.hpp"

namespace sketch {
enum class ArchiveRole { ordinary, recovery_copy };
struct RecoveryRecord {
    std::string record_id;
    std::string record_kind;
    nlohmann::json envelope;
};
using RecoveryLedger = std::vector<RecoveryRecord>;
struct DecodedRecoveryLedger {
    std::optional<WorkspaceHistoryRecord> history;
    std::optional<BoundaryActiveRecovery> active;
    std::optional<RecoveryCopyRecord> recovery_copy;
};
struct RecoveryLedgerDecodeResult {
    std::optional<DecodedRecoveryLedger> decoded;
    std::optional<RecoveryLedger> original_ledger;
    std::string diagnostic;
    [[nodiscard]] bool supported() const noexcept { return decoded.has_value(); }
    [[nodiscard]] bool opaque() const noexcept { return original_ledger.has_value() && !decoded; }
};
// Nonempty recovery ledgers only. Unknown kinds/versions and role mismatches
// retain the complete ledger without granting editable state. This validates
// borrowed values; disk schema, hashing and publication belong to ProjectStore.
[[nodiscard]] RecoveryLedgerDecodeResult decode_recovery_ledger(
    const DocumentSnapshot&, const RecoveryLedger&, ArchiveRole,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy,
    const WorkspaceRecoveryLimits& = {});
}
