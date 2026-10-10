#include "sketch/wall_join_removal.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_affected_walls = 4096;
constexpr std::size_t maximum_geometry_work = 262144;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Physical wall join removal: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity requires 1..128 supported ASCII characters");
}

// Read-only token reservation includes opaque strings and object keys. It is
// never used as a reference rewrite codec.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("source JSON node/nesting budget exceeded");
        const auto reserve = [&](const std::string& text) {
            if (text.size() > 64 * 1024 * 1024 - bytes) reject("source JSON string budget exceeded");
            bytes += text.size(); values.insert(text);
        };
        if (value.is_string()) reserve(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            reserve(key); read(child, depth + 1);
        }
    }
};
Strings occupied_strings(const PhysicalWallJoinRemovalEntities& source) {
    if (source.size() > maximum_entities) reject("source entity budget exceeded");
    Strings result;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) result.values.insert(key);
    for (const auto& [id, entity] : source) {
        if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source requires actual identified entity envelopes");
        result.read(Json(id)); result.read(Json(entity.type));
        result.read(entity.properties); result.read(entity.extensions);
    }
    return result;
}

// Unlike replay's per-source token inventory, this conservative lifetime scan
// keeps only the requested destinations and one cumulative work budget. Read
// retained typed fields directly: command_to_json also validates geometry and
// may omit dormant fields, neither of which is appropriate for reservation.
// No codec replay, command rewriting or native geometry is performed here.
struct LifetimeReservations {
    const Ids& fresh;
    std::size_t nodes{}, bytes{}, rows{};
    static constexpr std::size_t node_limit = 4 * 1024 * 1024;
    static constexpr std::size_t byte_limit = 64 * 1024 * 1024;
    static constexpr std::size_t row_limit = 262144;

