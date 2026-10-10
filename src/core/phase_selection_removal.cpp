#include "sketch/phase_selection_removal.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/phase_removal_selection.hpp"
#include "sketch/phase_wall_demolition.hpp"
#include "sketch/physical_wall_phase_review.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=DrawingSelectionRemovalEntities;
constexpr std::size_t wire_limit=1024*1024, selection_limit=1000;
constexpr std::size_t memo_limit=128*1024*1024;
struct ReplayMemo {
    std::map<std::pair<std::string,std::string>,Entities> results;
    std::size_t bytes{};
};
thread_local std::unique_ptr<ReplayMemo> replay_memo;
thread_local std::size_t replay_scope_depth{};

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase selection removal: "+reason);
}
void fields(const Json& value,std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size()!=names.size()) reject("unsupported intent fields");
    for (const auto* name:names) if (!value.contains(name)) reject("missing intent field");
}
// Conservative escaped-byte accounting happens before serialization/codecs.
struct Budget {
    std::size_t limit, bytes{}, nodes{};
    void add(std::size_t amount) {
        if (amount>limit-bytes) reject("aggregate wire/source byte budget exceeded");
        bytes+=amount;
    }
    void text(const std::string& value) {
        if (value.size()>(limit-bytes)/6) reject("string/key byte budget exceeded");
        add(value.size()*6);add(2);
    }
    void read(const Json& value,std::size_t depth=0) {
        if (depth>64 || ++nodes>2*1024*1024) reject("wire/source node or nesting budget exceeded");
        add(32);
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>())))
            reject("unsupported wire/source scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key,child]:value.items()) { text(key);read(child,depth+1); }
        else if (value.is_array()) for (const auto& child:value) read(child,depth+1);
    }
};
void bounded_wire(const Json& value) { Budget budget{wire_limit};budget.read(value); }
bool has_drawing(const DrawingSelectionRemovalIntent& drawing) {
    return !drawing.owner_ids.empty() || !drawing.annotations.empty();
}
ArchitecturalDrawingRemovalIntent authority(const PhaseSelectionRemovalIntent& intent) {
    if (intent.architectural.object_ids.empty() && intent.architectural.components.empty())
        reject("phase deletion requires explicit architectural roots/components");
    if (!intent.architectural.roof_additional_identities.empty()) reject("selection cannot supply destination authority");
    ArchitecturalDrawingRemovalIntent result;
    result.architectural=intent.architectural;result.drawing=intent.drawing;
    // The established codec checks exact sorting, qualified identities,
    // cross-lane overlap and the aggregate 1000-selection bound.
    (void)encode_architectural_drawing_removal_intent(result);
    return result;
}
bool exact(const ArchitecturalSelectionRemovalIntent& left,const ArchitecturalSelectionRemovalIntent& right) {
    return left.object_ids==right.object_ids && left.components==right.components &&
        left.roof_additional_identities==right.roof_additional_identities;
}
bool exact(const Entities& left,const Entities& right) {
    if (left.size()!=right.size()) return false;
    for (const auto& [id,entity]:left) {
        const auto found=right.find(id);
        if (found==right.end() || entity!=found->second || entity.properties.dump()!=found->second.properties.dump() ||
            entity.extensions.dump()!=found->second.extensions.dump()) return false;
    }
    return true;
}
void metadata(const DocumentSnapshot& source,const DocumentSnapshot& stage) {
    if (!stage.is_editable() || source.document_id()!=stage.document_id() || source.assets()!=stage.assets() ||
        source.saved_revision_optional()!=stage.saved_revision_optional() ||
        source.named_revisions()!=stage.named_revisions() || source.read_only_reason()!=stage.read_only_reason())
        reject("phase deletion changed assets or project metadata");
    for (const auto& [id,asset]:source.assets())
        if (asset.metadata.dump()!=stage.assets().at(id).metadata.dump()) reject("phase deletion changed asset metadata");
    if (stage.history().size()!=source.history().size()+1 || stage.revision()!=source.history().size())
        reject("phase deletion must create exactly one detached history event");
}
void source_budget(const DocumentSnapshot& source) {
    if (!source.is_editable() || source.history().empty() || source.history().size()>4096 ||
        source.revision()>=source.history().size() || source.history()[source.revision()].revision!=source.revision() ||
        source.assets().size()>65536 || source.named_revisions().size()>4096 ||
        (source.saved_revision_optional() && *source.saved_revision_optional()>=source.history().size()))
        reject("captured source history/save inventory is invalid or read-only");
    // Match ordinary46's capture inventories. The full snapshot digest binds
    // all retained history and actual asset bytes; Document preview validates
    // that history. Do not impose a smaller source byte limit on admitted data.
}
// Check all typed siblings before command_to_json can project them away.
void pure_base(const ApplyBoundaryConstraintChanges& c) {
    if (c.message.size()>1024 || !c.boundary_edits.empty() || !c.wall_edits.empty() || !c.entity_changes.empty() ||
        !c.physical_entity_changes.empty() || !c.exterior_source_edits.empty() || !c.supplemental_entity_changes.empty() ||
        !c.supplemental_asset_changes.empty() || !c.measured_stroke_edits.empty() || !c.dimension_placement_moves.empty() ||
        !c.selection_entity_changes.empty() || c.selection_completion || c.exterior_source_completion ||
        c.supplemental_source_completion || c.supplemental_asset_reference_completion || c.rigid_wall_transform_completion ||
        c.measured_source_completion || c.dimension_placement_completion || c.rigid_group_completion || c.rigid_group_transform ||
        c.wall_split || c.wall_merge || c.exterior_corner_move || c.exterior_segment_resize || c.exterior_segment_arc ||
        c.joint_translation_completion || c.joint_translation || c.room_review_completion || !c.room_review_intent.is_null() ||
        c.room_review_geometry_completion || !c.room_review_geometry_proof.is_null() || c.room_review_batch_completion ||
        !c.room_review_additional_intents.empty() || c.wall_dimension_completion || c.curve_construction_completion ||
        c.disto_measurement_completion || c.disto_measurement || c.independent_drawing_removal_completion ||
        !c.independent_drawing_removal_intent.is_null() || c.wall_group_scale_completion || c.wall_group_scale ||
        c.mixed_selection_removal_completion || !c.mixed_selection_removal_intent.is_null() ||
        c.ordinary_selection_removal_completion || !c.ordinary_selection_removal_intent.is_null() ||
        c.phase_selection_removal_completion || !c.phase_selection_removal_intent.is_null())
        reject("base requires an exclusive complete phase deletion");
    const bool phase34=c.phase_constraint_authoring_completion && !c.phase_constraint_authoring_intent.is_null();
    const bool phase33=c.phase_room_review_completion && !c.phase_room_review_intent.is_null();
    if (phase33==phase34 || (phase34 && (c.phase_room_review_completion || !c.phase_room_review_intent.is_null())) ||
        (phase33 && (c.phase_constraint_authoring_completion || !c.phase_constraint_authoring_intent.is_null())))
        reject("base requires exactly one pure outer33 or outer34 mode");
}
void preflight33(const Json& proof) {
    bounded_wire(proof);
    if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
        (proof.at("version")!=1 && proof.at("version")!=2) || !proof.contains("registry_command_proof") ||
        !proof.contains("planes") || !proof.at("planes").is_array() || proof.at("planes").size()>32)
        reject("requires a known complete phase room-review grammar");
    const auto& raw=proof.at("registry_command_proof");
    fields(raw,{"version","kind","expected_revision","message","entity_changes","asset_changes"});
    if (!raw.at("version").is_number_integer() || raw.at("version")!=1 || raw.at("kind")!="apply_entity_changes" ||
        !raw.at("asset_changes").is_array() || !raw.at("asset_changes").empty() ||
        !raw.at("entity_changes").is_array() || raw.at("entity_changes").size()!=1)
        reject("historical wall base requires one asset-free registry raw upsert");
    std::size_t old=0,fresh=0;
    for (const auto& plane:proof.at("planes")) {
        if (!plane.is_object() || !plane.contains("source_rooms") || !plane.at("source_rooms").is_array() ||
            !plane.contains("fresh") || !plane.at("fresh").is_array() ||
            plane.at("source_rooms").size()>2048-old || plane.at("fresh").size()>2048-fresh)
            reject("phase room-review decision budget exceeded");
        old+=plane.at("source_rooms").size();fresh+=plane.at("fresh").size();
    }
}
void preflight_base(const Json& wire) {
    bounded_wire(wire);
    if (!wire.is_object() || !wire.contains("version") || !wire.at("version").is_number_integer() ||
        !wire.contains("kind") || wire.at("kind")!="apply_boundary_constraint_changes")
        reject("base requires a closed phase deletion command");
    if (wire.at("version")==34) {
        fields(wire,{"version","kind","expected_revision","message","phase_constraint_authoring_completion",
            "phase_constraint_authoring_intent"});
        preflight_phase_removal_selection_proof(wire.at("phase_constraint_authoring_intent"));
    } else if (wire.at("version")==33) {
        fields(wire,{"version","kind","expected_revision","message","phase_room_review_completion","phase_room_review_intent"});
        preflight33(wire.at("phase_room_review_intent"));
    } else reject("base accepts only pure outer33 or outer34 deletion commands");
}
Json base_wire(const Command& command) {
    const auto* typed=std::get_if<ApplyBoundaryConstraintChanges>(&command);
    if (!typed) reject("base requires typed phase deletion authority");
    pure_base(*typed);
    if (typed->phase_constraint_authoring_completion)
        preflight_phase_removal_selection_proof(typed->phase_constraint_authoring_intent);
    else preflight33(typed->phase_room_review_intent);
    const auto wire=command_to_json(command);preflight_base(wire);return wire;
}
Command closed_base(const Json& wire) {
    preflight_base(wire);
    const auto command=command_from_json(wire);
    if (base_wire(command).dump()!=wire.dump()) reject("base command is not canonical");
    return command;
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source binding must be a digest");
    const auto& bytes=value.get_ref<const std::string&>();
    if (bytes.size()!=64 || !std::all_of(bytes.begin(),bytes.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    })) reject("source binding must be a canonical SHA-256 digest");
}
std::optional<Revision> saved_revision(const Json& value) {
    if (value.is_null()) return std::nullopt;
    if (!(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>()>=0)))
        reject("invalid captured saved revision");
    return value.get<Revision>();
}
void remember(const std::pair<std::string,std::string>& key,const Entities& result) {
    std::size_t bytes=0;
    const auto add=[&](std::size_t size) {
        if (size>memo_limit-bytes) reject("aggregate detached replay cache budget exceeded");
        bytes+=size;
    };
    add(key.first.size());add(key.second.size());
    for (const auto& [id,entity]:result) {
        add(sizeof(Entity));add(id.size());add(entity.id.size());add(entity.type.size());
        add(entity.properties.dump().size());add(entity.extensions.dump().size());
    }
    if (replay_memo->results.size()>=4096 || bytes>memo_limit-replay_memo->bytes)
        reject("aggregate detached replay cache budget exceeded");
    replay_memo->results.emplace(key,result);replay_memo->bytes+=bytes;
}
} // namespace

