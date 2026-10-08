#pragma once

#include "sketch/document.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

// Same-owner editing only: null leaves a field unchanged. There is no creation,
// deletion, rehosting or family switch, including from a bare cut to an assembly.
// Clearing a door operation is explicit: absence has no inferred swing and is
// physically different from an authored default hinged operation.
struct HostedOpeningProfileEditIntent {
    std::string opening_id;
    std::string wall_id;
    std::optional<Quantity> offset;
    std::optional<Quantity> width;
    std::optional<Quantity> sill;
    std::optional<Quantity> height;
    std::optional<OpeningAssembly> assembly;
    std::optional<DoorOperation> door_operation;
    bool clear_door_operation = false;
};

// Strict bounded version-1 schema; every optional field is an explicit null or
// its canonical quantity/assembly/operation record. Offset and sill admit zero.
[[nodiscard]] nlohmann::json encode_hosted_opening_profile_edit_intent(
    const HostedOpeningProfileEditIntent& intent);
[[nodiscard]] HostedOpeningProfileEditIntent decode_hosted_opening_profile_edit_intent(
    const nlohmann::json& value);

// Pure admission of an existing opening and host identity, dimensional scalars,
// aliases, retained known receipts, assembly family and operation authority.
// Does not manufacture an edit quantity or change the entity representation.
void validate_hosted_opening_profile_entity(const Entity& source);

// Detached replay for captured desktop edits. Checks exact target/host, scalar
// aliases, retained known receipts and assembly family. A same-value edit keeps
// the exact source record, including its representation and opaque metadata.
[[nodiscard]] Entity replay_hosted_opening_profile_entity(
    const Entity& source, const HostedOpeningProfileEditIntent& intent);

// Requires the actual full independently copied saved-active source map. Every
// affected host and sibling cut is admitted by shared physical factories; rooms
// remain intact for mandatory enclosing room review. False defers only final
// constraint residual admission to that completed enclosing candidate.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_hosted_opening_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningProfileEditIntent>& intents,
    bool validate_final_constraints = true);

} // namespace sketch
