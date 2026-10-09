#include "sketch/mixed_phase_wall_demolition.hpp"

#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_coordinated_demolition.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_stair_demolition.hpp"
#include "sketch/phase_structural_replacement.hpp"
#include "sketch/phase_wall_demolition.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t byte_limit = 64 * 1024 * 1024, node_limit = 4 * 1024 * 1024;
constexpr std::size_t row_limit = 262144, proof_limit = 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Mixed phase wall demolition: " + reason);
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
bool exact_json(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }

// Bound raw source, retained history and dormant typed fields before digest
// codecs or family replay. Counters span the entire captured snapshot, including
// asset hex expansion. No history command is replayed by this admission scan.
struct Budget {
    std::size_t limit{byte_limit};
    std::size_t bytes{}, nodes{}, rows{};
    void count(std::size_t n) {
        if (n > row_limit - rows) reject("aggregate retained row budget exceeded");
        rows += n;
    }
    void reserve(std::size_t n) {
        if (n > limit - bytes) reject("aggregate captured byte budget exceeded");
        bytes += n;
    }
    void text(const std::string& s) {
        // JSON escaping can expand every input byte to six output bytes.
        if (s.size() > (limit - bytes) / 6) reject("captured string budget exceeded");
        reserve(6 * s.size() + 2);
    }
    void read(const std::string& s) { text(s); }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) reject("JSON node/nesting budget exceeded");
        reserve(32);
        if (value.is_binary() || value.is_discarded()) reject("binary/discarded JSON is unsupported");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("nonfinite JSON scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
    template<class T> void optional(const std::optional<T>& v) { if (v) read(*v); }
    template<class T> void sequence(const std::vector<T>& v) {
        count(v.size()); reserve(32 * v.size());
        for (const auto& row : v) read(row);
    }
    void read(const Entity& v) { text(v.id); text(v.type); read(v.properties); read(v.extensions); }
    void read(const Asset& v) {
        text(v.id); text(v.media_type); text(v.sha256); read(v.metadata);
        if (v.bytes.size() > (limit - bytes) / 2) reject("asset hex expansion budget exceeded");
        reserve(2 * v.bytes.size());
    }
    void read(const EntityChange& v) { text(v.entity_id); read(v.entity); }
    void read(const AssetChange& v) { text(v.asset_id); read(v.asset); }
    void read(const Quantity& v) { text(v.original_expression); }
    void read(const AngleInput& v) { text(v.original_expression); text(v.normalized_expression); }
    void read(const ConstructionReceipt& v) {
        text(v.segment_id);
        if (v.chord_input) { read(v.chord_input->length); read(v.chord_input->heading); }
        optional(v.distance); optional(v.heading); optional(v.rise); optional(v.run); optional(v.turn);
        optional(v.angle); optional(v.height); optional(v.arc_length); optional(v.tangent); optional(v.sweep);
    }
    void read(const PhysicalWallRoomRepairIntent& v) {
        text(v.selected_wall_id); text(v.expected_descriptor_digest); read(v.reviewed_source_lineage);
    }
    void read(const BoundaryGeometryEdit& v) {
        text(v.boundary_id); text(v.target_id); text(v.new_vertex_id); text(v.new_segment_id); text(v.new_dimension_id);
        read(v.replacement_segments); read(v.replacement_authoring); read(v.replacement_properties);
        read(v.replacement_child_mapping); sequence(v.replacement_dimension_ids); sequence(v.replacement_removed_reference_ids);
        sequence(v.replacement_wall_source_ids); optional(v.arc_construction); optional(v.replacement_linework_sources);
        optional(v.physical_wall_room_repair); optional(v.wall_source_translation);
    }
    void read(const BoundaryTranslation& v) { text(v.boundary_id); }
    void read(const BoundaryTransformation& v) { text(v.boundary_id); }
    void read(const RigidOwnerTransformation& v) { text(v.owner_id); }
    void read(const TranslateBoundaries& v) { text(v.message); sequence(v.translations); sequence(v.entity_changes); }
    void read(const TransformBoundaries& v) {
        text(v.message); sequence(v.transformations); sequence(v.entity_changes); sequence(v.source_transformations);
    }
    void read(const ConstraintWallGeometryEdit& v) {
        text(v.wall_id); optional(v.length_entry); optional(v.curve_construction); optional(v.wall_classification);
    }
    void read(const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& v) {
        text(v.stroke_id); optional(v.authored_edit); optional(v.authored_length); sequence(v.vertex_edits);
    }
    void read(const ApplyBoundaryConstraintChanges::DimensionPlacementMove& v) { text(v.dimension_id); }
    void read(const ExteriorCornerMoveIntent& v) { text(v.boundary_id); text(v.vertex_id); }
    void read(const ExteriorSegmentResizeIntent& v) { text(v.boundary_id); text(v.segment_id); read(v.exact_length); }
    void read(const ExteriorSegmentArcIntent& v) { text(v.boundary_id); text(v.segment_id); read(v.arc_construction); }
    void read(const WallSplitMeasuredOwnerIds& v) {
        text(v.boundary_id); text(v.vertex_id); text(v.segment_id); text(v.automatic_dimension_id);
    }
    void read(const WallSplitPhysicalRoomIds& v) { text(v.boundary_id); sequence(v.new_segment_ids); sequence(v.new_vertex_ids); }
    void read(const WallSplitIntent& v) {
        text(v.wall_id); text(v.second_wall_id); text(v.seam_constraint_id); sequence(v.measured_owners); sequence(v.physical_room_owners);
        if (v.physical_room_phase_completion) reserve(64);
    }
    void read(const WallMergeIntent& v) {
        text(v.first_wall_id); text(v.second_wall_id);
        if (v.physical_room_phase_completion) reserve(64);
    }
    void read(const JointAnnotationTranslationIntent& v) { text(v.owner_id); text(v.child_id); }
    void read(const JointReferenceTranslationIntent& v) { text(v.reference_id); }
    void read(const JointOwnerTranslationIntent& v) { text(v.owner_id); }
    void read(const JointTranslationIntent& v) {
        sequence(v.rigid_boundary_ids); sequence(v.rigid_stroke_ids); sequence(v.partial_wall_ids); sequence(v.dimension_ids);
        sequence(v.annotation_translations); sequence(v.reference_translations); sequence(v.owner_translations);
        sequence(v.dimension_translations); sequence(v.owner_transformations);
    }
    void read(const DistoMeasurementAttachment& v) {
        text(v.owner_id); const auto& r = v.record;
        text(r.reading_id); text(r.target_field); text(r.unit); text(r.captured_at); text(r.model);
        text(r.firmware); text(r.transport); text(r.provenance);
    }
    void read(const ApplyBoundaryConstraintChanges& v) {
        text(v.message); sequence(v.boundary_edits); sequence(v.wall_edits); sequence(v.entity_changes);
        sequence(v.physical_entity_changes); sequence(v.exterior_source_edits); sequence(v.supplemental_entity_changes);
        sequence(v.supplemental_asset_changes); sequence(v.measured_stroke_edits); sequence(v.dimension_placement_moves);
        sequence(v.selection_entity_changes); sequence(v.room_review_additional_intents);
        optional(v.exterior_corner_move); optional(v.exterior_segment_resize); optional(v.exterior_segment_arc);
        optional(v.wall_split); optional(v.wall_merge); optional(v.rigid_group_transform); optional(v.joint_translation);
        optional(v.disto_measurement); read(v.room_review_intent); read(v.room_review_geometry_proof);
        read(v.phase_room_review_intent); read(v.phase_constraint_authoring_intent); read(v.independent_drawing_removal_intent);
    }
    void entities(const Entities& values) {
        count(values.size());
        for (const auto& [id, entity] : values) {
            if (id != entity.id || id.empty() || !entity.properties.is_object() || !entity.extensions.is_object())
                reject("captured entity envelope is invalid");
            text(id); read(entity);
        }
    }
    void assets(const std::map<std::string, Asset, std::less<>>& values) {
        count(values.size());
        for (const auto& [id, asset] : values) { if (id != asset.id) reject("captured asset envelope is invalid"); text(id); read(asset); }
    }
};