PhaseSelectionRemovalReplayScope::PhaseSelectionRemovalReplayScope() {
    if (!replay_scope_depth) replay_memo=std::make_unique<ReplayMemo>();
    ++replay_scope_depth;
}
PhaseSelectionRemovalReplayScope::~PhaseSelectionRemovalReplayScope() {
    if (!--replay_scope_depth) replay_memo.reset();
}

Json encode_phase_selection_removal_intent(const PhaseSelectionRemovalIntent& intent) {
    (void)authority(intent);
    Json components=Json::array();
    for (const auto& [catalog,local]:intent.architectural.components)
        components.push_back({{"catalog_id",catalog},{"instance_id",local}});
    Json value={{"version",1},{"kind","phase_deletion"},{"object_ids",intent.architectural.object_ids},
        {"components",std::move(components)},
        {"drawing",has_drawing(intent.drawing) ? encode_drawing_selection_removal_intent(intent.drawing) : Json(nullptr)}};
    bounded_wire(value);return value;
}
PhaseSelectionRemovalIntent decode_phase_selection_removal_intent(const Json& value) try {
    bounded_wire(value);fields(value,{"version","kind","object_ids","components","drawing"});
    if (!value.at("version").is_number_integer() || value.at("version")!=1 || value.at("kind")!="phase_deletion")
        reject("unsupported selection version or kind");
    const auto& objects=value.at("object_ids");const auto& components=value.at("components");
    if (!objects.is_array() || objects.size()>selection_limit || !components.is_array() ||
        components.size()>selection_limit-objects.size()) reject("aggregate architectural selection budget exceeded");
    std::size_t count=objects.size()+components.size();
    if (!value.at("drawing").is_null()) {
        const auto& drawing=value.at("drawing");fields(drawing,{"version","owner_ids","annotations"});
        for (const auto* name:{"owner_ids","annotations"}) {
            if (!drawing.at(name).is_array() || drawing.at(name).size()>selection_limit-count)
                reject("aggregate selection budget exceeded");
            count+=drawing.at(name).size();
        }
    }
    PhaseSelectionRemovalIntent result;result.architectural.object_ids=objects.get<std::vector<std::string>>();
    for (const auto& row:components) {
        fields(row,{"catalog_id","instance_id"});
        result.architectural.components.emplace_back(row.at("catalog_id").get<std::string>(),row.at("instance_id").get<std::string>());
    }
    if (!value.at("drawing").is_null()) result.drawing=decode_drawing_selection_removal_intent(value.at("drawing"));
    if (encode_phase_selection_removal_intent(result).dump()!=value.dump()) reject("selection is not canonical");
    return result;
} catch (const Json::exception& error) { reject(std::string("malformed selection: ")+error.what()); }

