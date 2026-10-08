#pragma once

#include "sketch/document.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/phase_wall_replacement_request.hpp"

namespace sketch {

enum class HostedOpeningFamily { door, window, opening };

// Conversion of one existing cut only. The host, dimensions, exact receipts,
// drawing context, layer and opaque owner data remain the source's authority.
// The final door/window assembly is explicit and must match target_family.
// A bare opening has no assembly or operation. An absent door operation is
// distinct from an explicitly authored default operation.
struct HostedOpeningFamilyEditIntent {
    std::string opening_id;
    std::string wall_id;
    HostedOpeningFamily target_family{HostedOpeningFamily::opening};
    std::optional<OpeningAssembly> assembly;
    std::optional<DoorOperation> door_operation;
};

// Strict bounded version-1 schema: exactly version, opening_id, wall_id,
// target_family, assembly and door_operation. Optional values are explicit nulls.
[[nodiscard]] nlohmann::json encode_hosted_opening_family_edit_intent(
    const HostedOpeningFamilyEditIntent& intent);
[[nodiscard]] HostedOpeningFamilyEditIntent decode_hosted_opening_family_edit_intent(
    const nlohmann::json& value);

// Detached recognition/replay without a fabricated map or snapshot. Independently
// admits both complete profiles through existing profile replay and unchanged
// captured quantity authority. A complete source-equivalent profile returns the
// exact source. Other same-family edits must use the existing profile edit API.
[[nodiscard]] Entity replay_hosted_opening_family_entity(
    const Entity& source, const HostedOpeningFamilyEditIntent& intent);

// Discover actual saved-active baseline hosts before solving original physical
// geometry or allocating identities. Every row in a proposed batch owns its
// seeded host in the same actual registry; a baseline opening on another host
// cannot use ordinary fallback. Distinct known registry memberships refuse even
// when no copy is required. Complete source-equivalent batches return nullopt.
[[nodiscard]] std::optional<PhaseWallReplacementRequest> phase_hosted_opening_family_replacement_request(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningFamilyEditIntent>& intents);

// Requires the actual complete independently copied saved-active source map.
// Admits original and final affected host cuts, assemblies and joins through
// shared physical factories. False defers only final constraint residual
// admission to mandatory enclosing room review. Input is immutable on refusal.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_hosted_opening_family_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningFamilyEditIntent>& intents,
    bool validate_final_constraints = true);

} // namespace sketch