    void count(std::size_t amount) {
        if (amount > row_limit - rows) reject("retained identity row budget exceeded");
        rows += amount;
    }
    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) reject("retained identity string budget exceeded");
        bytes += value.size();
        if (fresh.contains(value)) reject("fresh join identity is reserved by actual retained source or intent: " + value);
    }
    void read(const std::string& value) { text(value); }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || nodes == node_limit) reject("retained identity JSON node/nesting budget exceeded");
        ++nodes;
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) {
            if (value.size() > node_limit - nodes) reject("retained identity JSON node budget exceeded");
            for (const auto& child : value) read(child, depth + 1);
        } else if (value.is_object()) {
            if (value.size() > node_limit - nodes) reject("retained identity JSON node budget exceeded");
            for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
        } else if (value.is_binary()) {
            // Opaque binary values cannot reserve a string spelling, but still
            // consume the same aggregate byte budget before any alias work.
            const auto size = value.get_binary().size();
            if (size > byte_limit - bytes) reject("retained identity binary budget exceeded");
            bytes += size;
        }
    }
    template<class T> void optional(const std::optional<T>& value) {
        if (value) read(*value);
    }
    template<class T> void sequence(const std::vector<T>& values) {
        count(values.size());
        for (const auto& value : values) read(value);
    }
    void read(const Entity& value) {
        text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
    void read(const Asset& value) {
        text(value.id); text(value.media_type); text(value.sha256); read(value.metadata);
        // Asset payload bytes are not identity strings and are never copied.
    }
    void read(const EntityChange& value) { text(value.entity_id); read(value.entity); }
    void read(const AssetChange& value) { text(value.asset_id); read(value.asset); }
    void read(const PhaseEntityImportProof& value) {
        for (const auto* name : {"phase_entity_import", "import_phase_entities", "registry_ids", "entity_ids", "asset_ids", "reviewed_existing_hierarchy_ids"}) text(name);
        text(value.message); sequence(value.registry_ids); sequence(value.entity_ids); sequence(value.asset_ids);
        sequence(value.reviewed_existing_hierarchy_ids);
    }
    void read(const Quantity& value) { text(value.original_expression); }
    void read(const AngleInput& value) { text(value.original_expression); text(value.normalized_expression); }
    void read(const ConstructionReceipt& value) {
        text(value.segment_id);
        if (value.chord_input) { read(value.chord_input->length); read(value.chord_input->heading); }
        optional(value.distance); optional(value.heading); optional(value.rise); optional(value.run);
        optional(value.turn); optional(value.angle); optional(value.height); optional(value.arc_length);
        optional(value.tangent); optional(value.sweep);
    }
    void read(const PhysicalWallRoomRepairIntent& value) {
        text(value.selected_wall_id); text(value.expected_descriptor_digest); read(value.reviewed_source_lineage);
    }
    void read(const BoundaryGeometryEdit& value) {
        text(value.boundary_id); text(value.target_id); text(value.new_vertex_id);
        text(value.new_segment_id); text(value.new_dimension_id);
        read(value.replacement_segments); read(value.replacement_authoring); read(value.replacement_properties);
        read(value.replacement_child_mapping); sequence(value.replacement_dimension_ids);
        sequence(value.replacement_removed_reference_ids); sequence(value.replacement_wall_source_ids);
        optional(value.arc_construction); optional(value.replacement_linework_sources);
        optional(value.physical_wall_room_repair); optional(value.wall_source_translation);
    }
    void read(const BoundaryTranslation& value) { text(value.boundary_id); }
    void read(const BoundaryTransformation& value) { text(value.boundary_id); }
    void read(const RigidOwnerTransformation& value) { text(value.owner_id); }
    void read(const TranslateBoundaries& value) {
        text(value.message); sequence(value.translations); sequence(value.entity_changes);
    }
    void read(const TransformBoundaries& value) {
        text(value.message); sequence(value.transformations); sequence(value.entity_changes);
        sequence(value.source_transformations);
    }
    void read(const ConstraintWallGeometryEdit& value) {
        text(value.wall_id); optional(value.length_entry); optional(value.curve_construction);
        optional(value.wall_classification);
    }
    void read(const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& value) {
        text(value.stroke_id); optional(value.authored_edit); optional(value.authored_length);
        sequence(value.vertex_edits);
    }
    void read(const ApplyBoundaryConstraintChanges::DimensionPlacementMove& value) { text(value.dimension_id); }
    void read(const ExteriorCornerMoveIntent& value) { text(value.boundary_id); text(value.vertex_id); }
    void read(const ExteriorSegmentResizeIntent& value) {
        text(value.boundary_id); text(value.segment_id); read(value.exact_length);
    }
    void read(const ExteriorSegmentArcIntent& value) {
        text(value.boundary_id); text(value.segment_id); read(value.arc_construction);
    }
    void read(const WallSplitMeasuredOwnerIds& value) {
        text(value.boundary_id); text(value.vertex_id); text(value.segment_id); text(value.automatic_dimension_id);
    }
    void read(const WallSplitPhysicalRoomIds& value) {
        text(value.boundary_id); sequence(value.new_segment_ids); sequence(value.new_vertex_ids);
    }
    void read(const WallSplitIntent& value) {
        text(value.wall_id); text(value.second_wall_id); text(value.seam_constraint_id);
        sequence(value.measured_owners); sequence(value.physical_room_owners);
    }
    void read(const WallMergeIntent& value) { text(value.first_wall_id); text(value.second_wall_id); }
    void read(const WallGroupScaleIntent& value) { sequence(value.wall_ids); }
    void read(const JointAnnotationTranslationIntent& value) { text(value.owner_id); text(value.child_id); }
    void read(const JointReferenceTranslationIntent& value) { text(value.reference_id); }
    void read(const JointOwnerTranslationIntent& value) { text(value.owner_id); }
    void read(const JointTranslationIntent& value) {
        sequence(value.rigid_boundary_ids); sequence(value.rigid_stroke_ids); sequence(value.partial_wall_ids);
        sequence(value.dimension_ids); sequence(value.annotation_translations); sequence(value.reference_translations);
        sequence(value.owner_translations); sequence(value.dimension_translations); sequence(value.owner_transformations);
    }
    void read(const DistoMeasurementAttachment& value) {
        text(value.owner_id);
        const auto& record = value.record;
        text(record.reading_id); text(record.target_field); text(record.unit); text(record.captured_at);
        text(record.model); text(record.firmware); text(record.transport); text(record.provenance);
    }
    void read(const ApplyBoundaryConstraintChanges& value) {
        text(value.message); sequence(value.boundary_edits); sequence(value.entity_changes);
        sequence(value.wall_edits); sequence(value.physical_entity_changes); sequence(value.exterior_source_edits);
        sequence(value.supplemental_entity_changes); sequence(value.supplemental_asset_changes);
        sequence(value.measured_stroke_edits); sequence(value.dimension_placement_moves);
        sequence(value.selection_entity_changes); sequence(value.room_review_additional_intents);
        optional(value.exterior_corner_move); optional(value.wall_split); optional(value.exterior_segment_resize);
        optional(value.exterior_segment_arc); optional(value.rigid_group_transform); optional(value.joint_translation);
        optional(value.disto_measurement); optional(value.wall_merge);
        optional(value.wall_group_scale); read(value.independent_drawing_removal_intent);
        read(value.mixed_selection_removal_intent);
        read(value.ordinary_selection_removal_intent);
        read(value.room_review_intent); read(value.room_review_geometry_proof);
        read(value.phase_room_review_intent); read(value.phase_constraint_authoring_intent);
    }
    void aliases(const PhysicalWallJoinRemovalEntities& entities) {
        // The token scan has already bounded every raw roster before this
        // established, geometry-free namespace derivation allocates/sorts it.
        for (const auto& [id, entity] : entities) {
            if (entity.type == kAnnotationEntityType) {
                const auto state = entity.properties.find("state");
                if (state != entity.properties.end() && state->is_object())
                    for (const auto* key : {"labels", "symbols"}) {
                        const auto children = state->find(key);
                        if (children != state->end() && children->is_array()) count(children->size());
                    }
            }
            if (entity.type != "assembly_model") continue;
            const auto model = entity.properties.find("model");
            if (model == entity.properties.end() || !model->is_object()) continue;
            const auto instances = model->find("instances");
            if (instances == model->end() || !instances->is_array()) continue;
            count(instances->size());
            for (const auto& instance : *instances) {
                const auto local = instance.is_object() ? instance.find("id") : instance.end();
                if (local == instance.end() || !local->is_string()) continue;
                const auto& local_id = local->get_ref<const std::string&>();
                constexpr std::size_t separator_bytes = 10; // ":instance:"
                if (id.size() > byte_limit - bytes || separator_bytes > byte_limit - bytes - id.size() ||
                    local_id.size() > byte_limit - bytes - id.size() - separator_bytes)
                    reject("retained component alias string budget exceeded");
                text(id + ":instance:" + local_id);
            }
        }
        for (const auto& [binding, alias] : embedded_assembly_presentation_ids(entities)) {
            (void)binding; text(alias);
        }
    }
};