ArchitecturalDrawingRemovalIntent phase_selection_removal_authority(const PhaseSelectionRemovalIntent& intent) {
    (void)encode_phase_selection_removal_intent(intent);return authority(intent);
}

ArchitecturalSelectionRemovalIntent phase_selection_removal_base_authority(const DocumentSnapshot& source,
    const Command& base,const ArchitecturalSelectionRemovalIntent& explicit_selection) try {
    PhaseSelectionRemovalIntent selection;selection.architectural=explicit_selection;
    (void)encode_phase_selection_removal_intent(selection);source_budget(source);
    const auto wire=base_wire(base);const auto canonical=closed_base(wire);
    const auto& typed=std::get<ApplyBoundaryConstraintChanges>(canonical);
    if (typed.expected_revision!=source.revision()) reject("base revision differs from actual source");
    if (wire.at("version")==34) {
        const auto actual=phase_removal_selection_authority(source,typed);
        if (!exact(actual,explicit_selection)) reject("base demolition roots differ from explicit selection");
        return actual;
    }
    if (!explicit_selection.components.empty() || explicit_selection.object_ids.empty() ||
        explicit_selection.object_ids.size()>128) reject("historical outer33 requires only 1..128 explicit wall roots");
    for (const auto& id:explicit_selection.object_ids) {
        const auto found=source.entities().find(id);
        if (found==source.entities().end() || found->second.type!="wall") reject("historical root is not an actual wall");
    }
    const auto review=decode_physical_wall_phase_room_review_intent(typed.phase_room_review_intent);
    const auto raw=command_from_json(review.registry_command_proof);
    const auto* registry=std::get_if<ApplyEntityChanges>(&raw);
    if (!registry) reject("historical wall registry proof is not raw");
    const auto actual=prepare_phase_wall_demolition(source,explicit_selection.object_ids,registry->message);
    if (!actual || command_to_json(Command{*actual}).dump()!=review.registry_command_proof.dump())
        reject("historical registry proof differs from independently reconstructed explicit wall demolition");
    const auto classified=inspect_phase_wall_demolition_selection(source.entities(),explicit_selection.object_ids);
    if (!classified || !classified->ordinary_wall_ids.empty() || classified->baseline.registry_id!=review.registry_id ||
        std::optional<std::string>{classified->baseline.alternative_id}!=review.alternative_id ||
        classified->baseline.wall_ids!=explicit_selection.object_ids)
        reject("historical wall roots differ from the actual active baseline registry/alternative");
    return explicit_selection;
} catch (const Json::exception& error) { reject(std::string("malformed base/source: ")+error.what()); }

