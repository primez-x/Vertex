#include "sketch/project_workspace.hpp"

#include "sketch/project_store.hpp"
#include "sketch/recovery_ledger.hpp"
#include "sketch/workspace_history_record.hpp"
#include "sketch/workspace_recovery_budget.hpp"

#include <stdexcept>

namespace sketch {

std::unique_ptr<ProjectWorkspace> ProjectWorkspace::restore_archive(
    const ProjectArchiveSnapshot& archive, const DecodedRecoveryLedger& recovery) {
    // A public typed aggregate is not proof of supported decoding or of its
    // association with this archive. Recheck the enclosing role and wire data.
    if (!recovery.history)
        throw std::invalid_argument("workspace restoration requires history");
    const auto checked = decode_recovery_ledger(archive.document(), archive.recovery(), archive.role());
    if (!checked.supported() || !checked.decoded->history)
        throw std::invalid_argument("workspace restoration requires a supported recovery ledger");
    const auto& expected = *checked.decoded;
    const auto& history = *recovery.history;
    validate_workspace_recovery(archive.document(), history.document_history, history.events,
        history.navigation, recovery.active, history.retired);
    if (recovery.active.has_value() != expected.active.has_value() ||
        recovery.recovery_copy.has_value() != expected.recovery_copy.has_value() ||
        encode_workspace_history_record(archive.document(), history, recovery.active).dump() !=
            encode_workspace_history_record(archive.document(), *expected.history, expected.active).dump() ||
        (recovery.active && encode_boundary_active_recovery(*recovery.active).dump() !=
            encode_boundary_active_recovery(*expected.active).dump()) ||
        (recovery.recovery_copy && encode_recovery_copy_record(*recovery.recovery_copy).dump() !=
            encode_recovery_copy_record(*expected.recovery_copy).dump()))
        throw std::invalid_argument("decoded recovery does not match archive");

    // Install the independently decoded aggregate. The caller-supplied
    // aggregate may contain shared_ptr aliases to mutable recovery payloads;
    // retaining it would let external mutation bypass workspace generations.
    const auto& decoded_history = *expected.history;
    return restore_components(archive.document(), decoded_history.document_history,
        expected.active, decoded_history.navigation, decoded_history.events,
        decoded_history.retired, decoded_history.extensions, decoded_history.workspace_epoch,
        decoded_history.edited_generation, decoded_history.checkpoint_generation);
}

}  // namespace sketch
