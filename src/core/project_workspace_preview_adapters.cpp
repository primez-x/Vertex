#include "sketch/project_workspace.hpp"

#include "sketch/boundary_commit.hpp"
#include "sketch/constraint_authoring.hpp"

#include <utility>

namespace sketch {
namespace {
ApplyEntityChanges entity_diff(const DocumentSnapshot& source,
                               const DocumentSnapshot& candidate) {
    // Both maps have stable ID ordering. Preserve the validated command's
    // message while rebuilding only its entity changes for workspace history.
    ApplyEntityChanges command;
    command.expected_revision = source.revision();
    command.message = candidate.history().back().action;
    auto before = source.entities().begin();
    auto after = candidate.entities().begin();
    while (before != source.entities().end() || after != candidate.entities().end()) {
        if (after == candidate.entities().end() ||
            (before != source.entities().end() && before->first < after->first)) {
            command.entity_changes.push_back(EntityChange::erase(before++->first));
        } else if (before == source.entities().end() || after->first < before->first) {
            command.entity_changes.push_back(EntityChange::upsert((after++)->second));
        } else {
            if (before->second != after->second)
                command.entity_changes.push_back(EntityChange::upsert(after->second));
            ++before;
            ++after;
        }
    }
    if (source.assets() != candidate.assets() || command.entity_changes.empty())
        throw DocumentError(DocumentErrorCode::invalid_entity,
                            "workspace preview must contain only entity changes");
    return command;
}
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_constraint_authoring(
    const ConstraintAuthoringPreview& preview) const {
    const auto source = snapshot();
    auto candidate = Document::fork(source);
    apply_constraint_authoring(candidate, preview);
    return prepare(entity_diff(source, candidate.snapshot()));
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_boundary_commit(
    const BoundaryCommitPreview& preview) const {
    const auto source = snapshot();
    auto candidate = Document::fork(source);
    apply_boundary_commit(candidate, preview);
    return prepare(entity_diff(source, candidate.snapshot()));
}
}  // namespace sketch
