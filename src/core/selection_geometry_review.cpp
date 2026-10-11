#include "sketch/selection_geometry_review.hpp"

#include "sketch/document_digest.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/presentation_transform.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Operators = std::map<std::string, PlanarTransform, std::less<>>;

[[noreturn]] void invalid(const char* reason) {
    throw std::invalid_argument(std::string("Selected geometry review: ") + reason);
}

bool same_placements(const std::vector<PhysicalWallRoomDimensionPlacement>& actual,
    const std::vector<PhysicalWallRoomDimensionPlacement>& required) {
    if (actual.size() != required.size()) return false;
    for (std::size_t index = 0; index < required.size(); ++index) {
        if (actual[index].dimension_id != required[index].dimension_id ||
            Json::array({actual[index].offset.x, actual[index].offset.y}).dump() !=
                Json::array({required[index].offset.x, required[index].offset.y}).dump()) return false;
    }
    return true;
}
} // namespace

ReplayedPhysicalWallRoomReview replay_selection_geometry_review(
    const DocumentSnapshot& source, const SelectionGeometryTransformRequest& request,
    const std::vector<Json>& reviewed_room_intents) {
    // Keep the existing batch ceiling before decoding or rederiving anything.
    if (reviewed_room_intents.size() > 32) invalid("room reviews exceed the thirty-two group budget");
    const auto prepared = prepare_selection_geometry_transform(source, request);
    const auto& requirements = prepared.required_room_reviews();
    if (requirements.size() > 32 || reviewed_room_intents.size() != requirements.size())
        invalid("room reviews must cover every required context/plane exactly once");

    const auto snapshot_digest = document_snapshot_digest(source);
    const auto authoring_digest = document_authoring_source_digest_v2(source);
    const auto original_entities_digest = entity_map_digest(source.entities());
    const bool saved_phases = std::any_of(source.entities().begin(), source.entities().end(),
        [](const auto& entry) { return entry.second.type == "model_phases"; });
    const bool active_policy = saved_phases || source.uses_active_phase_constraints();
    std::vector<bool> covered(requirements.size(), false);
    Operators room_operators;
    for (const auto& encoded : reviewed_room_intents) {
        const auto intent = decode_physical_wall_room_review_intent(encoded);
        if (encode_physical_wall_room_review_intent(intent).dump() != encoded.dump())
            invalid("room review must use its canonical typed encoding");
        if (intent.source_snapshot_digest != snapshot_digest ||
            intent.source_authoring_digest != authoring_digest ||
            intent.source_saved_revision != source.saved_revision_optional())
            invalid("room review does not bind the complete original source");
        if (!intent.context_plane_selection || !intent.selected_wall_id.empty() ||
            intent.active_phase_room_scope != saved_phases)
            invalid("room review requires the actual ordinary context/plane scope");

        const auto found = std::find_if(requirements.begin(), requirements.end(), [&](const auto& required) {
            return required.context == intent.context &&
                required.effective_elevation_m == intent.effective_elevation_m;
        });
        if (found == requirements.end()) invalid("unrelated room review context/plane");
        const auto index = static_cast<std::size_t>(found - requirements.begin());
        if (covered[index]) invalid("duplicate room review context/plane");
        covered[index] = true;
        std::set<std::string, std::less<>> retained;
        std::set<std::string, std::less<>> retiring;
        for (const auto& decision : intent.retained)
            if (!retained.insert(decision.room_id).second) invalid("duplicate retained room decision");
            else if (decision.disposition == PhysicalWallRoomRetainedDisposition::retire)
                retiring.insert(decision.room_id);
        for (const auto& id : found->retained_room_ids)
            if (!retained.contains(id)) invalid("required retained room lacks an explicit disposition");
        // The room replay itself requires complete active coverage within this
        // plane, including unchanged neighbors needed for explicit disposition.
        const std::set<std::string, std::less<>> removed_references(
            intent.removed_reference_ids.begin(), intent.removed_reference_ids.end());
        std::vector<PhysicalWallRoomDimensionPlacement> surviving_placements;
        for (const auto& placement : found->dimension_placements) {
            const auto dimension = decode_boundary_dimension_entity(source.entities().at(placement.dimension_id));
            if (!dimension.supported()) invalid("source-derived room callout is unsupported");
            // Explicit reference removal owns deletion, never a new offset.
            // A retired room cannot retain its saved callout; an acknowledged
            // removal on a surviving room likewise has no placement to carry.
            if (removed_references.contains(placement.dimension_id)) continue;
            if (retiring.contains(dimension.dimension->boundary_id))
                invalid("retired selected room callout requires an explicit Remove decision");
            surviving_placements.push_back(placement);
        }
        if (!same_placements(intent.selected_dimension_placements, surviving_placements))
            invalid("selected room dimension placement differs from the source-derived requirement");
        if (intent.selected_dimension_source &&
            (intent.selected_dimension_source->original_revision != source.revision() ||
                intent.selected_dimension_source->original_entities_digest != original_entities_digest))
            invalid("selected room dimensions do not bind the actual original source");
        for (const auto& transformation : found->room_transformations)
            if (!room_operators.emplace(transformation.owner_id, transformation.transform).second)
                invalid("selected room has duplicate required operators");
    }
    if (std::any_of(covered.begin(), covered.end(), [](bool value) { return !value; }))
        invalid("required room review context/plane was omitted");

    // No synthetic empty command/proof is admitted for geometry-invariant room
    // placement. Its first reviewed entity digest is the actual original map.
    const auto& geometry_stage = prepared.makes_change()
        ? prepared.geometry_snapshot().entities() : source.entities();
    ReplayedPhysicalWallRoomReview result;
    if (reviewed_room_intents.empty()) result.entities = geometry_stage;
    else if (reviewed_room_intents.size() == 1)
        result = replay_physical_wall_room_review(geometry_stage, reviewed_room_intents.front(),
            active_policy, &source.entities());
    else result = replay_physical_wall_room_review_batch(geometry_stage, reviewed_room_intents,
        active_policy, &source.entities());

    // Explicit retirement leaves no final geometry on which to place a callout.
    // All retirement references/removals remain owned by the typed room replay.
    for (auto operation = room_operators.begin(); operation != room_operators.end();) {
        if (result.entities.contains(operation->first)) { ++operation; continue; }
        if (std::find(result.retired_room_ids.begin(), result.retired_room_ids.end(), operation->first) ==
            result.retired_room_ids.end()) invalid("selected room disappeared without explicit retirement");
        operation = room_operators.erase(operation);
    }
    // Room review owns retirement/reference cleanup. Reflect only those
    // independently replayed removals in the callout merge base, so surviving
    // room placement cannot reintroduce a retired room's saved override.
    auto callout_source = geometry_stage;
    std::set<std::string, std::less<>> removed;
    for (const auto& [id, entity] : geometry_stage) {
        (void)entity;
        if (!result.entities.contains(id)) removed.insert(id);
    }
    if (!removed.empty()) for (auto& [id, entity] : callout_source) {
        (void)id;
        if (entity.type != kAnnotationEntityType) continue;
        validate_annotation_entity(entity);
        if (!entity.properties.at("state").contains("overrides")) continue;
        auto& overrides = entity.properties.at("state").at("overrides");
        overrides.erase(std::remove_if(overrides.begin(), overrides.end(), [&](const auto& row) {
            return removed.contains(row.at("target_id").template get<std::string>());
        }), overrides.end());
        validate_annotation_entity(entity);
    }
    // This derived base still retains original surviving room placements,
    // unrelated rigid edits and their missing-role suffixes. The reviewed final
    // candidate supplies room geometry and cleanup, never placement authority.
    for (const auto& change : transformed_area_callout_entities(callout_source, result.entities, room_operators)) {
        const auto final = result.entities.find(change.entity.id);
        if (final == result.entities.end()) invalid("review removed a selected room callout provider");
        final->second = merge_selection_annotation_entities(callout_source.at(change.entity.id),
            change.entity, final->second, AnnotationMergeMode::source_callout_proof);
    }
    return result;
}

} // namespace sketch
