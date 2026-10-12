#include "sketch/phase_assembly_catalog_edit_preparation.hpp"

#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/phase_assembly_instance_property_edit.hpp"
#include "sketch/retained_authoring_identities.hpp"
#include "sketch/retained_geometry_work.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <functional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
[[noreturn]] void reject(const char* message) {
    throw std::invalid_argument(std::string("Phase assembly catalog preparation: ") + message);
}
// Visit raw values before any codec, dump, digest or scratch copy. The outer
// budget accommodates both separately bounded producer allocation dialects.
struct RequestBudget {
    std::size_t bytes{}, nodes{};
    void charge(std::size_t amount) {
        if (amount > 32 * 1024 * 1024 - bytes) reject("request byte budget exceeded");
        bytes += amount;
    }
    void text(std::string_view value) {
        charge(2);
        for (const unsigned char c : value) charge(c == '"' || c == '\\' ? 2 : c < 32 ? 6 : 1);
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 524288) reject("request node/nesting budget exceeded");
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable request JSON");
        if (value.is_object()) {
            charge(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) { text(key); charge(1); read(child, depth + 1); }
        } else if (value.is_array()) {
            charge(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) read(child, depth + 1);
        } else if (value.is_string()) text(value.get_ref<const std::string&>());
        else charge(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid captured revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source digest must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) reject("invalid source digest");
}
void message(std::string_view value) {
    if (value.empty() || value.size() > 1024 || value.find('\0') != std::string_view::npos)
        reject("message must contain 1 to 1024 bytes without NUL");
    // Match the command codec's strict UTF-8 contract before any producer codec.
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) { ++index; continue; }
        std::size_t count{};
        unsigned char minimum = 0x80, maximum = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) count = 1;
        else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            if (first == 0xe0) minimum = 0xa0;
            if (first == 0xed) maximum = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            if (first == 0xf0) minimum = 0x90;
            if (first == 0xf4) maximum = 0x8f;
        } else reject("message is not valid UTF-8");
        if (count >= value.size() - index) reject("message is not valid UTF-8");
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if (second < minimum || second > maximum) reject("message is not valid UTF-8");
        for (std::size_t offset = 2; offset <= count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if (continuation < 0x80 || continuation > 0xbf) reject("message is not valid UTF-8");
        }
        index += count + 1;
    }
}
void admit_typed_allocations(RequestBudget& budget, const PhaseAssemblyCatalogEditAuthoring& authoring,
    const PhaseAssemblyCatalogEditPresentationAllocation& presentation) {
    const auto& allocation = authoring.allocation;
    if (allocation.roots.size() > 4096 || allocation.membership_entity_ids.size() > 4096 ||
        presentation.overlay_ids.size() > 4096 || presentation.dimension_ids.size() > 4096)
        reject("typed allocation count exceeds bound");
    const auto text = [&](const std::string& value, std::size_t maximum) {
        if (value.size() > maximum) reject("typed allocation identity exceeds bound");
        budget.text(value);
    };
    budget.charge(256); // Closed envelope and collection punctuation/field names.
    for (const auto& [root, identity] : allocation.roots) {
        if (!root.part_path.empty()) reject("typed allocation must name an exact root");
        budget.charge(256);
        text(root.catalog_id, 128); text(root.instance_id, 4096);
        if (root.document_entity_id) text(*root.document_entity_id, 128);
        text(identity.private_catalog_id, 128);
        if (identity.replacement_entity_id) text(*identity.replacement_entity_id, 128);
        if (identity.replacement_instance_id) text(*identity.replacement_instance_id, 128);
    }
    for (const auto& [registry, membership] : allocation.membership_entity_ids) {
        budget.charge(128); text(registry, 128); text(membership, 128);
    }
    for (const auto& [original, proposed] : presentation.overlay_ids) {
        budget.charge(128); text(original, 128); text(proposed, 128);
    }
    for (const auto& [original, proposed] : presentation.dimension_ids) {
        budget.charge(128); text(original, 128); text(proposed, 128);
    }
}
void admit_request(const Json& value) {
    RequestBudget{}.read(value);
    fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "authoring", "presentation_allocation", "message"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1) reject("unsupported request version");
    (void)revision(value.at("expected_revision"));
    for (const auto* name : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(name));
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    if (!value.at("message").is_string()) reject("message must be a string");
    message(value.at("message").get_ref<const std::string&>());
}
// Private core-only preflight. Limits bound the complete retained inventory,
// rather than discounting undone records or shared logical payload rows.
// JSON is admitted before any history codec; catalog graph admission follows
// the complete raw pass. No asset bytes are copied or eagerly materialized.
struct SourceBudget {
    static constexpr std::size_t byte_limit = 64 * 1024 * 1024;
    static constexpr std::size_t node_limit = 2'000'000;
    static constexpr std::size_t work_limit = 67'108'864;
    std::size_t bytes{}, nodes{}, work{}, entity_rows{}, asset_rows{};
    std::uint64_t logical_asset_bytes{}, distinct_asset_bytes{};
    std::set<const std::byte*, std::less<const std::byte*>> asset_buffers;
    static void add(std::size_t& used, std::size_t amount, std::size_t limit, const char* reason) {
        if (amount > limit - used) reject(reason);
        used += amount;
    }
    void fixed(std::size_t amount) { add(bytes, amount, byte_limit, "retained source byte capacity exceeded"); }
    void text(const std::string& value) {
        if (value.size() > (byte_limit - bytes) / 6) reject("retained source text capacity exceeded");
        fixed(2 + value.size() * 6);
    }
    void read(const std::string& value) { text(value); }
    void read(double value) { if (!std::isfinite(value)) reject("nonfinite retained typed number"); }
    void read(const Vec2& value) { read(value.x); read(value.y); }
    void read(const Vec3& value) { read(value.x); read(value.y); read(value.z); }
    void read(const Segment& value) { read(value.start); read(value.end); read(value.sweep_radians); }
    void read(const PlanarTransform& value) { read(value.pivot); read(value.rotation_radians); read(value.offset); }
    void raw(const Json& value, std::size_t depth = 0) {
        if (depth > 64) reject("retained source nesting capacity exceeded");
        add(nodes, 1, node_limit, "retained source node capacity exceeded"); fixed(32);
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable retained source JSON");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_object() || value.is_array()) {
            // Raw JSON traversal/serialization is linear, including opaque
            // metadata. Catalog graph work is admitted separately after the
            // complete raw walk; it must not be invented for an opaque array.
            if (value.size() > node_limit - nodes) reject("retained source node capacity exceeded");
            add(work, value.size(), work_limit, "retained source traversal work capacity exceeded");
            if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); raw(child, depth + 1); }
            else for (const auto& child : value) raw(child, depth + 1);
        }
    }
    void read(const Json& value) { raw(value); }
    template<class T> void optional(const std::optional<T>& value) { if (value) { fixed(sizeof(T)); read(*value); } }
    template<class T> void sequence(const std::vector<T>& values) {
        if (values.size() > node_limit) reject("retained typed vector capacity exceeded");
        add(work, values.size(), work_limit, "retained typed vector traversal capacity exceeded");
        for (const auto& value : values) { fixed(sizeof(T)); read(value); }
    }
    void read(const Entity& value) { text(value.id); text(value.type); read(value.properties); read(value.extensions); }
    void read(const Asset& value) {
        text(value.id); text(value.media_type); text(value.sha256); read(value.metadata);
        const auto size = value.bytes.size();
        if (size > 256ULL * 1024 * 1024 - logical_asset_bytes) reject("retained logical asset hashing capacity exceeded");
        logical_asset_bytes += size;
        if (size && asset_buffers.insert(value.bytes.data()).second) {
            if (size > ProjectStore::maximum_total_asset_bytes - distinct_asset_bytes) reject("distinct retained asset capacity exceeded");
            distinct_asset_bytes += size;
        }
    }
    void read(const EntityChange& value) { text(value.entity_id); read(value.entity); }
    void read(const AssetChange& value) { text(value.asset_id); read(value.asset); }
    void read(const Quantity& value) { read(value.metres); text(value.original_expression); }
    void read(const AngleInput& value) { read(value.radians); text(value.original_expression); text(value.normalized_expression); }
    void read(const ConstructionReceipt& value) {
        text(value.segment_id); read(value.start); optional(value.chord_end); optional(value.closure_delta);
        if (value.chord_input) { read(value.chord_input->length); read(value.chord_input->heading); }
        optional(value.distance); optional(value.heading); optional(value.rise); optional(value.run); optional(value.turn);
        optional(value.angle); optional(value.height); optional(value.arc_length); optional(value.tangent); optional(value.sweep);
    }
    void read(const PhysicalWallRoomRepairIntent& value) {
        text(value.selected_wall_id); text(value.expected_descriptor_digest); read(value.reviewed_source_lineage); read(value.interior_witness);
    }
    void read(const BoundaryGeometryEdit& value) {
        text(value.boundary_id); text(value.target_id); text(value.new_vertex_id); text(value.new_segment_id); text(value.new_dimension_id);
        read(value.target_position); read(value.target_length_metres); read(value.fraction);
        read(value.replacement_segments); read(value.replacement_authoring); read(value.replacement_properties); read(value.replacement_child_mapping);
        sequence(value.replacement_dimension_ids); sequence(value.replacement_removed_reference_ids); sequence(value.replacement_wall_source_ids);
        optional(value.arc_construction); optional(value.replacement_linework_sources); optional(value.physical_wall_room_repair); optional(value.wall_source_translation);
    }
    void read(const BoundaryTranslation& value) { text(value.boundary_id); read(value.offset); }
    void read(const BoundaryTransformation& value) { text(value.boundary_id); read(value.transform); }
    void read(const RigidOwnerTransformation& value) { text(value.owner_id); read(value.transform); }
    void read(const SimilarityOwnerTransformation& value) { text(value.owner_id); read(value.transform); read(value.scale); }
    void read(const TranslateBoundaries& value) { text(value.message); sequence(value.translations); sequence(value.entity_changes); }
    void read(const TransformBoundaries& value) {
        text(value.message); sequence(value.transformations); sequence(value.entity_changes); sequence(value.source_transformations);
    }
    void read(const ConstraintWallGeometryEdit& value) {
        text(value.wall_id); read(value.baseline); optional(value.rigid_transform);
        optional(value.length_entry); optional(value.curve_construction); optional(value.wall_classification);
    }
    void read(const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& value) {
        text(value.stroke_id); optional(value.authored_edit); optional(value.authored_length); optional(value.rigid_transform); sequence(value.vertex_edits);
    }
    void read(const ApplyBoundaryConstraintChanges::DimensionPlacementMove& value) { text(value.dimension_id); read(value.offset); }
    void read(const ExteriorCornerMoveIntent& value) { text(value.boundary_id); text(value.vertex_id); read(value.target_position); }
    void read(const ExteriorSegmentResizeIntent& value) { text(value.boundary_id); text(value.segment_id); read(value.exact_length); }
    void read(const ExteriorSegmentArcIntent& value) { text(value.boundary_id); text(value.segment_id); read(value.arc_construction); }
    void read(const WallSplitMeasuredOwnerIds& value) {
        text(value.boundary_id); text(value.vertex_id); text(value.segment_id); text(value.automatic_dimension_id);
    }
    void read(const WallSplitPhysicalRoomIds& value) { text(value.boundary_id); sequence(value.new_segment_ids); sequence(value.new_vertex_ids); }
    void read(const WallSplitIntent& value) {
        text(value.wall_id); text(value.second_wall_id); text(value.seam_constraint_id); read(value.fraction);
        sequence(value.measured_owners); sequence(value.physical_room_owners);
    }
    void read(const WallMergeIntent& value) { text(value.first_wall_id); text(value.second_wall_id); }
    void read(const JointAnnotationTranslationIntent& value) { text(value.owner_id); text(value.child_id); read(value.offset); }
    void read(const JointReferenceTranslationIntent& value) { text(value.reference_id); read(value.offset); }
    void read(const JointOwnerTranslationIntent& value) { text(value.owner_id); read(value.offset); }
    void read(const JointTranslationIntent& value) {
        read(value.offset); optional(value.presentation_offset);
        sequence(value.rigid_boundary_ids); sequence(value.rigid_stroke_ids); sequence(value.partial_wall_ids); sequence(value.dimension_ids);
        sequence(value.annotation_translations); sequence(value.reference_translations); sequence(value.owner_translations);
        sequence(value.dimension_translations); sequence(value.owner_transformations); sequence(value.owner_similarities);
    }
    void read(const WallGroupScaleIntent& value) { sequence(value.wall_ids); read(value.pivot); read(value.scale); }
    void read(const DistoMeasurementAttachment& value) {
        text(value.owner_id); const auto& row = value.record; read(row.value);
        text(row.reading_id); text(row.target_field); text(row.unit); text(row.captured_at); text(row.model);
        text(row.firmware); text(row.transport); text(row.provenance);
    }
    void read(const PhaseEntityImportProof& value) {
        text(value.message); sequence(value.registry_ids); sequence(value.entity_ids); sequence(value.asset_ids); sequence(value.reviewed_existing_hierarchy_ids);
    }
    void read(const ApplyBoundaryConstraintChanges& value) {
        text(value.message); sequence(value.boundary_edits); sequence(value.wall_edits); sequence(value.entity_changes);
        sequence(value.physical_entity_changes); sequence(value.exterior_source_edits); sequence(value.supplemental_entity_changes);
        sequence(value.supplemental_asset_changes); sequence(value.measured_stroke_edits); sequence(value.dimension_placement_moves);
        sequence(value.selection_entity_changes); sequence(value.room_review_additional_intents);
        optional(value.exterior_corner_move); optional(value.exterior_segment_resize); optional(value.exterior_segment_arc);
        optional(value.wall_split); optional(value.wall_merge); optional(value.rigid_group_transform); optional(value.joint_translation);
        optional(value.wall_group_scale); optional(value.disto_measurement);
        read(value.room_review_intent); read(value.room_review_geometry_proof); read(value.phase_room_review_intent);
        read(value.phase_constraint_authoring_intent); read(value.independent_drawing_removal_intent); read(value.phase_selection_removal_intent);
        read(value.ordinary_selection_removal_intent); read(value.mixed_selection_removal_intent); read(value.mixed_selection_edit_intent);
        read(value.clipboard_placement_intent); read(value.assembly_catalog_edit_intent); read(value.phase_assembly_catalog_edit_intent);
        read(value.qualified_embedded_assembly_copy_intent);
    }
    void navigation(const std::vector<Revision>& values, std::size_t history_size) {
        if (values.size() > history_size) reject("retained navigation capacity exceeded");
        fixed(values.size() * sizeof(Revision));
        for (const auto value : values) if (value >= history_size) reject("retained navigation revision is absent");
    }
};
// Only semantic source-bound proofs authenticate/revisit complete preceding
// inventories during restore. Ordinary edits, names and navigation records do
// not each replay the whole history. Count nested room-review lanes as well;
// raw admission has already bounded every vector before this accounting.
std::size_t retained_prefix_uses(const RevisionRecord& record) {
    if (!record.boundary_constraint_changes) return 0;
    const auto& proof = *record.boundary_constraint_changes;
    std::size_t count = proof.room_review_additional_intents.size();
    for (const auto* intent : {&proof.room_review_intent, &proof.room_review_geometry_proof,
        &proof.phase_room_review_intent, &proof.phase_constraint_authoring_intent,
        &proof.independent_drawing_removal_intent, &proof.phase_selection_removal_intent,
        &proof.ordinary_selection_removal_intent, &proof.mixed_selection_removal_intent,
        &proof.mixed_selection_edit_intent, &proof.clipboard_placement_intent,
        &proof.phase_assembly_catalog_edit_intent, &proof.qualified_embedded_assembly_copy_intent})
        if (!intent->is_null()) ++count;
    return count;
}
struct ReplayInventory {
    std::size_t bytes{}, nodes{}, work{};
    std::uint64_t asset_bytes{};
    explicit ReplayInventory(const SourceBudget& budget)
        : bytes(budget.bytes), nodes(budget.nodes), work(budget.work), asset_bytes(budget.logical_asset_bytes) {}
};
struct ReplayBudget {
    // Reserve producer/digest/raw-validation passes for the full inventory,
    // plus that same reserve for each actual retained source-bound prefix.
    static constexpr std::size_t passes = 16;
    static constexpr std::size_t byte_limit = 512ULL * 1024 * 1024;
    static constexpr std::uint64_t asset_limit = 8ULL * 1024 * 1024 * 1024;
    std::size_t bytes{}, nodes{}, work{};
    std::uint64_t asset_bytes{};
    static void charge(std::size_t& used, std::size_t amount, std::size_t count, std::size_t limit) {
        if (amount > (limit - used) / count) reject("complete retained history replay capacity exceeded");
        used += amount * count;
    }
    void reserve(const ReplayInventory& inventory, std::size_t uses = 1) {
        if (!uses) return;
        const auto count = uses * passes; // <= (4096 + 11) * 16 after raw admission.
        charge(bytes, inventory.bytes, count, byte_limit);
        charge(nodes, inventory.nodes, count, SourceBudget::work_limit);
        charge(work, inventory.work, count, SourceBudget::work_limit);
        if (inventory.asset_bytes > (asset_limit - asset_bytes) / count)
            reject("complete retained asset replay capacity exceeded");
        asset_bytes += inventory.asset_bytes * count;
    }
};
void admit_source(const DocumentSnapshot& source) {
    const auto& history = source.history();
    if (history.empty() || history.size() > ProjectStore::maximum_revision_count || source.revision() >= history.size() ||
        (source.saved_revision_optional() && (*source.saved_revision_optional() > source.revision() || *source.saved_revision_optional() >= history.size())))
        reject("invalid source history/save metadata");
    SourceBudget budget; ReplayBudget replay;
    budget.text(source.document_id()); budget.text(source.read_only_reason());
    if (source.named_revisions().size() > history.size()) reject("named revision capacity exceeded");
    for (const auto& [name, revision] : source.named_revisions()) {
        budget.text(name); if (revision >= history.size()) reject("named revision is absent");
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto& record = history[index];
        const ReplayInventory preceding_prefix(budget);
        if (record.revision != index || (record.parent_revision && *record.parent_revision >= index) ||
            (record.source_revision && *record.source_revision >= index)) reject("retained history ancestry is invalid");
        budget.fixed(sizeof(RevisionRecord)); budget.text(record.action); if (record.name) budget.text(*record.name);
        SourceBudget::add(budget.entity_rows, record.entities.size(), ProjectStore::maximum_entity_rows, "retained entity row capacity exceeded");
        SourceBudget::add(budget.asset_rows, record.assets.size(), ProjectStore::maximum_asset_rows, "retained asset row capacity exceeded");
        for (const auto& [id, entity] : record.entities) {
            if (id.empty() || id != entity.id || !entity.properties.is_object() || !entity.extensions.is_object()) reject("invalid actual source entity envelope");
            budget.read(entity);
        }
        for (const auto& [id, asset] : record.assets) {
            if (id.empty() || id != asset.id || !asset.metadata.is_object()) reject("invalid actual source asset envelope");
            budget.read(asset);
        }
        budget.navigation(record.undo_stack, history.size()); budget.navigation(record.redo_stack, history.size());
        budget.optional(record.boundary_translation); budget.optional(record.boundary_transform); budget.optional(record.boundary_geometry_edit);
        budget.optional(record.boundary_constraint_changes); budget.optional(record.boundary_translations); budget.optional(record.boundary_transforms);
        budget.optional(record.phase_entity_import);
        replay.reserve(preceding_prefix, retained_prefix_uses(record));
    }
    replay.reserve(ReplayInventory(budget));
    // Complete raw admission precedes schema-owned topology work and codecs.
    // Opaque arrays and ID lists have linear traversal cost. Actual boundaries,
    // holes and retained geometry operations separately reserve their solve
    // cost, including preceding source prefixes revisited by semantic proofs.
    std::size_t geometry_work{};
    const RetainedGeometryReserveProduct geometry_product = [&](std::size_t a, std::size_t b) {
        if (b && a > (SourceBudget::work_limit - geometry_work) / b)
            reject("retained geometry work capacity exceeded");
        geometry_work += a * b;
    };
    const auto geometry_edit = [&](const BoundaryGeometryEdit& edit) {
        reserve_retained_geometry_replay_work(edit.replacement_segments, geometry_product, true, 1);
        reserve_retained_geometry_replay_work(edit.replacement_authoring, geometry_product, false, 1);
        if (edit.physical_wall_room_repair)
            reserve_retained_geometry_replay_work(edit.physical_wall_room_repair->reviewed_source_lineage,
                geometry_product, false, 1);
    };
    for (const auto& record : history) {
        const auto prefix_uses = retained_prefix_uses(record);
        if (prefix_uses) ReplayBudget::charge(replay.work, geometry_work,
            prefix_uses * ReplayBudget::passes, SourceBudget::work_limit);
        for (const auto& [id, entity] : record.entities) {
            (void)id;
            reserve_retained_geometry_owner_work(entity, geometry_product, 1);
        }
        if (record.boundary_geometry_edit) geometry_edit(*record.boundary_geometry_edit);
        if (record.boundary_constraint_changes) {
            const auto& proof = *record.boundary_constraint_changes;
            for (const auto& edit : proof.boundary_edits) geometry_edit(edit);
            for (const auto& edit : proof.exterior_source_edits) geometry_edit(edit);
            for (const auto& stroke : proof.measured_stroke_edits) {
                if (stroke.authored_edit) geometry_edit(*stroke.authored_edit);
                for (const auto& edit : stroke.vertex_edits) geometry_edit(edit);
            }
            for (const auto* intent : {&proof.room_review_intent, &proof.room_review_geometry_proof,
                &proof.phase_room_review_intent, &proof.phase_constraint_authoring_intent,
                &proof.phase_selection_removal_intent, &proof.ordinary_selection_removal_intent,
                &proof.mixed_selection_removal_intent, &proof.mixed_selection_edit_intent,
                &proof.clipboard_placement_intent, &proof.assembly_catalog_edit_intent,
                &proof.phase_assembly_catalog_edit_intent, &proof.qualified_embedded_assembly_copy_intent})
                reserve_retained_geometry_replay_work(*intent, geometry_product, false, 1);
            for (const auto& intent : proof.room_review_additional_intents)
                reserve_retained_geometry_replay_work(intent, geometry_product, false, 1);
        }
    }
    ReplayBudget::charge(replay.work, geometry_work, ReplayBudget::passes, SourceBudget::work_limit);
    // Catalog work is independent of geometry and follows the same raw pass.
    AssemblyCatalogTransferBudget catalogs;
    std::size_t catalog_replay_work{};
    for (const auto& record : history) {
        const auto prefix_uses = retained_prefix_uses(record);
        if (prefix_uses) ReplayBudget::charge(catalog_replay_work, catalogs.consumed_validation_work,
            prefix_uses * ReplayBudget::passes, catalogs.max_validation_work);
        for (const auto& [id, entity] : record.entities) {
            (void)id;
            if (entity.type == "assembly_model") admit_complete_assembly_catalog_source(entity, catalogs, AssemblyCatalogSourceReferencePolicy::document_authoring);
        }
    }
    ReplayBudget::charge(catalog_replay_work, catalogs.consumed_validation_work,
        ReplayBudget::passes, catalogs.max_validation_work);
    if (!source.is_editable()) reject("captured source is read-only");
}
void authenticate(const DocumentSnapshot& source, const Json& request) {
    const auto saved = request.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(request.at("source_saved_revision"))};
    if (source.revision() != revision(request.at("expected_revision")) || source.saved_revision_optional() != saved ||
        request.at("source_snapshot_digest") != document_snapshot_digest(source) ||
        request.at("source_authoring_digest") != document_authoring_source_digest_v2(source) ||
        request.at("source_entities_digest") != entity_map_digest(source.entities())) reject("complete captured source changed");
}
void decode_allocations(const Json& request) {
    (void)decode_phase_assembly_catalog_edit_authoring(request.at("authoring"));
    (void)decode_phase_assembly_catalog_edit_presentation_allocation(request.at("presentation_allocation"));
    // Strict UTF-8 admission after the bounded raw walk. Preserve exact input.
    (void)request.dump();
}
} // namespace

