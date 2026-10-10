#include "sketch/corner_window_removal.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/architectural_object_removal.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/corner_window.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Corner-window removal: " + reason);
}
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}
void selection(const CornerWindowRemovalEntities& actual, const std::vector<std::string>& ids) {
    if (actual.size() > 65536 || ids.empty() || ids.size() > 1000 ||
        !std::is_sorted(ids.begin(), ids.end()) || std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        reject("requires a bounded sorted unique roster of actual owners");
    for (const auto& id : ids) {
        const auto found = actual.find(id);
        if (found == actual.end() || found->second.id != id || found->second.type != "corner_window")
            reject("selected identity is not an actual corner-window owner: " + id);
    }
}
void constraints(const CornerWindowRemovalEntities& actual, bool active) {
    const auto error = active ? validate_active_phase_constraint_integrity(actual) : validate_constraint_integrity(actual);
    if (error) reject(*error);
}
} // namespace

CornerWindowRemovalEntities replay_corner_window_removal(const CornerWindowRemovalEntities& actual,
    const std::vector<std::string>& sorted_corner_owner_ids, bool active_phase_constraints) {
    selection(actual, sorted_corner_owner_ids);
    // The shared analytical admission bounds the complete source, closure and
    // source/candidate native inventories before any physical factory runs.
    preflight_architectural_object_removal(actual, sorted_corner_owner_ids, {}, 0, false, false, true);
    constraints(actual, active_phase_constraints);
    auto candidate = replay_architectural_object_removal(actual, sorted_corner_owner_ids, {}, false, 0, false, false, true);
    constraints(candidate, active_phase_constraints);
    return candidate;
}

ApplyEntityChanges prepare_corner_window_removal(const DocumentSnapshot& source,
    const std::vector<std::string>& sorted_corner_owner_ids, const std::string& message) try {
    if (!source.is_editable()) throw DocumentError(DocumentErrorCode::read_only, source.read_only_reason());
    if (message.size() > 4096) reject("command message budget exceeded");
    const auto expected = replay_corner_window_removal(source.entities(), sorted_corner_owner_ids,
        source.uses_active_phase_constraints());
    ApplyEntityChanges command{source.revision(), {}, {}, message};
    std::set<std::string, std::less<>> hosts;
    for (const auto& id : sorted_corner_owner_ids) {
        const auto corner = parse_corner_window(source.entities().at(id));
        hosts.insert(corner.wall_ids.begin(), corner.wall_ids.end());
    }
    for (const auto& [id, entity] : source.entities()) {
        const auto after = expected.find(id);
        if (after == expected.end()) command.entity_changes.push_back(EntityChange::erase(id));
        else if (!exact(entity, after->second)) command.entity_changes.push_back(EntityChange::upsert(after->second));
    }
    for (const auto& [id, entity] : expected) {
        (void)entity;
        if (!source.entities().contains(id)) reject("source-derived removal allocated an identity: " + id);
    }
    if (command.entity_changes.empty() || command.entity_changes.size() > 4096)
        reject("complete removal change inventory is invalid");
    const auto preview = Document::preview_command(source, Command{command});
    if (preview.entities().size() != expected.size() || preview.assets() != source.assets() || !preview.is_editable() ||
        preview.document_id() != source.document_id() || preview.named_revisions() != source.named_revisions() ||
        preview.saved_revision_optional() != source.saved_revision_optional() ||
        preview.uses_active_phase_constraints() != source.uses_active_phase_constraints())
        reject("raw preview changed source inventory, assets, metadata or constraint policy");
    for (const auto& [id, entity] : expected) {
        const auto found = preview.entities().find(id);
        if (found == preview.entities().end() || !exact(entity, found->second))
            reject("raw preview differs from complete actual-source reconstruction: " + id);
    }
    validate_architectural_geometry_changes(source, preview, std::vector<std::string>(hosts.begin(), hosts.end()));
    return command;
} catch (const nlohmann::json::exception& error) {
    reject(std::string("malformed actual source: ") + error.what());
} catch (const Standard_Failure& error) {
    const auto* message = error.GetMessageString();
    reject(std::string("native source/candidate admission failed: ") + (message ? message : "Open CASCADE failure"));
}
} // namespace sketch