Ids fresh_join_identities(const std::vector<std::string>& supplied) {
    if (supplied.size()>maximum_affected_walls) reject("fresh join identity budget exceeded");
    Ids fresh;
    for (const auto& id:supplied) {
        identity(id);
        if (!fresh.insert(id).second) reject("duplicate fresh join identity: "+id);
    }
    return fresh;
}

Ids fresh_join_identities(const PhysicalWallJoinRemovalAdditionalIdentities& supplied) {
    if (supplied.size() > maximum_affected_walls) reject("fresh join owner budget exceeded");
    // Bound all counts before token allocations or source/history traversal.
    std::size_t count{};
    for (const auto& [owner, destinations] : supplied) {
        identity(owner);
        if (destinations.empty()) reject("fresh join owner requires a nonempty destination list: " + owner);
        if (destinations.size() > maximum_affected_walls - count) reject("fresh join identity budget exceeded");
        count += destinations.size();
    }
    Ids fresh;
    for (const auto& [owner, destinations] : supplied) {
        (void)owner;
        for (const auto& id : destinations) {
            identity(id);
            if (supplied.contains(id)) reject("fresh join identity aliases a supplied source join key: " + id);
            if (!fresh.insert(id).second) reject("duplicate fresh join identity: " + id);
        }
    }
    return fresh;
}

void reserve_join_envelope_names(LifetimeReservations& reservations) {
    // Fixed envelope fields are reservations even when the corresponding lane
    // is absent. JSON intent trees additionally reserve every actual key/value.
    for (const auto* name : {
        "id", "type", "properties", "required", "extensions", "version", "kind", "expected_revision",
        "entity", "entity_id", "entity_changes", "asset", "asset_id", "asset_changes", "message",
        "media_type", "sha256", "metadata", "bytes_hex", "metadata_sha256", "byte_count",
        "revision", "parent_revision", "source_revision", "action", "name", "entities", "assets",
        "undo_stack", "redo_stack", "document_id", "saved_revision", "named_revisions",
        "translation", "translations", "transformation", "transformations", "source_transformations",
        "boundary_id", "owner_id", "offset", "transform", "edit", "boundary_edits", "wall_edits",
        "wall_id", "baseline", "length_entry", "rigid_transform", "curve_construction", "wall_classification",
        "physical_entity_changes", "exterior_source_edits", "source_completion", "exterior_source_completion",
        "supplemental_entity_changes", "supplemental_asset_changes", "supplemental_source_completion",
        "supplemental_asset_reference_completion", "rigid_wall_transform_completion", "exterior_corner_move",
        "vertex_id", "target_position", "move_connected_objects", "measured_stroke_edits", "stroke_id",
        "authored_edit", "authored_length", "vertex_edits", "measured_source_completion", "wall_split",
        "second_wall_id", "fraction", "seam_constraint_id", "measured_owners", "physical_room_owners",
        "segment_id", "automatic_dimension_id", "new_segment_ids", "new_vertex_ids", "physical_room_completion",
        "physical_room_phase_completion", "physical_room_dimension_completion",
        "exterior_segment_resize", "exact_length", "fixed_endpoint", "move_boundary_chain", "exterior_segment_arc",
        "arc_construction", "dimension_id", "dimension_placement_moves", "dimension_placement_completion",
        "rigid_group_transform", "rigid_group_completion", "joint_translation", "joint_translation_completion",
        "rigid_boundary_ids", "rigid_stroke_ids", "partial_wall_ids", "dimension_ids", "presentation_offset",
        "annotation_translations", "child_id", "reference_translations", "reference_id", "owner_translations",
        "dimension_translations", "owner_transformations", "per_target_presentation_completion",
        "per_owner_translation_completion", "per_owner_rigid_completion", "per_owner_transform_completion",
        "wall_dimension_completion", "measured_stroke_transform_completion", "room_review_intent",
        "room_review_completion", "room_review_geometry_proof", "room_review_geometry_completion",
        "room_review_batch_completion", "room_review_additional_intents", "phase_room_review_intent",
        "phase_room_review_completion", "phase_constraint_authoring_intent", "phase_constraint_authoring_completion",
        "proof", "disto_measurement", "disto_measurement_completion", "record", "replace_existing",
        "reading_id", "target_field", "unit", "captured_at", "model", "firmware", "transport", "provenance",
        "wall_merge", "first_wall_id", "selection_entity_changes", "selection_completion", "curve_construction_completion",
        "independent_drawing_removal_completion", "independent_drawing_removal_intent",
        "wall_group_scale_completion", "wall_group_scale", "pivot_m", "scale", "move_connected_walls",
        "mixed_selection_removal_completion", "mixed_selection_removal_intent", "ordinary", "ordinary_command",
        "other", "geometry_proof", "hosted_opening", "wall_geometry",
        "ordinary_selection_removal_completion", "ordinary_selection_removal_intent", "selection", "base_command",
        "members", "roof_id", "opening_id", "child_command", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "stage_snapshot_digest", "stage_authoring_digest",
        "target_id", "target_length_metres", "move_connected", "new_vertex_id", "new_segment_id", "new_dimension_id",
        "replacement_segments", "replacement_authoring", "replacement_properties", "replacement_dimension_ids",
        "replacement_child_mapping", "replacement_removed_reference_ids", "replacement_wall_source_ids",
        "replacement_linework_sources", "fresh_topology", "allow_automatic_angle_removal", "physical_wall_room_repair",
        "wall_source_translation", "selected_wall_id", "interior_witness", "reviewed_source_lineage",
        "expected_descriptor_digest", "complete_hosted_removal", "complete_join_removal",
        "additional_join_identities", "wall_ids"}) reservations.text(name);
}

