#pragma once

#include "sketch/model_phases.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {

struct Entity;
inline constexpr std::string_view assembly_phase_membership_entity_type = "assembly_phase_membership";

// A document catalog identity and its exact local embedded instance identity.
// A pair never aliases an independent document instance or a delimiter-encoded ID.
using AssemblyPhaseRoot = std::pair<std::string, std::string>;

struct AssemblyPhaseAlternative {
    std::string alternative_id;
    std::vector<AssemblyPhaseRoot> demolished_roots;
    std::vector<AssemblyPhaseRoot> proposed_roots;
    bool operator==(const AssemblyPhaseAlternative&) const = default;
};

// Immutable row membership; the actual ModelPhases registry supplies selection.
// Each declared root belongs to the baseline or exactly one proposed alternative.
// A registry alternative without a companion row naturally uses only baseline.
class AssemblyPhaseMembership final {
public:
    [[nodiscard]] static AssemblyPhaseMembership create(std::string registry_id,
        std::vector<AssemblyPhaseRoot> roots, std::vector<AssemblyPhaseRoot> baseline_roots,
        std::vector<AssemblyPhaseAlternative> alternatives);
    [[nodiscard]] static AssemblyPhaseMembership from_json(const nlohmann::json& value);
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] const std::string& registry_id() const noexcept { return registry_id_; }
    [[nodiscard]] const std::vector<AssemblyPhaseRoot>& roots() const noexcept { return roots_; }
    [[nodiscard]] const std::vector<AssemblyPhaseRoot>& baseline_roots() const noexcept { return baseline_roots_; }
    [[nodiscard]] const std::vector<AssemblyPhaseAlternative>& alternatives() const noexcept { return alternatives_; }
    void validate_registry(const ModelPhases& registry) const;
    [[nodiscard]] std::map<AssemblyPhaseRoot, ModelPhase> state(const ModelPhases& registry) const;
    // nullopt explicitly requests baseline; it does not mean saved selection.
    [[nodiscard]] std::map<AssemblyPhaseRoot, ModelPhase> state(const ModelPhases& registry,
        const std::optional<std::string>& alternative) const;
private:
    AssemblyPhaseMembership() = default;
    std::string registry_id_;
    std::vector<AssemblyPhaseRoot> roots_;
    std::vector<AssemblyPhaseRoot> baseline_roots_;
    std::vector<AssemblyPhaseAlternative> alternatives_;
};

// Keep the original version representation and surviving root/alternative order.
// New roots and alternatives append in canonical order. Both inputs are admitted.
[[nodiscard]] nlohmann::json retain_assembly_phase_membership_source(
    const nlohmann::json& saved, const AssemblyPhaseMembership& proposed);

struct AssemblyPhaseMembershipOwner {
    std::string membership_entity_id;
    std::string registry_id;
    bool operator==(const AssemblyPhaseMembershipOwner&) const = default;
};
struct AssemblyPhaseMembershipScope {
    // Absent state with present owners means declared but absent in this design.
    // Absence from owners means legacy behavior: no row membership is inferred.
    std::map<AssemblyPhaseRoot, ModelPhase> states;
    std::set<AssemblyPhaseRoot> inactive_roots;
    std::map<AssemblyPhaseRoot, AssemblyPhaseMembershipOwner> owners;
};

// Complete-map read-only admission. Required companions, exact entity IDs,
// actual registry/catalog models and actual embedded instance rows are checked.
// Duplicate qualified ownership refuses, including duplicates in one registry.
// Catalog membership and host visibility remain separate consumer semantics.
[[nodiscard]] AssemblyPhaseMembershipScope resolve_assembly_phase_memberships(
    const std::map<std::string, Entity, std::less<>>& entities);
// Only the named actual registry uses this explicit comparison choice; every
// other registry keeps its saved choice. The named registry must exist even
// when no companion refers to it. No document or selection is modified.
[[nodiscard]] AssemblyPhaseMembershipScope resolve_assembly_phase_memberships(
    const std::map<std::string, Entity, std::less<>>& entities,
    std::string_view registry_id, const std::optional<std::string>& alternative);

} // namespace sketch