void snapshot_bound(const DocumentSnapshot& source) {
    Budget budget;
    const auto& history = source.history();
    if (history.empty() || history.size() > 4096 || source.revision() >= history.size()) reject("captured history is invalid");
    budget.count(history.size()); budget.text(source.document_id()); budget.text(source.read_only_reason());
    budget.entities(source.entities()); budget.assets(source.assets());
    for (std::size_t i = 0; i < history.size(); ++i) {
        const auto& r = history[i];
        if (r.revision != i) reject("captured history is not contiguous");
        budget.text(r.action); if (r.name) budget.text(*r.name);
        budget.entities(r.entities); budget.assets(r.assets);
        budget.count(r.undo_stack.size()); budget.count(r.redo_stack.size());
        budget.reserve(32 * (r.undo_stack.size() + r.redo_stack.size()));
        budget.optional(r.boundary_translation); budget.optional(r.boundary_transform); budget.optional(r.boundary_geometry_edit);
        budget.optional(r.boundary_constraint_changes); budget.optional(r.boundary_translations); budget.optional(r.boundary_transforms);
    }
    if (source.saved_revision_optional() && (*source.saved_revision_optional() > source.revision() ||
        *source.saved_revision_optional() >= history.size())) reject("captured saved revision is invalid");
    budget.count(source.named_revisions().size());
    for (const auto& [name, revision] : source.named_revisions()) {
        budget.text(name); if (revision >= history.size()) reject("named revision is outside captured history");
    }
}