void reserve_join_entity_map(LifetimeReservations& reservations, const PhysicalWallJoinRemovalEntities& source) {
    for (const auto& [id, entity] : source) {
        if (id.empty() || entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("retained source requires actual identified entity envelopes");
        reservations.text(id); reservations.read(entity);
    }
}

void reserve_join_history(LifetimeReservations& reservations, const PhysicalWallJoinRemovalEntities& source,
    const std::vector<RevisionRecord>& history, std::size_t preceding_records) {
    constexpr std::size_t maximum_history_records = 4096;
    if (preceding_records > history.size() || preceding_records > maximum_history_records)
        reject("retained identity history prefix is invalid or exceeds the record budget");
    if (source.size() > maximum_entities) reject("actual identity source entity budget exceeded");
    reservations.count(source.size());
    // Admit all map counts before any JSON/string traversal or alias derivation.
    for (std::size_t index = 0; index < preceding_records; ++index) {
        const auto& record = history[index];
        if (record.entities.size() > maximum_entities || record.assets.size() > maximum_entities)
            reject("retained identity entity/asset map budget exceeded");
        reservations.count(record.entities.size()); reservations.count(record.assets.size());
    }
    reserve_join_envelope_names(reservations);
    reserve_join_entity_map(reservations, source);
    for (std::size_t index = 0; index < preceding_records; ++index) {
        const auto& record = history[index];
        reservations.text(record.action); reservations.optional(record.name);
        reserve_join_entity_map(reservations, record.entities);
        for (const auto& [id, asset] : record.assets) {
            reservations.text(id); reservations.read(asset);
        }
        reservations.optional(record.boundary_translation); reservations.optional(record.boundary_transform);
        reservations.optional(record.boundary_geometry_edit); reservations.optional(record.boundary_constraint_changes);
        reservations.optional(record.boundary_translations); reservations.optional(record.boundary_transforms);
        reservations.optional(record.phase_entity_import);
    }
    // Every raw JSON/proof token across the prefix is bounded before invoking
    // the namespace helper. Reserve raw aliases as well as escaped actual ones.
    reservations.aliases(source);
    for (std::size_t index = 0; index < preceding_records; ++index) reservations.aliases(history[index].entities);
}

bool touches(const Json& value, const Ids& ids) {
    if (ids.empty()) return false;
    Strings strings; strings.read(value);
    return std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return strings.values.contains(id); });
}
bool contains(const std::vector<std::string>& rows, const std::string& id) {
    return std::find(rows.begin(), rows.end(), id) != rows.end();
}
void remove_ids(Json& rows, const Ids& removed) {
    auto kept = Json::array();
    for (const auto& row : rows) if (!removed.contains(row.get<std::string>())) kept.push_back(row);
    rows = std::move(kept);
}

