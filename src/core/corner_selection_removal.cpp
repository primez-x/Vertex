#include "sketch/corner_selection_removal.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/architectural_object_removal.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/corner_window_removal.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/wall_join_removal.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = CornerSelectionRemovalEntities;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t selection_limit = 1000, change_limit = 4096;
constexpr std::size_t intent_byte_limit = 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Corner selection removal: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity requires 1..128 supported ASCII characters");
}
bool architectural(const ArchitecturalDrawingRemovalIntent& intent) {
    return !intent.architectural.object_ids.empty() || !intent.architectural.components.empty();
}
bool drawing(const ArchitecturalDrawingRemovalIntent& intent) {
    return !intent.drawing.owner_ids.empty() || !intent.drawing.annotations.empty();
}
bool other_present(const ArchitecturalDrawingRemovalIntent& intent) {
    return architectural(intent) || drawing(intent);
}
void bounded(const CornerSelectionRemovalIntent& intent) {
    if (intent.corner_ids.empty() || intent.corner_ids.size() > selection_limit)
        reject("requires 1..1000 explicit actual corner owners");
    if (!std::is_sorted(intent.corner_ids.begin(), intent.corner_ids.end()) ||
        std::adjacent_find(intent.corner_ids.begin(), intent.corner_ids.end()) != intent.corner_ids.end())
        reject("corner owners must be sorted and unique");
    std::size_t count = intent.corner_ids.size();
    for (const auto size : {intent.other.architectural.object_ids.size(), intent.other.architectural.components.size(),
            intent.other.drawing.owner_ids.size(), intent.other.drawing.annotations.size()}) {
        if (size > selection_limit - count) reject("selection exceeds 1000 aggregate roots/components/rows");
        count += size;
    }
    if (other_present(intent.other)) (void)encode_architectural_drawing_removal_intent(intent.other);
    else if (intent.other.allow_manufactured_opening_hosts ||
        !intent.other.architectural.roof_additional_identities.empty())
        reject("absent other selection cannot carry architectural options");
    for (const auto& id : intent.corner_ids) {
        identity(id);
        if (std::binary_search(intent.other.architectural.object_ids.begin(), intent.other.architectural.object_ids.end(), id) ||
            std::binary_search(intent.other.drawing.owner_ids.begin(), intent.other.drawing.owner_ids.end(), id))
            reject("corner owner overlaps another selected root");
    }
    for (const auto& [owner, rows] : intent.other.architectural.roof_additional_identities) {
        (void)owner;
        for (const auto& id : rows) {
            if (std::binary_search(intent.corner_ids.begin(), intent.corner_ids.end(), id))
                reject("roof destination borrows a selected corner owner");
            for (const auto* token : {"corner_ids", "other", "corner_removal"})
                if (id == token) reject("roof destination borrows a corner intent token");
        }
    }
}
void wire_budget(const Json& value, std::size_t& nodes, std::size_t& bytes, std::size_t depth = 0) {
    // The wrapper adds one level to the closed historical child codec.
    if (depth > 9 || ++nodes > 32768) reject("intent nesting/node budget exceeded");
    const auto text = [&](const std::string& text_value) {
        if (text_value.size() > intent_byte_limit - bytes) reject("intent string/key budget exceeded");
        bytes += text_value.size();
    };
    if (value.is_binary() || value.is_discarded()) reject("unsupported intent JSON value");
    if (value.is_string()) text(value.get_ref<const std::string&>());
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        text(key); wire_budget(child, nodes, bytes, depth + 1);
    } else if (value.is_array()) for (const auto& child : value) wire_budget(child, nodes, bytes, depth + 1);
}
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}
void constraints(const Entities& entities, bool active) {
    const auto reason = active ? validate_active_phase_constraint_integrity(entities) :
        validate_constraint_integrity(entities);
    if (reason) reject("unsupported constraint semantics: " + *reason);
}
void snapshot_bound(const DocumentSnapshot& source, const CornerSelectionRemovalIntent& intent) {
    if (!source.is_editable()) reject("captured source is read-only");
    const auto& history = source.history();
    if (history.empty() || history.size() > change_limit || source.revision() >= history.size() ||
        history[source.revision()].revision != source.revision()) reject("captured revision/history is invalid");
    if (source.assets().size() > 65536 || source.named_revisions().size() > change_limit)
        reject("captured asset/named-revision inventory exceeded");
    if (source.saved_revision_optional() && *source.saved_revision_optional() >= history.size())
        reject("saved revision is outside history");
    for (const auto& [name, revision] : source.named_revisions()) {
        (void)name;
        if (revision >= history.size()) reject("named revision is outside history");
    }
    validate_physical_wall_join_removal_identity_lifetime(source, intent.other.architectural.roof_additional_identities);
}
bool physical(const Entity& entity) {
    return entity.type == "wall" || entity.type == "opening" || entity.type == "corner_window" ||
        entity.type == "roof" || entity.type == "slab" || entity.type == "stair" || entity.type == "railing" ||
        entity.type == "column" || entity.type == "beam";
}
void metadata(const DocumentSnapshot& source, const DocumentSnapshot& preview) {
    if (!preview.is_editable() || preview.document_id() != source.document_id() ||
        preview.assets() != source.assets() || preview.read_only_reason() != source.read_only_reason() ||
        preview.named_revisions() != source.named_revisions() ||
        preview.saved_revision_optional() != source.saved_revision_optional() ||
        preview.uses_active_phase_constraints() != source.uses_active_phase_constraints())
        reject("real command admission changed captured assets, project metadata or constraint policy");
    for (const auto& [id, asset] : source.assets())
        if (asset.metadata.dump() != preview.assets().at(id).metadata.dump()) reject("retained asset metadata changed");
    if (preview.history().size() != source.history().size() + 1 || preview.revision() != source.history().size())
        reject("removal must create exactly one detached history event");
}
} // namespace