void outer34_only(const ApplyBoundaryConstraintChanges& c) {
    if (!c.phase_constraint_authoring_completion || c.phase_constraint_authoring_intent.is_null() ||
        c.message.size() > 4096 || !c.boundary_edits.empty() || !c.wall_edits.empty() || !c.entity_changes.empty() ||
        !c.physical_entity_changes.empty() || !c.exterior_source_edits.empty() || !c.supplemental_entity_changes.empty() ||
        !c.supplemental_asset_changes.empty() || !c.measured_stroke_edits.empty() || !c.dimension_placement_moves.empty() ||
        !c.selection_entity_changes.empty() || c.selection_completion || c.exterior_source_completion ||
        c.supplemental_source_completion || c.supplemental_asset_reference_completion || c.rigid_wall_transform_completion ||
        c.measured_source_completion || c.dimension_placement_completion || c.rigid_group_completion || c.rigid_group_transform ||
        c.wall_split || c.wall_merge || c.exterior_corner_move || c.exterior_segment_resize || c.exterior_segment_arc ||
        c.joint_translation_completion || c.joint_translation || c.room_review_completion || !c.room_review_intent.is_null() ||
        c.room_review_geometry_completion || !c.room_review_geometry_proof.is_null() || c.room_review_batch_completion ||
        !c.room_review_additional_intents.empty() || c.phase_room_review_completion || !c.phase_room_review_intent.is_null() ||
        c.wall_dimension_completion || c.curve_construction_completion || c.disto_measurement_completion || c.disto_measurement ||
        c.independent_drawing_removal_completion || !c.independent_drawing_removal_intent.is_null())
        reject("other demolition requires the exclusive intact outer34 command");
    Budget budget; budget.limit = proof_limit; budget.read(c.phase_constraint_authoring_intent);
    if (c.phase_constraint_authoring_intent.dump().size() > proof_limit) reject("other demolition proof byte budget exceeded");
}

void binding(const DocumentSnapshot& source, const PhaseConstraintAuthoringIntent& intent) {
    if (intent.expected_revision != source.revision() || intent.source_saved_revision != source.saved_revision_optional() ||
        intent.source_snapshot_digest != document_snapshot_digest(source) ||
        intent.source_authoring_digest != document_authoring_source_digest_v2(source) ||
        intent.source_entities_digest != entity_map_digest(source.entities()) ||
        !exact_json(intent.phase_selections, phase_constraint_authoring_selections(source.entities())))
        reject("other demolition differs from actual captured history, entities or saved choices");
}

void demolition_leaf(const PhaseConstraintAuthoringIntent& leaf, const PhaseWallDemolitionIntent& wall) {
    const auto same_choice = [&](const auto& value) {
        if (value.registry_id != wall.registry_id || value.alternative_id != wall.alternative_id)
            reject("demolition families require the same actual saved registry and alternative");
    };
    if (!leaf.opening_demolition.is_null()) same_choice(decode_phase_opening_demolition_intent(leaf.opening_demolition));
    else if (!leaf.slab_demolition.is_null()) same_choice(decode_slab_demolition_intent(leaf.slab_demolition));
    else if (!leaf.stair_demolition.is_null()) same_choice(decode_stair_demolition_intent(leaf.stair_demolition));
    else if (!leaf.roof_replacement.is_null()) {
        const auto value = decode_phase_roof_replacement_authoring(leaf.roof_replacement);
        same_choice(value);
        if (!value.demolition || !value.identities.empty() || !value.demolition_additional_identities.empty() ||
            !value.roof_profiles.empty() || !value.roof_opening_edits.empty() || !value.roof_edits.empty() ||
            !value.ordinary_roof_edits.empty()) reject("roof demolition cannot carry edits or fresh destinations");
    } else if (!leaf.structural_replacement.is_null()) {
        const auto value = decode_phase_structural_replacement_authoring(leaf.structural_replacement);
        same_choice(value);
        if (!value.demolition || !value.edits.empty() || !value.identities.empty() || value.complete_hosted ||
            !value.hosted_instance_identities.empty()) reject("structural demolition cannot carry replacement authority");
    } else reject("only retained baseline demolition families can accompany walls");
}