struct Phases {
    std::map<std::string, ModelPhases, std::less<>> models;
    std::map<std::string, std::string, std::less<>> memberships;
    std::map<std::string, ModelPhase, std::less<>> active_states;
    Ids inactive;
    explicit Phases(const PhysicalWallJoinRemovalEntities& source) {
        std::size_t work{};
        const auto add = [&](std::size_t count) {
            if (count > maximum_geometry_work - work) reject("phase source work budget exceeded");
            work += count;
        };
        for (const auto& [id, entity] : source) if (entity.type == "model_phases") {
            if (models.size() >= 128) reject("phase registry budget exceeded");
            const auto& raw = entity.properties.at("model");
            // Bound all rows before canonical codec copying/sorting.
            add(raw.at("entity_ids").size()); add(raw.at("baseline_ids").size());
            if (raw.at("alternatives").size() > 128) reject("phase alternative budget exceeded");
            for (const auto& alternative : raw.at("alternatives")) {
                add(alternative.at("proposed_ids").size()); add(alternative.at("demolished_ids").size());
            }
            auto model = ModelPhases::from_json(raw);
            const auto active = model.active_state();
            for (const auto& owner : model.entity_ids()) {
                const auto found = source.find(owner);
                if (found == source.end() || !is_model_phase_entity_type(found->second.type))
                    reject("phase registry has missing/unsupported actual owner: " + owner);
                if (!memberships.emplace(owner, id).second) reject("overlapping actual phase membership: " + owner);
                const auto state = active.find(owner);
                if (state == active.end() || state->second == ModelPhase::demolished) inactive.insert(owner);
                else active_states.emplace(owner, state->second);
            }
            models.emplace(id, std::move(model));
        }
    }
    std::string membership(const std::string& id) const {
        const auto found = memberships.find(id);
        return found == memberships.end() ? std::string{} : found->second;
    }
    void mutable_owner(const PhysicalWallJoinRemovalEntities& source, const std::string& id) const {
        if (source.at(id).required) reject("required actual owner cannot be changed: " + id);
        if (inactive.contains(id)) reject("affected owner is inactive in its actual saved design: " + id);
        const auto registry = membership(id);
        if (registry.empty()) return;
        const auto& model = models.at(registry);
        if (!model.alternatives().empty() && contains(model.baseline_ids(), id))
            reject("shared baseline owner with retained alternatives requires typed phase removal: " + id);
        for (const auto& alternative : model.alternatives()) {
            if (model.active_alternative() == std::optional<std::string>{alternative.id}) continue;
            if (contains(alternative.proposed_ids, id) || contains(alternative.demolished_ids, id))
                reject("affected owner has protected other-alternative usage: " + id);
        }
    }
};

// Mask only codec-qualified references/declarations for inspection. Local
// sheet/view/overlay/annotation identities do not confer document ownership.
Entity reference_remainder(Entity entity, const Ids& affected,const Ids& retained_views,bool& presentation) {
    auto& p = entity.properties;
    if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model"));
        auto& raw = p.at("model");
        remove_ids(raw.at("entity_ids"), affected); remove_ids(raw.at("baseline_ids"), affected);
        for (auto& alternative : raw.at("alternatives")) {
            remove_ids(alternative.at("proposed_ids"), affected);
            remove_ids(alternative.at("demolished_ids"), affected);
        }
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("id");
            if (view.contains("object_ids")) {
                if (touches(view.at("object_ids"), affected)) presentation = true;
                remove_ids(view.at("object_ids"), affected);
            }
            auto& appearance = view.at("presentation");
            if (appearance.contains("appearance") && !appearance.at("appearance").is_null())
                for (auto& row : appearance.at("appearance").at("objects")) {
                    if (affected.contains(row.at("object_id").get<std::string>())) {
                        presentation = true; row.erase("object_id");
                    }
                }
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                row.erase("id");
                if (row.contains("object_id") && row.at("object_id").is_string() &&
                    affected.contains(row.at("object_id").get<std::string>())) {
                    presentation = true; row.erase("object_id");
                }
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) {
                    auto& binding = row.at("dimension_binding");
                    if (affected.contains(binding.at("object_id").get<std::string>())) {
                        presentation = true; binding.erase("object_id");
                    }
                }
            }
        }
        auto& model = p.at("model");
        model.erase("sheet_order");
        for (auto& sheet : model.at("sheets")) {
            sheet.erase("id");
            for (auto& viewport : sheet.at("viewports")) {
                viewport.erase("id"); viewport.erase("view_id");
            }
            if (sheet.contains("revisions")) for (auto& revision : sheet.at("revisions")) revision.erase("id");
            for (auto& callout : sheet.at("callouts")) {
                callout.erase("id"); callout.erase("target_sheet_id"); callout.erase("target_viewport_id");
            }
            for (auto& schedule : sheet.at("schedules")) schedule.erase("id");
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (const auto* collection : {"labels", "symbols"})
            for (auto& row : p.at("state").at(collection)) row.erase("id");
        for (auto& row : p.at("state").at("overrides")) {
            if (row.at("target_kind") == "object" && affected.contains(row.at("target_id").get<std::string>())) {
                presentation = true; row.erase("target_id");
            }
            else if (row.at("target_kind") == "output_view" &&
                retained_views.contains(row.at("target_id").get<std::string>())) row.erase("target_id");
        }
    } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (decoded.supported() && affected.contains(decoded.dimension->boundary_id)) {
            presentation = true; p.at("target").erase("entity_id");
        }
    } else if (entity.type=="constraint") {
        const auto decoded=decode_constraint_entity(entity);
        if (decoded.supported()) for (auto& binding:p.at("bindings")) {
            if (affected.contains(binding.at("owner_id").get<std::string>())) {
                presentation=true;binding.erase("owner_id");
            }
        }
    }
    return entity;
}