DocumentSnapshot prepare_phase_selection_removal_stage(const DocumentSnapshot& source,
    const PhaseSelectionRemovalIntent& intent,const Command& original) try {
    PhaseSelectionRemovalReplayScope scope;
    (void)encode_phase_selection_removal_intent(intent);source_budget(source);
    const auto* typed=std::get_if<ApplyBoundaryConstraintChanges>(&original);
    if (!typed || typed->expected_revision!=source.revision()) reject("original requires actual-source typed revision authority");
    Entities expected;
    const bool completed=typed->phase_selection_removal_completion || !typed->phase_selection_removal_intent.is_null();
    if (completed) {
        const auto retained=validate_completed_phase_selection_removal_intent(typed->phase_selection_removal_intent);
        if (!typed->phase_selection_removal_completion ||
            retained.at("selection").dump()!=encode_phase_selection_removal_intent(intent).dump())
            reject("completed phase command differs from exact helper selection");
        expected=replay_completed_phase_selection_removal(source,retained);
    } else {
        (void)phase_selection_removal_base_authority(source,original,intent.architectural);
        if (has_drawing(intent.drawing)) reject("drawing selection requires its complete outer47 phase wrapper");
    }
    const auto wire=command_to_json(original);bounded_wire(wire);
    const auto canonical=command_from_json(wire);
    if (command_to_json(canonical).dump()!=wire.dump() ||
        (completed && (wire.at("version")!=47 || wire.at("kind")!="apply_boundary_constraint_changes")))
        reject("original phase command is not canonical");
    const auto stage=Document::preview_command(source,original);metadata(source,stage);
    if (completed && !exact(stage.entities(),expected))
        reject("complete phase command differs from independently reconstructed phase/drawing removal");
    validate_architectural_geometry_changes(source,stage);
    return stage;
} catch (const Json::exception& error) { reject(std::string("malformed captured command/source: ")+error.what()); }

