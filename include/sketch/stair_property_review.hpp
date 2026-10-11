#pragma once

#include "sketch/stair_edit_dependencies.hpp"
#include "sketch/stair_profile_edit.hpp"

namespace sketch {

// Closed scalar authority and explicit ordinary dependency review against one
// complete actual snapshot. Ascending profiles include retained selected owners;
// decisions cover exactly the actual affected rail roster, also in ascending
// order. No raw entity, candidate map, topology or publication authority.
struct StairPropertyReviewRequest {
    Revision expected_revision{};
    std::string source_snapshot_digest;
    std::string source_authoring_digest;
    std::string source_entities_digest;
    std::optional<Revision> source_saved_revision;
    std::vector<StairProfileEditIntent> profiles;
    std::vector<PhaseStairReplacementDependencyDisposition> dependency_dispositions;
    std::string message;
};

// Exact canonical v1: version, expected_revision, source_snapshot_digest,
// source_authoring_digest, source_entities_digest, source_saved_revision,
// profiles, dependency_dispositions, message. Decision rows are exactly
// {rail_id,action:"retire"} or {rail_id,action:"rehost",target_key}.
// Nested profile v1 keeps every nullable scalar. 1..1000 owners, at most 128
// decisions, 4 MiB wire, 100000 values, depth 64. No historical raw v1 command
// is accepted as review input or used as an independently supplied proof.
[[nodiscard]] nlohmann::json encode_stair_property_review_request(const StairPropertyReviewRequest&);
[[nodiscard]] StairPropertyReviewRequest decode_stair_property_review_request(const nlohmann::json&);

// Analytical only: derives scalar editor entities from the actual source, then
// invokes the existing ordinary inspector for changed owners. No native work,
// history mutation, invented decisions or automatic retirement.
[[nodiscard]] PhaseStairReplacementDependencyPlan inspect_stair_property_review_dependencies(
    const DocumentSnapshot&, const std::vector<StairProfileEditIntent>&);

// Sealed read-only output from independently reconstructed scalar intent and
// the existing ordinary producer. These getters supply composition evidence,
// never a Document publication token. The map is the producer-derived result;
// erased IDs are its complete actual removal closure, while retired rail IDs
// are exactly the explicitly chosen rail subset. No external candidate enters.
class StairPropertyReviewPreparation final {
public:
    [[nodiscard]] const nlohmann::json& request() const noexcept;
    [[nodiscard]] const ApplyEntityChanges& command() const noexcept;
    [[nodiscard]] const std::map<std::string, Entity, std::less<>>& entities() const noexcept;
    [[nodiscard]] const std::vector<std::string>& retired_rail_ids() const noexcept;
    [[nodiscard]] const std::vector<std::string>& erased_entity_ids() const noexcept;
    [[nodiscard]] bool makes_change() const noexcept;

private:
    StairPropertyReviewPreparation(nlohmann::json, ApplyEntityChanges,
        std::map<std::string, Entity, std::less<>>, std::vector<std::string>,
        std::vector<std::string>);
    nlohmann::json request_;
    ApplyEntityChanges command_;
    std::map<std::string, Entity, std::less<>> entities_;
    std::vector<std::string> retired_rails_, erased_entities_;
    friend StairPropertyReviewPreparation replay_stair_property_review_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
};

// Retained all-null/same-value members do not request ordinary authoring of a
// protected baseline. Changed protected/inactive owners refuse in the existing
// dependency producer; active-baseline replacement remains a separate lane.
// A wholly invariant cohort returns makes_change()==false and no changes.
[[nodiscard]] StairPropertyReviewPreparation prepare_stair_property_review(
    const DocumentSnapshot&, const std::vector<StairProfileEditIntent>&,
    const std::vector<PhaseStairReplacementDependencyDisposition>&,
    const std::string& message);
[[nodiscard]] StairPropertyReviewPreparation replay_stair_property_review_preparation(
    const DocumentSnapshot&, const nlohmann::json&);

// Exact history/assets/navigation/names/save/editability fence. This does not
// certify enclosing mixed composition or permit command52 to erase entities.
void require_stair_property_review_source_current(
    const DocumentSnapshot&, const StairPropertyReviewPreparation&);

} // namespace sketch