struct Derivation {
    PhysicalWallJoinRemovalPlan plan;
    std::map<std::string, std::string, std::less<>> memberships;
};
Derivation derive(const PhysicalWallJoinRemovalEntities& source, const std::vector<std::string>& selected) {
    Derivation result;
    auto& plan = result.plan;
    plan.selected_wall_ids = selected;
    try {
        (void)occupied_strings(source);
        if (selected.empty() || selected.size() > 128) reject("requires 1..128 explicit actual selected walls");
        std::sort(plan.selected_wall_ids.begin(), plan.selected_wall_ids.end());
        if (std::adjacent_find(plan.selected_wall_ids.begin(), plan.selected_wall_ids.end()) != plan.selected_wall_ids.end())
            reject("selected wall identities must be unique");
        const Ids seeds(plan.selected_wall_ids.begin(), plan.selected_wall_ids.end());
        const Phases phases(source);
        result.memberships = phases.memberships;
        for (const auto& id : seeds) {
            identity(id);
            if (!source.contains(id) || source.at(id).type != "wall") reject("selection must identify actual walls: " + id);
            phases.mutable_owner(source, id);
        }
        std::map<std::string, WallJoin, std::less<>> joins;
        Ids admitted_walls, affected_joins;
        for (const auto& [id, entity] : source) if (entity.type == "wall_join" &&
            (touches(entity.properties, seeds) || touches(entity.extensions, seeds))) {
            const auto join = parse_wall_join(entity.properties, id);
            if (std::none_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& member) { return seeds.contains(member); }))
                reject("opaque affected wall join reference has no removal codec: " + id);
            phases.mutable_owner(source, id);
            if (touches(entity.extensions, seeds)) reject("affected join extensions reference removed walls opaquely: " + id);
            affected_joins.insert(id); joins.emplace(id, join);
            admitted_walls.insert(join.wall_ids.begin(), join.wall_ids.end());
        }
        if (admitted_walls.size() > maximum_affected_walls) reject("affected wall expansion budget exceeded");
        // A surviving/selected member cannot remain owned by an unrelated join,
        // including a parked or alternative-owned join.
        std::map<std::string, std::string, std::less<>> joined;
        for (const auto& [id, entity] : source) if (entity.type == "wall_join" &&
            entity.properties.contains("wall_ids") && touches(entity.properties.at("wall_ids"), admitted_walls)) {
            const auto join = parse_wall_join(entity.properties, id);
            for (const auto& member : join.wall_ids) if (admitted_walls.contains(member))
                if (!joined.emplace(member, id).second) reject("affected wall remains owned by multiple actual joins: " + member);
        }
        // Inspect references before any kernel work. Known presentation remains
        // an explicit enclosing-operation obligation; no opaque data is stripped.
        Ids retained_views;
        for (const auto& [id,entity]:source) {
            (void)id;
            if (entity.type!=kSheetViewEntityType) continue;
            const auto model=decode_sheet_view_entity(entity);
            for (const auto& view:model.views()) retained_views.insert(view.id);
        }
        for (const auto& [id, entity] : source) {
            if (!touches(entity.properties, affected_joins) && !touches(entity.extensions, affected_joins)) continue;
            bool presentation = false;
            const auto remainder = reference_remainder(entity, affected_joins,retained_views,presentation);
            if (touches(remainder.properties, affected_joins) || touches(remainder.extensions, affected_joins))
                reject("unsupported/opaque affected join reference must be preserved: " + id);
            if (presentation) {
                plan.presentation_cleanup_entity_ids.push_back(id);
                plan.diagnostics.push_back({id, "admitted join presentation requires enclosing complete-removal cleanup/review", false});
            }
        }
        std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
        for (const auto& [id, entity] : source) if (entity.type == "opening" && !phases.inactive.contains(id)) {
            const auto host = entity.properties.find("wall_id");
            if (host != entity.properties.end() && host->is_string() && admitted_walls.contains(host->get_ref<const std::string&>()))
                openings[host->get_ref<const std::string&>()].push_back(&entity);
        }
        std::size_t work{};
        const auto add = [&](std::size_t count) {
            if (count > maximum_geometry_work - work) reject("aggregate wall/contact geometry work budget exceeded");
            work += count;
        };
        std::map<std::string, Wall, std::less<>> walls;
        const std::vector<std::string> wall_ids(admitted_walls.begin(), admitted_walls.end());
        const auto effective = resolve_vertical_placements(source, wall_ids);
        for (const auto& id : admitted_walls) {
            if (!source.contains(id) || source.at(id).type != "wall") reject("join member is not an actual wall: " + id);
            if (phases.inactive.contains(id)) reject("affected join member is inactive: " + id);
            const auto& raw = source.at(id).properties;
            const auto layers = raw.contains("layers") ? raw.at("layers").size() : 0;
            if (layers > 32 || openings[id].size() > 128) reject("actual wall layer/opening budget exceeded: " + id);
            // Source shape, original union and final unions/contact admission.
            add(4 * (1 + layers) * (1 + openings[id].size()));
            Wall wall; std::string error;
            if (!read_document_wall(effective.at(id), openings[id], wall, error))
                reject("actual wall descriptor is unsupported: " + id + ": " + error);
            validate_wall_semantics(wall);
            walls.emplace(id, std::move(wall));
        }
        for (const auto& [id, join] : joins) {
            const auto registry = phases.membership(id);
            for (const auto& member : join.wall_ids) {
                const auto member_registry = phases.membership(member);
                if (!member_registry.empty() && member_registry != registry)
                    reject("affected join spans unrelated actual phase registries: " + id);
                // The join cannot change a parked/protected baseline wall's
                // ownership across another saved phase choice.
                if (!member_registry.empty()) {
                    if (phases.active_states.at(member) != phases.active_states.at(id))
                        reject("affected join spans different actual phase roles: " + id);
                }
            }
            add(2 * join.wall_ids.size() * join.wall_ids.size());
        }
        // Every bound and semantic failure above occurs before native kernels.
        std::map<std::string, TopoDS_Shape, std::less<>> shapes;
        for (const auto& [id, wall] : walls) {
            auto shape = make_wall(wall);
            const auto volume = solid_volume(shape);
            if (!std::isfinite(volume) || volume <= 0) reject("actual wall has no finite positive material: " + id);
            shapes.emplace(id, std::move(shape));
        }
        // Admit every complete original join before deriving any consequences.
        for (const auto& [id, join] : joins) {
            std::vector<Wall> members;
            for (const auto& member : join.wall_ids) members.push_back(walls.at(member));
            const auto volume = solid_volume(make_wall_join(join, members));
            if (!std::isfinite(volume) || volume <= 0) reject("actual complete join has no finite positive material: " + id);
        }
        for (const auto& [id, join] : joins) {
            plan.source_join_ids.push_back(id);
            std::vector<Wall> survivors;
            std::vector<TopoDS_Shape> survivor_shapes;
            for (const auto& member : join.wall_ids) if (!seeds.contains(member)) {
                survivors.push_back(walls.at(member)); survivor_shapes.push_back(shapes.at(member));
            }
            std::size_t retained_count{};
            auto& components = plan.surviving_join_components[id];
            for (const auto& indices : wall_shape_connected_components(survivors, survivor_shapes)) {
                auto& component = components.emplace_back();
                for (const auto index : indices) component.push_back(survivors.at(index).id);
                if (component.size() >= 2) {
                    ++retained_count;
                    std::vector<Wall> members;
                    for (const auto& member : component) members.push_back(walls.at(member));
                    // Consequence components must satisfy the full union contract,
                    // not merely graph connectivity.
                    const auto volume = solid_volume(make_wall_join(WallJoin{id, component, join.style}, members));
                    if (!std::isfinite(volume) || volume <= 0) reject("surviving join has no finite positive material: " + id);
                }
            }
            if (retained_count == 0) plan.retired_join_ids.push_back(id);
            else if (retained_count > 1) plan.additional_identity_counts.emplace(id, retained_count - 1);
        }
        if (!plan.retired_join_ids.empty()) {
            // Retiring a shadowing document ID can also change an unrelated
            // computed catalog alias, even though every catalog row is exact.
            auto namespace_candidate = source;
            for (const auto& id : plan.retired_join_ids) namespace_candidate.erase(id);
            const auto original_aliases=embedded_assembly_presentation_ids(source);
            const auto surviving_aliases=embedded_assembly_presentation_ids(namespace_candidate);
            std::map<std::string,AssemblyModel,std::less<>> catalogs;
            for (const auto& [key,alias]:original_aliases) {
                const auto after=surviving_aliases.find(key);
                if (after!=surviving_aliases.end() && after->second==alias) continue;
                if (!catalogs.contains(key.first))
                    catalogs.emplace(key.first,AssemblyModel::from_json(source.at(key.first).properties.at("model")));
                const auto& instances=catalogs.at(key.first).instances();
                const auto row=std::find_if(instances.begin(),instances.end(),[&](const auto& instance){return instance.id==key.second;});
                // The enclosing complete wall producer independently retires
                // exactly these actual wall-hosted qualified rows. Its final
                // namespace check still protects every surviving component.
                if (row!=instances.end() && row->placement && seeds.contains(row->placement->host_entity_id)) continue;
                reject("join retirement would change a surviving catalog presentation alias");
            }
        }
    } catch (const std::exception& error) {
        plan.diagnostics.push_back({{}, error.what(), true});
    }
    return result;
}

