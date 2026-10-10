#include "sketch/mixed_selection_removal.hpp"

#include "sketch/document_digest.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_removal_selection.hpp"
#include "sketch/roof_entity_codec.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=DrawingSelectionRemovalEntities;
struct ReplayMemo {
    std::map<std::pair<std::string,std::string>,Entities> results;
    std::size_t bytes{};
};
thread_local std::unique_ptr<ReplayMemo> replay_memo;
thread_local std::size_t replay_scope_depth{};
constexpr std::size_t memo_byte_limit=128*1024*1024;
[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Mixed selection removal: "+reason);
}
void fields(const Json& value,std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size()!=names.size()) reject("unsupported intent fields");
    for (const auto* name:names) if (!value.contains(name)) reject("missing intent field");
}
void wire_budget(const Json& value,std::size_t& nodes,std::size_t& bytes,std::size_t depth=0) {
    if (depth>64 || ++nodes>131072) reject("intent nesting/node budget exceeded");
    const auto text=[&](const std::string& string) {
        if (string.size()>1024*1024-bytes) reject("intent string/key budget exceeded");
        bytes+=string.size();
    };
    if (value.is_binary() || value.is_discarded()) reject("unsupported intent JSON value");
    if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("intent scalar must be finite");
    if (value.is_string()) text(value.get_ref<const std::string&>());
    if (value.is_object()) for (const auto& [key,child]:value.items()) {
        text(key);wire_budget(child,nodes,bytes,depth+1);
    } else if (value.is_array()) for (const auto& child:value) wire_budget(child,nodes,bytes,depth+1);
}
void identity(const std::string& id) {
    if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            c=='-' || c=='_' || c=='.' || c==':';
    })) reject("invalid member identity");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("source binding must be a digest");
    const auto& bytes=value.get_ref<const std::string&>();
    if (bytes.size()!=64 || !std::all_of(bytes.begin(),bytes.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    })) reject("source binding must be a canonical SHA-256 digest");
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
void metadata(const DocumentSnapshot& before,const DocumentSnapshot& after) {
    if (!after.is_editable() || before.document_id()!=after.document_id() || before.assets()!=after.assets() ||
        before.saved_revision_optional()!=after.saved_revision_optional() || before.named_revisions()!=after.named_revisions() ||
        before.read_only_reason()!=after.read_only_reason()) reject("removal changed assets or project metadata");
    for (const auto& [id,asset]:before.assets())
        if (asset.metadata.dump()!=after.assets().at(id).metadata.dump()) reject("removal changed asset metadata");
}
Json encode_members(const std::vector<RoofOpeningGroupMember>& members) {
    if (members.empty() || members.size()>1000) reject("requires one to 1000 actual roof children");
    auto sorted=members;
    const auto key=[](const auto& member) { return std::pair{member.roof_id,member.opening_id}; };
    std::sort(sorted.begin(),sorted.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});
    Json rows=Json::array();std::set<std::pair<std::string,std::string>> unique;
    for (const auto& member:sorted) {
        identity(member.roof_id);identity(member.opening_id);
        if (!unique.emplace(key(member)).second) reject("duplicate roof child");
        rows.push_back({{"roof_id",member.roof_id},{"opening_id",member.opening_id}});
    }
    return rows;
}
std::vector<RoofOpeningGroupMember> decode_members(const Json& value) {
    if (!value.is_array() || value.empty() || value.size()>1000) reject("invalid roof child inventory");
    std::vector<RoofOpeningGroupMember> result;
    for (const auto& row:value) {
        fields(row,{"roof_id","opening_id"});
        if (!row.at("roof_id").is_string() || !row.at("opening_id").is_string()) reject("roof child requires identities");
        result.push_back({row.at("roof_id").get<std::string>(),row.at("opening_id").get<std::string>()});
    }
    if (encode_members(result).dump()!=value.dump()) reject("roof child inventory must be sorted and unique");
    return result;
}
Command closed_command(const Json& value,bool child) {
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        !value.contains("kind")) reject("requires an explicit unnested preceding command");
    const bool raw=value.at("version")==1 && value.at("kind")=="apply_entity_changes";
    const bool phase=child && value.at("version")==34 && value.at("kind")=="apply_boundary_constraint_changes";
    if (!raw && !phase) reject("unsupported preceding command authority");
    auto command=command_from_json(value);
    if (command_to_json(command).dump()!=value.dump()) reject("preceding command is not canonical");
    if (raw && !std::get<ApplyEntityChanges>(command).asset_changes.empty()) reject("removal cannot change assets");
    if (phase) {
        const auto& typed=std::get<ApplyBoundaryConstraintChanges>(command);
        const auto intent=decode_phase_constraint_authoring_intent(typed.phase_constraint_authoring_intent);
        if (intent.roof_replacement.is_null()) reject("child phase command must retain a roof replacement");
        const auto replacement=decode_phase_roof_replacement_authoring(intent.roof_replacement);
        if (replacement.demolition || !replacement.phase_qualified_joins || !replacement.include_hosted_instances ||
            !replacement.roof_profiles.empty() || !replacement.roof_opening_edits.empty() || replacement.roof_edits.empty())
            reject("child replacement must retain combined roof edit authority only");
    }
    return command;
}
Json encode_phase_selection(const ArchitecturalSelectionRemovalIntent& ordinary) {
    if (!ordinary.roof_additional_identities.empty() ||
        ordinary.object_ids.size()+ordinary.components.size()==0 ||
        ordinary.object_ids.size()+ordinary.components.size()>1000)
        reject("phase selection requires explicit roots without destination authority");
    Json components=Json::array();
    for (std::size_t i=0;i<ordinary.object_ids.size();++i) {
        identity(ordinary.object_ids[i]);
        if (i && ordinary.object_ids[i-1]>=ordinary.object_ids[i])
            reject("phase object selection must be ascending and unique");
    }
    for (std::size_t i=0;i<ordinary.components.size();++i) {
        const auto& key=ordinary.components[i];identity(key.first);identity(key.second);
        if (i && ordinary.components[i-1]>=key) reject("phase component selection must be ascending and unique");
        components.push_back({{"catalog_id",key.first},{"instance_id",key.second}});
    }
    return {{"version",1},{"kind","phase_demolition"},{"object_ids",ordinary.object_ids},{"components",components}};
}
ArchitecturalSelectionRemovalIntent decode_phase_selection(const Json& value) {
    fields(value,{"version","kind","object_ids","components"});
    if (!value.at("version").is_number_integer() || value.at("version")!=1 || value.at("kind")!="phase_demolition" ||
        !value.at("object_ids").is_array() || !value.at("components").is_array() ||
        value.at("object_ids").size()+value.at("components").size()>1000)
        reject("unsupported phase selection inventory");
    ArchitecturalSelectionRemovalIntent result;
    for (const auto& id:value.at("object_ids")) {
        if (!id.is_string()) reject("phase object selection requires identities");
        result.object_ids.push_back(id.get<std::string>());
    }
    for (const auto& row:value.at("components")) {
        fields(row,{"catalog_id","instance_id"});
        if (!row.at("catalog_id").is_string() || !row.at("instance_id").is_string())
            reject("phase component selection requires qualified identities");
        result.components.emplace_back(row.at("catalog_id").get<std::string>(),row.at("instance_id").get<std::string>());
    }
    if (encode_phase_selection(result).dump()!=value.dump()) reject("phase selection is not canonical");
    return result;
}
Command closed_phase_removal_command(const Json& value) {
    if (!value.is_object() || value.value("version",0)!=34 || value.value("kind",std::string{})!="apply_boundary_constraint_changes")
        reject("phase ordinary removal requires a pure outer34 command");
    preflight_phase_removal_selection_proof(value.at("phase_constraint_authoring_intent"));
    const auto command=command_from_json(value);
    if (command_to_json(command).dump()!=value.dump()) reject("phase ordinary command is not canonical");
    const auto* typed=std::get_if<ApplyBoundaryConstraintChanges>(&command);
    if (!typed || !typed->phase_constraint_authoring_completion || typed->phase_constraint_authoring_intent.is_null())
        reject("phase ordinary command lacks complete phase authority");
    // The actual-source extractor below owns demolition-only leaf admission.
    // Decoding here establishes the exact closed retained command envelope.
    (void)decode_phase_constraint_authoring_intent(typed->phase_constraint_authoring_intent);
    return command;
}
Command closed_ordinary_deletion_command(const Json& value) {
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        !value.contains("kind") || !value.at("kind").is_string()) reject("requires a closed ordinary deletion command");
    const auto version=value.at("version").get<int>();
    if (version==1 && value.at("kind")=="apply_entity_changes") return closed_command(value,false);
    if (value.at("kind")!="apply_boundary_constraint_changes" ||
        (version!=27 && version!=30 && version!=31 && version!=35 && version!=36 && version!=37 &&
            version!=38 && version!=39 && version!=40 && version!=41 && version!=46))
        reject("ordinary deletion requires a raw command or closed wall-deletion room review");
    const auto command=command_from_json(value);
    if (command_to_json(command).dump()!=value.dump()) reject("ordinary deletion command is not canonical");
    return command;
}
Command closed_phase_drawing_deletion_command(const Json& value) {
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        value.value("kind",std::string{})!="apply_boundary_constraint_changes")
        reject("phase drawing removal requires a closed original deletion");
    const auto version=value.at("version").get<int>();
    if (version!=33 && version!=34 && version!=47)
        reject("phase drawing removal requires pure phase deletion or its completed enclosure");
    if (version==34) preflight_phase_removal_selection_proof(value.at("phase_constraint_authoring_intent"));
    if (version==47) (void)validate_completed_phase_selection_removal_intent(value.at("phase_selection_removal_intent"));
    const auto command=command_from_json(value);
    if (command_to_json(command).dump()!=value.dump()) reject("phase drawing removal command is not canonical");
    return command;
}
Json encoded_edits(const std::vector<RoofEditIntent>& edits) {
    Json rows=Json::array();for (const auto& edit:edits) rows.push_back(encode_roof_edit_intent(edit));return rows;
}
void remember(const std::pair<std::string,std::string>& key,const Entities& result) {
    // A bounded cache avoids regenerating every prior mixed event recursively
    // during detached stage restoration. Account retained serialized content
    // plus typed envelopes before copying; refuse a bounded operation rather
    // than evicting proofs and falling back into exponential recomputation.
    std::size_t bytes=key.first.size()+key.second.size();
    const auto add=[&](std::size_t size) {
        if (size>memo_byte_limit-bytes) reject("aggregate detached replay cache budget exceeded");
        bytes+=size;
    };
    for (const auto& [id,entity]:result) {
        add(sizeof(Entity));add(id.size());add(entity.id.size());add(entity.type.size());
        add(entity.properties.dump().size());add(entity.extensions.dump().size());
    }
    if (replay_memo->results.size()>=4096 || bytes>memo_byte_limit-replay_memo->bytes)
        reject("aggregate detached replay cache budget exceeded");
    replay_memo->results.emplace(key,result);replay_memo->bytes+=bytes;
}
void validate_child_command(const DocumentSnapshot& stage,const Command& command,const std::vector<RoofEditIntent>& edits) {
    if (std::visit([&](const auto& typed){return typed.expected_revision!=stage.revision();},command))
        reject("child command does not bind the exact detached stage");
    const auto partition=partition_phase_roof_geometry_edits(stage.entities(),edits);
    if (const auto* raw=std::get_if<ApplyEntityChanges>(&command)) {
        if (partition.replacement || !raw->asset_changes.empty()) reject("baseline children require typed phase replacement");
        const auto candidate=Document::preview_command(stage,command);
        metadata(stage,candidate);
        if (!exact(candidate.entities(),replay_roof_edit_entities(stage.entities(),edits)))
            reject("raw child command differs from actual removal-only replay");
        return;
    }
    const auto* typed=std::get_if<ApplyBoundaryConstraintChanges>(&command);
    if (!typed || !partition.replacement) reject("child phase authority lacks an actual baseline replacement");
    const auto intent=decode_phase_constraint_authoring_intent(typed->phase_constraint_authoring_intent);
    const auto replacement=decode_phase_roof_replacement_authoring(intent.roof_replacement);
    const auto& request=*partition.replacement;
    if (replacement.registry_id!=request.registry_id || replacement.alternative_id!=request.alternative_id ||
        replacement.seed_roof_ids!=request.seed_roof_ids ||
        encoded_edits(replacement.roof_edits).dump()!=encoded_edits(partition.baseline_roof_edits).dump() ||
        encoded_edits(replacement.ordinary_roof_edits).dump()!=encoded_edits(partition.ordinary_roof_edits).dump())
        reject("phase child proof differs from actual removal-only partition");
    ConstraintAuthoringIntent semantic;semantic.message=typed->message;
    auto expected=make_phase_constraint_authoring_intent(stage,semantic);
    expected.roof_replacement=encode_phase_roof_replacement_authoring(replacement);
    if (encode_phase_constraint_authoring_intent(expected).dump()!=typed->phase_constraint_authoring_intent.dump())
        reject("child replacement carries unrelated authoring or stale staged source authority");
}
} // namespace