// Historical opening replay writes a sorted semantic set. Only after an
// authenticated family of the closed phase34 proof replays on the actual source
// may this lane restore retained order and append independently derived IDs.
// No other field, registry roster or entity envelope has normalization authority.
void retain_replayed_demolition_order(const Entities& actual, Entities& candidate,
    const PhaseWallDemolitionIntent& wall) {
    if (candidate.size() != actual.size()) reject("demolition cannot erase or create physical owners");
    Budget budget; budget.entities(candidate);
    for (const auto& [id, entity] : actual) {
        const auto found = candidate.find(id);
        if (found == candidate.end()) reject("demolition removed an actual owner");
        if (id != wall.registry_id && !exact(entity, found->second))
            reject("demolition changed a physical/catalog/presentation owner: " + id);
    }
    const auto& original = actual.at(wall.registry_id);
    const auto& replacement = candidate.at(wall.registry_id);
    if (original.type != "model_phases") reject("demolition requires the actual saved registry");
    const auto original_phases = ModelPhases::from_json(original.properties.at("model"));
    if (original_phases.active_alternative() != std::optional<std::string>{wall.alternative_id})
        reject("demolition requires the actual saved active alternative");
    const auto& alternatives = original.properties.at("model").at("alternatives");
    const auto& next_alternatives = replacement.properties.at("model").at("alternatives");
    if (!next_alternatives.is_array() || next_alternatives.size() != alternatives.size())
        reject("saved alternative roster changed");
    std::optional<std::size_t> active_index;
    for (std::size_t i = 0; i < alternatives.size(); ++i) {
        if (alternatives[i].at("id") != wall.alternative_id) continue;
        if (active_index) reject("ambiguous saved active alternative");
        active_index = i;
    }
    if (!active_index) reject("saved active alternative is missing");
    const auto& retained = alternatives[*active_index].at("demolished_ids");
    const auto& replayed = next_alternatives[*active_index].at("demolished_ids");
    if (!retained.is_array() || !replayed.is_array() || replayed.size() <= retained.size() ||
        replayed.size() - retained.size() > 4096)
        reject("demolition requires bounded nonempty active additions");
    Ids retained_ids, replayed_ids, added;
    for (const auto& row : retained) {
        if (!row.is_string() || !retained_ids.insert(row.get_ref<const std::string&>()).second)
            reject("retained demolition identity is invalid or ambiguous");
    }
    for (const auto& row : replayed) {
        if (!row.is_string()) reject("replayed demolition identity must be a string");
        const auto& id = row.get_ref<const std::string&>();
        if (!replayed_ids.insert(id).second) reject("replayed demolition identity is ambiguous");
        if (!retained_ids.contains(id)) added.insert(id);
    }
    for (const auto& id : retained_ids)
        if (!replayed_ids.contains(id)) reject("replayed demolition removed a retained identity");

    // Both registries are bounded before their codecs, copies or byte compares.
    // Restoring this sole list must recover every other actual byte exactly.
    const auto replayed_phases = ModelPhases::from_json(replacement.properties.at("model"));
    auto normalized = replacement;
    auto& rows = normalized.properties.at("model").at("alternatives")[*active_index].at("demolished_ids");
    rows = retained;
    if (!exact(original, normalized)) reject("demolition changed saved metadata, roster or another alternative");
    for (const auto& id : added) rows.push_back(id);
    const auto normalized_phases = ModelPhases::from_json(normalized.properties.at("model"));
    if (normalized_phases.entity_ids() != replayed_phases.entity_ids() ||
        normalized_phases.baseline_ids() != replayed_phases.baseline_ids() ||
        normalized_phases.alternatives() != replayed_phases.alternatives() ||
        normalized_phases.active_alternative() != replayed_phases.active_alternative())
        reject("retained demolition order adapter changed typed saved-design semantics");
    const auto replayed_inactive = constraint_phase_scope(candidate).inactive_owner_ids;
    candidate.at(wall.registry_id) = std::move(normalized);
    if (constraint_phase_scope(candidate).inactive_owner_ids != replayed_inactive)
        reject("retained demolition order adapter changed saved activity");
}