void complete_registries(PhysicalWallJoinRemovalEntities& candidate,
    const PhysicalWallJoinRemovalEntities& source, const Derivation& derived,
    const PhysicalWallJoinRemovalAdditionalIdentities& copies) {
    const Ids retired(derived.plan.retired_join_ids.begin(), derived.plan.retired_join_ids.end());
    for (const auto& [id, original] : source) if (original.type == "model_phases") {
        auto raw = original.properties.at("model");
        const auto model = ModelPhases::from_json(raw);
        Ids actual_retired;
        for (const auto& owner : retired) if (derived.memberships.contains(owner) && derived.memberships.at(owner) == id)
            actual_retired.insert(owner);
        bool changed = !actual_retired.empty();
        remove_ids(raw.at("entity_ids"), actual_retired); remove_ids(raw.at("baseline_ids"), actual_retired);
        for (auto& alternative : raw.at("alternatives")) if (model.active_alternative() && alternative.at("id") == *model.active_alternative()) {
            remove_ids(alternative.at("proposed_ids"), actual_retired);
            remove_ids(alternative.at("demolished_ids"), actual_retired);
        }
        for (const auto& [owner, destinations] : copies) {
            if (!derived.memberships.contains(owner) || derived.memberships.at(owner) != id) continue;
            const bool baseline = contains(model.baseline_ids(), owner);
            for (const auto& destination : destinations) {
                raw.at("entity_ids").push_back(destination);
                if (baseline) raw.at("baseline_ids").push_back(destination);
                else {
                    if (!model.active_alternative()) reject("split join lacks its source's actual active alternative");
                    for (auto& alternative : raw.at("alternatives")) if (alternative.at("id") == *model.active_alternative())
                        alternative.at("proposed_ids").push_back(destination);
                }
                changed = true;
            }
        }
        if (changed) {
            (void)ModelPhases::from_json(raw);
            candidate.at(id).properties.at("model") = std::move(raw);
        }
    }
}
void reserve_join_snapshot(const DocumentSnapshot& source,const Ids& fresh) {
    const auto& history = source.history();
    if (history.empty() || source.revision() >= history.size()) reject("actual snapshot revision is outside retained history");
    if (history.size() > 4096) reject("actual snapshot identity history record budget exceeded");
    LifetimeReservations reservations{fresh};
    if (source.assets().size() > maximum_entities || source.named_revisions().size() > 4096)
        reject("actual snapshot asset/named revision budget exceeded");
    reservations.count(source.assets().size()); reservations.count(source.named_revisions().size());
    reservations.text(source.document_id()); reservations.text(source.read_only_reason());
    for (const auto& [name, revision] : source.named_revisions()) { (void)revision; reservations.text(name); }
    for (const auto& [id, asset] : source.assets()) { reservations.text(id); reservations.read(asset); }
    reserve_join_history(reservations, source.entities(), history, history.size());
}
} // namespace