void admit_retained_assembly_authoring_source(const DocumentSnapshot& source) {
    admit_source(source);
}

PhaseAssemblyCatalogEditPreparation::PhaseAssemblyCatalogEditPreparation(Json request, PhaseAssemblyCatalogEditResult result)
    : request_(std::move(request)), result_(std::move(result)),
      digest_(request_.at("source_snapshot_digest").get<std::string>()),
      message_(request_.at("message").get<std::string>()), revision_(revision(request_.at("expected_revision"))) {}

Json validate_phase_assembly_catalog_edit_request(const Json& request) {
    admit_request(request); decode_allocations(request); return request;
}
PhaseAssemblyCatalogEditPreparation prepare_phase_assembly_catalog_edit(const DocumentSnapshot& source,
    const PhaseAssemblyCatalogEditAuthoring& authoring,
    const PhaseAssemblyCatalogEditPresentationAllocation& presentation, std::string request_message) {
    RequestBudget allocation_budget;
    allocation_budget.read(authoring.catalog_request);
    admit_typed_allocations(allocation_budget, authoring, presentation);
    message(request_message);
    admit_source(source);
    // Both complete typed inventories have passed raw admission before either
    // owning encoder can allocate JSON. Codecs retain their exact dialects.
    auto wire = encode_phase_assembly_catalog_edit_authoring(authoring);
    auto overlays = encode_phase_assembly_catalog_edit_presentation_allocation(presentation);
    Json request{{"version", 1}, {"expected_revision", source.revision()},
        {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"authoring", std::move(wire)}, {"presentation_allocation", std::move(overlays)}, {"message", std::move(request_message)}};
    return replay_phase_assembly_catalog_edit_preparation(source, request);
}
PhaseAssemblyCatalogEditPreparation replay_phase_assembly_catalog_edit_preparation(const DocumentSnapshot& source,
    const Json& request) {
    admit_request(request); admit_source(source);
    decode_allocations(request); authenticate(source, request);
    // Admission only of the real source. No fabricated preview or publication.
    const auto admitted = Document::fork(source);
    if (!admitted.is_editable()) reject("actual admitted source is read-only");
    const auto authoring = decode_phase_assembly_catalog_edit_authoring(request.at("authoring"));
    const auto presentation = decode_phase_assembly_catalog_edit_presentation_allocation(request.at("presentation_allocation"));
    auto result = replay_phase_assembly_catalog_edit_presentation_entities(source.entities(), authoring.catalog_request,
        authoring.allocation, presentation);
    for (const auto& record : source.history()) {
        validate_phase_assembly_instance_property_edit_fresh_identities(record.entities, result.fresh_identity_ids);
        for (const auto& id : result.fresh_identity_ids)
            if (record.assets.contains(id)) reject("fresh identity reuses retained asset identity");
    }
    const std::set<std::string, std::less<>> fresh(result.fresh_identity_ids.begin(), result.fresh_identity_ids.end());
    validate_retained_authoring_fresh_identities(source.history(), source.history().size(), fresh);
    return {request, std::move(result)};
}
void require_phase_assembly_catalog_edit_source_current(const DocumentSnapshot& source,
    const PhaseAssemblyCatalogEditPreparation& preparation) {
    admit_request(preparation.request()); admit_source(source);
    decode_allocations(preparation.request()); authenticate(source, preparation.request());
}

} // namespace sketch