// Every actual envelope must remain exact except the one supported raw suffix.
// Model parsing admits the complete roster, but never canonicalizes that roster
// into the returned raw entity. This preserves numerical forms and raw order.
Ids registry_only(const Entities& actual, const Entities& candidate, const PhaseWallDemolitionIntent& wall) {
    if (candidate.size() != actual.size()) reject("demolition cannot erase or create physical owners");
    Budget budget; budget.entities(candidate);
    for (const auto& [id, entity] : actual) {
        const auto found = candidate.find(id);
        if (found == candidate.end()) reject("demolition removed an actual owner");
        if (id != wall.registry_id && !exact(entity, found->second)) reject("demolition changed a physical/catalog/presentation owner: " + id);
    }
    const auto& original = actual.at(wall.registry_id);
    const auto& replacement = candidate.at(wall.registry_id);
    const auto phases = ModelPhases::from_json(original.properties.at("model"));
    if (original.type != "model_phases" || phases.active_alternative() != std::optional<std::string>{wall.alternative_id})
        reject("demolition requires the actual saved registry");
    const auto& alternatives = original.properties.at("model").at("alternatives");
    auto normalized = replacement;
    auto& next_alternatives = normalized.properties.at("model").at("alternatives");
    if (!next_alternatives.is_array() || next_alternatives.size() != alternatives.size()) reject("saved alternative roster changed");
    Ids added;
    bool active_found = false;
    const auto scope = constraint_phase_scope(actual);
    for (std::size_t i = 0; i < alternatives.size(); ++i) {
        if (alternatives[i].at("id") != wall.alternative_id) continue;
        if (active_found) reject("ambiguous saved active alternative");
        active_found = true;
        const auto& retained = alternatives[i].at("demolished_ids");
        auto& rows = next_alternatives[i].at("demolished_ids");
        if (!retained.is_array() || !rows.is_array() || rows.size() <= retained.size() || rows.size() - retained.size() > 4096)
            reject("demolition requires bounded nonempty active additions");
        for (std::size_t j = 0; j < retained.size(); ++j)
            if (!exact_json(retained[j], rows[j])) reject("retained demolition row bytes/order changed");
        for (std::size_t j = retained.size(); j < rows.size(); ++j) {
            if (!rows[j].is_string()) reject("demolition addition must name an actual owner");
            const auto id = rows[j].get<std::string>();
            const auto owner = actual.find(id);
            if (owner == actual.end() || owner->second.required || scope.inactive_owner_ids.contains(id) ||
                !std::binary_search(phases.baseline_ids().begin(), phases.baseline_ids().end(), id) || !added.insert(id).second)
                reject("demolition addition lacks active nonrequired baseline authority: " + id);
            const auto& type = owner->second.type;
            if (type != "wall" && type != "opening" && type != "roof" && type != "slab" &&
                type != "column" && type != "beam" && type != "stair" && type != "railing")
                reject("unsupported semantic demolition addition: " + id);
        }
        rows = retained;
    }
    if (!active_found || !exact(original, normalized)) reject("demolition changed saved metadata, roster or another alternative");
    (void)ModelPhases::from_json(replacement.properties.at("model"));
    auto expected_inactive = scope.inactive_owner_ids;
    expected_inactive.insert(added.begin(), added.end());
    if (constraint_phase_scope(candidate).inactive_owner_ids != expected_inactive) reject("demolition changed unrelated saved activity");
    return added;
}
} // namespace

