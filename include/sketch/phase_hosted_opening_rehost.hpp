#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"
#include "sketch/phase_wall_replacement_request.hpp"

#include <optional>

namespace sketch {

// Move an existing opening to an actual saved-active wall on the same floor
// and resolved base plane. Its family, dimensions, assembly, operation, layer
// and opaque owner data remain authoritative; only host and station may change.
struct HostedOpeningRehostIntent {
    std::string opening_id;
    std::string original_wall_id;
    std::string target_wall_id;
    Quantity offset;
};

// Strict bounded version-1 record with exactly version, opening_id,
// original_wall_id, target_wall_id and offset. Offset uses an exact quantity
// receipt and admits zero. Unknown fields and inconsistent quantities fail.
[[nodiscard]] nlohmann::json encode_hosted_opening_rehost_intent(
    const HostedOpeningRehostIntent& intent);
[[nodiscard]] HostedOpeningRehostIntent decode_hosted_opening_rehost_intent(
    const nlohmann::json& value);

// Pure preflight against the complete original source. Both actual host roots
// contribute seeds; every rehost in a proposed batch must qualify through one
// of them in the same actual saved-active registry. No roots returns nullopt
// only when ordinary replay would not mutate a retained shared-baseline owner.
// Unsupported baseline openings on other hosts and mixed registries refuse.
// No geometry is solved, identity allocated or source record changed here.
[[nodiscard]] std::optional<PhaseWallReplacementRequest> phase_hosted_opening_rehost_replacement_request(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningRehostIntent>& intents);

// Detached replay of the actual complete source map, after the enclosing phase
// review has separately qualified and mapped all three identities through its
// actual proposed closure. No command, phase copies or history are fabricated.
// Both original and final affected-host graphs are admitted, including active
// sibling cuts, assemblies and joins. Organization and physical plane must be
// resolved; the opening's drawing context is retained exactly. Ordinary maps
// without a phase registry use the same saved-active resolver.
// False defers only final constraint residual admission to the enclosing phase
// or room review. The source is immutable on every success/failure path; same
// host and same station preserve the exact opening record.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_hosted_opening_rehost_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningRehostIntent>& intents,
    bool validate_final_constraints = true);

} // namespace sketch