Json encode_corner_selection_removal_intent(const CornerSelectionRemovalIntent& intent) {
    bounded(intent);
    Json result{{"version", 1}, {"kind", "corner_removal"}, {"corner_ids", intent.corner_ids},
        {"other", other_present(intent.other) ? encode_architectural_drawing_removal_intent(intent.other) : Json(nullptr)}};
    if (result.dump().size() > intent_byte_limit) reject("intent exceeds one MiB");
    return result;
}

CornerSelectionRemovalIntent decode_corner_selection_removal_intent(const Json& value) try {
    std::size_t nodes = 0, bytes = 0;
    wire_budget(value, nodes, bytes);
    if (value.dump().size() > intent_byte_limit) reject("intent exceeds one MiB");
    if (!value.is_object() || value.size() != 4 || !value.contains("version") || !value.contains("kind") ||
        !value.contains("corner_ids") || !value.contains("other")) reject("unsupported intent field set");
    if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.at("kind").is_string() || value.at("kind") != "corner_removal") reject("unsupported intent version/kind");
    const auto& ids = value.at("corner_ids");
    if (!ids.is_array() || ids.empty() || ids.size() > selection_limit) reject("invalid explicit corner inventory");
    CornerSelectionRemovalIntent result;
    for (const auto& id : ids) {
        if (!id.is_string()) reject("corner owner must be an identity");
        result.corner_ids.push_back(id.get<std::string>());
    }
    if (!value.at("other").is_null()) result.other = decode_architectural_drawing_removal_intent(value.at("other"));
    if (encode_corner_selection_removal_intent(result).dump() != value.dump()) reject("intent is not canonical");
    return result;
} catch (const Json::exception& error) { reject(std::string("malformed intent: ") + error.what()); }

ArchitecturalDrawingRemovalIntent corner_selection_removal_authority(const CornerSelectionRemovalIntent& intent) {
    (void)encode_corner_selection_removal_intent(intent);
    auto result = intent.other;
    result.architectural.object_ids.insert(result.architectural.object_ids.end(), intent.corner_ids.begin(), intent.corner_ids.end());
    std::sort(result.architectural.object_ids.begin(), result.architectural.object_ids.end());
    return result;
}

