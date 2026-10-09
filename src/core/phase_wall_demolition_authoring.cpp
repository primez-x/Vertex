#include "sketch/phase_wall_demolition_authoring.hpp"

#include "sketch/architectural_object_removal.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/hosted_opening_removal.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_coordinated_demolition.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_stair_demolition.hpp"
#include "sketch/phase_stair_demolition_retirement.hpp"
#include "sketch/phase_structural_replacement.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/stair_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
using Ids=std::set<std::string,std::less<>>;
constexpr std::size_t source_limit=64*1024*1024, proof_limit=1024*1024;
[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Wall demolition authoring: "+reason);
}
void keys(const Json& value,std::initializer_list<const char*> fields) {
    if (!value.is_object() || value.size()!=fields.size()) invalid("unsupported fields");
    for (const auto* key:fields) if (!value.contains(key)) invalid("missing required field");
}
void identity(const std::string& id) {
    if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            c=='-' || c=='_' || c=='.' || c==':';
    })) invalid("invalid actual identity");
}
// Conservative escaped byte accounting precedes all dumps, codecs and copies.
struct Budget {
    std::size_t limit,bytes{},nodes{};
    void reserve(std::size_t n) {
        if (n>limit-bytes) invalid("aggregate byte budget exceeded");
        bytes+=n;
    }
    void text(const std::string& value) {
        if (value.size()>(limit-bytes)/6) invalid("string budget exceeded");
        reserve(value.size()*6);reserve(2);
    }
    void read(const Json& value,std::size_t depth=0) {
        if (depth>64 || ++nodes>4*1024*1024) invalid("JSON nesting/node budget exceeded");
        reserve(32);
        if (value.is_binary() || value.is_discarded() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) invalid("unsupported JSON scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key,row]:value.items()) {text(key);read(row,depth+1);}
        else if (value.is_array()) for (const auto& row:value) read(row,depth+1);
    }
    void read(const std::string& value) {text(value);}
    template<class T> void optional(const std::optional<T>& value) {if (value) read(*value);}
    template<class T> void sequence(const std::vector<T>& values) {
        if (values.size()>262144) invalid("retained collection budget exceeded");
        reserve(values.size()*32);for (const auto& value:values) read(value);
    }
    void read(const Entity& value) {text(value.id);text(value.type);read(value.properties);read(value.extensions);}
    void read(const Asset& value) {
        text(value.id);text(value.media_type);text(value.sha256);read(value.metadata);
        if (value.bytes.size()>(limit-bytes)/2) invalid("asset hex expansion budget exceeded");
        reserve(value.bytes.size()*2);
    }
    void read(const EntityChange& value) {text(value.entity_id);read(value.entity);}
    void read(const AssetChange& value) {text(value.asset_id);read(value.asset);}
    void read(const Quantity& value) {text(value.original_expression);}
    void read(const AngleInput& value) {text(value.original_expression);text(value.normalized_expression);}
    void read(const ConstructionReceipt& value) {
        text(value.segment_id);
        if (value.chord_input) {read(value.chord_input->length);read(value.chord_input->heading);}
        optional(value.distance);optional(value.heading);optional(value.rise);optional(value.run);optional(value.turn);
        optional(value.angle);optional(value.height);optional(value.arc_length);optional(value.tangent);optional(value.sweep);
    }
    void read(const PhysicalWallRoomRepairIntent& value) {
        text(value.selected_wall_id);text(value.expected_descriptor_digest);read(value.reviewed_source_lineage);
    }
    void read(const BoundaryGeometryEdit& value) {
        text(value.boundary_id);text(value.target_id);text(value.new_vertex_id);text(value.new_segment_id);text(value.new_dimension_id);
        read(value.replacement_segments);read(value.replacement_authoring);read(value.replacement_properties);
        read(value.replacement_child_mapping);sequence(value.replacement_dimension_ids);sequence(value.replacement_removed_reference_ids);
        sequence(value.replacement_wall_source_ids);optional(value.arc_construction);optional(value.replacement_linework_sources);
        optional(value.physical_wall_room_repair);optional(value.wall_source_translation);
    }
    void read(const BoundaryTranslation& value) {text(value.boundary_id);}
    void read(const BoundaryTransformation& value) {text(value.boundary_id);}
    void read(const RigidOwnerTransformation& value) {text(value.owner_id);}
    void read(const TranslateBoundaries& value) {text(value.message);sequence(value.translations);sequence(value.entity_changes);}
    void read(const TransformBoundaries& value) {
        text(value.message);sequence(value.transformations);sequence(value.entity_changes);sequence(value.source_transformations);
    }
    void read(const ConstraintWallGeometryEdit& value) {
        text(value.wall_id);optional(value.length_entry);optional(value.curve_construction);optional(value.wall_classification);
    }
    void read(const ApplyBoundaryConstraintChanges::MeasuredStrokeEdit& value) {
        text(value.stroke_id);optional(value.authored_edit);optional(value.authored_length);sequence(value.vertex_edits);
    }
    void read(const ApplyBoundaryConstraintChanges::DimensionPlacementMove& value) {text(value.dimension_id);}
    void read(const ExteriorCornerMoveIntent& value) {text(value.boundary_id);text(value.vertex_id);}
    void read(const ExteriorSegmentResizeIntent& value) {text(value.boundary_id);text(value.segment_id);read(value.exact_length);}
    void read(const ExteriorSegmentArcIntent& value) {text(value.boundary_id);text(value.segment_id);read(value.arc_construction);}
    void read(const WallSplitMeasuredOwnerIds& value) {
        text(value.boundary_id);text(value.vertex_id);text(value.segment_id);text(value.automatic_dimension_id);
    }
    void read(const WallSplitPhysicalRoomIds& value) {text(value.boundary_id);sequence(value.new_segment_ids);sequence(value.new_vertex_ids);}
    void read(const WallSplitIntent& value) {
        text(value.wall_id);text(value.second_wall_id);text(value.seam_constraint_id);sequence(value.measured_owners);sequence(value.physical_room_owners);
    }
    void read(const WallMergeIntent& value) {text(value.first_wall_id);text(value.second_wall_id);}
    void read(const JointAnnotationTranslationIntent& value) {text(value.owner_id);text(value.child_id);}
    void read(const JointReferenceTranslationIntent& value) {text(value.reference_id);}
    void read(const JointOwnerTranslationIntent& value) {text(value.owner_id);}
    void read(const JointTranslationIntent& value) {
        sequence(value.rigid_boundary_ids);sequence(value.rigid_stroke_ids);sequence(value.partial_wall_ids);sequence(value.dimension_ids);
        sequence(value.annotation_translations);sequence(value.reference_translations);sequence(value.owner_translations);
        sequence(value.dimension_translations);sequence(value.owner_transformations);
    }
    void read(const DistoMeasurementAttachment& value) {
        text(value.owner_id);const auto& row=value.record;
        text(row.reading_id);text(row.target_field);text(row.unit);text(row.captured_at);text(row.model);
        text(row.firmware);text(row.transport);text(row.provenance);
    }
    void read(const ApplyBoundaryConstraintChanges& value) {
        text(value.message);sequence(value.boundary_edits);sequence(value.wall_edits);sequence(value.entity_changes);
        sequence(value.physical_entity_changes);sequence(value.exterior_source_edits);sequence(value.supplemental_entity_changes);
        sequence(value.supplemental_asset_changes);sequence(value.measured_stroke_edits);sequence(value.dimension_placement_moves);
        sequence(value.selection_entity_changes);sequence(value.room_review_additional_intents);
        optional(value.exterior_corner_move);optional(value.exterior_segment_resize);optional(value.exterior_segment_arc);
        optional(value.wall_split);optional(value.wall_merge);optional(value.rigid_group_transform);optional(value.joint_translation);
        optional(value.disto_measurement);read(value.room_review_intent);read(value.room_review_geometry_proof);
        read(value.phase_room_review_intent);read(value.phase_constraint_authoring_intent);read(value.independent_drawing_removal_intent);
    }
    void entities(const Entities& source) {
        if (source.size()>65536) invalid("source entity budget exceeded");
        for (const auto& [id,entity]:source) {
            identity(id);
            if (entity.id!=id || !entity.properties.is_object() || !entity.extensions.is_object())
                invalid("invalid actual entity envelope");
            text(id);text(entity.type);read(entity.properties);read(entity.extensions);reserve(128);
        }
    }
    void assets(const std::map<std::string,Asset,std::less<>>& values) {
        if (values.size()>65536) invalid("asset inventory budget exceeded");
        for (const auto& [id,asset]:values) {
            if (id!=asset.id) invalid("asset identity differs from actual key");
            text(id);read(asset);
        }
    }
};
void source_bound(const Entities& source) {Budget budget{source_limit};budget.entities(source);}
void snapshot_bound(const DocumentSnapshot& source) {
    Budget budget{source_limit};const auto& history=source.history();
    if (history.empty() || history.size()>4096 || source.revision()>=history.size()) invalid("invalid actual history inventory");
    budget.text(source.document_id());budget.text(source.read_only_reason());
    budget.entities(source.entities());budget.assets(source.assets());
    for (std::size_t i=0;i<history.size();++i) {
        const auto& row=history[i];
        if (row.revision!=i) invalid("actual history is not contiguous");
        budget.text(row.action);budget.optional(row.name);budget.entities(row.entities);budget.assets(row.assets);
        if (row.undo_stack.size()>262144 || row.redo_stack.size()>262144) invalid("history stack budget exceeded");
        budget.reserve(32*(row.undo_stack.size()+row.redo_stack.size()));
        budget.optional(row.boundary_translation);budget.optional(row.boundary_transform);budget.optional(row.boundary_geometry_edit);
        budget.optional(row.boundary_constraint_changes);budget.optional(row.boundary_translations);budget.optional(row.boundary_transforms);
    }
    if (source.named_revisions().size()>4096) invalid("named revision budget exceeded");
    for (const auto& [name,revision]:source.named_revisions()) {
        budget.text(name);if (revision>=history.size()) invalid("named revision is absent from actual history");
    }
    if (source.saved_revision_optional() && (*source.saved_revision_optional()>source.revision() ||
        *source.saved_revision_optional()>=history.size())) invalid("saved revision is absent from actual history");
}
void proof_bound(const Json& proof) {Budget budget{proof_limit};budget.read(proof);}
bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
bool exact_json(const Json& a,const Json& b) {return a==b && a.dump()==b.dump();}
void sorted_ids(const std::vector<std::string>& ids,std::size_t limit) {
    if (ids.size()>limit) invalid("selection budget exceeded");
    for (std::size_t i=0;i<ids.size();++i) {
        identity(ids[i]);if (i && ids[i-1]>=ids[i]) invalid("identities must be ascending and unique");
    }
}
Json wall_destinations_wire(const PhaseWallDemolitionAuthoring& intent) {
    sorted_ids(intent.ordinary_wall_ids,128);
    if (intent.ordinary_wall_ids.empty()) {
        if (!intent.wall_additional_identities.empty()) invalid("wall destinations require ordinary wall roots");
    } else {
        if (intent.wall_demolition.wall_ids.size()>128-intent.ordinary_wall_ids.size())
            invalid("combined wall selection budget exceeded");
        for (const auto& id:intent.ordinary_wall_ids)
            if (std::find(intent.wall_demolition.wall_ids.begin(),intent.wall_demolition.wall_ids.end(),id)!=
                intent.wall_demolition.wall_ids.end()) invalid("baseline and ordinary wall roots overlap");
    }
    if (intent.wall_additional_identities.size()>4096) invalid("wall destination key budget exceeded");
    Json additional=Json::object();Ids fresh;std::size_t count{};
    for (const auto& [owner,rows]:intent.wall_additional_identities) {
        identity(owner);
        if (rows.empty() || rows.size()>4096-count) invalid("wall destination slot budget exceeded");
        count+=rows.size();
        for (const auto& id:rows) {
            identity(id);
            if (!fresh.insert(id).second) invalid("repeated wall destination");
        }
        additional[owner]=rows;
    }
    for (const auto& [owner,rows]:intent.wall_additional_identities) {
        (void)rows;if (fresh.contains(owner)) invalid("wall destination borrows a source join identity");
    }
    return additional;
}
Json ordinary_wire(const ArchitecturalSelectionRemovalIntent& ordinary) {
    sorted_ids(ordinary.object_ids,1000);
    if (ordinary.components.size()>1000-ordinary.object_ids.size()) invalid("ordinary selection budget exceeded");
    Json components=Json::array(),additional=Json::object();
    for (std::size_t i=0;i<ordinary.components.size();++i) {
        const auto& key=ordinary.components[i];identity(key.first);identity(key.second);
        if (i && ordinary.components[i-1]>=key) invalid("component keys must be ascending and unique");
        components.push_back({{"catalog_id",key.first},{"instance_id",key.second}});
    }
    if (ordinary.roof_additional_identities.size()>4096) invalid("roof destination key budget exceeded");
    std::size_t count=0;Ids fresh;
    for (const auto& [old,rows]:ordinary.roof_additional_identities) {
        identity(old);
        if (rows.empty() || rows.size()>4096-count) invalid("roof destination slot budget exceeded");
        count+=rows.size();
        for (const auto& id:rows) {identity(id);if (!fresh.insert(id).second) invalid("repeated roof destination");}
        additional[old]=rows;
    }
    if (ordinary.object_ids.empty() && ordinary.components.empty() && !additional.empty())
        invalid("roof destinations need an ordinary selection");
    return {{"object_ids",ordinary.object_ids},{"components",std::move(components)},
        {"roof_additional_identities",std::move(additional)}};
}
ArchitecturalSelectionRemovalIntent ordinary_from_wire(const Json& wire) {
    keys(wire,{"object_ids","components","roof_additional_identities"});
    if (!wire.at("object_ids").is_array() || wire.at("object_ids").size()>1000 ||
        !wire.at("components").is_array() || wire.at("components").size()>1000 ||
        !wire.at("roof_additional_identities").is_object()) invalid("invalid ordinary selection shape");
    ArchitecturalSelectionRemovalIntent result;
    for (const auto& row:wire.at("object_ids")) {
        if (!row.is_string()) invalid("ordinary identity must be a string");
        result.object_ids.push_back(row.get<std::string>());
    }
    for (const auto& row:wire.at("components")) {
        keys(row,{"catalog_id","instance_id"});
        if (!row.at("catalog_id").is_string() || !row.at("instance_id").is_string()) invalid("invalid component identity");
        result.components.emplace_back(row.at("catalog_id").get<std::string>(),row.at("instance_id").get<std::string>());
    }
    for (const auto& [old,rows]:wire.at("roof_additional_identities").items()) {
        if (!rows.is_array() || rows.size()>4096) invalid("invalid roof destination slots");
        for (const auto& row:rows) {
            if (!row.is_string()) invalid("roof destination must be a string");
            result.roof_additional_identities[old].push_back(row.get<std::string>());
        }
    }
    if (!exact_json(ordinary_wire(result),wire)) invalid("ordinary selection is not canonical");
    return result;
}
void message_only(const ConstraintAuthoringIntent& intent) {
    if (intent.wall_resize || intent.wall_geometry_move || intent.wall_curve_construction || intent.boundary_resize ||
        intent.boundary_vertex_move || intent.exterior_corner_move || intent.exterior_segment_resize ||
        intent.exterior_segment_arc || intent.measured_stroke_resize || intent.measured_stroke_vertex_move ||
        intent.measured_stroke_transform || intent.joint_translation || intent.relation_anchor ||
        !intent.relation_mutations.empty() || !intent.relation_move_connected_walls)
        invalid("demolition cannot carry geometry or relationship authority");
    if (intent.message.size()>4096 || intent.message.find('\0')!=std::string::npos) invalid("message budget exceeded");
}
void same_binding(const PhaseConstraintAuthoringIntent& root,const PhaseConstraintAuthoringIntent& child) {
    if (root.expected_revision!=child.expected_revision || root.source_snapshot_digest!=child.source_snapshot_digest ||
        root.source_authoring_digest!=child.source_authoring_digest || root.source_entities_digest!=child.source_entities_digest ||
        root.source_saved_revision!=child.source_saved_revision || !exact_json(root.phase_selections,child.phase_selections))
        invalid("historical child differs from original captured source binding");
}
void leaf_choice(const PhaseConstraintAuthoringIntent& leaf,const PhaseWallDemolitionIntent& wall) {
    message_only(leaf.intent);
    const auto choice=[&](const auto& value) {
        if (value.registry_id!=wall.registry_id || value.alternative_id!=wall.alternative_id)
            invalid("all demolition leaves require the wall's actual saved choice");
    };
    if (!leaf.opening_demolition.is_null()) choice(decode_phase_opening_demolition_intent(leaf.opening_demolition));
    else if (!leaf.slab_demolition.is_null()) choice(decode_slab_demolition_intent(leaf.slab_demolition));
    else if (!leaf.stair_demolition.is_null()) choice(decode_stair_demolition_intent(leaf.stair_demolition));
    else if (!leaf.stair_demolition_retirement.is_null()) choice(decode_stair_demolition_retirement_intent(leaf.stair_demolition_retirement));
    else if (!leaf.roof_replacement.is_null()) {
        const auto value=decode_phase_roof_replacement_authoring(leaf.roof_replacement);choice(value);
        if (!value.demolition || !value.roof_profiles.empty() || !value.roof_opening_edits.empty() ||
            !value.roof_edits.empty() || !value.ordinary_roof_edits.empty()) invalid("roof child must be demolition only");
    } else if (!leaf.structural_replacement.is_null()) {
        const auto value=decode_phase_structural_replacement_authoring(leaf.structural_replacement);choice(value);
        if (!value.demolition || !value.identities.empty() || !value.edits.empty() || value.complete_hosted ||
            !value.hosted_instance_identities.empty()) invalid("structural child must be demolition only");
    } else invalid("unsupported historical demolition child");
}
struct Historical {
    std::vector<PhaseConstraintAuthoringIntent> leaves;
    std::optional<PhaseCoordinatedOrdinaryRemoval> ordinary;
};
Historical historical(const Json& wire,const PhaseWallDemolitionIntent& wall,
    const PhaseConstraintAuthoringIntent* root=nullptr) {
    if (wire.is_null()) return {};
    proof_bound(wire);
    if (!wire.is_object() || !wire.contains("version") || !wire.at("version").is_number_integer() ||
        (wire.at("version")!=3 && wire.at("version")!=4 && wire.at("version")!=6 && wire.at("version")!=9 &&
         wire.at("version")!=11 && wire.at("version")!=13 && wire.at("version")!=15))
        invalid("other authoring requires a historical demolition envelope");
    if (wire.at("version")==15) {
        const auto coordinated=wire.find("coordinated_demolition");
        if (coordinated==wire.end() || !coordinated->is_object() || !coordinated->contains("version") ||
            !coordinated->at("version").is_number_integer() ||
            (coordinated->at("version")!=1 && coordinated->at("version")!=2 && coordinated->at("version")!=3))
            invalid("wall demolition permits only historical coordinated inner versions one through three");
    }
    auto child=decode_phase_constraint_authoring_intent(wire);
    if (!exact_json(encode_phase_constraint_authoring_intent(child),wire)) invalid("historical envelope is not canonical");
    message_only(child.intent);if (root) same_binding(*root,child);
    Historical result;
    if (wire.at("version")==15) {
        // Extract before replay: the old coordinator can reject sorted opening
        // demolition rows before this new dialect's order adapter can run.
        result.leaves=phase_coordinated_demolition_components(child.coordinated_demolition,child);
        result.ordinary=phase_coordinated_demolition_ordinary_removal(child.coordinated_demolition,child);
    } else result.leaves.push_back(std::move(child));
    for (const auto& leaf:result.leaves) {leaf_choice(leaf,wall);if (root) same_binding(*root,leaf);}
    return result;
}
// Only codec-known ModelPhases ID arrays have set semantics. Restore survivor
// source order and append admitted fresh IDs without rewriting metadata.
void retain_registry_order(const Entities& actual,Entities& candidate,const PhaseWallDemolitionIntent& wall) {
    source_bound(candidate);
    for (const auto& [id,entity]:actual) {
        if (entity.type!="model_phases") continue;
        const auto found=candidate.find(id);
        if (found==candidate.end()) invalid("cannot remove an actual phase registry");
        if (exact(entity,found->second)) continue;
        if (id!=wall.registry_id) invalid("leaf changed a foreign phase registry");
        const auto before=ModelPhases::from_json(entity.properties.at("model"));
        const auto after=ModelPhases::from_json(found->second.properties.at("model"));
        if (before.active_alternative()!=std::optional<std::string>{wall.alternative_id} ||
            after.active_alternative()!=before.active_alternative()) invalid("saved active choice changed");
        auto envelope=found->second;
        std::vector<Json::json_pointer> paths{Json::json_pointer("/model/entity_ids"),Json::json_pointer("/model/baseline_ids")};
        const auto& alternatives=entity.properties.at("model").at("alternatives");
        if (envelope.properties.at("model").at("alternatives").size()!=alternatives.size()) invalid("alternative roster changed");
        for (std::size_t i=0;i<alternatives.size();++i) if (alternatives[i].at("id")==wall.alternative_id) {
            const auto prefix="/model/alternatives/"+std::to_string(i);
            paths.emplace_back(prefix+"/proposed_ids");paths.emplace_back(prefix+"/demolished_ids");
        }
        for (const auto& path:paths) {
            const auto& retained=entity.properties.at(path);const auto& rows=found->second.properties.at(path);
            if (!retained.is_array() || !rows.is_array()) invalid("phase IDs must remain arrays");
            Ids originals,present;
            for (const auto& row:retained) {
                if (!row.is_string() || !originals.insert(row.get<std::string>()).second) invalid("ambiguous retained phase identity");
            }
            for (const auto& row:rows) {
                if (!row.is_string() || !present.insert(row.get<std::string>()).second) invalid("ambiguous replayed phase identity");
            }
            Json normalized=Json::array();
            for (const auto& row:retained) if (present.contains(row.get<std::string>())) normalized.push_back(row);
            for (const auto& row:rows) if (!originals.contains(row.get<std::string>())) normalized.push_back(row);
            found->second.properties.at(path)=std::move(normalized);
            envelope.properties.at(path)=retained;
        }
        if (!exact(entity,envelope)) invalid("leaf changed registry metadata or another alternative");
        if (ModelPhases::from_json(found->second.properties.at("model")).to_json()!=after.to_json())
            invalid("order adapter changed typed phase semantics");
    }
}
void protected_source(const Entities& actual,const Entities& result,const PhaseWallDemolitionIntent& wall,
    bool complete=false,const Ids& retired_independent_openings={}) {
    source_bound(result);
    Ids baseline,other_proposals;
    for (const auto& [id,entity]:actual) if (entity.type=="model_phases") {
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        baseline.insert(model.baseline_ids().begin(),model.baseline_ids().end());
        for (const auto& alternative:model.alternatives()) if (id!=wall.registry_id || alternative.id!=wall.alternative_id) {
            other_proposals.insert(alternative.proposed_ids.begin(),alternative.proposed_ids.end());
            if (complete) other_proposals.insert(alternative.demolished_ids.begin(),alternative.demolished_ids.end());
        }
        if (complete && id!=wall.registry_id)
            other_proposals.insert(model.entity_ids().begin(),model.entity_ids().end());
        if (id!=wall.registry_id && (!result.contains(id) || !exact(entity,result.at(id)))) invalid("foreign registry changed");
    }
    const auto scope=constraint_phase_scope(actual);
    for (const auto& [id,entity]:actual) {
        const bool retained=entity.required || scope.inactive_owner_ids.contains(id) ||
            other_proposals.contains(id) ||
            (baseline.contains(id) && entity.type!="assembly_model");
        if (retained && id!=wall.registry_id && (!result.contains(id) || !exact(entity,result.at(id))))
            invalid("changed a required, baseline or inactive source owner: "+id);
        if (entity.type!="assembly_model") continue;
        const auto candidate=result.find(id);
        if (candidate==result.end()) invalid("actual catalog carrier was removed");
        if (exact(entity,candidate->second)) continue;
        const auto& rows=entity.properties.at("model").at("instances");
        const auto& next=candidate->second.properties.at("model").at("instances");
        for (const auto& row:rows) {
            if (!row.contains("placement") || row.at("placement").is_null()) continue;
            const auto& placement=row.at("placement");
            if (!placement.contains("host_entity_id") || !placement.at("host_entity_id").is_string()) invalid("invalid source component host");
            const auto host=placement.at("host_entity_id").get<std::string>();
            bool baseline_host=baseline.contains(host) || scope.inactive_owner_ids.contains(host) || other_proposals.contains(host);
            const auto owner=actual.find(host);
            if (owner!=actual.end() && owner->second.type=="opening") {
                const auto& wall_id=owner->second.properties.at("wall_id");
                if (!wall_id.is_string()) invalid("invalid baseline opening host");
                const auto host_wall=wall_id.get<std::string>();
                const auto original_wall=actual.find(host_wall),remaining_wall=result.find(host_wall);
                // This exception is supplied only after the complete actual
                // opening facade admitted and physically retired an independent
                // selected cut. Its original baseline wall remains byte-exact.
                const bool retired_proposed_opening=complete && retired_independent_openings.contains(host) &&
                    !result.contains(host) && !baseline.contains(host) && !owner->second.required &&
                    !scope.inactive_owner_ids.contains(host) && !other_proposals.contains(host) &&
                    original_wall!=actual.end() && remaining_wall!=result.end() && !original_wall->second.required &&
                    exact(original_wall->second,remaining_wall->second);
                baseline_host=baseline_host || (baseline.contains(host_wall) && !retired_proposed_opening) ||
                    scope.inactive_owner_ids.contains(host_wall) || other_proposals.contains(host_wall);
            }
            if (!baseline_host) continue;
            const auto found=std::find_if(next.begin(),next.end(),[&](const Json& item) {return item.at("id")==row.at("id");});
            if (found==next.end() || !exact_json(*found,row)) invalid("changed an actual component on a retained baseline host");
        }
    }
}
void ordinary_authority(const Entities& actual,const Entities& result,const PhaseWallDemolitionIntent& wall,
    bool complete=false,const Ids& retired_independent_openings={}) {
    const auto scope=constraint_phase_scope(actual);
    const auto compatible=[&](const std::string& id) {
        if (!actual.contains(id) || scope.inactive_owner_ids.contains(id)) invalid("ordinary consequence has an absent/inactive host or carrier");
        for (const auto& registry:scope.registries)
            if (std::find(registry.registered_entity_ids.begin(),registry.registered_entity_ids.end(),id)!=registry.registered_entity_ids.end() &&
                (registry.registry_id!=wall.registry_id || registry.alternative_id!=std::optional<std::string>{wall.alternative_id}))
                invalid("ordinary host or carrier belongs to a foreign saved choice");
    };
    for (const auto& registry:scope.registries) {
        for (const auto& id:registry.registered_entity_ids) {
            const auto found=actual.find(id);if (found==actual.end()) continue;
            const auto after=result.find(id);
            if (after!=result.end() && exact(found->second,after->second)) continue;
            if (registry.registry_id!=wall.registry_id || registry.alternative_id!=std::optional<std::string>{wall.alternative_id})
                invalid("ordinary consequence belongs to a foreign saved choice: "+id);
        }
    }
    for (const auto& [id,entity]:actual) if (entity.type=="assembly_model") {
        const auto found=result.find(id);
        if (found==result.end()) invalid("ordinary removal cannot erase its catalog carrier");
        if (exact(entity,found->second)) continue;
        const auto before=AssemblyModel::from_json(entity.properties.at("model"));
        const auto after=AssemblyModel::from_json(found->second.properties.at("model"));
        Ids remaining;for (const auto& row:after.instances()) remaining.insert(row.id);
        for (const auto& row:before.instances()) if (!remaining.contains(row.id)) {
            compatible(id);if (row.placement) compatible(row.placement->host_entity_id);
        }
    }
    for (const auto& [id,entity]:actual) {
        const auto found=result.find(id);
        if (found!=result.end() && exact(entity,found->second)) continue;
        if (entity.type=="railing") {
            const auto rail=decode_railing_properties(id,entity.properties);
            if (rail.host) compatible(rail.host->stair_id);
            if (rail.landing_host) compatible(rail.landing_host->stair_id);
        } else if (entity.type=="roof_join") {
            const auto join=parse_roof_join(entity.properties,id);
            for (const auto& roof:join.roof_ids) compatible(roof);
        }
    }
    protected_source(actual,result,wall,complete,retired_independent_openings);
}
void exclusive_root(const PhaseConstraintAuthoringIntent& intent) {
    message_only(intent.intent);
    Budget budget{proof_limit};budget.read(intent.wall_demolition);budget.read(intent.phase_selections);
    budget.text(intent.source_snapshot_digest);budget.text(intent.source_authoring_digest);
    budget.text(intent.source_entities_digest);budget.text(intent.intent.message);
    if (intent.wall_demolition.is_null() || !intent.wall_replacement.is_null() || !intent.opening_demolition.is_null() ||
        !intent.roof_replacement.is_null() || !intent.slab_replacement.is_null() || !intent.slab_demolition.is_null() ||
        !intent.coordinated_replacements.is_null() || !intent.structural_replacement.is_null() ||
        !intent.stair_demolition.is_null() || !intent.stair_replacement.is_null() ||
        !intent.stair_demolition_retirement.is_null() || !intent.coordinated_demolition.is_null())
        invalid("wall demolition must be the exclusive root dialect");
}
Entities physical_stage(const Entities& actual,const PhaseConstraintAuthoringIntent& root,
    const PhaseWallDemolitionAuthoring& edit,Ids* admitted_openings=nullptr) {
    source_bound(actual);exclusive_root(root);
    if (root.source_entities_digest!=entity_map_digest(actual) ||
        !exact_json(root.phase_selections,phase_constraint_authoring_selections(actual))) invalid("actual source or saved choices changed");
    // Analytical work admission is repeated here before any leaf can invoke
    // its own native roof/opening/component validators.
    validate_mixed_wall_removal_source_admission(actual,true);
    const auto source_scope=constraint_phase_scope(actual);
    const auto selected_owner=[&](const std::string& id) {
        const auto found=actual.find(id);
        if (found==actual.end() || found->second.required || source_scope.inactive_owner_ids.contains(id))
            invalid("selected ordinary owner/host/carrier is absent, required or inactive: "+id);
        for (const auto& registry:source_scope.registries) {
            if (std::find(registry.registered_entity_ids.begin(),registry.registered_entity_ids.end(),id)==registry.registered_entity_ids.end()) continue;
            const auto state=registry.states.find(id);
            if (registry.registry_id!=edit.wall_demolition.registry_id ||
                registry.alternative_id!=std::optional<std::string>{edit.wall_demolition.alternative_id} ||
                state==registry.states.end() || state->second==ModelPhase::demolished)
                invalid("selected ordinary owner/host/carrier has a foreign saved phase: "+id);
        }
    };
    // Root authority precedes every native-capable leaf, including join
    // inference. Saved active visibility alone cannot erase a shared baseline
    // or an owner also used by another preserved alternative.
    Ids retained_baseline,protected_alternatives;
    if (!edit.ordinary_wall_ids.empty() || edit.complete_hosted_catalog_consequences) {
        if (!edit.ordinary_wall_ids.empty()) validate_stair_attachment_state(actual);
        for (const auto& registry:source_scope.registries) {
            const auto model=ModelPhases::from_json(actual.at(registry.registry_id).properties.at("model"));
            retained_baseline.insert(model.baseline_ids().begin(),model.baseline_ids().end());
            for (const auto& alternative:model.alternatives()) {
                protected_alternatives.insert(alternative.demolished_ids.begin(),alternative.demolished_ids.end());
                if (registry.registry_id!=edit.wall_demolition.registry_id || alternative.id!=edit.wall_demolition.alternative_id)
                    protected_alternatives.insert(alternative.proposed_ids.begin(),alternative.proposed_ids.end());
            }
        }
    }
    for (const auto& id:edit.ordinary_wall_ids) {
        selected_owner(id);
        if (actual.at(id).type!="wall") invalid("ordinary wall selection requires an actual physical wall");
        if (retained_baseline.contains(id)) invalid("ordinary wall root has retained baseline authority: "+id);
        if (protected_alternatives.contains(id)) invalid("ordinary wall root has protected alternative usage: "+id);
    }
    Historical other;
    if (edit.complete_hosted_catalog_consequences) {
        // Authenticate the complete explicit selection on the original source
        // before historical leaves or ordinary producers can invoke native work.
        other=historical(edit.other_authoring,edit.wall_demolition,&root);
        const auto selected=std::find_if(source_scope.registries.begin(),source_scope.registries.end(),[&](const auto& registry) {
            return registry.registry_id==edit.wall_demolition.registry_id;
        });
        if (selected==source_scope.registries.end() ||
            selected->alternative_id!=std::optional<std::string>{edit.wall_demolition.alternative_id})
            invalid("complete demolition requires the actual saved registry/alternative");
        for (const auto& id:edit.wall_demolition.wall_ids) {
            selected_owner(id);
            if (actual.at(id).type!="wall" || !retained_baseline.contains(id))
                invalid("baseline demolition requires an actual original wall");
        }
        for (const auto& id:edit.opening_ids) {
            selected_owner(id);
            const auto& opening=actual.at(id);
            if (opening.type!="opening") invalid("opening selection requires an actual semantic owner");
            const auto& host=opening.properties.at("wall_id");
            if (!host.is_string()) invalid("opening host must be an actual identity");
            const auto wall_id=host.get<std::string>();selected_owner(wall_id);
            if (actual.at(wall_id).type!="wall") invalid("opening requires an actual physical wall host");
            const bool independent=!std::binary_search(edit.wall_demolition.wall_ids.begin(),edit.wall_demolition.wall_ids.end(),wall_id) &&
                !std::binary_search(edit.ordinary_wall_ids.begin(),edit.ordinary_wall_ids.end(),wall_id);
            if (independent && (retained_baseline.contains(id) || protected_alternatives.contains(id) ||
                protected_alternatives.contains(wall_id))) invalid("independent opening or host has protected saved ownership");
            if (independent && retained_baseline.contains(wall_id) && !selected->states.contains(id))
                invalid("an independent opening on an original wall requires actual proposed registration");
        }
        Ids roots;std::set<std::pair<std::string,std::string>> components;
        const auto aliases=embedded_assembly_presentation_ids(actual);
        const auto authenticate=[&](const ArchitecturalSelectionRemovalIntent& selection) {
            for (const auto& id:selection.object_ids) {
                selected_owner(id);
                const auto& type=actual.at(id).type;
                if (type!="roof" && type!="slab" && type!="column" && type!="beam" && type!="stair" && type!="railing")
                    invalid("ordinary root requires its dedicated authoring lane");
                if (!roots.insert(id).second) invalid("ordinary lanes repeat an explicit actual owner");
                if (retained_baseline.contains(id)) invalid("ordinary root has retained baseline authority");
                if (protected_alternatives.contains(id)) invalid("ordinary root has protected alternative usage");
                if (type=="railing") {
                    const auto rail=decode_railing_properties(id,actual.at(id).properties);
                    if (rail.host) selected_owner(rail.host->stair_id);
                    if (rail.landing_host) selected_owner(rail.landing_host->stair_id);
                }
            }
            for (const auto& key:selection.components) {
                if (!components.insert(key).second) invalid("ordinary lanes repeat an explicit qualified component");
                selected_owner(key.first);
                if (actual.at(key.first).type!="assembly_model" || !aliases.contains(key))
                    invalid("selected component requires an actual qualified catalog row");
                if (protected_alternatives.contains(key.first)) invalid("selected catalog has protected alternative usage");
                const auto model=AssemblyModel::from_json(actual.at(key.first).properties.at("model"));
                const auto row=std::find_if(model.instances().begin(),model.instances().end(),[&](const auto& item) {return item.id==key.second;});
                if (row==model.instances().end()) invalid("component requires an actual qualified row");
                if (row->placement) {
                    const auto& host=row->placement->host_entity_id;selected_owner(host);
                    if (protected_alternatives.contains(host)) invalid("selected component host has protected alternative usage");
                    if (actual.at(host).type=="opening") {
                        const auto& wall_id=actual.at(host).properties.at("wall_id");
                        if (!wall_id.is_string()) invalid("component opening has an invalid actual wall host");
                        selected_owner(wall_id.get<std::string>());
                        if (protected_alternatives.contains(wall_id.get<std::string>()))
                            invalid("selected component wall has protected alternative usage");
                    }
                }
            }
            if (!selection.roof_additional_identities.empty() && std::none_of(selection.object_ids.begin(),selection.object_ids.end(),
                [&](const auto& id) {return actual.at(id).type=="roof";})) invalid("roof destinations require an actual selected roof");
        };
        if (other.ordinary) authenticate({other.ordinary->object_ids,other.ordinary->components,other.ordinary->roof_additional_identities});
        authenticate(edit.ordinary);
    }
    const auto wall_candidate=replay_phase_wall_demolition_entities(actual,edit.wall_demolition);
    if (!edit.complete_hosted_catalog_consequences) other=historical(edit.other_authoring,edit.wall_demolition,&root);
    std::vector<Entities> candidates;
    candidates.push_back(wall_candidate);
    std::optional<std::size_t> ordinary_wall_lane;
    Ids ordinary_wall_retired_owners;
    if (!edit.ordinary_wall_ids.empty()) {
        preflight_physical_walls_deletion_join_inference(
            actual,edit.ordinary_wall_ids,true,edit.complete_hosted_catalog_consequences);
        auto candidate=replay_complete_physical_walls_deletion(
            actual,edit.ordinary_wall_ids,edit.wall_additional_identities,true,edit.complete_hosted_catalog_consequences);
        retain_registry_order(actual,candidate,edit.wall_demolition);
        ordinary_authority(actual,candidate,edit.wall_demolition,edit.complete_hosted_catalog_consequences);
        for (const auto& [id,entity]:actual) {
            (void)entity;if (!candidate.contains(id)) ordinary_wall_retired_owners.insert(id);
        }
        ordinary_wall_lane=candidates.size();candidates.push_back(std::move(candidate));
    }
    Ids historical_retired_owners;
    for (const auto& leaf:other.leaves) {
        auto candidate=replay_phase_constraint_authoring(actual,encode_phase_constraint_authoring_intent(leaf));
        retain_registry_order(actual,candidate,edit.wall_demolition);
        if (!leaf.opening_demolition.is_null()) {
            // The entire original leaf has already independently admitted its
            // roots. Identical host-induced demolition is one registry effect.
            auto& alternatives=candidate.at(edit.wall_demolition.registry_id).properties.at("model").at("alternatives");
            const auto& original=actual.at(edit.wall_demolition.registry_id).properties.at("model").at("alternatives");
            const auto& walls=wall_candidate.at(edit.wall_demolition.registry_id).properties.at("model").at("alternatives");
            for (std::size_t i=0;i<alternatives.size();++i) if (alternatives[i].at("id")==edit.wall_demolition.alternative_id) {
                const auto& existing=original[i].at("demolished_ids");const auto& parked=walls[i].at("demolished_ids");
                auto& rows=alternatives[i].at("demolished_ids");
                std::erase_if(rows.get_ref<Json::array_t&>(),[&](const Json& row) {
                    return std::find(existing.begin(),existing.end(),row)==existing.end() &&
                        std::find(parked.begin(),parked.end(),row)!=parked.end();
                });
            }
        }
        protected_source(actual,candidate,edit.wall_demolition,edit.complete_hosted_catalog_consequences);
        for (const auto& [id,entity]:actual) if (!candidate.contains(id) &&
            (entity.type=="stair" || entity.type=="railing" || entity.type=="slab" ||
             entity.type=="column" || entity.type=="beam" || entity.type=="roof")) historical_retired_owners.insert(id);
        candidates.push_back(std::move(candidate));
    }
    std::vector<std::string> independent;
    for (const auto& id:edit.opening_ids) {
        selected_owner(id);
        const auto found=actual.find(id);
        if (found==actual.end() || found->second.type!="opening") invalid("opening selection requires an actual semantic owner");
        const auto& host=found->second.properties.at("wall_id");
        if (!host.is_string()) invalid("opening host must be an actual identity");
        selected_owner(host.get<std::string>());
        if (std::binary_search(edit.wall_demolition.wall_ids.begin(),edit.wall_demolition.wall_ids.end(),host.get<std::string>())) {
            if (!wall_candidate.contains(id) || !exact(found->second,wall_candidate.at(id))) invalid("wall leaf failed to retain original hosted opening");
        } else if (std::binary_search(edit.ordinary_wall_ids.begin(),edit.ordinary_wall_ids.end(),host.get<std::string>())) {
            if (!ordinary_wall_lane || candidates[*ordinary_wall_lane].contains(id))
                invalid("ordinary wall leaf failed to retire its actual hosted opening");
        } else independent.push_back(id);
    }
    Ids retired_independent_openings;
    if (!independent.empty()) {
        auto opening=replay_hosted_opening_removal(actual,independent,true,edit.complete_hosted_catalog_consequences);
        if (!opening) invalid("independent opening selection was not admitted");
        if (edit.complete_hosted_catalog_consequences) for (const auto& id:independent) {
            if (opening->contains(id)) invalid("complete independent opening producer did not physically retire its selection");
            retired_independent_openings.insert(id);
        }
        ordinary_authority(actual,*opening,edit.wall_demolition,edit.complete_hosted_catalog_consequences,retired_independent_openings);
        candidates.push_back(std::move(*opening));
    }
    // Retained baseline hosts can become inactive through any admitted family,
    // not only the wall leaf. Their explicitly selected components follow that
    // visibility consequence after original carrier/host admission below.
    const auto original_scope=constraint_phase_scope(actual);
    Ids demolished_hosts;
    for (const auto& candidate:candidates)
        for (const auto& id:constraint_phase_scope(candidate).inactive_owner_ids)
            if (!original_scope.inactive_owner_ids.contains(id)) demolished_hosts.insert(id);
    Ids ordinary_roots;
    std::set<std::pair<std::string,std::string>> ordinary_components;
    const auto add_ordinary=[&](ArchitecturalSelectionRemovalIntent selection,bool historical_primitive) {
        if (selection.object_ids.empty() && selection.components.empty()) return;
        // Validate every explicit source selection before any closure collapse.
        for (const auto& id:selection.object_ids) {
            selected_owner(id);
            if (!edit.ordinary_wall_ids.empty()) {
                const auto& type=actual.at(id).type;
                if (type!="roof" && type!="slab" && type!="column" && type!="beam" &&
                    type!="stair" && type!="railing")
                    invalid("ordinary architectural roots cannot borrow a covered wall or reference lane");
            }
            if (!ordinary_roots.insert(id).second) invalid("ordinary lanes repeat an explicit actual owner");
        }
        for (const auto& key:selection.components) {
            if (!ordinary_components.insert(key).second) invalid("ordinary lanes repeat an explicit qualified component");
            selected_owner(key.first);
            const auto carrier=actual.find(key.first);
            if (carrier->second.type!="assembly_model") invalid("component requires an actual catalog carrier");
            const auto model=AssemblyModel::from_json(carrier->second.properties.at("model"));
            const auto row=std::find_if(model.instances().begin(),model.instances().end(),[&](const auto& item) {return item.id==key.second;});
            if (row==model.instances().end()) invalid("component requires an actual qualified row");
            if (row->placement) {
                const auto& host=row->placement->host_entity_id;selected_owner(host);
                const auto& owner=actual.at(host);
                if (owner.type=="opening") {
                    const auto& wall_id=owner.properties.at("wall_id");
                    if (!wall_id.is_string()) invalid("component opening has an invalid actual wall host");
                    selected_owner(wall_id.get<std::string>());
                }
            }
        }
        // Only independently authenticated historical and complete wall
        // closures subsume explicit roots after original owner/host admission.
        std::erase_if(selection.object_ids,[&](const auto& id) {
            return historical_retired_owners.contains(id) || ordinary_wall_retired_owners.contains(id);
        });
        const auto aliases=embedded_assembly_presentation_ids(actual);
        for (const auto& key:selection.components) if (!aliases.contains(key)) invalid("selected component is absent from actual source");
        // A selection on a retained host follows that host's independently
        // admitted demolition visibility; it cannot erase the original row.
        std::erase_if(selection.components,[&](const auto& key) {
            const auto model=AssemblyModel::from_json(actual.at(key.first).properties.at("model"));
            const auto row=std::find_if(model.instances().begin(),model.instances().end(),[&](const auto& item) {return item.id==key.second;});
            if (row==model.instances().end() || !row->placement) return false;
            const auto& host=row->placement->host_entity_id;
            if (demolished_hosts.contains(host)) return true;
            const auto found=actual.find(host);
            if (found==actual.end() || found->second.type!="opening") return false;
            const auto& wall_id=found->second.properties.at("wall_id");
            return wall_id.is_string() && demolished_hosts.contains(wall_id.get<std::string>());
        });
        // Only actual qualified rows already retired by a complete leaf collapse.
        for (const auto& candidate:candidates) {
            const auto surviving=embedded_assembly_presentation_ids(candidate);
            std::erase_if(selection.components,[&](const auto& key) {return aliases.contains(key) && !surviving.contains(key);});
        }
        if (selection.object_ids.empty() && selection.components.empty()) {
            if (!selection.roof_additional_identities.empty()) invalid("roof slots lost their selected producer");
            return;
        }
        for (const auto& id:selection.object_ids) {
            const auto found=actual.find(id);
            if (found==actual.end() || found->second.type=="wall" || found->second.type=="opening" ||
                found->second.type=="room" || found->second.type=="boundary") invalid("ordinary root requires its dedicated authoring lane");
        }
        auto candidate=historical_primitive ? replay_architectural_object_removal(actual,selection.object_ids,selection.components) :
            replay_architectural_selection_removal(actual,selection,true,edit.complete_hosted_catalog_consequences);
        retain_registry_order(actual,candidate,edit.wall_demolition);
        ordinary_authority(actual,candidate,edit.wall_demolition,edit.complete_hosted_catalog_consequences);
        candidates.push_back(std::move(candidate));
    };
    if (other.ordinary) add_ordinary({other.ordinary->object_ids,other.ordinary->components,other.ordinary->roof_additional_identities},
        other.ordinary->version==1);
    add_ordinary(edit.ordinary,false);
    const auto original_aliases=embedded_assembly_presentation_ids(actual);
    auto expected_aliases=original_aliases;
    auto expected_inactive=constraint_phase_scope(actual).inactive_owner_ids;
    for (const auto& candidate:candidates) {
        const auto aliases=embedded_assembly_presentation_ids(candidate);
        for (const auto& [key,alias]:aliases) {
            const auto found=original_aliases.find(key);
            // Historical roof demolition may derive fresh hosted copies.
            // Their leaf owns freshness; surviving actual aliases stay exact.
            if (found!=original_aliases.end() && found->second!=alias) invalid("leaf changed a surviving actual component alias");
            if (found==original_aliases.end()) {
                if (!expected_aliases.emplace(key,alias).second) invalid("fresh hosted component destinations overlap");
            }
        }
        for (const auto& [key,alias]:original_aliases) {
            (void)alias;if (!aliases.contains(key)) expected_aliases.erase(key);
        }
        const auto phase=constraint_phase_scope(candidate);
        expected_inactive.insert(phase.inactive_owner_ids.begin(),phase.inactive_owner_ids.end());
    }
    auto result=std::move(candidates.front());
    for (std::size_t i=1;i<candidates.size();++i) {
        // This new dialect alone opts into the closed complete catalog
        // consequence composer. Historical codecs keep their original rules.
        result=compose_ordinary_architectural_removal_candidates(actual,{result,candidates[i]},true,true);
    }
    protected_source(actual,result,edit.wall_demolition,edit.complete_hosted_catalog_consequences,retired_independent_openings);
    if (embedded_assembly_presentation_ids(result)!=expected_aliases) invalid("composition changed complete surviving alias inventory");
    if (constraint_phase_scope(result).inactive_owner_ids!=expected_inactive) invalid("composition changed independently admitted inactive ownership");
    // Keep the original detached room lineage intact until explicit review.
    for (const auto& [id,entity]:actual) if (entity.type=="room" || entity.type=="boundary")
        if (!result.contains(id) || !exact(entity,result.at(id))) invalid("physical stage changed room lineage");
    if (admitted_openings) *admitted_openings=std::move(retired_independent_openings);
    return result;
}
PhysicalWallRoomPhaseReviewIntent room_binding(const PhaseConstraintAuthoringIntent& root,
    const PhaseWallDemolitionAuthoring& edit,const Entities& stage) {
    PhysicalWallRoomPhaseReviewIntent binding;
    binding.source_snapshot_digest=root.source_snapshot_digest;
    binding.source_authoring_digest=root.source_authoring_digest;
    binding.source_saved_revision=root.source_saved_revision;binding.expected_revision=root.expected_revision;
    binding.source_entities_digest=entity_map_digest(stage);
    binding.registry_id=edit.wall_demolition.registry_id;binding.alternative_id=edit.wall_demolition.alternative_id;
    const auto& registry=stage.at(binding.registry_id);
    binding.source_registry_entity_digest=entity_map_digest(Entities{{registry.id,registry}});
    binding.registry_command_proof=command_to_json(Command{ApplyEntityChanges{root.expected_revision,
        {EntityChange::upsert(registry)}, {},"Review demolished wall rooms"}});
    return binding;
}
bool requires_rooms(const PhysicalWallRoomPhaseReviewInventory& inventory) {
    return std::any_of(inventory.reports.begin(),inventory.reports.end(),[](const auto& report) {
        return !report.correspondence.retained.empty() || !report.correspondence.fresh.empty();
    });
}
void validate_room_binding(const PhaseConstraintAuthoringIntent& root,const PhaseWallDemolitionAuthoring& edit,
    const Entities& stage,const PhysicalWallRoomPhaseReviewIntent& room,const PhysicalWallRoomPhaseReviewInventory& inventory) {
    const auto expected=room_binding(root,edit,stage);
    if (room.expected_revision!=expected.expected_revision || room.source_snapshot_digest!=expected.source_snapshot_digest ||
        room.source_authoring_digest!=expected.source_authoring_digest || room.source_saved_revision!=expected.source_saved_revision ||
        room.source_entities_digest!=expected.source_entities_digest || room.registry_id!=expected.registry_id ||
        room.alternative_id!=expected.alternative_id || room.source_registry_entity_digest!=expected.source_registry_entity_digest ||
        !exact_json(room.registry_command_proof,expected.registry_command_proof))
        invalid("room review must bind the original capture and exact complete stage identity upsert");
    if (room.planes.size()!=inventory.intent.planes.size() ||
        room.proposed_room_completion!=inventory.intent.proposed_room_completion) invalid("room review plane/completion inventory differs");
    for (std::size_t i=0;i<room.planes.size();++i) {
        const auto& given=room.planes[i];const auto& required=inventory.intent.planes[i];
        if (given.context!=required.context || given.effective_elevation_m!=required.effective_elevation_m)
            invalid("room review must cover every affected plane exactly in inventory order");
    }
}
} // namespace