MixedSelectionRemovalReplayScope::MixedSelectionRemovalReplayScope() {
    if (!replay_scope_depth) replay_memo=std::make_unique<ReplayMemo>();
    ++replay_scope_depth;
}
MixedSelectionRemovalReplayScope::~MixedSelectionRemovalReplayScope() {
    if (!--replay_scope_depth) replay_memo.reset();
}

Json validate_mixed_selection_removal_intent(const Json& value) {
    try {
        std::size_t nodes=0,bytes=0;wire_budget(value,nodes,bytes);
        if (value.dump().size()>1024*1024) reject("intent exceeds one MiB");
        fields(value,{"version","ordinary","ordinary_command","members","child_command","source_snapshot_digest",
            "source_authoring_digest","source_entities_digest","source_saved_revision","stage_snapshot_digest","stage_authoring_digest"});
        if (!value.at("version").is_number_integer() || (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4 && value.at("version")!=5 && value.at("version")!=6))
            reject("unsupported intent version");
        const auto members=decode_members(value.at("members"));
        std::size_t count=0;
        if (value.at("version")==1) {
            const auto ordinary=decode_architectural_drawing_removal_intent(value.at("ordinary"));
            count=ordinary.architectural.object_ids.size()+ordinary.architectural.components.size()+
                ordinary.drawing.owner_ids.size()+ordinary.drawing.annotations.size();
            (void)closed_command(value.at("ordinary_command"),false);
        } else if (value.at("version")==2) {
            const auto ordinary=decode_phase_selection(value.at("ordinary"));
            count=ordinary.object_ids.size()+ordinary.components.size();
            (void)closed_phase_removal_command(value.at("ordinary_command"));
        } else if (value.at("version")==3) {
            const auto ordinary=decode_ordinary_selection_removal_intent(value.at("ordinary"));
            const auto authority=ordinary_selection_removal_authority(ordinary);
            count=authority.architectural.object_ids.size()+authority.architectural.components.size()+
                authority.drawing.owner_ids.size()+authority.drawing.annotations.size();
            (void)closed_ordinary_deletion_command(value.at("ordinary_command"));
            if (value.at("ordinary_command").at("version")==46) {
                const auto completed=validate_completed_ordinary_selection_removal_intent(
                    value.at("ordinary_command").at("ordinary_selection_removal_intent"));
                if (completed.at("selection").dump()!=value.at("ordinary").dump())
                    reject("completed ordinary command differs from the mixed selection intent");
            }
        } else if (value.at("version")==4) {
            const auto ordinary=decode_phase_selection_removal_intent(value.at("ordinary"));
            const auto authority=phase_selection_removal_authority(ordinary);
            count=authority.architectural.object_ids.size()+authority.architectural.components.size()+
                authority.drawing.owner_ids.size()+authority.drawing.annotations.size();
            (void)closed_phase_drawing_deletion_command(value.at("ordinary_command"));
            if (value.at("ordinary_command").at("version")==47) {
                const auto completed=validate_completed_phase_selection_removal_intent(
                    value.at("ordinary_command").at("phase_selection_removal_intent"));
                if (completed.at("selection").dump()!=value.at("ordinary").dump())
                    reject("completed phase deletion differs from the mixed selection intent");
            }
        } else {
            const auto ordinary=decode_corner_selection_removal_intent(value.at("ordinary"));
            if (ordinary.complete_corner_catalog_hosts != (value.at("version")==6))
                reject("corner selection authority differs from its mixed history dialect");
            const auto authority=corner_selection_removal_authority(ordinary);
            count=authority.architectural.object_ids.size()+authority.architectural.components.size()+
                authority.drawing.owner_ids.size()+authority.drawing.annotations.size();
            (void)closed_command(value.at("ordinary_command"),false);
        }
        if (count>1000-members.size()) reject("mixed selection exceeds 1000 aggregate members");
        if (!value.at("child_command").is_null()) (void)closed_command(value.at("child_command"),true);
        for (const auto* name:{"source_snapshot_digest","source_authoring_digest","source_entities_digest",
                "stage_snapshot_digest","stage_authoring_digest"}) digest(value.at(name));
        const auto& saved=value.at("source_saved_revision");
        if (!saved.is_null() && !(saved.is_number_unsigned() || (saved.is_number_integer() && saved.get<std::int64_t>()>=0)))
            reject("invalid captured saved revision");
        return value;
    } catch (const Json::exception& error) { reject(std::string("malformed intent: ")+error.what()); }
}
bool mixed_selection_removal_active_phase_policy(const Json& value) {
    (void)validate_mixed_selection_removal_intent(value);
    return value.at("version")==2 || value.at("version")==4 || value.at("version")==5 || value.at("version")==6 ||
        (!value.at("child_command").is_null() && value.at("child_command").at("version")==34);
}
std::optional<Revision> mixed_selection_removal_source_saved_revision(const Json& value) {
    (void)validate_mixed_selection_removal_intent(value);
    return value.at("source_saved_revision").is_null() ? std::nullopt : std::optional<Revision>{value.at("source_saved_revision").get<Revision>()};
}
DocumentSnapshot prepare_mixed_selection_removal_stage(const DocumentSnapshot& source,
    const ArchitecturalDrawingRemovalIntent& ordinary,const Command& ordinary_command) {
    const auto wire=command_to_json(ordinary_command);(void)closed_command(wire,false);
    if (std::get<ApplyEntityChanges>(ordinary_command).expected_revision!=source.revision()) reject("ordinary source revision changed");
    const auto expected=replay_architectural_drawing_removal(source,ordinary);
    const auto stage=Document::preview_command(source,ordinary_command);metadata(source,stage);
    if (!exact(stage.entities(),expected)) reject("ordinary command differs from complete actual-source selection replay");
    if (stage.history().size()!=source.history().size()+1 || stage.revision()!=source.history().size())
        reject("ordinary removal must create exactly one detached history event");
    return stage;
}
std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(const DocumentSnapshot& source,
    const ArchitecturalDrawingRemovalIntent& ordinary,const std::vector<RoofOpeningGroupMember>& members) {
    (void)encode_architectural_drawing_removal_intent(ordinary);
    return mixed_selection_removal_remaining_children(source,ordinary.architectural,members);
}
std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(const DocumentSnapshot& source,
    const ArchitecturalSelectionRemovalIntent& ordinary,const std::vector<RoofOpeningGroupMember>& members) {
    const auto canonical=decode_members(encode_members(members));
    // This helper is also called by the original raw architectural dialect,
    // whose allocated roof destinations are not part of selection authority.
    auto roots=ordinary;roots.roof_additional_identities.clear();
    if (!roots.object_ids.empty() || !roots.components.empty()) (void)encode_phase_selection(roots);
    // Even children whose selected roof dominates must be real actual members.
    // Membership does not grant independent removal authority: a whole selected
    // roof can retire a child carrying a future receipt that prevents a local
    // child edit. Only surviving children receive removal replay at the stage.
    std::map<std::string,RoofObject,std::less<>> roofs;
    for (const auto& member:canonical) {
        auto owner=roofs.find(member.roof_id);
        if (owner==roofs.end()) {
            const auto found=source.entities().find(member.roof_id);
            if (found==source.entities().end() || found->second.type!="roof") reject("selected child lacks an actual roof");
            owner=roofs.emplace(member.roof_id,decode_roof_entity(found->second)).first;
        }
        std::visit([&](const auto& roof) {
            const auto found=std::find_if(roof.openings.begin(),roof.openings.end(),[&](const auto& child) {
                return child.id==member.opening_id;
            });
            if (found==roof.openings.end() || !found->skylight) reject("selected member is not an actual profiled skylight");
        },owner->second);
    }
    auto remaining=canonical;
    std::erase_if(remaining,[&](const auto& member) {
        return std::binary_search(ordinary.object_ids.begin(),ordinary.object_ids.end(),member.roof_id);
    });
    return remaining;
}
Entities replay_mixed_selection_removal(const DocumentSnapshot& source,const Json& value) {
    MixedSelectionRemovalReplayScope scope;
    (void)validate_mixed_selection_removal_intent(value);
    if (source.history().empty() || source.history().size()>4096 || source.revision()>=source.history().size() ||
        source.history()[source.revision()].revision!=source.revision() || source.assets().size()>65536 ||
        source.named_revisions().size()>4096 ||
        (source.saved_revision_optional() && *source.saved_revision_optional()>=source.history().size()))
        reject("captured source history/metadata inventory is invalid");
    const auto source_digest=document_snapshot_digest(source);
    if (!source.is_editable() || source.saved_revision_optional()!=mixed_selection_removal_source_saved_revision(value) ||
        source_digest!=value.at("source_snapshot_digest") ||
        document_authoring_source_digest_v2(source)!=value.at("source_authoring_digest") ||
        entity_map_digest(source.entities())!=value.at("source_entities_digest")) reject("captured full source authority changed");
    const auto key=std::pair{source_digest,value.dump()};
    if (const auto retained=replay_memo->results.find(key);retained!=replay_memo->results.end()) return retained->second;
    const auto members=decode_members(value.at("members"));
    const auto phase=value.at("version")==2;
    const auto complete=value.at("version")==3;
    const auto phase_drawing=value.at("version")==4;
    const auto corners=value.at("version")==5 || value.at("version")==6;
    const auto ordinary=phase || complete || phase_drawing || corners ? ArchitecturalDrawingRemovalIntent{} : decode_architectural_drawing_removal_intent(value.at("ordinary"));
    const auto phase_selection=phase ? decode_phase_selection(value.at("ordinary")) : ArchitecturalSelectionRemovalIntent{};
    const auto complete_selection=complete ? decode_ordinary_selection_removal_intent(value.at("ordinary")) : OrdinarySelectionRemovalIntent{};
    const auto phase_drawing_selection=phase_drawing ? decode_phase_selection_removal_intent(value.at("ordinary")) : PhaseSelectionRemovalIntent{};
    const auto corner_selection=corners ? decode_corner_selection_removal_intent(value.at("ordinary")) : CornerSelectionRemovalIntent{};
    const auto remaining=corners ? mixed_selection_removal_remaining_children(source,corner_selection,members) :
        phase_drawing ? mixed_selection_removal_remaining_children(source,phase_drawing_selection,members) :
        complete ? mixed_selection_removal_remaining_children(source,complete_selection,members) :
        phase ? mixed_selection_removal_remaining_children(source,phase_selection,members) :
        mixed_selection_removal_remaining_children(source,ordinary,members);
    const auto stage=corners ? prepare_mixed_selection_removal_stage(source,corner_selection,closed_command(value.at("ordinary_command"),false)) :
        phase_drawing ? prepare_mixed_selection_removal_stage(source,phase_drawing_selection,closed_phase_drawing_deletion_command(value.at("ordinary_command"))) :
        complete ? prepare_mixed_selection_removal_stage(source,complete_selection,closed_ordinary_deletion_command(value.at("ordinary_command"))) :
        phase ? prepare_mixed_selection_removal_stage(source,phase_selection,closed_phase_removal_command(value.at("ordinary_command"))) :
        prepare_mixed_selection_removal_stage(source,ordinary,closed_command(value.at("ordinary_command"),false));
    if (document_snapshot_digest(stage)!=value.at("stage_snapshot_digest") ||
        document_authoring_source_digest_v2(stage)!=value.at("stage_authoring_digest")) reject("detached ordinary stage authority changed");
    if (remaining.empty()) {
        if (!value.at("child_command").is_null()) reject("selected roofs dominate children; no replacement authority remains");
        remember(key,stage.entities());
        return stage.entities();
    }
    if (value.at("child_command").is_null()) reject("surviving selected children require exact removal authority");
    const auto edits=prepare_roof_opening_group_removal(stage.entities(),remaining);
    const auto command=closed_command(value.at("child_command"),true);
    validate_child_command(stage,command,edits);
    const auto candidate=Document::preview_command(stage,command);metadata(stage,candidate);
    if (candidate.history().size()!=stage.history().size()+1) reject("child removal must create exactly one detached event");
    remember(key,candidate.entities());
    return candidate.entities();
}
Json make_mixed_selection_removal_intent(const DocumentSnapshot& source,const ArchitecturalDrawingRemovalIntent& ordinary,
    const Command& ordinary_command,const std::vector<RoofOpeningGroupMember>& members,const std::optional<Command>& child_command) {
    const auto stage=prepare_mixed_selection_removal_stage(source,ordinary,ordinary_command);
    Json value={{"version",1},{"ordinary",encode_architectural_drawing_removal_intent(ordinary)},
        {"ordinary_command",command_to_json(ordinary_command)},{"members",encode_members(members)},
        {"child_command",child_command ? command_to_json(*child_command) : Json(nullptr)},
        {"source_snapshot_digest",document_snapshot_digest(source)},
        {"source_authoring_digest",document_authoring_source_digest_v2(source)},
        {"source_entities_digest",entity_map_digest(source.entities())},
        {"source_saved_revision",source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"stage_snapshot_digest",document_snapshot_digest(stage)},
        {"stage_authoring_digest",document_authoring_source_digest_v2(stage)}};
    (void)replay_mixed_selection_removal(source,value);
    return value;
}
DocumentSnapshot prepare_mixed_selection_removal_stage(const DocumentSnapshot& source,
    const ArchitecturalSelectionRemovalIntent& ordinary,const Command& ordinary_command) {
    const auto selection=encode_phase_selection(ordinary);
    const auto canonical=closed_phase_removal_command(command_to_json(ordinary_command));
    const auto& typed=std::get<ApplyBoundaryConstraintChanges>(canonical);
    const auto actual=phase_removal_selection_authority(source,typed);
    if (encode_phase_selection(actual).dump()!=selection.dump())
        reject("ordinary phase proof does not represent the exact explicit selection");
    const auto stage=Document::preview_command(source,canonical);metadata(source,stage);
    if (stage.history().size()!=source.history().size()+1 || stage.revision()!=source.history().size())
        reject("ordinary phase removal must create exactly one detached history event");
    return stage;
}
Json make_mixed_selection_removal_intent(const DocumentSnapshot& source,const ArchitecturalSelectionRemovalIntent& ordinary,
    const Command& ordinary_command,const std::vector<RoofOpeningGroupMember>& members,const std::optional<Command>& child_command) {
    const auto stage=prepare_mixed_selection_removal_stage(source,ordinary,ordinary_command);
    Json value={{"version",2},{"ordinary",encode_phase_selection(ordinary)},
        {"ordinary_command",command_to_json(ordinary_command)},{"members",encode_members(members)},
        {"child_command",child_command ? command_to_json(*child_command) : Json(nullptr)},
        {"source_snapshot_digest",document_snapshot_digest(source)},
        {"source_authoring_digest",document_authoring_source_digest_v2(source)},
        {"source_entities_digest",entity_map_digest(source.entities())},
        {"source_saved_revision",source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"stage_snapshot_digest",document_snapshot_digest(stage)},
        {"stage_authoring_digest",document_authoring_source_digest_v2(stage)}};
    (void)replay_mixed_selection_removal(source,value);
    return value;
}
DocumentSnapshot prepare_mixed_selection_removal_stage(const DocumentSnapshot& source,
    const OrdinarySelectionRemovalIntent& ordinary,const Command& ordinary_command) {
    (void)closed_ordinary_deletion_command(command_to_json(ordinary_command));
    const auto stage=prepare_ordinary_selection_removal_stage(source,ordinary,ordinary_command);metadata(source,stage);
    if (stage.history().size()!=source.history().size()+1 || stage.revision()!=source.history().size())
        reject("ordinary deletion must create exactly one detached history event");
    return stage;
}
std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(const DocumentSnapshot& source,
    const OrdinarySelectionRemovalIntent& ordinary,const std::vector<RoofOpeningGroupMember>& members) {
    const auto authority=ordinary_selection_removal_authority(ordinary);
    return mixed_selection_removal_remaining_children(source,authority.architectural,members);
}
Json make_mixed_selection_removal_intent(const DocumentSnapshot& source,const OrdinarySelectionRemovalIntent& ordinary,
    const Command& ordinary_command,const std::vector<RoofOpeningGroupMember>& members,const std::optional<Command>& child_command) {
    const auto stage=prepare_mixed_selection_removal_stage(source,ordinary,ordinary_command);
    Json value={{"version",3},{"ordinary",encode_ordinary_selection_removal_intent(ordinary)},
        {"ordinary_command",command_to_json(ordinary_command)},{"members",encode_members(members)},
        {"child_command",child_command ? command_to_json(*child_command) : Json(nullptr)},
        {"source_snapshot_digest",document_snapshot_digest(source)},
        {"source_authoring_digest",document_authoring_source_digest_v2(source)},
        {"source_entities_digest",entity_map_digest(source.entities())},
        {"source_saved_revision",source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"stage_snapshot_digest",document_snapshot_digest(stage)},
        {"stage_authoring_digest",document_authoring_source_digest_v2(stage)}};
    (void)replay_mixed_selection_removal(source,value);
    return value;
}
DocumentSnapshot prepare_mixed_selection_removal_stage(const DocumentSnapshot& source,
    const PhaseSelectionRemovalIntent& ordinary,const Command& ordinary_command) {
    (void)closed_phase_drawing_deletion_command(command_to_json(ordinary_command));
    const auto stage=prepare_phase_selection_removal_stage(source,ordinary,ordinary_command);metadata(source,stage);
    if (stage.history().size()!=source.history().size()+1 || stage.revision()!=source.history().size())
        reject("phase drawing deletion must create exactly one detached history event");
    return stage;
}
std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(const DocumentSnapshot& source,
    const PhaseSelectionRemovalIntent& ordinary,const std::vector<RoofOpeningGroupMember>& members) {
    const auto authority=phase_selection_removal_authority(ordinary);
    return mixed_selection_removal_remaining_children(source,authority.architectural,members);
}
Json make_mixed_selection_removal_intent(const DocumentSnapshot& source,const PhaseSelectionRemovalIntent& ordinary,
    const Command& ordinary_command,const std::vector<RoofOpeningGroupMember>& members,const std::optional<Command>& child_command) {
    const auto stage=prepare_mixed_selection_removal_stage(source,ordinary,ordinary_command);
    Json value={{"version",4},{"ordinary",encode_phase_selection_removal_intent(ordinary)},
        {"ordinary_command",command_to_json(ordinary_command)},{"members",encode_members(members)},
        {"child_command",child_command ? command_to_json(*child_command) : Json(nullptr)},
        {"source_snapshot_digest",document_snapshot_digest(source)},
        {"source_authoring_digest",document_authoring_source_digest_v2(source)},
        {"source_entities_digest",entity_map_digest(source.entities())},
        {"source_saved_revision",source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"stage_snapshot_digest",document_snapshot_digest(stage)},
        {"stage_authoring_digest",document_authoring_source_digest_v2(stage)}};
    (void)replay_mixed_selection_removal(source,value);
    return value;
}
DocumentSnapshot prepare_mixed_selection_removal_stage(const DocumentSnapshot& source,
    const CornerSelectionRemovalIntent& ordinary,const Command& ordinary_command) {
    const auto canonical=closed_command(command_to_json(ordinary_command),false);
    if (std::visit([&](const auto& typed){return typed.expected_revision!=source.revision();},canonical))
        reject("corner removal source revision changed");
    const auto expected=replay_corner_selection_removal(source,ordinary);
    const auto stage=Document::preview_command(source,canonical);metadata(source,stage);
    if (!exact(stage.entities(),expected)) reject("corner command differs from complete actual-source reconstruction");
    if (stage.history().size()!=source.history().size()+1 || stage.revision()!=source.history().size())
        reject("corner removal must create exactly one detached history event");
    return stage;
}
std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(const DocumentSnapshot& source,
    const CornerSelectionRemovalIntent& ordinary,const std::vector<RoofOpeningGroupMember>& members) {
    const auto authority=corner_selection_removal_authority(ordinary);
    return mixed_selection_removal_remaining_children(source,authority.architectural,members);
}
Json make_mixed_selection_removal_intent(const DocumentSnapshot& source,const CornerSelectionRemovalIntent& ordinary,
    const Command& ordinary_command,const std::vector<RoofOpeningGroupMember>& members,const std::optional<Command>& child_command) {
    const auto stage=prepare_mixed_selection_removal_stage(source,ordinary,ordinary_command);
    Json value={{"version",ordinary.complete_corner_catalog_hosts ? 6 : 5},{"ordinary",encode_corner_selection_removal_intent(ordinary)},
        {"ordinary_command",command_to_json(ordinary_command)},{"members",encode_members(members)},
        {"child_command",child_command ? command_to_json(*child_command) : Json(nullptr)},
        {"source_snapshot_digest",document_snapshot_digest(source)},
        {"source_authoring_digest",document_authoring_source_digest_v2(source)},
        {"source_entities_digest",entity_map_digest(source.entities())},
        {"source_saved_revision",source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"stage_snapshot_digest",document_snapshot_digest(stage)},
        {"stage_authoring_digest",document_authoring_source_digest_v2(stage)}};
    (void)replay_mixed_selection_removal(source,value);
    return value;
}
} // namespace sketch
