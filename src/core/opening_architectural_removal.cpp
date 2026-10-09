#include "sketch/opening_architectural_removal.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/hosted_opening_removal.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/wall_join_removal.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Entities = RoofRemovalEntities;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t root_limit = 1000, change_limit = 4096;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Opening architectural removal: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity requires 1..128 supported ASCII characters");
}
template<class T> void canonical(const std::vector<T>& rows) {
    if (!std::is_sorted(rows.begin(), rows.end()) || std::adjacent_find(rows.begin(), rows.end()) != rows.end())
        reject("selected identities/qualified pairs must be sorted and unique");
}
void intent_bound(const OpeningArchitecturalRemovalIntent& intent) {
    if (intent.opening_ids.empty() || intent.opening_ids.size() > root_limit ||
        intent.other.object_ids.size() > root_limit - intent.opening_ids.size() ||
        intent.other.components.size() > root_limit - intent.opening_ids.size() - intent.other.object_ids.size() ||
        (intent.other.object_ids.empty() && intent.other.components.empty()))
        reject("requires openings and other actual selections within 1000 aggregate roots");
    canonical(intent.opening_ids); canonical(intent.other.object_ids); canonical(intent.other.components);
    for (const auto& id : intent.opening_ids) identity(id);
    for (const auto& id : intent.other.object_ids) identity(id);
    for (const auto& [catalog, local] : intent.other.components) { identity(catalog); identity(local); }
    Ids fresh;
    if (intent.other.roof_additional_identities.size() > change_limit) reject("roof destination owner budget exceeded");
    for (const auto& [owner, rows] : intent.other.roof_additional_identities) {
        identity(owner);
        if (rows.empty() || rows.size() > change_limit - fresh.size()) reject("roof destination slot budget exceeded");
        for (const auto& id : rows) {
            identity(id);
            if (!fresh.insert(id).second) reject("roof destinations overlap");
        }
    }
    for (const auto& [owner, rows] : intent.other.roof_additional_identities) {
        (void)rows;
        if (fresh.contains(owner)) reject("roof destination borrows a source slot owner");
    }
    for (const auto& id : intent.opening_ids) if (fresh.contains(id)) reject("roof destination borrows a selected opening");
    for (const auto& id : intent.other.object_ids) if (fresh.contains(id)) reject("roof destination borrows a selected owner");
}
bool physical(const Entity& entity) {
    return entity.type == "wall" || entity.type == "roof" || entity.type == "slab" || entity.type == "stair" ||
        entity.type == "railing" || entity.type == "column" || entity.type == "beam";
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
void snapshot_bound(const DocumentSnapshot& source, const OpeningArchitecturalRemovalIntent& intent) {
    if (!source.is_editable()) reject("captured source is read-only");
    const auto& history = source.history();
    if (history.empty() || history.size() > 4096 || source.revision() >= history.size() ||
        history[source.revision()].revision != source.revision()) reject("captured revision/history is invalid");
    if (source.assets().size() > 65536 || source.named_revisions().size() > 4096)
        reject("captured asset/named-revision inventory exceeded");
    if (source.saved_revision_optional() && *source.saved_revision_optional() >= history.size())
        reject("saved revision is outside history");
    validate_physical_wall_join_removal_identity_lifetime(source, intent.other.roof_additional_identities);
}
} // namespace

