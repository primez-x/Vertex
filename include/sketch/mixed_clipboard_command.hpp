#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Closed version-one compound authority: one optional canonical standalone
// roof phase intent (4/17/20/21), fresh-only ApplyEntityChanges, and the captured
// destination registry. Known phase/page enrollment is independently derived;
// no caller-supplied existing registry/view payload is admitted.
// Bounds: 4096 fresh changes, 4 MiB compact-wire upper bound, 100000 JSON
// values, depth 64. Preflight precedes copies/serialization/family replay.
[[nodiscard]] nlohmann::json make_mixed_clipboard_placement_intent(
    const DocumentSnapshot& source, const nlohmann::json& roof_authoring,
    const ApplyEntityChanges& fresh_additions, std::string_view selected_registry_id,
    std::string_view message);
[[nodiscard]] nlohmann::json validate_mixed_clipboard_placement_intent(
    const nlohmann::json& value);
// Exact editable snapshot/history binding and retained/undone fresh-name
// reservation, including current owned children and generated assembly aliases.
// Existing roof-leaf lifetime admission remains the enclosing Document's job.
void validate_mixed_clipboard_placement_source(
    const DocumentSnapshot& source, const nlohmann::json& value);
// A separately reviewed room suffix may create room/edge/vertex/callout names.
// Reserve them jointly against the complete admitted geometry, current proof,
// retained ancestry and its opaque content before publishing the whole edit.
// Historical standalone room commands retain their original namespace rules.
void validate_mixed_clipboard_followup_identities(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<RevisionRecord>& history, std::size_t preceding_records,
    const nlohmann::json& placement_intent,
    const std::vector<std::string>& fresh_names);
[[nodiscard]] bool mixed_clipboard_placement_active_phase_policy(
    const nlohmann::json& value);
[[nodiscard]] std::optional<Revision> mixed_clipboard_placement_source_saved_revision(
    const nlohmann::json& value);

// Replays the roof leaf against this original actual map, then appends fresh
// objects and computes narrow phase/Pinc page enrollments. Never previews or
// manufactures a Document from an intermediate/synthetic source. The enclosing
// Document must validate complete snapshot/history bindings, lifetime, phase
// children, final state/geometry and assets before its single publication.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_mixed_clipboard_placement(
    const std::map<std::string, Entity, std::less<>>& actual,
    const nlohmann::json& value);

} // namespace sketch