Entities replay_corner_selection_removal_architectural(const Entities& actual,
    const CornerSelectionRemovalIntent& intent, bool active_phase_constraints) try {
    (void)encode_corner_selection_removal_intent(intent);
    // Reserve the shared source/host/roof/native work before either complete
    // producer runs. The ordinary lane retains its historical authority.
    validate_mixed_wall_removal_source_admission(actual, true);
    validate_physical_wall_join_removal_identity_lifetime(actual, {}, 0, intent.other.architectural.roof_additional_identities);
    for (const auto& id : intent.corner_ids) {
        const auto found = actual.find(id);
        if (found == actual.end() || found->second.type != "corner_window") reject("requires an actual corner owner: " + id);
    }
    constraints(actual, active_phase_constraints);
    const auto original_aliases = embedded_assembly_presentation_ids(actual);
    auto expected_aliases = original_aliases;
    auto expected_inactive = constraint_phase_scope(actual).inactive_owner_ids;
    std::vector<Entities> candidates;
    candidates.push_back(replay_corner_window_removal(actual, intent.corner_ids, active_phase_constraints));
    if (architectural(intent.other)) candidates.push_back(replay_architectural_selection_removal(
        actual, intent.other.architectural, intent.other.allow_manufactured_opening_hosts));
    for (const auto& candidate : candidates) {
        const auto aliases = embedded_assembly_presentation_ids(candidate);
        for (const auto& [key, alias] : aliases) {
            const auto before = original_aliases.find(key);
            if (before == original_aliases.end() || before->second != alias) reject("leaf changed a surviving computed component alias");
        }
        for (const auto& [key, alias] : original_aliases) {
            (void)alias;
            if (!aliases.contains(key)) expected_aliases.erase(key);
        }
        const auto scope = constraint_phase_scope(candidate);
        expected_inactive.insert(scope.inactive_owner_ids.begin(), scope.inactive_owner_ids.end());
    }
    auto result = candidates.size() == 1 ? std::move(candidates.front()) :
        compose_ordinary_architectural_removal_candidates(actual, candidates, true, true);
    validate_mixed_wall_removal_source_admission(result, true);
    validate_corner_window_state(result);
    validate_document_assembly_instances(result);
    if (embedded_assembly_presentation_ids(result) != expected_aliases) reject("composition changed exact surviving component aliases");
    if (constraint_phase_scope(result).inactive_owner_ids != expected_inactive) reject("composition changed protected inactive ownership");
    Ids retired;
    for (const auto& [id, entity] : actual) {
        (void)entity;
        if (!result.contains(id)) retired.insert(id);
    }
    validate_completed_architectural_retirement_references(result, retired);
    validate_constraint_transition(actual, result);
    constraints(result, active_phase_constraints);
    return result;
} catch (const Json::exception& error) { reject(std::string("malformed actual source/intent: ") + error.what()); }
catch (const Standard_Failure& error) {
    const auto* detail = error.GetMessageString();
    reject(std::string("native source admission failed: ") + (detail ? detail : "Open CASCADE failure"));
}

Entities replay_corner_selection_removal(const DocumentSnapshot& source, const CornerSelectionRemovalIntent& intent) {
    (void)encode_corner_selection_removal_intent(intent);
    snapshot_bound(source, intent);
    return drawing(intent.other) ? replay_drawing_selection_removal_with_corners(
        source.entities(), intent.other.drawing, intent, true) :
        replay_corner_selection_removal_architectural(source.entities(), intent, true);
}

ApplyEntityChanges prepare_corner_selection_removal(const DocumentSnapshot& source,
    const CornerSelectionRemovalIntent& intent, const std::string& message) try {
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    const auto expected = replay_corner_selection_removal(source, intent);
    ApplyEntityChanges command{source.revision(), {}, {}, message};
    Ids required;
    for (const auto& id : intent.corner_ids) {
        const auto corner = parse_corner_window(source.entities().at(id));
        required.insert(corner.wall_ids.begin(), corner.wall_ids.end());
    }
    for (const auto& [id, entity] : source.entities()) {
        const auto after = expected.find(id);
        if (after == expected.end()) command.entity_changes.push_back(EntityChange::erase(id));
        else if (!exact(entity, after->second)) {
            command.entity_changes.push_back(EntityChange::upsert(after->second));
            if (physical(after->second)) required.insert(id);
        }
    }
    for (const auto& [id, entity] : expected) if (!source.entities().contains(id)) {
        command.entity_changes.push_back(EntityChange::upsert(entity));
        if (physical(entity)) required.insert(id);
    }
    if (command.entity_changes.empty() || command.entity_changes.size() > change_limit) reject("raw change inventory exceeded");
    const auto preview = Document::preview_command(source, Command{command});
    metadata(source, preview);
    if (preview.entities().size() != expected.size()) reject("real command admission changed candidate inventory");
    for (const auto& [id, entity] : expected) {
        const auto after = preview.entities().find(id);
        if (after == preview.entities().end() || !exact(entity, after->second)) reject("real command admission changed exact candidate representation");
    }
    std::erase_if(required, [&](const auto& id) { return !expected.contains(id); });
    validate_architectural_geometry_changes(source, preview, std::vector<std::string>(required.begin(), required.end()));
    return command;
} catch (const Json::exception& error) { reject(std::string("malformed captured source: ") + error.what()); }
catch (const Standard_Failure& error) {
    const auto* detail = error.GetMessageString();
    reject(std::string("native captured source admission failed: ") + (detail ? detail : "Open CASCADE failure"));
}
} // namespace sketch
