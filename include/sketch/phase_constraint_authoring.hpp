#pragma once

#include "sketch/constraint_authoring.hpp"

namespace sketch {

// Versioned semantic authority. Geometry payloads are outputs of replay;
// every source binding and saved phase choice describes the actual source.
struct PhaseConstraintAuthoringIntent {
    Revision expected_revision{};
    std::string source_snapshot_digest;
    std::string source_authoring_digest;
    std::string source_entities_digest;
    std::optional<Revision> source_saved_revision;
    nlohmann::json phase_selections;
    ConstraintAuthoringIntent intent;
    // Dialect two only: a typed wall identity replacement and reviewed room
    // completion, independently replayed before publication. Null keeps the
    // version-one semantics and exact wire keys.
    nlohmann::json wall_replacement=nullptr;
    // Dialect three only: registry-only opening demolition. It cannot borrow
    // wall replacements, geometry edits, relationships or arbitrary payloads.
    nlohmann::json opening_demolition=nullptr;
    // Dialect four only: source-derived roof replacement. No ordinary entity
    // payload, wall edit or relationship authority accompanies this operation.
    nlohmann::json roof_replacement=nullptr;
    // Dialect five: source-derived horizontal assembly replacement. The
    // baseline and its actual entered dimensions remain retained verbatim.
    nlohmann::json slab_replacement=nullptr;
    // Dialect six: registry-only demolition of actual shared baseline slabs.
    // No replacement identity or ordinary geometry authority accompanies it.
    nlohmann::json slab_demolition=nullptr;
};

[[nodiscard]] PhaseConstraintAuthoringIntent decode_phase_constraint_authoring_intent(
    const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_phase_constraint_authoring_intent(
    const PhaseConstraintAuthoringIntent& intent);
[[nodiscard]] PhaseConstraintAuthoringIntent make_phase_constraint_authoring_intent(
    const DocumentSnapshot& source, const ConstraintAuthoringIntent& intent);
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_phase_constraint_authoring(
    const std::map<std::string,Entity,std::less<>>& source, const nlohmann::json& proof);

// Independently evaluated saved selections, including the empty-registry case.
[[nodiscard]] nlohmann::json phase_constraint_authoring_selections(
    const std::map<std::string,Entity,std::less<>>& source);

} // namespace sketch
