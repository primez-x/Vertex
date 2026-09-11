#include "sketch/workspace_save_coordinator.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
bool valid_text(std::string_view text, std::size_t maximum) {
    if (text.empty() || text.size() > maximum || text.find('\0') != std::string_view::npos)
        return false;
    // Use the same strict portable UTF-8 contract as persisted recovery text.
    try { (void)nlohmann::json(std::string(text)).dump(); }
    catch (const nlohmann::json::exception&) { return false; }
    return true;
}
bool valid_digest(std::string_view text) noexcept {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool valid_binding(const SavePublicationBinding& binding) {
    return valid_text(binding.owner_token, 128) &&
        (binding.role == ArchiveRole::ordinary || binding.role == ArchiveRole::recovery_copy) &&
        valid_text(binding.destination_identity, 128) && valid_text(binding.destination_path, 131072);
}
bool valid_backup_path(const std::optional<std::filesystem::path>& path) {
    if (!path) return true;
    try {
        const auto utf8 = path->generic_u8string();
        return valid_text(std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()), 131072);
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}
}

struct SavePublicationTicket::State {
    std::string workspace_identity;
    SavePublicationBinding binding;
    std::string document_id;
    Revision revision;
    std::uint64_t epoch;
    std::uint64_t edited_generation;
    std::uint64_t checkpoint_generation;
    std::string source_digest;
};

SavePublicationTicket::SavePublicationTicket(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
SavePublicationTicket::SavePublicationTicket(SavePublicationTicket&&) noexcept = default;
SavePublicationTicket& SavePublicationTicket::operator=(SavePublicationTicket&&) noexcept = default;
SavePublicationTicket::~SavePublicationTicket() = default;

SavePublicationTicket WorkspaceSaveCoordinator::capture(
    const ProjectWorkspaceSnapshot& snapshot, const SavePublicationBinding& binding,
    std::string_view authoring_source_digest) {
    if (!valid_binding(binding) || !valid_digest(authoring_source_digest) ||
        !valid_text(snapshot.identity(), 128) || !valid_text(snapshot.document().document_id(), 128))
        throw std::invalid_argument("save publication: invalid binding, identity or authoring source digest");
    return SavePublicationTicket(std::make_unique<SavePublicationTicket::State>(
        SavePublicationTicket::State{snapshot.identity(), binding, snapshot.document().document_id(),
            snapshot.document().revision(), snapshot.epoch(), snapshot.edited_generation(),
            snapshot.checkpoint_generation(), std::string(authoring_source_digest)}));
}

SaveAcknowledgementResult WorkspaceSaveCoordinator::accept(
    SavePublicationTicket& ticket, const SaveReceipt& receipt,
    const ProjectWorkspaceSnapshot& current, const SavePublicationBinding& current_binding,
    std::string_view current_authoring_source_digest) {
    // Transfer before any validation (including allocations) so no failed
    // completion can be retried as a different storage result.
    const auto state = std::move(ticket.state_);
    if (!state) return {SaveAcknowledgementStatus::consumed_ticket, false};
    if (receipt.revision != state->revision || !valid_digest(receipt.file_sha256) ||
        !valid_backup_path(receipt.backup_path))
        return {SaveAcknowledgementStatus::invalid_receipt, false};
    if (!valid_binding(current_binding))
        return {SaveAcknowledgementStatus::invalid_current_binding, true};
    if (state->binding != current_binding)
        return {SaveAcknowledgementStatus::binding_mismatch, true};
    if (state->workspace_identity != current.identity() ||
        state->document_id != current.document().document_id() ||
        state->revision != current.document().revision() || state->epoch != current.epoch() ||
        state->edited_generation != current.edited_generation() ||
        state->checkpoint_generation != current.checkpoint_generation())
        return {SaveAcknowledgementStatus::stale_workspace, true};
    if (!valid_digest(current_authoring_source_digest) || state->source_digest != current_authoring_source_digest)
        return {SaveAcknowledgementStatus::source_mismatch, true};
    return {SaveAcknowledgementStatus::acknowledged, true};
}

}  // namespace sketch
