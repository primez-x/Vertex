#pragma once

#include "sketch/phase_stair_replacement.hpp"

namespace sketch {

// Ordinary actual-owner authoring only. The phase-named DTO is shared with the
// existing review dialog; this path does not stage or replace baseline owners.
// Inspection is analytical: no native geometry, history mutation or producer
// admission. A protected/inactive source is a blocking diagnostic.
[[nodiscard]] PhaseStairReplacementDependencyPlan inspect_ordinary_stair_edit_dependencies(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<Entity>& edited_editor_entities);

// One explicit ascending decision per actual affected rail. Rehost choices name
// an independently derived resulting same-owner target; retirement uses the
// complete original-source architectural removal closure. Child lifetime is
// reserved against the real retained snapshot before any native work. The
// resulting existing raw v1 command retains exact admitted supported state.
// Publication still requires the caller's complete captured-source fence.
[[nodiscard]] ApplyEntityChanges prepare_ordinary_stair_dependency_edit(
    const DocumentSnapshot& actual, const std::vector<Entity>& edited_editor_entities,
    const std::vector<PhaseStairReplacementDependencyDisposition>& choices,
    const std::string& message);

} // namespace sketch