Json encode_phase_wall_demolition_authoring(const PhaseWallDemolitionAuthoring& intent) {
    proof_bound(intent.other_authoring);proof_bound(intent.room_review_intent);
    (void)historical(intent.other_authoring,intent.wall_demolition);
    sorted_ids(intent.opening_ids,1000);
    auto wall_additional=wall_destinations_wire(intent);
    if (!intent.room_review_intent.is_null()) {
        const auto room=decode_physical_wall_phase_room_review_intent(intent.room_review_intent);
        if (!exact_json(encode_physical_wall_phase_room_review_intent(room),intent.room_review_intent)) invalid("room review is not canonical");
    }
    Json result{{"version",1},{"wall_demolition",encode_phase_wall_demolition_intent(intent.wall_demolition)},
        {"other_authoring",intent.other_authoring},{"ordinary",ordinary_wire(intent.ordinary)},
        {"opening_ids",intent.opening_ids},{"room_review_intent",intent.room_review_intent}};
    if (intent.complete_hosted_catalog_consequences || !intent.ordinary_wall_ids.empty()) {
        result["version"]=intent.complete_hosted_catalog_consequences ? 3 : 2;
        result["ordinary_wall_ids"]=intent.ordinary_wall_ids;
        result["wall_additional_identities"]=std::move(wall_additional);
    }
    if (intent.complete_hosted_catalog_consequences) result["complete_hosted_catalog_consequences"]=true;
    proof_bound(result);return result;
}
PhaseWallDemolitionAuthoring decode_phase_wall_demolition_authoring(const Json& value) try {
    proof_bound(value);
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3)) invalid("unsupported inner demolition version");
    const bool complete=value.at("version")==3;
    const bool ordinary_walls=value.at("version")!=1;
    if (complete) {
        keys(value,{"version","wall_demolition","other_authoring","ordinary","opening_ids","room_review_intent",
            "ordinary_wall_ids","wall_additional_identities","complete_hosted_catalog_consequences"});
        if (!value.at("complete_hosted_catalog_consequences").is_boolean() ||
            value.at("complete_hosted_catalog_consequences")!=true) invalid("inner three requires complete hosted catalog consequences");
    } else if (ordinary_walls) keys(value,{"version","wall_demolition","other_authoring","ordinary","opening_ids","room_review_intent",
        "ordinary_wall_ids","wall_additional_identities"});
    else keys(value,{"version","wall_demolition","other_authoring","ordinary","opening_ids","room_review_intent"});
    if (!value.at("opening_ids").is_array() || value.at("opening_ids").size()>1000)
        invalid("unsupported inner demolition version/selection");
    PhaseWallDemolitionAuthoring result;
    result.complete_hosted_catalog_consequences=complete;
    result.wall_demolition=decode_phase_wall_demolition_intent(value.at("wall_demolition"));
    result.other_authoring=value.at("other_authoring");result.ordinary=ordinary_from_wire(value.at("ordinary"));
    for (const auto& row:value.at("opening_ids")) {
        if (!row.is_string()) invalid("opening identity must be a string");
        result.opening_ids.push_back(row.get<std::string>());
    }
    result.room_review_intent=value.at("room_review_intent");
    if (ordinary_walls) {
        const auto& roots=value.at("ordinary_wall_ids");const auto& additional=value.at("wall_additional_identities");
        if (!roots.is_array() || (!complete && roots.empty()) || roots.size()>128 || !additional.is_object() || additional.size()>4096)
            invalid("unsupported ordinary wall selection/destinations");
        for (const auto& row:roots) {
            if (!row.is_string()) invalid("ordinary wall identity must be a string");
            result.ordinary_wall_ids.push_back(row.get<std::string>());
        }
        std::size_t slots{};
        for (const auto& [owner,rows]:additional.items()) {
            if (!rows.is_array() || rows.empty() || rows.size()>4096-slots) invalid("invalid wall destination slots");
            slots+=rows.size();
            for (const auto& row:rows) {
                if (!row.is_string()) invalid("wall destination must be a string");
                result.wall_additional_identities[owner].push_back(row.get<std::string>());
            }
        }
    }
    if (!exact_json(encode_phase_wall_demolition_authoring(result),value)) invalid("demolition authoring is not canonical");
    return result;
} catch (const Json::exception& error) {invalid(std::string("malformed demolition authoring: ")+error.what());}

