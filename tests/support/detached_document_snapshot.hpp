#pragma once

#include "sketch/document.hpp"

#include <utility>

namespace sketch::test {

// Writable adversarial fixture storage is always separate from captured history.
// Every freeze copies it into a const allocation, so later fixture edits cannot
// modify any published snapshot. Production code has no writable snapshot API.
class DetachedDocumentSnapshotFixture final {
public:
    DetachedDocumentSnapshotFixture(const DocumentSnapshot& source)
        : metadata_(source), history_(source.history()) {}

    DetachedDocumentSnapshotFixture& operator=(const DocumentSnapshot& source) {
        auto history = source.history();
        metadata_ = source;
        history_ = std::move(history);
        return *this;
    }

    [[nodiscard]] DocumentSnapshot freeze() const {
        auto result = metadata_;
        result.history_ = std::make_shared<const std::vector<RevisionRecord>>(history_);
        return result;
    }
    operator DocumentSnapshot() const { return freeze(); }

    template<class Mutation>
    [[nodiscard]] static DocumentSnapshot mutate(const DocumentSnapshot& source, Mutation&& mutation) {
        DetachedDocumentSnapshotFixture fixture(source);
        std::forward<Mutation>(mutation)(fixture);
        return fixture.freeze();
    }

    auto& history() noexcept { return history_; }
    const auto& history() const noexcept { return history_; }
    auto& entities() { return history_.at(static_cast<std::size_t>(revision())).entities; }
    const auto& entities() const { return history_.at(static_cast<std::size_t>(revision())).entities; }
    auto& assets() { return history_.at(static_cast<std::size_t>(revision())).assets; }
    const auto& assets() const { return history_.at(static_cast<std::size_t>(revision())).assets; }
    const auto& document_id() const noexcept { return metadata_.document_id(); }
    Revision revision() const noexcept { return metadata_.revision(); }
    auto saved_revision_optional() const noexcept { return metadata_.saved_revision_optional(); }
    auto saved_revision() const noexcept { return metadata_.saved_revision(); }
    auto dirty() const noexcept { return metadata_.dirty(); }
    auto is_editable() const noexcept { return metadata_.is_editable(); }
    const auto& read_only_reason() const noexcept { return metadata_.read_only_reason(); }
    const auto& named_revisions() const noexcept { return metadata_.named_revisions(); }

private:
    DocumentSnapshot metadata_;
    std::vector<RevisionRecord> history_;
};

} // namespace sketch::test
