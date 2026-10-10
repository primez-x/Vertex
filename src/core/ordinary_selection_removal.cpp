#include "sketch/ordinary_selection_removal.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/hosted_opening_removal.hpp"
#include "sketch/mixed_wall_opening_removal.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/wall_join_removal.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=DrawingSelectionRemovalEntities;
constexpr std::size_t byte_limit=1024*1024, selection_limit=1000;
constexpr std::size_t memo_byte_limit=128*1024*1024;
struct ReplayMemo {
    std::map<std::pair<std::string,std::string>,Entities> results;
    std::size_t bytes{};
};
thread_local std::unique_ptr<ReplayMemo> replay_memo;
thread_local std::size_t replay_scope_depth{};

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Ordinary selection removal: "+reason);
}
void fields(const Json& value,std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size()!=names.size()) reject("unsupported intent fields");
    for (const auto* name:names) if (!value.contains(name)) reject("missing intent field");
}
void wire_budget(const Json& value,std::size_t& nodes,std::size_t& bytes,std::size_t depth=0) {
    if (depth>64 || ++nodes>131072) reject("intent nesting/node budget exceeded");
    const auto text=[&](const std::string& string) {
        if (string.size()>byte_limit-bytes) reject("intent string/key budget exceeded");
        bytes+=string.size();
    };
    if (value.is_binary() || value.is_discarded()) reject("unsupported intent JSON value");
    if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("intent scalar must be finite");
    if (value.is_string()) text(value.get_ref<const std::string&>());
    if (value.is_object()) for (const auto& [key,child]:value.items()) {
        text(key);wire_budget(child,nodes,bytes,depth+1);
    } else if (value.is_array()) for (const auto& child:value) wire_budget(child,nodes,bytes,depth+1);
}
void bounded_wire(const Json& value) {
    std::size_t nodes=0,bytes=0;wire_budget(value,nodes,bytes);
    if (value.dump().size()>byte_limit) reject("intent exceeds one MiB");
}
void identity(const std::string& id) {
    if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            c=='-' || c=='_' || c=='.' || c==':';
    })) reject("invalid actual owner identity");
}
void canonical_ids(const std::vector<std::string>& ids) {
    if (ids.empty() || ids.size()>selection_limit || !std::is_sorted(ids.begin(),ids.end()) ||
        std::adjacent_find(ids.begin(),ids.end())!=ids.end()) reject("roots must be nonempty, sorted and unique");
    for (const auto& id:ids) identity(id);
}
bool has_other(const ArchitecturalSelectionRemovalIntent& intent) {
    return !intent.object_ids.empty() || !intent.components.empty();
}
bool has_drawing(const DrawingSelectionRemovalIntent& intent) {
    return !intent.owner_ids.empty() || !intent.annotations.empty();
}
void aggregate(const ArchitecturalDrawingRemovalIntent& intent) {
    std::size_t count=0;
    for (const auto size:{intent.architectural.object_ids.size(),intent.architectural.components.size(),
            intent.drawing.owner_ids.size(),intent.drawing.annotations.size()}) {
        if (size>selection_limit-count) reject("selection exceeds 1000 aggregate roots/components/rows");
        count+=size;
    }
    if (!count) reject("selection is empty");
    // The old codec checks cross-lane ownership, fresh roof slots and canonical
    // qualified component/drawing order without interpreting physical roots.
    (void)encode_architectural_drawing_removal_intent(intent);
}
void add_roots(ArchitecturalSelectionRemovalIntent& intent,const std::vector<std::string>& roots) {
    intent.object_ids.insert(intent.object_ids.end(),roots.begin(),roots.end());
    std::sort(intent.object_ids.begin(),intent.object_ids.end());
    if (std::adjacent_find(intent.object_ids.begin(),intent.object_ids.end())!=intent.object_ids.end())
        reject("an explicit root occurs in multiple deletion lanes");
}
Command closed_raw(const Json& wire) {
    bounded_wire(wire);
    fields(wire,{"version","kind","expected_revision","message","entity_changes","asset_changes"});
    if (!wire.at("version").is_number_integer() || wire.at("version")!=1 ||
        wire.at("kind")!="apply_entity_changes") reject("requires a canonical raw version-one command");
    const auto command=command_from_json(wire);
    const auto* raw=std::get_if<ApplyEntityChanges>(&command);
    if (!raw || !raw->asset_changes.empty() || raw->entity_changes.empty() || raw->entity_changes.size()>4096 ||
        command_to_json(command).dump()!=wire.dump()) reject("requires a bounded canonical asset-free raw deletion");
    return command;
}
struct WallAuthority {
    Command command;
    ArchitecturalSelectionRemovalIntent architectural;
    std::vector<std::string> wall_ids;
    PhysicalWallJoinRemovalAdditionalIdentities destinations;
    bool opening_hosts{};
};
void merge_destinations(PhysicalWallJoinRemovalAdditionalIdentities& target,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional) {
    std::set<std::string,std::less<>> fresh;
    for (const auto& [owner,ids]:target) { (void)owner;fresh.insert(ids.begin(),ids.end()); }
    for (const auto& [owner,ids]:additional) {
        if (target.contains(owner)) reject("wall and roof destination owners overlap");
        for (const auto& id:ids) if (!fresh.insert(id).second) reject("wall and roof destination identities overlap");
        target.emplace(owner,ids);
    }
}
WallAuthority wall_authority(const Json& proof,const std::vector<std::string>& claimed_walls) {
    bounded_wire(proof);canonical_ids(claimed_walls);
    if (!proof.is_object() || !proof.contains("version") || !proof.at("version").is_number_integer() ||
        !proof.contains("kind") || !proof.at("kind").is_string()) reject("requires an explicit closed deletion proof");
    WallAuthority result;
    const auto& kind=proof.at("kind");
    const auto& version=proof.at("version");
    if (kind=="physical_wall_deletion" && (version==31 || version==35 || version==36 || version==38)) {
        result.command=decode_physical_wall_deletion_review_proof(proof);
        // Read only after the closed public decoder has admitted the inventory.
        result.wall_ids=proof.at("wall_ids").get<std::vector<std::string>>();
        if (version==36 || version==38)
            result.destinations=proof.at("additional_join_identities").get<PhysicalWallJoinRemovalAdditionalIdentities>();
        result.opening_hosts=version==38;
    } else if (kind=="mixed_wall_deletion" && (version==37 || version==39)) {
        const auto decoded=decode_mixed_wall_deletion_review_proof(proof);
        result.command=Command{decoded.command};result.wall_ids=decoded.intent.wall_ids;
        result.architectural=decoded.intent.other;result.destinations=decoded.intent.wall_additional_identities;
        result.opening_hosts=decoded.complete_opening_hosted_removal;
    } else if (kind=="mixed_wall_opening_deletion" && version==40) {
        const auto decoded=decode_mixed_wall_opening_deletion_review_proof(proof);
        result.command=Command{decoded.command};result.wall_ids=decoded.intent.wall_ids;
        result.architectural=decoded.intent.other;add_roots(result.architectural,decoded.intent.opening_ids);
        result.destinations=decoded.intent.wall_additional_identities;result.opening_hosts=true;
    } else if (kind=="apply_entity_changes" && version==1) {
        if (claimed_walls.size()!=1) reject("legacy raw deletion requires exactly one explicitly claimed wall");
        result.command=closed_raw(proof);result.wall_ids=claimed_walls;
    } else reject("unsupported wall deletion proof authority");
    if (result.wall_ids!=claimed_walls) reject("claimed walls differ from the typed deletion selection");
    add_roots(result.architectural,result.wall_ids);
    merge_destinations(result.destinations,result.architectural.roof_additional_identities);
    (void)closed_raw(command_to_json(result.command));
    return result;
}
ArchitecturalDrawingRemovalIntent authority(const OrdinarySelectionRemovalIntent& intent) {
    if (intent.openings.has_value()==!intent.wall_geometry_proof.is_null()) reject("requires exactly one ordinary deletion mode");
    ArchitecturalDrawingRemovalIntent result;result.drawing=intent.drawing;
    if (intent.openings) {
        if (!intent.wall_ids.empty()) reject("hosted opening mode cannot claim walls");
        const auto& opening=*intent.openings;canonical_ids(opening.opening_ids);
        if (!has_other(opening.other) && !opening.other.roof_additional_identities.empty())
            reject("roof destinations require other architectural selections");
        result.architectural=opening.other;add_roots(result.architectural,opening.opening_ids);
        result.allow_manufactured_opening_hosts=true;
    } else {
        const auto wall=wall_authority(intent.wall_geometry_proof,intent.wall_ids);
        result.architectural=wall.architectural;result.allow_manufactured_opening_hosts=wall.opening_hosts;
    }
    aggregate(result);return result;
}
bool exact_entity(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
bool exact(const Entities& a,const Entities& b) {
    if (a.size()!=b.size()) return false;
    for (const auto& [id,entity]:a) {
        const auto after=b.find(id);
        if (after==b.end() || !exact_entity(entity,after->second)) return false;
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
Entities raw_candidate(const Entities& actual,const Command& command) {
    auto result=actual;std::set<std::string,std::less<>> changed;
    for (const auto& row:std::get<ApplyEntityChanges>(command).entity_changes) {
        const auto& id=row.kind==EntityChangeKind::erase ? row.entity_id : row.entity.id;
        if (!changed.insert(id).second) reject("raw deletion repeats an owner");
        if (row.kind==EntityChangeKind::erase) {
            if (!result.erase(id)) reject("raw deletion erases an absent actual owner");
        } else result.insert_or_assign(id,row.entity);
    }
    return result;
}
void actual_wall_roots(const Entities& actual,const Entities& candidate,const std::vector<std::string>& claimed) {
    for (const auto& id:claimed) {
        const auto found=actual.find(id);
        if (found==actual.end() || found->second.type!="wall" || candidate.contains(id))
            reject("explicit wall root is not an actual removed wall");
    }
    std::vector<std::string> removed;
    for (const auto& [id,entity]:actual) if (entity.type=="wall" && !candidate.contains(id)) removed.push_back(id);
    if (removed!=claimed) reject("actual wall retirement differs from the explicit wall selection");
}
bool room_deletion_version(const Json& version) {
    return version.is_number_integer() && (version==27 || version==30 || version==31 || version==35 ||
        version==36 || version==37 || version==38 || version==39 || version==40);
}
Command closed_base(const Json& wire) {
    bounded_wire(wire);
    if (!wire.is_object() || !wire.contains("version") || !wire.at("version").is_number_integer() ||
        !wire.contains("kind")) reject("requires a closed base deletion command");
    if (wire.at("version")==1 && wire.at("kind")=="apply_entity_changes") return closed_raw(wire);
    if (wire.at("kind")!="apply_boundary_constraint_changes" || !room_deletion_version(wire.at("version")))
        reject("base requires a pure deletion room review or asset-free raw command");
    const auto command=command_from_json(wire);
    const auto* typed=std::get_if<ApplyBoundaryConstraintChanges>(&command);
    if (!typed || !typed->room_review_completion || !typed->room_review_geometry_completion ||
        typed->independent_drawing_removal_completion || !typed->independent_drawing_removal_intent.is_null() ||
        command_to_json(command).dump()!=wire.dump()) reject("base deletion room review is not pure and canonical");
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
} // namespace

OrdinarySelectionRemovalReplayScope::OrdinarySelectionRemovalReplayScope() {
    if (!replay_scope_depth) replay_memo=std::make_unique<ReplayMemo>();
    ++replay_scope_depth;
}
OrdinarySelectionRemovalReplayScope::~OrdinarySelectionRemovalReplayScope() {
    if (!--replay_scope_depth) replay_memo.reset();
}

Json encode_ordinary_selection_removal_intent(const OrdinarySelectionRemovalIntent& intent) {
    (void)authority(intent);
    const auto drawing=has_drawing(intent.drawing) ? encode_drawing_selection_removal_intent(intent.drawing) : Json(nullptr);
    Json result;
    if (intent.openings) {
        ArchitecturalDrawingRemovalIntent other;other.architectural=intent.openings->other;
        other.allow_manufactured_opening_hosts=true;
        result={{"version",1},{"kind","hosted_opening"},{"opening_ids",intent.openings->opening_ids},
            {"other",has_other(other.architectural) ? encode_architectural_drawing_removal_intent(other) : Json(nullptr)},
            {"drawing",drawing}};
    } else result={{"version",1},{"kind","wall_geometry"},{"wall_ids",intent.wall_ids},
        {"geometry_proof",intent.wall_geometry_proof},{"drawing",drawing}};
    bounded_wire(result);return result;
}

OrdinarySelectionRemovalIntent decode_ordinary_selection_removal_intent(const Json& value) try {
    bounded_wire(value);
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        value.at("version")!=1 || !value.contains("kind") || !value.at("kind").is_string())
        reject("unsupported intent version or kind");
    OrdinarySelectionRemovalIntent result;
    if (value.at("kind")=="hosted_opening") {
        fields(value,{"version","kind","opening_ids","other","drawing"});
        if (!value.at("opening_ids").is_array() || value.at("opening_ids").size()>selection_limit)
            reject("invalid opening inventory");
        OpeningArchitecturalRemovalIntent opening;
        opening.opening_ids=value.at("opening_ids").get<std::vector<std::string>>();
        if (!value.at("other").is_null()) {
            const auto other=decode_architectural_drawing_removal_intent(value.at("other"));
            if (!has_other(other.architectural) || has_drawing(other.drawing) || !other.allow_manufactured_opening_hosts)
                reject("other lane requires only explicit architectural selections and opening-host admission");
            opening.other=other.architectural;
        }
        result.openings=std::move(opening);
    } else if (value.at("kind")=="wall_geometry") {
        fields(value,{"version","kind","wall_ids","geometry_proof","drawing"});
        if (!value.at("wall_ids").is_array() || value.at("wall_ids").size()>selection_limit)
            reject("invalid wall inventory");
        result.wall_ids=value.at("wall_ids").get<std::vector<std::string>>();
        result.wall_geometry_proof=value.at("geometry_proof");
    } else reject("unsupported ordinary deletion kind");
    if (!value.at("drawing").is_null()) result.drawing=decode_drawing_selection_removal_intent(value.at("drawing"));
    if (encode_ordinary_selection_removal_intent(result).dump()!=value.dump()) reject("intent is not canonical");
    return result;
} catch (const Json::exception& error) { reject(std::string("malformed intent: ")+error.what()); }

ArchitecturalDrawingRemovalIntent ordinary_selection_removal_authority(const OrdinarySelectionRemovalIntent& intent) {
    // Encoding also bounds the whole nested wire, not just its separate leaves.
    (void)encode_ordinary_selection_removal_intent(intent);return authority(intent);
}

DocumentSnapshot prepare_ordinary_selection_removal_stage(const DocumentSnapshot& source,
    const OrdinarySelectionRemovalIntent& intent,const Command& original_command) try {
    OrdinarySelectionRemovalReplayScope scope;
    (void)encode_ordinary_selection_removal_intent(intent);
    if (!source.is_editable()) reject("captured source is read-only");
    const auto wire=command_to_json(original_command);bounded_wire(wire);
    const auto canonical=command_from_json(wire);
    if (command_to_json(canonical).dump()!=wire.dump() ||
        std::visit([&](const auto& command){return command.expected_revision!=source.revision();},canonical))
        reject("original command does not retain canonical actual source revision authority");
    const bool active=source.uses_active_phase_constraints();
    Entities expected;
    const bool completed=wire.at("kind")=="apply_boundary_constraint_changes" && wire.at("version")==46;
    if (completed) {
        const auto& typed=std::get<ApplyBoundaryConstraintChanges>(canonical);
        const auto retained=validate_completed_ordinary_selection_removal_intent(typed.ordinary_selection_removal_intent);
        if (!typed.ordinary_selection_removal_completion ||
            retained.at("selection").dump()!=encode_ordinary_selection_removal_intent(intent).dump())
            reject("completed ordinary command differs from the exact helper selection");
        expected=replay_completed_ordinary_selection_removal(source,retained);
    } else if (intent.openings) {
        (void)closed_raw(wire);
        validate_physical_wall_join_removal_identity_lifetime(source,intent.openings->other.roof_additional_identities);
        if (has_drawing(intent.drawing)) expected=replay_drawing_selection_removal_with_openings(
            source.entities(),intent.drawing,*intent.openings,active);
        else if (has_other(intent.openings->other)) expected=replay_opening_architectural_removal(
            source.entities(),*intent.openings,active);
        else {
            const auto opening=replay_hosted_opening_removal(source.entities(),intent.openings->opening_ids,active,true);
            if (!opening) reject("actual source does not admit the explicit opening selection");
            expected=*opening;
        }
    } else {
        const auto wall=wall_authority(intent.wall_geometry_proof,intent.wall_ids);
        if (std::get<ApplyEntityChanges>(wall.command).expected_revision!=source.revision())
            reject("retained wall proof does not bind the actual source revision");
        validate_physical_wall_join_removal_identity_lifetime(source,wall.destinations);
        const auto geometry=raw_candidate(source.entities(),wall.command);
        actual_wall_roots(source.entities(),geometry,intent.wall_ids);
        validate_physical_wall_room_deletion_review_source(source.entities(),geometry,wall.command,
            intent.wall_geometry_proof,active);
        if (std::holds_alternative<ApplyEntityChanges>(canonical)) {
            (void)closed_raw(wire);
            if (!has_drawing(intent.drawing) && wire.dump()!=command_to_json(wall.command).dump())
                reject("ordinary wall command differs from the complete retained raw proof");
            expected=has_drawing(intent.drawing) ? replay_drawing_selection_removal_with_deletion_geometry(
                source.entities(),intent.drawing,intent.wall_geometry_proof,active) : geometry;
        } else {
            if (wire.at("kind")!="apply_boundary_constraint_changes") reject("requires the original deletion room-review command");
            const bool drawing=wire.at("version")==41;
            if (drawing!=has_drawing(intent.drawing)) reject("drawing authority differs from the original review command");
            const auto& review_wire=drawing ? wire.at("proof") : wire;
            if (!review_wire.is_object() || !review_wire.contains("version") ||
                !room_deletion_version(review_wire.at("version"))) reject("unsupported deletion room-review enclosure");
            const auto review=command_from_json(review_wire);
            const auto* typed=std::get_if<ApplyBoundaryConstraintChanges>(&review);
            if (!typed || !typed->room_review_completion || !typed->room_review_geometry_completion ||
                typed->room_review_geometry_proof.dump()!=intent.wall_geometry_proof.dump() ||
                command_to_json(review).dump()!=review_wire.dump()) reject("review does not retain the exact typed wall deletion proof");
            if (drawing) {
                const auto& complete=std::get<ApplyBoundaryConstraintChanges>(canonical);
                if (!complete.independent_drawing_removal_completion ||
                    decode_drawing_selection_removal_intent(complete.independent_drawing_removal_intent)!=intent.drawing ||
                    complete.room_review_geometry_proof.dump()!=intent.wall_geometry_proof.dump())
                    reject("review drawing authority differs from the complete explicit helper intent");
            }
            // Preview the complete room decisions independently. The closed
            // geometry proof was reconstructed above from the untouched actual
            // source. The source-owning drawing producer independently admits
            // the exact complete room-review stage before composing its rows.
            if (drawing) expected=replay_drawing_selection_removal_with_deletion_review(
                source,intent.drawing,intent.wall_geometry_proof,review);
            else {
                const auto reviewed=Document::preview_command(source,review);metadata(source,reviewed);
                expected=reviewed.entities();
            }
        }
    }
    const auto stage=Document::preview_command(source,original_command);metadata(source,stage);
    if (!exact(stage.entities(),expected)) reject("complete original command differs from independently reconstructed ordinary removal");
    if (stage.history().size()!=source.history().size()+1 || stage.revision()!=source.history().size())
        reject("ordinary removal must create exactly one detached history event");
    if (intent.openings) {
        std::vector<std::string> hosts;
        for (const auto& id:intent.openings->opening_ids) {
            const auto& opening=source.entities().at(id);
            const auto host=opening.properties.at("wall_id").get<std::string>();
            if (stage.entities().contains(host)) hosts.push_back(host);
        }
        std::sort(hosts.begin(),hosts.end());hosts.erase(std::unique(hosts.begin(),hosts.end()),hosts.end());
        validate_architectural_geometry_changes(source,stage,hosts);
    } else validate_architectural_geometry_changes(source,stage);
    return stage;
} catch (const Json::exception& error) { reject(std::string("malformed captured command/source: ")+error.what()); }

Json validate_completed_ordinary_selection_removal_intent(const Json& value) try {
    bounded_wire(value);
    fields(value,{"version","selection","base_command","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision"});
    if (!value.at("version").is_number_integer() || value.at("version")!=1)
        reject("unsupported completed intent version");
    const auto selection=decode_ordinary_selection_removal_intent(value.at("selection"));
    if (!has_drawing(selection.drawing)) reject("completed deletion requires explicit selected drawing owners/rows");
    const auto base=closed_base(value.at("base_command"));
    if (selection.openings) {
        if (!std::holds_alternative<ApplyEntityChanges>(base)) reject("hosted opening base must be a raw deletion");
    } else {
        const auto wall=wall_authority(selection.wall_geometry_proof,selection.wall_ids);
        if (std::holds_alternative<ApplyEntityChanges>(base)) {
            if (command_to_json(base).dump()!=command_to_json(wall.command).dump())
                reject("base raw command differs from the exact retained wall deletion");
        } else {
            const auto& review=std::get<ApplyBoundaryConstraintChanges>(base);
            if (review.room_review_geometry_proof.dump()!=selection.wall_geometry_proof.dump())
                reject("base room review differs from the exact retained wall deletion");
        }
    }
    for (const auto* name:{"source_snapshot_digest","source_authoring_digest","source_entities_digest"})
        digest(value.at(name));
    (void)saved_revision(value.at("source_saved_revision"));
    // Both nested codecs require exact canonical shape; all remaining fields
    // have a single closed representation, including the captured save value.
    return value;
} catch (const Json::exception& error) { reject(std::string("malformed completed intent: ")+error.what()); }

std::optional<Revision> ordinary_selection_removal_source_saved_revision(const Json& value) {
    (void)validate_completed_ordinary_selection_removal_intent(value);
    return saved_revision(value.at("source_saved_revision"));
}

Entities replay_completed_ordinary_selection_removal(const DocumentSnapshot& source,const Json& value) try {
    OrdinarySelectionRemovalReplayScope scope;
    const auto retained=validate_completed_ordinary_selection_removal_intent(value);
    if (source.history().empty() || source.history().size()>4096 || source.revision()>=source.history().size() ||
        source.history()[source.revision()].revision!=source.revision() || source.assets().size()>65536 ||
        source.named_revisions().size()>4096 ||
        (source.saved_revision_optional() && *source.saved_revision_optional()>=source.history().size()))
        reject("captured source history/metadata inventory is invalid");
    const auto source_digest=document_snapshot_digest(source);
    if (!source.is_editable() || source.saved_revision_optional()!=saved_revision(retained.at("source_saved_revision")) ||
        source_digest!=retained.at("source_snapshot_digest") ||
        document_authoring_source_digest_v2(source)!=retained.at("source_authoring_digest") ||
        entity_map_digest(source.entities())!=retained.at("source_entities_digest"))
        reject("captured complete source authority changed");
    const auto key=std::pair{source_digest,retained.dump()};
    if (const auto found=replay_memo->results.find(key);found!=replay_memo->results.end()) return found->second;
    const auto selection=decode_ordinary_selection_removal_intent(retained.at("selection"));
    const auto base=closed_base(retained.at("base_command"));
    // The complete original deletion has its own source-derived expected map.
    // Never compare this base preview to the later drawing-composed result.
    auto without_drawing=selection;without_drawing.drawing={};
    (void)prepare_ordinary_selection_removal_stage(source,without_drawing,base);
    Entities expected;
    if (selection.openings) expected=replay_drawing_selection_removal_with_openings(
        source.entities(),selection.drawing,*selection.openings,source.uses_active_phase_constraints());
    else if (std::holds_alternative<ApplyEntityChanges>(base))
        expected=replay_drawing_selection_removal_with_deletion_geometry(source.entities(),selection.drawing,
            selection.wall_geometry_proof,source.uses_active_phase_constraints());
    else expected=replay_drawing_selection_removal_with_deletion_review(
        source,selection.drawing,selection.wall_geometry_proof,base);
    remember(key,expected);
    return expected;
} catch (const Json::exception& error) { reject(std::string("malformed completed source/intent: ")+error.what()); }

ApplyBoundaryConstraintChanges prepare_completed_ordinary_selection_removal(const DocumentSnapshot& source,
    const OrdinarySelectionRemovalIntent& intent,const Command& base_command) {
    OrdinarySelectionRemovalReplayScope scope;
    Json value={{"version",1},{"selection",encode_ordinary_selection_removal_intent(intent)},
        {"base_command",command_to_json(base_command)},{"source_snapshot_digest",document_snapshot_digest(source)},
        {"source_authoring_digest",document_authoring_source_digest_v2(source)},
        {"source_entities_digest",entity_map_digest(source.entities())},
        {"source_saved_revision",source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)}};
    (void)validate_completed_ordinary_selection_removal_intent(value);
    const auto expected=replay_completed_ordinary_selection_removal(source,value);
    ApplyBoundaryConstraintChanges command;
    command.expected_revision=source.revision();
    command.message=std::visit([](const auto& typed){return typed.message;},base_command);
    command.ordinary_selection_removal_completion=true;
    command.ordinary_selection_removal_intent=std::move(value);
    const auto candidate=prepare_ordinary_selection_removal_stage(source,intent,Command{command});
    if (!exact(candidate.entities(),expected)) reject("completed ordinary command changed its admitted candidate");
    return command;
}

} // namespace sketch