PhaseWallDemolitionAuthoringPreview inspect_phase_wall_demolition_authoring(
    const DocumentSnapshot& actual,const PhaseConstraintAuthoringIntent& intent) {
    snapshot_bound(actual);exclusive_root(intent);
    // Snapshot lifetime admission belongs to Document. This is an exact binding
    // check against the actual capture, never a history inferred from entities.
    if (!actual.is_editable() || intent.expected_revision!=actual.revision() ||
        intent.source_snapshot_digest!=document_snapshot_digest(actual) ||
        intent.source_authoring_digest!=document_authoring_source_digest_v2(actual) ||
        intent.source_saved_revision!=actual.saved_revision_optional()) invalid("original actual capture changed");
    const auto edit=decode_phase_wall_demolition_authoring(intent.wall_demolition);
    // Direct preview consumers reserve against the same complete actual
    // lifetime before the ordinary lane can invoke native join inference.
    validate_physical_wall_join_removal_identity_lifetime(actual,edit.wall_additional_identities);
    auto stage=physical_stage(actual.entities(),intent,edit);
    auto inventory=inspect_physical_wall_phase_room_review_entities(stage,room_binding(intent,edit,stage));
    const bool needed=requires_rooms(inventory);
    return {std::move(stage),std::move(inventory),needed};
}
Entities replay_phase_wall_demolition_authoring(const Entities& actual,const PhaseConstraintAuthoringIntent& intent) {
    source_bound(actual);exclusive_root(intent);
    const auto edit=decode_phase_wall_demolition_authoring(intent.wall_demolition);
    Ids admitted_openings;
    auto stage=physical_stage(actual,intent,edit,&admitted_openings);
    const auto inventory=inspect_physical_wall_phase_room_review_entities(stage,room_binding(intent,edit,stage));
    if (edit.room_review_intent.is_null()) {
        if (requires_rooms(inventory)) invalid("explicit complete room decisions are required");
    } else {
        const auto room=decode_physical_wall_phase_room_review_intent(edit.room_review_intent);
        validate_room_binding(intent,edit,stage,room,inventory);
        stage=replay_physical_wall_phase_room_review(stage,edit.room_review_intent,true).entities;
    }
    protected_source(actual,stage,edit.wall_demolition,edit.complete_hosted_catalog_consequences,admitted_openings);
    if (!edit.ordinary_wall_ids.empty()) validate_stair_attachment_state(stage);
    if (const auto error=validate_active_phase_constraint_integrity(stage)) invalid(*error);
    return stage;
}
ApplyBoundaryConstraintChanges phase_wall_demolition_authoring_command(const PhaseConstraintAuthoringIntent& intent) {
    exclusive_root(intent);(void)decode_phase_wall_demolition_authoring(intent.wall_demolition);
    ApplyBoundaryConstraintChanges command;command.expected_revision=intent.expected_revision;
    command.message=intent.intent.message;command.phase_constraint_authoring_completion=true;
    command.phase_constraint_authoring_intent=encode_phase_constraint_authoring_intent(intent);
    return command;
}
} // namespace sketch