std::optional<ApplyEntityChanges> prepare_mixed_phase_wall_demolition(
    const DocumentSnapshot& source, const std::vector<std::string>& wall_ids,
    const std::optional<ApplyBoundaryConstraintChanges>& other_demolition, const std::string& message) try {
    // The established wall facade owns classification, wall/host protection and
    // analytical source bounds. An ordinary-only wall cohort stays in its lane.
    const auto wall_command = prepare_phase_wall_demolition(source, wall_ids, message);
    if (!wall_command) return std::nullopt;
    if (message.size() > 4096) reject("mixed command message budget exceeded");
    const auto& registry = wall_command->entity_changes.at(0).entity;
    const auto phases = ModelPhases::from_json(registry.properties.at("model"));
    if (!phases.active_alternative()) reject("wall demolition has no saved active alternative");
    auto sorted_walls = wall_ids;
    std::sort(sorted_walls.begin(), sorted_walls.end());
    const PhaseWallDemolitionIntent wall{registry.id, *phases.active_alternative(), std::move(sorted_walls)};
    const auto& actual = source.entities();
    auto wall_candidate = replay_phase_wall_demolition_entities(actual, wall);
    const auto wall_additions = registry_only(actual, wall_candidate, wall);
    if (!other_demolition) return wall_command;

    outer34_only(*other_demolition);
    snapshot_bound(source);
    const auto intent = decode_phase_constraint_authoring_intent(other_demolition->phase_constraint_authoring_intent);
    if (!exact_json(encode_phase_constraint_authoring_intent(intent), other_demolition->phase_constraint_authoring_intent))
        reject("other demolition semantic proof is not byte-canonical");
    if (other_demolition->expected_revision != source.revision() || intent.expected_revision != other_demolition->expected_revision ||
        intent.intent.message != other_demolition->message) reject("other demolition command identity differs from its proof");
    const Json expected{{"version", 34}, {"kind", "apply_boundary_constraint_changes"},
        {"expected_revision", other_demolition->expected_revision}, {"message", other_demolition->message},
        {"phase_constraint_authoring_completion", true}, {"phase_constraint_authoring_intent", other_demolition->phase_constraint_authoring_intent}};
    if (!exact_json(command_to_json(Command{*other_demolition}), expected) || expected.dump().size() > proof_limit)
        reject("other demolition lacks its closed canonical outer34 envelope");
    binding(source, intent);
    std::vector<PhaseConstraintAuthoringIntent> leaves;
    if (!intent.coordinated_demolition.is_null()) {
        if (phase_coordinated_demolition_ordinary_removal(intent.coordinated_demolition, intent))
            reject("ordinary/proposed retirement needs a separate complete lane");
        if (intent.coordinated_demolition.at("version") != 1)
            reject("mixed walls require the closed retained-baseline inner1 demolition lane");
        // The historical component codec checks each complete canonical child
        // and its exact enclosing history/source/saved-choice binding. Keep its
        // leaves intact; only this producer's candidate composition is adapted.
        leaves = phase_coordinated_demolition_components(intent.coordinated_demolition, intent);
        if (leaves.size() < 2 || leaves.size() > 5) reject("coordinated demolition family count is invalid");
    } else leaves.push_back(intent);
    std::vector<Entities> other_candidates;
    other_candidates.reserve(leaves.size());
    Ids other_additions;
    for (const auto& child : leaves) {
        demolition_leaf(child, wall);
        // Every complete authenticated family replays on the same full actual
        // source, never wall_candidate or a filtered/fabricated snapshot. The
        // historical standalone coordinator and leaf grammars remain intact.
        auto candidate = replay_phase_constraint_authoring(actual, encode_phase_constraint_authoring_intent(child));
        retain_replayed_demolition_order(actual, candidate, wall);
        const auto additions = registry_only(actual, candidate, wall);
        other_additions.insert(additions.begin(), additions.end());
        other_candidates.push_back(std::move(candidate));
    }
    auto other_candidate = other_candidates.size() == 1 ? std::move(other_candidates.front()) :
        compose_phase_demolition_candidates(actual, other_candidates);
    if (registry_only(actual, other_candidate, wall) != other_additions)
        reject("other family composition changed independently admitted demolition additions");
    Ids expected_additions = wall_additions;
    expected_additions.insert(other_additions.begin(), other_additions.end());
    auto composed = compose_phase_demolition_candidates(actual, {wall_candidate, other_candidate});
    if (registry_only(actual, composed, wall) != expected_additions) reject("composition changed independently admitted demolition additions");
    ApplyEntityChanges result{source.revision(), {}, {}, message};
    result.entity_changes.push_back(EntityChange::upsert(std::move(composed.at(wall.registry_id))));
    return result;
} catch (const Json::exception& error) {
    reject(std::string("malformed source or proof: ") + error.what());
}
} // namespace sketch