Entities replay_opening_architectural_removal(const Entities& actual,
    const OpeningArchitecturalRemovalIntent& intent, bool active_phase_constraints) {
    try {
        intent_bound(intent);
        bool selected_roof = false;
        for (const auto& id : intent.opening_ids) {
            const auto found = actual.find(id);
            if (found == actual.end() || found->second.type != "opening") reject("requires actual semantic opening: " + id);
        }
        for (const auto& id : intent.other.object_ids) {
            const auto found = actual.find(id);
            if (found == actual.end() || !physical(found->second) || found->second.type == "wall")
                reject("requires an admitted non-wall architectural owner: " + id);
            selected_roof = selected_roof || found->second.type == "roof";
        }
        if (!selected_roof && !intent.other.roof_additional_identities.empty()) reject("roof destinations lack a selected roof");
        validate_physical_wall_join_removal_identity_lifetime(actual, {}, 0, intent.other.roof_additional_identities);
        // The conservative shared bound reserves all leaf/composer admissions
        // before any factory, including manufactured opening host copies.
        validate_mixed_wall_removal_source_admission(actual, true);
        const auto original_aliases = embedded_assembly_presentation_ids(actual);
        for (const auto& key : intent.other.components)
            if (!original_aliases.contains(key)) reject("qualified component is absent from actual source");
        auto opening = replay_hosted_opening_removal(actual, intent.opening_ids, active_phase_constraints);
        if (!opening) reject("opening producer did not admit the selection");
        const auto opening_aliases = embedded_assembly_presentation_ids(*opening);
        auto other = intent.other;
        std::erase_if(other.components, [&](const auto& key) { return !opening_aliases.contains(key); });
        std::vector<Entities> candidates; candidates.push_back(std::move(*opening));
        if (!other.object_ids.empty() || !other.components.empty())
            candidates.push_back(replay_architectural_selection_removal(actual, other,true));
        auto expected_aliases = original_aliases;
        auto expected_inactive = constraint_phase_scope(actual).inactive_owner_ids;
        for (const auto& candidate : candidates) {
            const auto remaining = embedded_assembly_presentation_ids(candidate);
            for (const auto& [key, alias] : remaining) {
                const auto before = original_aliases.find(key);
                if (before == original_aliases.end() || before->second != alias) reject("leaf changed a surviving alias");
            }
            for (const auto& [key, alias] : original_aliases) { (void)alias; if (!remaining.contains(key)) expected_aliases.erase(key); }
            const auto scope = constraint_phase_scope(candidate);
            expected_inactive.insert(scope.inactive_owner_ids.begin(), scope.inactive_owner_ids.end());
        }
        // Explicit component selections can all collapse into the opening
        // producer. A sole complete candidate needs no composition; the shared
        // semantic composer admits two or more independent candidates only.
        auto result = candidates.size() == 1 ? std::move(candidates.front()) :
            compose_ordinary_architectural_removal_candidates(actual, candidates);
        if (embedded_assembly_presentation_ids(result) != expected_aliases) reject("composition changed surviving aliases");
        if (constraint_phase_scope(result).inactive_owner_ids != expected_inactive) reject("composition changed inactive ownership");
        for (const auto& [id, entity] : actual) if (entity.type == "room" || entity.type == "boundary") {
            const auto after = result.find(id);
            if (after == result.end() || !exact(entity, after->second)) reject("composition changed retained room/boundary lineage");
        }
        return result;
    } catch (const nlohmann::json::exception& error) { reject(std::string("malformed actual source/intent: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* detail=error.GetMessageString();
        reject(std::string("native source admission failed: ")+(detail ? detail : "Open CASCADE failure"));
    }
}

ApplyEntityChanges prepare_opening_architectural_removal(const DocumentSnapshot& source,
    const OpeningArchitecturalRemovalIntent& intent, const std::string& message) try {
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    intent_bound(intent); snapshot_bound(source, intent);
    const auto& actual = source.entities();
    const auto candidate = replay_opening_architectural_removal(actual, intent, source.uses_active_phase_constraints());
    ApplyEntityChanges command{source.revision(), {}, {}, message};
    Ids required;
    for (const auto& id : intent.opening_ids) required.insert(actual.at(id).properties.at("wall_id").get<std::string>());
    for (const auto& [id, entity] : actual) {
        const auto after = candidate.find(id);
        if (after != candidate.end() && exact(entity, after->second)) continue;
        if (physical(entity)) required.insert(id);
        if (after == candidate.end()) command.entity_changes.push_back(EntityChange::erase(id));
        else command.entity_changes.push_back(EntityChange::upsert(after->second));
    }
    for (const auto& [id, entity] : candidate) if (!actual.contains(id)) {
        command.entity_changes.push_back(EntityChange::upsert(entity));
        if (physical(entity)) required.insert(id);
    }
    if (command.entity_changes.empty() || command.entity_changes.size() > change_limit) reject("raw change inventory exceeded");
    const auto preview = Document::preview_command(source, Command{command});
    if (preview.entities().size() != candidate.size() || preview.assets() != source.assets() || !preview.is_editable())
        reject("real command admission changed candidate/assets/editability");
    for (const auto& [id, entity] : candidate) {
        const auto after = preview.entities().find(id);
        if (after == preview.entities().end() || !exact(entity, after->second)) reject("real command admission changed exact entity representation");
    }
    // Removed owners are validated through source/candidate changes; required
    // IDs represent retained affected roots and actual opening wall hosts.
    std::erase_if(required, [&](const auto& id) { return !candidate.contains(id); });
    validate_architectural_geometry_changes(source, preview, std::vector<std::string>(required.begin(), required.end()));
    return command;
} catch (const nlohmann::json::exception& error) {
    reject(std::string("malformed captured source: ")+error.what());
} catch (const Standard_Failure& error) {
    const auto* detail=error.GetMessageString();
    reject(std::string("native captured source admission failed: ")+(detail ? detail : "Open CASCADE failure"));
}
} // namespace sketch