Json validate_completed_phase_selection_removal_intent(const Json& value) try {
    bounded_wire(value);
    fields(value,{"version","selection","base_command","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision"});
    if (!value.at("version").is_number_integer() || value.at("version")!=1)
        reject("unsupported completed intent version");
    const auto selection=decode_phase_selection_removal_intent(value.at("selection"));
    if (!has_drawing(selection.drawing)) reject("completed phase deletion requires explicit drawing owners/rows");
    const auto base=closed_base(value.at("base_command"));
    if (std::get<ApplyBoundaryConstraintChanges>(base).phase_room_review_completion &&
        (!selection.architectural.components.empty() || selection.architectural.object_ids.size()>128))
        reject("historical outer33 selection requires only explicit wall roots");
    for (const auto* name:{"source_snapshot_digest","source_authoring_digest","source_entities_digest"}) digest(value.at(name));
    (void)saved_revision(value.at("source_saved_revision"));
    return value;
} catch (const Json::exception& error) { reject(std::string("malformed completed intent: ")+error.what()); }

std::optional<Revision> phase_selection_removal_source_saved_revision(const Json& value) {
    (void)validate_completed_phase_selection_removal_intent(value);return saved_revision(value.at("source_saved_revision"));
}

Entities replay_completed_phase_selection_removal(const DocumentSnapshot& source,const Json& value) try {
    PhaseSelectionRemovalReplayScope scope;
    const auto retained=validate_completed_phase_selection_removal_intent(value);source_budget(source);
    const auto source_digest=document_snapshot_digest(source);
    if (source.saved_revision_optional()!=saved_revision(retained.at("source_saved_revision")) ||
        source_digest!=retained.at("source_snapshot_digest") ||
        document_authoring_source_digest_v2(source)!=retained.at("source_authoring_digest") ||
        entity_map_digest(source.entities())!=retained.at("source_entities_digest"))
        reject("captured complete source authority changed");
    const auto key=std::pair{source_digest,retained.dump()};
    if (const auto found=replay_memo->results.find(key);found!=replay_memo->results.end()) return found->second;
    const auto selection=decode_phase_selection_removal_intent(retained.at("selection"));
    const auto base=closed_base(retained.at("base_command"));
    auto without_drawing=selection;without_drawing.drawing={};
    (void)prepare_phase_selection_removal_stage(source,without_drawing,base);
    // This producer independently owns the same complete source and re-admits
    // the typed base. No externally validated candidate supplies permission.
    const auto expected=replay_drawing_selection_removal_with_phase(source,selection.drawing,selection.architectural,base);
    remember(key,expected);return expected;
} catch (const Json::exception& error) { reject(std::string("malformed completed source/intent: ")+error.what()); }

ApplyBoundaryConstraintChanges prepare_completed_phase_selection_removal(const DocumentSnapshot& source,
    const PhaseSelectionRemovalIntent& intent,const Command& base) {
    PhaseSelectionRemovalReplayScope scope;source_budget(source);
    Json value={{"version",1},{"selection",encode_phase_selection_removal_intent(intent)},
        {"base_command",base_wire(base)},{"source_snapshot_digest",document_snapshot_digest(source)},
        {"source_authoring_digest",document_authoring_source_digest_v2(source)},
        {"source_entities_digest",entity_map_digest(source.entities())},
        {"source_saved_revision",source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)}};
    (void)validate_completed_phase_selection_removal_intent(value);
    const auto expected=replay_completed_phase_selection_removal(source,value);
    ApplyBoundaryConstraintChanges command;command.expected_revision=source.revision();
    command.message=std::get<ApplyBoundaryConstraintChanges>(base).message;
    command.phase_selection_removal_completion=true;command.phase_selection_removal_intent=std::move(value);
    const auto candidate=prepare_phase_selection_removal_stage(source,intent,Command{command});
    if (!exact(candidate.entities(),expected)) reject("completed phase command changed its admitted candidate");
    return command;
}

} // namespace sketch
