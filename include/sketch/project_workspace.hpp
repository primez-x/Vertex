#pragma once

#include "sketch/document.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace sketch {

class ProjectWorkspace;

// A sealed, instance-bound candidate. Returned snapshots are detached values.
// Ticket access, moves and destruction must be serialized with commit on the
// owning thread. A consumed ticket retains the full retired Document until
// destruction/replacement; release it promptly rather than storing it as history.
class PreparedWorkspaceEdit final {
public:
    PreparedWorkspaceEdit(PreparedWorkspaceEdit&&) noexcept;
    PreparedWorkspaceEdit& operator=(PreparedWorkspaceEdit&&) noexcept;
    PreparedWorkspaceEdit(const PreparedWorkspaceEdit&) = delete;
    PreparedWorkspaceEdit& operator=(const PreparedWorkspaceEdit&) = delete;
    ~PreparedWorkspaceEdit();

    [[nodiscard]] DocumentSnapshot preview() const;

private:
    friend class ProjectWorkspace;
    struct State;
    explicit PreparedWorkspaceEdit(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

// Initial document mutation authority. Recovery ledger and persistence
// coordination are separate, pending layers of the workspace contract.
// Confined to its owning application thread: member calls must not overlap.
// Storage workers receive detached snapshots, never the workspace itself.
class ProjectWorkspace final {
public:
    explicit ProjectWorkspace(const DocumentSnapshot& source);
    ProjectWorkspace(const ProjectWorkspace&) = delete;
    ProjectWorkspace& operator=(const ProjectWorkspace&) = delete;
    ProjectWorkspace(ProjectWorkspace&&) = delete;
    ProjectWorkspace& operator=(ProjectWorkspace&&) = delete;
    ~ProjectWorkspace();

    [[nodiscard]] const std::string& identity() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] DocumentSnapshot snapshot() const;
    [[nodiscard]] PreparedWorkspaceEdit prepare(const Command& command) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_undo() const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_redo() const;
    Revision commit(PreparedWorkspaceEdit& edit);

private:
    enum class Operation { apply, undo, redo };
    [[nodiscard]] PreparedWorkspaceEdit prepare_impl(
        Operation operation, const Command* command) const;

    std::unique_ptr<Document> document_;
    const std::string identity_;
    std::uint64_t epoch_ = 0;
};

}  // namespace sketch