void validate_physical_wall_join_removal_identity_lifetime(
    const PhysicalWallJoinRemovalEntities& source, const std::vector<RevisionRecord>& history,
    std::size_t preceding_records, const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities) {
    if (additional_join_identities.empty()) return;
    const auto fresh = fresh_join_identities(additional_join_identities);
    LifetimeReservations reservations{fresh};
    reserve_join_history(reservations, source, history, preceding_records);
}

void validate_physical_wall_join_removal_identity_lifetime(const DocumentSnapshot& source,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities) {
    if (!additional_join_identities.empty()) reserve_join_snapshot(source,fresh_join_identities(additional_join_identities));
}

void validate_physical_wall_join_removal_identity_lifetime(const DocumentSnapshot& source,
    const std::vector<std::string>& fresh_join_ids) {
    if (!fresh_join_ids.empty()) reserve_join_snapshot(source,fresh_join_identities(fresh_join_ids));
}

bool PhysicalWallJoinRemovalPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.blocking; });
}

PhysicalWallJoinRemovalPlan inspect_physical_wall_join_removal(
    const PhysicalWallJoinRemovalEntities& source, const std::vector<std::string>& selected_wall_ids) {
    return derive(source, selected_wall_ids).plan;
}

PhysicalWallJoinRemovalEntities replay_physical_wall_join_removal(
    const PhysicalWallJoinRemovalEntities& source, const std::vector<std::string>& selected_wall_ids,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities) {
    const auto derived = derive(source, selected_wall_ids);
    if (!derived.plan.ready()) for (const auto& item : derived.plan.diagnostics) if (item.blocking) reject(item.reason);
    if (additional_join_identities.size() != derived.plan.additional_identity_counts.size())
        reject("requires exact source-derived additional join identity keys");
    auto occupied = occupied_strings(source);
    // Adding a document identity must not force an unrelated catalog instance's
    // computed presentation alias to change namespaces.
    for (const auto& [key, alias] : embedded_assembly_presentation_ids(source)) {
        (void)key; occupied.values.insert(alias);
    }
    Ids fresh;
    std::size_t count{};
    for (const auto& [owner, destinations] : additional_join_identities) {
        const auto expected = derived.plan.additional_identity_counts.find(owner);
        if (expected == derived.plan.additional_identity_counts.end() || destinations.size() != expected->second)
            reject("additional join slots differ from actual surviving clusters: " + owner);
        if (destinations.size() > maximum_affected_walls - count) reject("fresh join identity budget exceeded");
        count += destinations.size();
        for (const auto& destination : destinations) {
            identity(destination);
            if (occupied.values.contains(destination) || !fresh.insert(destination).second)
                reject("fresh join collides with actual global/local/opaque source token: " + destination);
        }
    }
    auto candidate = source;
    for (const auto& id : derived.plan.retired_join_ids) candidate.erase(id);
    for (const auto& [id, components] : derived.plan.surviving_join_components) {
        std::size_t index{};
        for (const auto& component : components) if (component.size() >= 2) {
            auto copy = source.at(id);
            copy.properties.at("wall_ids") = component;
            if (index == 0) candidate.at(id) = std::move(copy);
            else {
                copy.id = additional_join_identities.at(id).at(index - 1);
                const auto destination = copy.id;
                (void)parse_wall_join(copy.properties, destination);
                if (!candidate.emplace(destination, std::move(copy)).second) reject("split join insertion collided");
            }
            ++index;
        }
    }
    complete_registries(candidate, source, derived, additional_join_identities);
    if (candidate.size() > maximum_entities) reject("final source entity budget exceeded");
    (void)occupied_strings(candidate);
    return candidate;
}

} // namespace sketch
