#include "sketch/physical_wall_room_review.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "support/noninteractive_errors.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool ok,const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    catch (const DocumentError&) { return; }
    throw std::runtime_error("invalid room review accepted");
}
Entity entity(std::string id,std::string type,Json properties=Json::object()) {
    return {std::move(id),std::move(type),std::move(properties),false,Json::object()};
}
Entity wall(std::string id,Vec2 a,Vec2 b) {
    return entity(std::move(id),"wall",{{"baseline",{{"start",{a.x,a.y}},{"end",{b.x,b.y}},{"sweep_radians",0}}},
        {"thickness_m",0.2},{"height_m",3.0},{"elevation_m",0.0},{"layer_id","layer"}});
}
std::vector<Entity> fixture(bool split=false) {
    std::vector<Entity> result{entity("property","property"),entity("building","building",{{"property_id","property"}}),
        entity("floor","floor",{{"building_id","building"}}),entity("layer","layer",{{"floor_id","floor"}}),
        wall("bottom",{0,0},{4,0}),wall("right",{4,0},{4,3}),wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0})};
    if (split) result.push_back(wall("divider",{2,0},{2,3}));
    return result;
}
std::vector<Entity> copy(const DocumentSnapshot& s) {
    std::vector<Entity> result; for (const auto& [id,e]:s.entities()) { (void)id; result.push_back(e); } return result;
}
PhysicalWallRoomReviewIntent review(const DocumentSnapshot& s,const PhysicalWallRoomCorrespondenceReport& report) {
    PhysicalWallRoomReviewIntent result;
    result.selected_wall_id=report.selected_wall_id; result.source_snapshot_digest=document_snapshot_digest(s);
    result.source_authoring_digest=document_authoring_source_digest_v2(s);
    result.source_saved_revision=s.saved_revision_optional();
    result.source_entities_digest=entity_map_digest(s.entities());result.context=report.context;
    result.effective_elevation_m=report.effective_elevation_m;
    for (const auto& old:report.retained)
        result.retained.push_back({old.room.id,old.descriptor_digest,PhysicalWallRoomRetainedDisposition::retire});
    for (const auto& candidate:report.fresh) {
        PhysicalWallRoomFreshDecision d; d.candidate_index=candidate.index;d.reviewed_source_lineage=candidate.source_lineage;
        const auto bounds=boundary_bounds(candidate.boundary);
        d.interior_witness={(bounds.minimum.x+bounds.maximum.x)/2,(bounds.minimum.y+bounds.maximum.y)/2};
        result.fresh.push_back(std::move(d));
    }
    return result;
}
void assign(PhysicalWallRoomFreshDecision& d,std::string room_id,PhysicalWallRoomFreshDisposition disposition,
    const PhysicalWallRoomCorrespondenceReport& report) {
    d.room_id=std::move(room_id); d.disposition=disposition;
    for (std::size_t i=0;i<report.fresh.at(d.candidate_index).boundary.size();++i) {
        d.fresh_ids.segment_ids.push_back(d.room_id+"-fresh-s"+std::to_string(i));
        d.fresh_ids.vertex_ids.push_back(d.room_id+"-fresh-v"+std::to_string(i));
    }
    if (disposition==PhysicalWallRoomFreshDisposition::create) {
        d.name="Explicit new room";d.classification="bedroom";d.context=report.context;
    }
}
PhysicalWallRoomReviewIntent retain_current_rooms(const DocumentSnapshot& source,const PhysicalWallRoomCorrespondenceReport& report) {
    auto intent=review(source,report);for (auto& old:intent.retained) old.disposition=PhysicalWallRoomRetainedDisposition::retain;
    for (const auto& candidate:report.fresh) {
        const auto owner=std::find_if(report.retained.begin(),report.retained.end(),[&](const auto& old){
            return decode_physical_wall_room_descriptor(old.room).source_lineage==candidate.source_lineage;
        });
        require(owner!=report.retained.end(),"current-room control requires an explicit exact-lineage owner per fresh destination");
        assign(intent.fresh.at(candidate.index),owner->room.id,PhysicalWallRoomFreshDisposition::retained,report);
    }
    return intent;
}
std::string captured_state(const DocumentSnapshot& snapshot) {
    return document_snapshot_digest(snapshot)+":"+document_authoring_source_digest_v2(snapshot);
}
template<class F> void refuses_unchanged(const Document& document,F action) {
    const auto before=captured_state(document.snapshot());rejects(action);
    require(captured_state(document.snapshot())==before,"review refusal changed the complete live snapshot");
}
ApplyBoundaryConstraintChanges command_for(const DocumentSnapshot& source,const PreparedPhysicalWallRoomReview& prepared) {
    ApplyBoundaryConstraintChanges command;command.expected_revision=source.revision();
    command.room_review_completion=true;command.room_review_intent=prepared.intent;command.message="Reviewed physical rooms";
    return command;
}
struct TemporaryReviewDirectory {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("physical-room-review-"+make_stable_id());
    TemporaryReviewDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryReviewDirectory() { std::error_code ignored;std::filesystem::remove_all(path,ignored); }
};
bool retains_physical_room_dimension(const DocumentSnapshot& snapshot) {
    for (const auto& record:snapshot.history()) for (const auto& [id,value]:record.entities) {
        (void)id;if (!can_recognize_boundary_dimension_entity_type(value.type)) continue;
        const auto dimension=decode_boundary_dimension_entity(value);if (!dimension.supported()) continue;
        const auto owner=record.entities.find(dimension.dimension->boundary_id);
        if (owner!=record.entities.end() && owner->second.extensions.contains("physical_wall_room")) return true;
    }
    return false;
}
void lifecycle(Document& document,const DocumentSnapshot& source,const PreparedPhysicalWallRoomReview& prepared) {
    const auto captured=captured_state(source);const auto command=command_for(source,prepared);
    const auto wire=command_to_json(Command{command});
    require(wire.at("version")==18 && command_to_json(command_from_json(wire))==wire,"dedicated review command codec lost semantic intent");
    const auto preview=Document::preview_command(source,Command{command});
    require(preview.entities()==prepared.entities && preview.assets()==source.assets(),"actual command preview differs from prepared room batch");
    require(captured_state(document.snapshot())==captured,"review preparation/preview changed full captured source");
    require(document.apply(Command{command})==source.revision()+1 && document.snapshot().history().size()==source.history().size()+1,
        "review batch did not apply exactly one revision");
    const auto applied=document.snapshot();
    require(applied.entities()==prepared.entities && applied.assets()==source.assets(),"review Apply differs from exact preview");
    require(command_to_json(Command{*applied.history().back().boundary_constraint_changes})==wire,"retained review proof differs from applied command");
    require(Document::fork(applied).snapshot().entities()==prepared.entities,"imported review history failed independent replay");
    document.undo(document.revision());const auto undone=document.snapshot();
    require(undone.entities()==source.entities() && undone.assets()==source.assets() && undone.saved_revision_optional()==source.saved_revision_optional(),
        "review Undo lost exact pre-review entities/assets/save marker");
    document.redo(document.revision());
    require(document.snapshot().entities()==prepared.entities && document.snapshot().assets()==source.assets(),"review Redo lost exact reviewed batch");
    TemporaryReviewDirectory temp;
    for (const auto& snapshot:{applied,undone}) {
        const bool physical_dimension=retains_physical_room_dimension(snapshot);
        require(ProjectStore::required_format_version(snapshot)==(physical_dimension ? 50U : 49U),"review or physical-room dimension lost its exact native reader floor");
        const auto suffix=std::to_string(snapshot.revision());const auto file=temp.path/(suffix+".bldproj");
        (void)ProjectStore::save(file,snapshot);auto reopened=ProjectStore::load(file);
        require(reopened.document.snapshot().entities()==snapshot.entities() && reopened.document.snapshot().assets()==snapshot.assets() &&
            reopened.document.snapshot().history().size()==snapshot.history().size(),"native49 reopen lost exact review state/history/assets");
        if (snapshot.revision()==applied.revision()) {
            for (const auto& [id,value]:prepared.entities) {
                (void)id;if (!can_recognize_boundary_dimension_entity_type(value.type)) continue;
                const auto decoded=decode_boundary_dimension_entity(value);if (!decoded.supported()) continue;
                const auto owner=prepared.entities.find(decoded.dimension->boundary_id);
                if (owner==prepared.entities.end() || !owner->second.extensions.contains("physical_wall_room")) continue;
                const auto expected=resolve_boundary_dimension(*decoded.dimension,prepared.entities);
                const auto actual=resolve_boundary_dimension(*decoded.dimension,reopened.document.snapshot());
                require(actual.kind==expected.kind && std::abs(actual.area()-expected.area())<1e-9 &&
                    std::abs(actual.angle()-expected.angle())<1e-9 && std::abs(actual.segment_length()-expected.segment_length())<1e-9,
                    "reopened physical-room dimension lost current authoritative quantity");
            }
            reopened.document.undo(reopened.document.revision());
            require(reopened.document.snapshot().entities()==source.entities() && reopened.document.snapshot().assets()==source.assets(),"reopened review Undo lost original state");
            reopened.document.redo(reopened.document.revision());
            require(reopened.document.snapshot().entities()==prepared.entities,"reopened review Redo lost reviewed state");
        }
        const auto extract=temp.path/("extract-"+suffix);extract_project(snapshot,extract);
        std::ifstream input(extract/"project.json");const auto manifest=Json::parse(input);
        require(manifest.at("exchange_version")== (physical_dimension ? 48 : 47) && manifest.at("revisions").size()==snapshot.history().size(),"review or physical-room dimension extract lost exact reader floor/history");
        const auto& proof=manifest.at("revisions").at(static_cast<std::size_t>(applied.revision())).at("boundary_constraint_changes");
        require(proof==wire && command_to_json(command_from_json(proof))==wire,"extracted review command did not round trip exactly");
    }
}
void imported_intent_tampering(const DocumentSnapshot& source,const PreparedPhysicalWallRoomReview& prepared) {
    auto document=Document::fork(source);document.apply(command_for(source,prepared));const auto accepted=document.snapshot();
    const auto accepted_state=captured_state(accepted);
    const std::vector<std::function<void(ApplyBoundaryConstraintChanges&)>> mutations{
        [](auto& proof){proof.room_review_intent["source_snapshot_digest"]=std::string(64,'a');},
        [](auto& proof){proof.room_review_intent["source_authoring_digest"]=std::string(64,'b');},
        [](auto& proof){proof.room_review_intent["source_saved_revision"]=0;},
        [](auto& proof){proof.room_review_intent["source_entities_digest"]=std::string(64,'c');},
        [](auto& proof){proof.room_review_intent["retained"][0]["expected_descriptor_digest"]=std::string(64,'d');},
        [](auto& proof){proof.room_review_intent["fresh"][1]["classification"]="altered reviewed classification";},
        [](auto& proof){++proof.expected_revision;},
        [](auto& proof){proof.room_review_intent=nullptr;},
    };
    for (const auto& mutate:mutations) {
        auto forged=accepted;mutate(*const_cast<std::vector<RevisionRecord>&>(forged.history()).back().boundary_constraint_changes);
        const auto before=captured_state(forged);rejects([&]{(void)Document::fork(forged);});
        require(captured_state(forged)==before && captured_state(document.snapshot())==accepted_state,"import refusal changed forged or accepted complete snapshot");
    }
    auto changed_asset=accepted;
    auto& prefix=const_cast<std::vector<RevisionRecord>&>(changed_asset.history()).front();
    const auto& asset=prefix.assets.begin()->second;prefix.assets.at(asset.id)=Asset::create(asset.id,asset.media_type,{std::byte{77}},asset.metadata);
    require(changed_asset.entities()==accepted.entities() && changed_asset.assets()==accepted.assets() && changed_asset.revision()==accepted.revision(),
        "prefix asset tampering must preserve exact visible head");
    const auto before=captured_state(changed_asset);rejects([&]{(void)Document::fork(changed_asset);});
    require(captured_state(changed_asset)==before && captured_state(document.snapshot())==accepted_state,"asset-history refusal changed captured snapshot");
}
void dedicated_command_refusals(Document& document,const DocumentSnapshot& source,const PreparedPhysicalWallRoomReview& prepared) {
    const auto command=command_for(source,prepared);
    const std::vector<std::function<void(ApplyBoundaryConstraintChanges&)>> mutations{
        [](auto& c){c.room_review_intent=nullptr;},[](auto& c){c.room_review_completion=false;},
        [](auto& c){++c.expected_revision;},
        [](auto& c){c.entity_changes.push_back(EntityChange::erase("property"));},
        [](auto& c){c.boundary_edits.emplace_back();},[](auto& c){c.wall_edits.emplace_back();},
        [](auto& c){c.physical_entity_changes.push_back(EntityChange::erase("bottom"));},
        [](auto& c){c.exterior_source_edits.emplace_back();},
        [](auto& c){c.supplemental_entity_changes.push_back(EntityChange::erase("property"));},
        [](auto& c){c.supplemental_asset_changes.push_back(AssetChange::erase("review-asset"));},
        [](auto& c){c.measured_stroke_edits.emplace_back();},[](auto& c){c.dimension_placement_moves.emplace_back();},
        [](auto& c){c.exterior_source_completion=true;},[](auto& c){c.supplemental_source_completion=true;},
        [](auto& c){c.supplemental_asset_reference_completion=true;},[](auto& c){c.rigid_wall_transform_completion=true;},
        [](auto& c){c.measured_source_completion=true;},[](auto& c){c.dimension_placement_completion=true;},
        [](auto& c){c.rigid_group_completion=true;},[](auto& c){c.joint_translation_completion=true;},
        [](auto& c){c.wall_split=WallSplitIntent{};},[](auto& c){c.exterior_corner_move=ExteriorCornerMoveIntent{};},
        [](auto& c){c.exterior_segment_resize=ExteriorSegmentResizeIntent{};},[](auto& c){c.exterior_segment_arc=ExteriorSegmentArcIntent{};},
        [](auto& c){c.rigid_group_transform=TransformBoundaries{};},[](auto& c){c.joint_translation=JointTranslationIntent{};},
    };
    for (const auto& mutate:mutations) {
        auto bad=command;mutate(bad);
        refuses_unchanged(document,[&]{(void)Document::preview_command(source,Command{bad});});
        refuses_unchanged(document,[&]{(void)document.apply(Command{bad});});
    }
}
void split_and_cancel() {
    auto original=Document::create(fixture());
    auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");
    const auto old=initial.entity_changes.at(0).entity.id;original.apply(initial);
    auto values=copy(original.snapshot());values.push_back(wall("divider",{2,0},{2,3}));
    const auto asset=Asset::create("review-asset","application/octet-stream",{std::byte{1}});
    auto document=Document::create(values,{asset});const auto source=document.snapshot();
    const auto report=physical_wall_room_correspondence(source,"bottom");
    require(report.retained.size()==1 && report.fresh.size()==2,"split expectation must contain one old and two fresh rooms");
    auto intent=review(source,report);intent.retained.at(0).disposition=PhysicalWallRoomRetainedDisposition::retain;
    assign(intent.fresh.at(0),old,PhysicalWallRoomFreshDisposition::retained,report);
    assign(intent.fresh.at(1),"explicit-new",PhysicalWallRoomFreshDisposition::create,report);
    const auto before=captured_state(source);
    const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    require(captured_state(document.snapshot())==before,"cancel/preparation altered complete document state");
    require(prepared.entities.at(old).properties.at("classification")=="office","retained classification transferred or lost");
    require(prepared.entities.at("explicit-new").properties.at("classification")=="bedroom","new explicit classification missing");
    require(!prepared.entities.at("explicit-new").properties.contains("area_m2"),"review fabricated stored area authority");
    const auto left=boundary_geometry(decode_identified_boundary_entity(prepared.entities.at(old)));
    const auto right=boundary_geometry(decode_identified_boundary_entity(prepared.entities.at("explicit-new")));
    require(std::abs(std::abs(signed_area(left))-5.04)<1e-9 && std::abs(std::abs(signed_area(right))-5.04)<1e-9,
        "split clear-room areas disagree with independent rectangular expectations");
    require(replay_physical_wall_room_review_entities(source.entities(),prepared.intent)==prepared.entities,"pure replay differs from preparation");
    auto missing=intent;missing.fresh.pop_back();rejects([&]{(void)prepare_physical_wall_room_review(source,report,missing);});
    auto duplicate=intent;duplicate.fresh.at(1).room_id=old;duplicate.fresh.at(1).disposition=PhysicalWallRoomFreshDisposition::retained;
    duplicate.fresh.at(1).name.clear();duplicate.fresh.at(1).classification.clear();duplicate.fresh.at(1).context={};
    rejects([&]{(void)prepare_physical_wall_room_review(source,report,duplicate);});
    auto outside=intent;outside.fresh.at(0).interior_witness={0,0};rejects([&]{(void)prepare_physical_wall_room_review(source,report,outside);});
    auto stale=source.entities();stale.at("property").extensions["changed"]=true;
    rejects([&]{(void)replay_physical_wall_room_review_entities(stale,prepared.intent);});
    auto unknown=prepared.intent;unknown["extra_authority"]=true;rejects([&]{(void)decode_physical_wall_room_review_intent(unknown);});
    auto codec=encode_physical_wall_room_review_intent(decode_physical_wall_room_review_intent(prepared.intent));
    require(codec==prepared.intent,"strict intent codec did not round trip");
    dedicated_command_refusals(document,source,prepared);
    imported_intent_tampering(source,prepared);
    auto split_document=Document::fork(source);lifecycle(split_document,source,prepared);
    auto altered=source;
    const_cast<std::vector<RevisionRecord>&>(altered.history()).front().assets.at(asset.id)=
        Asset::create(asset.id,asset.media_type,{std::byte{2}});
    require(altered.document_id()==source.document_id() && altered.revision()==source.revision() && altered.entities()==source.entities(),
        "asset replacement fixture must retain exact same head and entities");
    rejects([&]{(void)prepare_physical_wall_room_review(altered,report,intent);});
    auto plane=intent;plane.effective_elevation_m=1;
    rejects([&]{(void)replay_physical_wall_room_review_entities(source.entities(),encode_physical_wall_room_review_intent(plane));});
    auto wrong_context=intent;wrong_context.fresh.at(1).context.layer_id="different-layer";
    rejects([&]{(void)encode_physical_wall_room_review_intent(wrong_context);});
    auto corrupt=source.entities();
    corrupt.at(old).extensions["physical_wall_room"]["source_lineage"]["outer"]["edges"][0]["source_uses"][0]["parameter_end"]=2;
    auto corrupt_intent=intent;corrupt_intent.source_entities_digest=entity_map_digest(corrupt);
    corrupt_intent.retained.at(0).expected_descriptor_digest=physical_wall_room_descriptor_digest(corrupt.at(old));
    rejects([&]{(void)replay_physical_wall_room_review_entities(corrupt,encode_physical_wall_room_review_intent(corrupt_intent));});
    auto retired_new=intent;retired_new.retained.at(0).disposition=PhysicalWallRoomRetainedDisposition::retire;
    retired_new.fresh.at(0)={};retired_new.fresh.at(0).candidate_index=0;
    retired_new.fresh.at(0).reviewed_source_lineage=report.fresh.at(0).source_lineage;
    const auto explicit_retirement=prepare_physical_wall_room_review(source,report,retired_new);
    require(!explicit_retirement.entities.contains(old) && explicit_retirement.created_room_ids==std::vector<std::string>{"explicit-new"},
        "explicit unclassified/retire/new decisions were silently inferred or ignored");
    auto retire_and_create=Document::fork(source);lifecycle(retire_and_create,source,explicit_retirement);
    require(captured_state(document.snapshot())==before,"detached preparation/refusals altered complete captured split source");
    document.mark_saved(document.revision());
    rejects([&]{(void)prepare_physical_wall_room_review(document.snapshot(),report,intent);});
}
void merge_and_explicit_references() {
    auto original=Document::create(fixture(true));
    auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0,1},"office");
    original.apply(initial);auto values=copy(original.snapshot());
    values.erase(std::remove_if(values.begin(),values.end(),[](const auto& e){return e.id=="divider";}),values.end());
    const auto kept=initial.entity_changes.at(0).entity.id,retired=initial.entity_changes.at(1).entity.id;
    BoundaryDimension dimension;dimension.id="retired-area";dimension.boundary_id=retired;dimension.kind=BoundaryDimensionKind::area;
    dimension.text_position={3,1};values.push_back(encode_boundary_dimension_entity(dimension));
    const RoomRelation relation{kept,retired,RoomRelationKind::independent};
    const auto graph=RoomRelationshipSnapshot::create({{kept,RoomReferenceKind::room_boundary},{retired,RoomReferenceKind::room_boundary}}, {relation});
    values.push_back(entity("relationships","room_relationships",{{"model",graph.to_json()},{"unrelated","preserved"}}));
    values.back().extensions["vendor"]={{"retain",true},{"opaque",Json::array({17,"kept annotation"})}};
    auto document=Document::create(values);const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    require(report.retained.size()==2 && report.fresh.size()==1,"merge expectation must contain two old and one fresh room");
    auto intent=review(source,report);
    for (auto& d:intent.retained) if (d.room_id==kept) d.disposition=PhysicalWallRoomRetainedDisposition::retain;
    assign(intent.fresh.at(0),kept,PhysicalWallRoomFreshDisposition::retained,report);
    rejects([&]{(void)prepare_physical_wall_room_review(source,report,intent);});
    intent.removed_reference_ids={"retired-area"};
    intent.relationship_removals={{"relationships",{retired},{graph.relations().at(0)}}};
    const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    require(prepared.retained_edits.size()==1 && prepared.retired_room_ids==std::vector<std::string>{retired} && prepared.created_room_ids.empty(),
        "merge did not expose reconstructed typed lifetime transitions");
    require(!prepared.entities.contains(retired) && !prepared.entities.contains("retired-area"),"explicit retirement/reference removal absent");
    require(prepared.entities.at("relationships").properties.at("unrelated")=="preserved","graph removal lost unrelated properties");
    require(prepared.entities.at("relationships").extensions==source.entities().at("relationships").extensions,"graph retirement discarded unrelated opaque extensions");
    const auto result=RoomRelationshipSnapshot::from_json(prepared.entities.at("relationships").properties.at("model"));
    require(result.references().size()==1 && result.references().at(0).id==kept && result.relations().empty(),"graph removal did not preserve exact remaining references");
    require(std::abs(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(prepared.entities.at(kept)))))-10.64)<1e-9,
        "merged clear-room area disagrees with independent rectangular expectation");
    auto unacknowledged=intent;unacknowledged.relationship_removals.at(0).acknowledged_relations.clear();
    rejects([&]{(void)prepare_physical_wall_room_review(source,report,unacknowledged);});
    auto opaque=values;opaque.push_back(entity("unknown-dependent","future_reference",{{"owner_id",retired}}));
    auto unknown=Document::create(opaque);const auto unknown_report=physical_wall_room_correspondence(unknown.snapshot(),"bottom");
    auto unknown_intent=intent;unknown_intent.source_snapshot_digest=document_snapshot_digest(unknown.snapshot());
    unknown_intent.source_authoring_digest=document_authoring_source_digest_v2(unknown.snapshot());
    unknown_intent.source_saved_revision=unknown.saved_revision_optional();
    unknown_intent.source_entities_digest=entity_map_digest(unknown.snapshot().entities());
    rejects([&]{(void)prepare_physical_wall_room_review(unknown.snapshot(),unknown_report,unknown_intent);});
    require(captured_state(document.snapshot())==captured_state(source),"merge preparation/refusals mutated complete source");
    lifecycle(document,source,prepared);
}
void complete_review_swaps_retained_room_identities_after_wall_edit() {
    auto document=Document::create(fixture(true));std::vector<std::string> room_ids;
    for (std::size_t index=0;index<2;++index) {
        auto creation=prepare_physical_wall_rooms(document.snapshot(),"bottom",{index},index==0 ? "office" : "bedroom");
        require(creation.entity_changes.size()==1,"permutation fixture must create exactly the explicitly selected room");
        auto& room=creation.entity_changes.at(0).entity;room.properties["name"]=index==0 ? "Office identity" : "Bedroom identity";
        room.properties["chosen_metadata"]={{"priority",static_cast<int>(index)},{"reviewed",true}};
        room.extensions["vendor"]={{"retain_name",room.properties.at("name")},{"opaque",Json::array({17,1.0})}};
        room_ids.push_back(room.id);document.apply(creation);
    }
    const auto bounds0=boundary_bounds(boundary_geometry(decode_identified_boundary_entity(document.snapshot().entities().at(room_ids.at(0)))));
    const auto left=bounds0.maximum.x<2 ? room_ids.at(0) : room_ids.at(1);
    const auto right=left==room_ids.at(0) ? room_ids.at(1) : room_ids.at(0);
    auto divider=document.snapshot().entities().at("divider");divider.properties["thickness_m"]=.4;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(divider)}, {},"Change both clear-room destinations"});
    const auto detection=detect_physical_wall_spaces(document.snapshot(),"bottom");
    require(detection.spaces.size()==2,"divider edit must preserve two uniquely reviewable destinations");
    std::size_t left_index=0,right_index=0;
    for (std::size_t index=0;index<detection.spaces.size();++index) {
        const auto bounds=boundary_bounds(detection.spaces.at(index).boundary);
        if (bounds.maximum.x<2) left_index=index;else if (bounds.minimum.x>2) right_index=index;
    }
    require(left_index!=right_index,"independent bounds must distinguish edited left and right destinations");
    const auto right_bounds=boundary_bounds(detection.spaces.at(right_index).boundary);
    const Vec2 right_witness{(right_bounds.minimum.x+right_bounds.maximum.x)/2,(right_bounds.minimum.y+right_bounds.maximum.y)/2};
    LegacyBoundaryIdentityOptions refresh_ids;
    for (std::size_t index=0;index<detection.spaces.at(right_index).boundary.size();++index) {
        refresh_ids.segment_ids.push_back("before-swap-refresh-s"+std::to_string(index));
        refresh_ids.vertex_ids.push_back("before-swap-refresh-v"+std::to_string(index));
    }
    const auto refresh=prepare_physical_wall_room_repair(document.snapshot(),right,"bottom",right_witness,
        detection.spaces.at(right_index).source_lineage,physical_wall_room_descriptor_digest(document.snapshot().entities().at(right)),refresh_ids,{});
    document.apply(refresh);const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    require(report.retained.size()==2 && report.fresh.size()==2 && physical_wall_room_checks(source).at(right).current,
        "single-room exclusion regression requires another current destination owner after the wall edit");
    auto intent=review(source,report);for (auto& decision:intent.retained) decision.disposition=PhysicalWallRoomRetainedDisposition::retain;
    for (const auto& candidate:report.fresh) {
        const auto bounds=boundary_bounds(candidate.boundary);
        assign(intent.fresh.at(candidate.index),bounds.minimum.x>2 ? left : right,PhysicalWallRoomFreshDisposition::retained,report);
    }
    const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    require(prepared.retained_edits.size()==2 && prepared.created_room_ids.empty() && prepared.retired_room_ids.empty(),
        "complete swap must retain both identities without inferring new or retired owners");
    const auto swapped_left=boundary_bounds(boundary_geometry(decode_identified_boundary_entity(prepared.entities.at(left))));
    const auto swapped_right=boundary_bounds(boundary_geometry(decode_identified_boundary_entity(prepared.entities.at(right))));
    require(swapped_left.minimum.x>2 && swapped_right.maximum.x<2,"complete review did not honor explicit cross-assignment witnesses");
    for (const auto& id:room_ids) {
        const auto& before=source.entities().at(id);const auto& after=prepared.entities.at(id);
        for (const auto* key:{"name","classification","measurement_classification","chosen_metadata","factor","factor_expression"})
            require(after.properties.at(key)==before.properties.at(key),"swap transferred another room's chosen name, classification or metadata");
        require(after.extensions.at("vendor")==before.extensions.at("vendor"),"swap lost retained-owner opaque metadata");
        require(std::abs(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(after))))-4.76)<1e-9,
            "swapped room clear quantity disagrees with independent 1.7 by 2.8 rectangle");
    }
    const auto edit=std::find_if(prepared.retained_edits.begin(),prepared.retained_edits.end(),[&](const auto& value){return value.boundary_id==left;});
    require(edit!=prepared.retained_edits.end(),"complete swap must expose the left identity's independently reconstructed edit");
    const auto assigned=std::find_if(intent.fresh.begin(),intent.fresh.end(),[&](const auto& decision){return decision.room_id==left;});
    require(assigned!=intent.fresh.end(),"swap must retain explicit reviewed destination witness");
    refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_repair(source,left,"bottom",assigned->interior_witness,
        assigned->reviewed_source_lineage,physical_wall_room_descriptor_digest(source.entities().at(left)),assigned->fresh_ids,{});});
    const std::set<std::string> complete{left,right};
    require(validate_physical_wall_room_repair(source.entities(),*edit,complete).source_lineage==edit->physical_wall_room_repair->reviewed_source_lineage,
        "complete same-context scope must qualify explicitly assigned occupied destination");
    refuses_unchanged(document,[&]{(void)validate_physical_wall_room_repair(source.entities(),*edit);});
    refuses_unchanged(document,[&]{(void)edited_boundary_entities(source.entities(),*edit);});
    refuses_unchanged(document,[&]{(void)document.apply(EditBoundaryGeometry{source.revision(),*edit});});
    for (const auto& scope:std::vector<std::set<std::string>>{{},{left},{right},{left,right,"bottom"},{left,right,"missing-room"}}) {
        refuses_unchanged(document,[&]{(void)validate_physical_wall_room_repair(source.entities(),*edit,scope);});
        refuses_unchanged(document,[&]{(void)edited_boundary_entities_for_room_review(source.entities(),*edit,scope);});
    }
    auto foreign_source=source.entities();foreign_source.emplace("foreign-floor",entity("foreign-floor","floor",{{"building_id","building"}}));
    foreign_source.emplace("foreign-layer",entity("foreign-layer","layer",{{"floor_id","foreign-floor"}}));
    auto foreign=source.entities().at(left);foreign.id="foreign-room";foreign.properties["floor_id"]="foreign-floor";foreign.properties["layer_id"]="foreign-layer";
    foreign.extensions.erase("boundary_geometry_derivation");auto foreign_boundary=decode_identified_boundary_entity(foreign);
    for (std::size_t index=0;index<foreign_boundary.segments.size();++index) {
        foreign_boundary.segments[index].segment_id="foreign-s"+std::to_string(index);
        foreign_boundary.segments[index].start_vertex_id="foreign-v"+std::to_string(index);
        foreign_boundary.segments[index].end_vertex_id="foreign-v"+std::to_string((index+1)%foreign_boundary.segments.size());
    }
    foreign=encode_identified_boundary_entity(foreign_boundary,&foreign);foreign_source.emplace("foreign-room",std::move(foreign));
    const auto foreign_digest=entity_map_digest(foreign_source);const std::set<std::string> wrong_context{left,right,"foreign-room"};
    refuses_unchanged(document,[&]{(void)validate_physical_wall_room_repair(foreign_source,*edit,wrong_context);});
    refuses_unchanged(document,[&]{(void)edited_boundary_entities_for_room_review(foreign_source,*edit,wrong_context);});
    require(entity_map_digest(foreign_source)==foreign_digest,"wrong-context refusal mutated detached source map");
    auto incomplete=intent;incomplete.retained.pop_back();refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_review(source,report,incomplete);});
    incomplete=intent;incomplete.fresh.pop_back();refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_review(source,report,incomplete);});
    lifecycle(document,source,prepared);
    const auto checks=physical_wall_room_checks(document.snapshot());require(checks.at(left).current && checks.at(right).current,"applied permutation left either retained room stale");
}
void kept_area_resolves_current_net_clear_quantity() {
    auto original=Document::create(fixture());auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");
    original.apply(initial);const auto id=initial.entity_changes.at(0).entity.id;auto values=copy(original.snapshot());
    BoundaryDimension dimension;dimension.id="kept-area";dimension.boundary_id=id;dimension.kind=BoundaryDimensionKind::area;
    dimension.text_position={2,1.5};dimension.presentation=BoundaryDimensionPresentation{3.0,"#123456",true,false,true,.25};
    auto callout=encode_boundary_dimension_entity(dimension);callout.extensions["vendor"]={{"keep",true}};values.push_back(callout);
    values.push_back(encode_identified_boundary_entity(IdentifiedBoundary{"unrelated-area-owner","measurement_boundary",{
        {"u-ab","u-a","u-b",{{100,100},{102,100},0}}, {"u-bc","u-b","u-c",{{102,100},{102,102},0}},
        {"u-cd","u-c","u-d",{{102,102},{100,102},0}}, {"u-da","u-d","u-a",{{100,102},{100,100},0}}}}));
    BoundaryDimension unrelated;unrelated.id="unrelated-area-callout";unrelated.boundary_id="unrelated-area-owner";unrelated.kind=BoundaryDimensionKind::area;
    values.push_back(encode_boundary_dimension_entity(unrelated));
    values.push_back(wall("island",{1,1},{3,1}));
    auto d=Document::create(values);const auto source=d.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");auto intent=review(source,report);
    intent.retained.at(0).disposition=PhysicalWallRoomRetainedDisposition::retain;assign(intent.fresh.at(0),id,PhysicalWallRoomFreshDisposition::retained,report);
    refuses_unchanged(d,[&]{(void)prepare_physical_wall_room_review(source,report,intent);});
    intent.kept_reference_ids={dimension.id};const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    require(prepared.entities.at(dimension.id)==callout,"area Keep changed stable callout presentation or opaque metadata");
    require(decode_physical_wall_room_descriptor(prepared.entities.at(id)).holes.size()==1 &&
        std::abs(resolve_boundary_dimension(dimension,prepared.entities).area_square_metres-10.24)<1e-9,
        "kept area must measure current net clear area with wall-island hole");
    const auto preview=Document::preview_command(source,command_for(source,prepared));
    require(std::abs(dimension.resolve(preview).area_square_metres-10.24)<1e-9 && !prepared.entities.at(id).properties.contains("area_m2"),
        "snapshot area Keep used stale stored or gross outer quantity");
    const std::vector<std::function<void(PhysicalWallRoomReviewIntent&)>> bad_decisions{
        [](auto& v){v.kept_reference_ids.clear();},[](auto& v){v.kept_reference_ids.push_back("missing-reference");},
        [](auto& v){v.kept_reference_ids.push_back("bottom");},[](auto& v){v.kept_reference_ids.push_back("kept-area");},
        [](auto& v){v.kept_reference_ids.push_back("unrelated-area-callout");},
        [](auto& v){v.removed_reference_ids={"kept-area"};},
        [](auto& v){v.retained.at(0).replacement_dimension_ids={"unused-generated-dimension"};},
    };
    for (const auto& mutate:bad_decisions) {auto invalid=intent;mutate(invalid);refuses_unchanged(d,[&]{(void)prepare_physical_wall_room_review(source,report,invalid);});}
    auto remove=intent;remove.kept_reference_ids.clear();remove.removed_reference_ids={dimension.id};
    require(!prepare_physical_wall_room_review(source,report,remove).entities.contains(dimension.id),"explicit area Remove lost removal authority");
    lifecycle(d,source,prepared);
}
void endpoint_constraint_requires_a_decision() {
    auto original=Document::create(fixture());auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");
    original.apply(initial);const auto id=initial.entity_changes.at(0).entity.id;auto values=copy(original.snapshot());
    const auto old=decode_identified_boundary_entity(original.snapshot().entities().at(id)).segments.at(0);
    PersistentConstraint constraint;constraint.id="review-anchor";constraint.relation=ConstraintRelationKind::fixed_anchor;
    constraint.bindings={{id,WallEndpointRole::start,old.segment_id,old.start_vertex_id}};constraint.anchor=old.segment.start;
    values.push_back(encode_constraint_entity(constraint));values.push_back(wall("divider",{2,0},{2,3}));
    auto document=Document::create(values);const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    auto intent=review(source,report);intent.retained.at(0).disposition=PhysicalWallRoomRetainedDisposition::retain;
    assign(intent.fresh.at(0),id,PhysicalWallRoomFreshDisposition::retained,report);
    rejects([&]{(void)prepare_physical_wall_room_review(source,report,intent);});
    intent.removed_reference_ids={constraint.id};const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    require(!prepared.entities.contains(constraint.id),"explicit endpoint-constraint removal missing");
    require(prepared.entities.at(id).properties.at("classification")=="office","constraint decision changed retained classification");
    auto mapped=review(source,report);mapped.retained.at(0).disposition=PhysicalWallRoomRetainedDisposition::retain;
    mapped.kept_reference_ids={constraint.id};
    bool found=false;
    for (const auto& candidate:report.fresh) for (std::size_t edge_index=0;edge_index<candidate.boundary.size() && !found;++edge_index) {
        const auto point=candidate.boundary[edge_index].start;
        if (std::hypot(point.x-old.segment.start.x,point.y-old.segment.start.y)>1e-9) continue;
        auto& destination=mapped.fresh.at(candidate.index);assign(destination,id,PhysicalWallRoomFreshDisposition::retained,report);
        mapped.retained.at(0).child_mapping={{"segments",{{old.segment_id,destination.fresh_ids.segment_ids.at(edge_index)}}},
            {"vertices",{{old.start_vertex_id,destination.fresh_ids.vertex_ids.at(edge_index)}}}};
        found=true;
    }
    require(found,"independent old anchor corner must survive in one split component");
    const auto remapped=prepare_physical_wall_room_review(source,report,mapped);
    const auto binding=decode_constraint_entity(remapped.entities.at(constraint.id)).constraint->bindings.at(0);
    require(binding.owner_id==id && binding.segment_id!=old.segment_id && binding.vertex_id!=old.start_vertex_id &&
        binding.role==WallEndpointRole::start,"explicit endpoint mapping did not retain owner/role and replace child identities");
}
void kept_manual_length_angle_and_chain_map_exact_targets() {
    auto original=Document::create(fixture());auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");original.apply(initial);
    const auto id=initial.entity_changes.at(0).entity.id;const auto old=decode_identified_boundary_entity(original.snapshot().entities().at(id));
    require(old.segments.size()==4,"manual mapping fixture requires four rectangle edges");auto values=copy(original.snapshot());
    BoundaryDimension length;length.id="manual-room-length";length.boundary_id=id;length.segment_id=old.segments.at(0).segment_id;
    length.text_position={1,1};length.presentation=BoundaryDimensionPresentation{3.5,"#112233",true,true,false,.2};
    auto angle=length;angle.id="manual-room-angle";angle.kind=BoundaryDimensionKind::angle;
    angle.secondary_segment_id=old.segments.at(1).segment_id;angle.vertex_id=old.segments.at(0).end_vertex_id;
    auto chain=length;chain.id="manual-room-chain";chain.segment_chain_ids={old.segments.at(0).segment_id,old.segments.at(1).segment_id};
    for (const auto& dimension:{length,angle,chain}) {
        auto value=encode_boundary_dimension_entity(dimension);value.extensions["vendor"]={{"decimal",1.0},{"annotation","retained"}};
        values.push_back(std::move(value));
    }
    auto document=Document::create(values);const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    auto intent=review(source,report);intent.retained.at(0).disposition=PhysicalWallRoomRetainedDisposition::retain;
    assign(intent.fresh.at(0),id,PhysicalWallRoomFreshDisposition::retained,report);const auto& destination=intent.fresh.at(0);
    intent.kept_reference_ids={length.id,angle.id,chain.id};
    intent.retained.at(0).child_mapping={{"segments",{{old.segments.at(0).segment_id,destination.fresh_ids.segment_ids.at(0)},
        {old.segments.at(1).segment_id,destination.fresh_ids.segment_ids.at(1)}}},
        {"vertices",{{old.segments.at(0).end_vertex_id,destination.fresh_ids.vertex_ids.at(1)}}}};
    const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    for (const auto& dimension:{length,angle,chain}) {
        auto expected=source.entities().at(dimension.id);auto& target=expected.properties.at("target");
        if (dimension.segment_chain_ids.empty()) target["segment_id"]=destination.fresh_ids.segment_ids.at(0);
        else target["segment_ids"]=Json::array({destination.fresh_ids.segment_ids.at(0),destination.fresh_ids.segment_ids.at(1)});
        if (dimension.kind==BoundaryDimensionKind::angle) {target["second_segment_id"]=destination.fresh_ids.segment_ids.at(1);target["vertex_id"]=destination.fresh_ids.vertex_ids.at(1);}
        require(prepared.entities.at(dimension.id)==expected && prepared.entities.at(dimension.id).properties.dump()==expected.properties.dump(),
            "explicit manual mapping changed identity, presentation, numeric representation or opaque metadata");
    }
    const auto first=old.segments.at(0).segment;
    require(std::abs(decode_boundary_dimension_entity(prepared.entities.at(length.id)).dimension->resolve(prepared.entities).segment_length()-
        std::hypot(first.end.x-first.start.x,first.end.y-first.start.y))<1e-9 &&
        std::abs(decode_boundary_dimension_entity(prepared.entities.at(angle.id)).dimension->resolve(prepared.entities).angle()-std::numbers::pi/2)<1e-9 &&
        std::abs(decode_boundary_dimension_entity(prepared.entities.at(chain.id)).dimension->resolve(prepared.entities).segment_length()-6.6)<1e-9,
        "mapped manual length/angle/chain did not resolve independent clear-rectangle quantities");
    auto omitted=intent;omitted.kept_reference_ids.pop_back();refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_review(source,report,omitted);});
    auto unmapped=intent;unmapped.retained.at(0).child_mapping["segments"].erase(old.segments.at(1).segment_id);
    refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_review(source,report,unmapped);});
    lifecycle(document,source,prepared);
}
void kept_automatic_lengths_regenerate_fresh_style_and_history() {
    auto original=Document::create(fixture());auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");original.apply(initial);
    const auto id=initial.entity_changes.at(0).entity.id;const auto old=decode_identified_boundary_entity(original.snapshot().entities().at(id));
    auto values=copy(original.snapshot());std::vector<std::string> automatic_ids;
    const BoundaryDimensionPresentation style{4.0,"#234567",true,true,false,.35};
    for (std::size_t i=0;i<old.segments.size();++i) {
        BoundaryDimension dimension;dimension.id="old-automatic-"+std::to_string(i);dimension.boundary_id=id;dimension.segment_id=old.segments.at(i).segment_id;
        dimension.text_position={2+.1*static_cast<double>(i),1+.2*static_cast<double>(i)};
        dimension.placement=BoundaryDimensionPlacement::automatic;dimension.automatic_placement_version=2;dimension.presentation=style;
        auto value=encode_boundary_dimension_entity(dimension);value.properties["layer_id"]="layer";
        value.properties["vendor_label"]={{"decimal",1.0},{"annotation","retained on regeneration"}};
        value.extensions["original-provenance"]={{"retained_in_history",true}};
        automatic_ids.push_back(dimension.id);values.push_back(std::move(value));
    }
    auto previous=values.back();previous.id="previously-retired-automatic";values.push_back(previous);
    const auto asset=Asset::create("template-audit-asset","application/octet-stream",{std::byte{43}});
    auto document=Document::create(values,{asset});document.apply(ApplyEntityChanges{document.revision(),{EntityChange::erase(previous.id)}, {},"Retire previous automatic callout"});
    const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");auto intent=review(source,report);
    intent.retained.at(0).disposition=PhysicalWallRoomRetainedDisposition::retain;assign(intent.fresh.at(0),id,PhysicalWallRoomFreshDisposition::retained,report);
    intent.kept_reference_ids=automatic_ids;
    for (std::size_t i=0;i<report.fresh.at(0).boundary.size();++i) intent.retained.at(0).replacement_dimension_ids.push_back("regenerated-automatic-"+std::to_string(i));
    const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    for (int differing=0;differing<3;++differing) {
        auto variant=Document::fork(source);auto modified=variant.snapshot().entities().at(automatic_ids.at(1));
        if (differing==0) modified.properties["presentation"]["color"]="#abcdef";
        if (differing==1) modified.properties["vendor_label"]["annotation"]="different per-edge metadata";
        if (differing==2) modified.extensions["different_template"]={{"preserve",true}};
        variant.apply(ApplyEntityChanges{variant.revision(),{EntityChange::upsert(modified)}, {},"Keep heterogeneous automatic template"});
        const auto changed=variant.snapshot();const auto changed_report=physical_wall_room_correspondence(changed,"bottom");
        auto changed_intent=retain_current_rooms(changed,changed_report);changed_intent.kept_reference_ids=automatic_ids;
        changed_intent.retained.at(0).replacement_dimension_ids=intent.retained.at(0).replacement_dimension_ids;
        refuses_unchanged(variant,[&]{(void)prepare_physical_wall_room_review(changed,changed_report,changed_intent);});
    }
    for (bool key_only:{false,true}) {
        auto variant=Document::fork(source);auto incoming=entity("incoming-old-automatic","future_reference");
        incoming.properties["nested"]=key_only ? Json{{automatic_ids.at(0),true}} : Json{{"opaque_value",automatic_ids.at(0)}};
        variant.apply(ApplyEntityChanges{variant.revision(),{EntityChange::upsert(incoming)}, {},"Retain incoming automatic-label reference"});
        const auto changed=variant.snapshot();const auto changed_report=physical_wall_room_correspondence(changed,"bottom");
        auto changed_intent=retain_current_rooms(changed,changed_report);changed_intent.kept_reference_ids=automatic_ids;
        changed_intent.retained.at(0).replacement_dimension_ids=intent.retained.at(0).replacement_dimension_ids;
        refuses_unchanged(variant,[&]{(void)prepare_physical_wall_room_review(changed,changed_report,changed_intent);});
    }
    require(prepared.retained_edits.at(0).replacement_dimension_ids==intent.retained.at(0).replacement_dimension_ids,"typed retained edit lost explicit automatic regeneration IDs");
    for (const auto& old_id:automatic_ids) require(!prepared.entities.contains(old_id),"kept automatic topology policy retained an obsolete edge label identity");
    for (std::size_t i=0;i<intent.retained.at(0).replacement_dimension_ids.size();++i) {
        const auto& value=prepared.entities.at(intent.retained.at(0).replacement_dimension_ids.at(i));const auto dimension=*decode_boundary_dimension_entity(value).dimension;
        require(dimension.boundary_id==id && dimension.segment_id==intent.fresh.at(0).fresh_ids.segment_ids.at(i) &&
            dimension.placement==BoundaryDimensionPlacement::automatic && dimension.automatic_placement_version==2 && dimension.presentation==style &&
            value.properties.at("layer_id")=="layer" && std::isfinite(dimension.resolve(prepared.entities).segment_length()),
            "automatic regeneration lost fresh analytical target, placement provenance, style or drawing context");
        require(value.properties.contains("vendor_label") && value.properties.at("vendor_label")==source.entities().at(automatic_ids.at(0)).properties.at("vendor_label") &&
            value.extensions==source.entities().at(automatic_ids.at(0)).extensions,
            "automatic regeneration discarded original template opaque properties or extensions");
    }
    for (int error=0;error<5;++error) {
        auto invalid=intent;
        if (error==0) invalid.retained.at(0).replacement_dimension_ids.clear();
        if (error==1) invalid.retained.at(0).replacement_dimension_ids.push_back("extra-automatic");
        if (error==2) invalid.retained.at(0).replacement_dimension_ids.at(1)=invalid.retained.at(0).replacement_dimension_ids.at(0);
        if (error==3) invalid.retained.at(0).replacement_dimension_ids.at(0)=automatic_ids.at(0);
        if (error==4) invalid.retained.at(0).replacement_dimension_ids.at(0)=previous.id;
        refuses_unchanged(document,[&]{const auto candidate=prepare_physical_wall_room_review(source,report,invalid);(void)document.apply(command_for(source,candidate));});
    }
    auto omitted=intent;omitted.kept_reference_ids.pop_back();refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_review(source,report,omitted);});
    lifecycle(document,source,prepared);
    require(document.snapshot().history().at(static_cast<std::size_t>(source.revision())).entities.at(automatic_ids.at(0))==source.entities().at(automatic_ids.at(0)),
        "automatic regeneration rewrote original style/provenance retained in history");
}
void explicit_new_and_retire_only_batches() {
    auto fresh=Document::create(fixture());const auto source=fresh.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    auto intent=review(source,report);assign(intent.fresh.at(0),"explicit-only-new",PhysicalWallRoomFreshDisposition::create,report);
    const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    require(prepared.created_room_ids==std::vector<std::string>{"explicit-only-new"} && prepared.retired_room_ids.empty(),"new-only batch inferred another lifetime transition");
    lifecycle(fresh,source,prepared);
    const auto retained=fresh.snapshot();const auto retained_report=physical_wall_room_correspondence(retained,"bottom");
    const auto retirement=prepare_physical_wall_room_review(retained,retained_report,review(retained,retained_report));
    require(retirement.retired_room_ids==std::vector<std::string>{"explicit-only-new"} && retirement.created_room_ids.empty() &&
        !retirement.entities.contains("explicit-only-new"),"retire-only batch classified an unassigned space");
    lifecycle(fresh,retained,retirement);
}
void retired_history_identities_cannot_be_reused() {
    auto document=Document::create(fixture());auto initial=prepare_physical_wall_rooms(document.snapshot(),"bottom",{0},"office");
    const auto old=initial.entity_changes.at(0).entity.id;document.apply(initial);
    const auto boundary=decode_identified_boundary_entity(document.snapshot().entities().at(old));
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::erase(old)}, {},"Retire old room"});
    const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    for (int reused=0;reused<3;++reused) {
        auto intent=review(source,report);assign(intent.fresh.at(0),reused==0 ? old : "new-after-retirement",PhysicalWallRoomFreshDisposition::create,report);
        if (reused==1) intent.fresh.at(0).fresh_ids.segment_ids.at(0)=boundary.segments.at(0).segment_id;
        if (reused==2) intent.fresh.at(0).fresh_ids.vertex_ids.at(0)=boundary.segments.at(0).start_vertex_id;
        refuses_unchanged(document,[&]{const auto prepared=prepare_physical_wall_room_review(source,report,intent);(void)document.apply(command_for(source,prepared));});
    }
}
void opaque_key_only_room_and_child_references_refuse_without_mutation() {
    auto original=Document::create(fixture());auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");original.apply(initial);
    const auto old=initial.entity_changes.at(0).entity.id;const auto boundary=decode_identified_boundary_entity(original.snapshot().entities().at(old));
    for (const auto& token:{old,boundary.segments.at(0).segment_id,boundary.segments.at(0).start_vertex_id}) {
        for (bool extension:{false,true}) {
            auto values=copy(original.snapshot());auto unknown=entity("opaque-dependent","future_reference");
            auto& data=extension ? unknown.extensions : unknown.properties;
            data["nested"]=Json::array({Json{{token,true},{"unrelated",17}}});values.push_back(std::move(unknown));
            auto document=Document::create(values);const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
            auto intent=review(source,report);
            refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_review(source,report,intent);});
        }
    }
}
void retained_owner_opaque_child_references_cannot_silently_detach() {
    const auto asset=Asset::create("retained-reference-asset","application/octet-stream",{std::byte{19},std::byte{23}});
    auto document=Document::create(fixture(true),{asset});auto initial=prepare_physical_wall_rooms(document.snapshot(),"bottom",{0,1},"office");
    require(initial.entity_changes.size()==2,"retained metadata regression needs two current owners");document.apply(initial);
    const std::vector<std::string> owners{initial.entity_changes.at(0).entity.id,initial.entity_changes.at(1).entity.id};
    const auto spaces=detect_physical_wall_spaces(document.snapshot(),"bottom");const auto bounds=boundary_bounds(spaces.spaces.at(0).boundary);
    LegacyBoundaryIdentityOptions ids;
    for (std::size_t index=0;index<spaces.spaces.at(0).boundary.size();++index) {
        ids.segment_ids.push_back("metadata-history-s"+std::to_string(index));ids.vertex_ids.push_back("metadata-history-v"+std::to_string(index));
    }
    document.apply(prepare_physical_wall_room_repair(document.snapshot(),owners.at(0),"bottom",
        {(bounds.minimum.x+bounds.maximum.x)/2,(bounds.minimum.y+bounds.maximum.y)/2},spaces.spaces.at(0).source_lineage,
        physical_wall_room_descriptor_digest(document.snapshot().entities().at(owners.at(0))),ids,{}));
    const auto source=document.snapshot();const auto original_state=captured_state(source);
    const auto report=physical_wall_room_correspondence(source,"bottom");const auto intent=retain_current_rooms(source,report);
    const auto control=prepare_physical_wall_room_review(source,report,intent);
    const auto& original_receipt=source.entities().at(owners.at(0)).extensions.at("boundary_geometry_derivation");
    const auto& next_receipt=control.entities.at(owners.at(0)).extensions.at("boundary_geometry_derivation");
    auto preserved=next_receipt;preserved["operations"]=original_receipt.at("operations");
    require(preserved==original_receipt && next_receipt.at("operations").size()==original_receipt.at("operations").size()+1,
        "recognized historical geometry receipt was stripped or rewritten during retained review");
    for (std::size_t index=0;index<original_receipt.at("operations").size();++index)
        require(next_receipt.at("operations").at(index)==original_receipt.at("operations").at(index),"retained review changed an archived derivation operation");
    for (bool cross_owner:{false,true}) {
        const auto target=decode_identified_boundary_entity(source.entities().at(owners.at(cross_owner ? 1 : 0)));
        for (const auto& token:{target.segments.at(0).segment_id,target.segments.at(0).start_vertex_id}) {
            for (bool extension:{false,true}) for (bool key_only:{false,true}) {
                auto variant=Document::fork(source);auto owner=variant.snapshot().entities().at(owners.at(0));
                auto& metadata=extension ? owner.extensions : owner.properties;
                metadata["opaque_child_reference"]=Json::array({key_only ? Json{{token,true}} : Json{{"opaque_value",token}}});
                variant.apply(ApplyEntityChanges{variant.revision(),{EntityChange::upsert(owner)}, {},"Retain opaque old-child reference"});
                const auto changed=variant.snapshot();const auto changed_report=physical_wall_room_correspondence(changed,"bottom");
                const auto changed_intent=retain_current_rooms(changed,changed_report);
                refuses_unchanged(variant,[&]{(void)prepare_physical_wall_room_review(changed,changed_report,changed_intent);});
                require(captured_state(document.snapshot())==original_state,"retained metadata refusal altered original history/assets");
            }
        }
    }
    lifecycle(document,source,control);
}
void incoming_removed_reference_identities_cannot_silently_detach() {
    auto original=Document::create(fixture());auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");original.apply(initial);
    const auto owner=initial.entity_changes.at(0).entity.id;const auto topology=decode_identified_boundary_entity(original.snapshot().entities().at(owner));
    BoundaryDimension dimension;dimension.id="explicit-removed-area";dimension.boundary_id=owner;dimension.kind=BoundaryDimensionKind::area;dimension.text_position={2,1.5};
    PersistentConstraint constraint;constraint.id="explicit-removed-anchor";constraint.relation=ConstraintRelationKind::fixed_anchor;
    constraint.bindings={{owner,WallEndpointRole::start,topology.segments.at(0).segment_id,topology.segments.at(0).start_vertex_id}};
    constraint.anchor=topology.segments.at(0).segment.start;
    auto values=copy(original.snapshot());values.push_back(encode_boundary_dimension_entity(dimension));values.push_back(encode_constraint_entity(constraint));
    const auto asset=Asset::create("removed-reference-asset","application/octet-stream",{std::byte{71}});
    auto document=Document::create(values,{asset});const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    auto intent=retain_current_rooms(source,report);intent.removed_reference_ids={dimension.id,constraint.id};
    const auto control=prepare_physical_wall_room_review(source,report,intent);
    require(!control.entities.contains(dimension.id) && !control.entities.contains(constraint.id),"control must explicitly retire both supported reference identities");
    for (const auto& token:{dimension.id,constraint.id}) for (bool key_only:{false,true}) {
        auto variant=Document::fork(source);auto incoming=entity("incoming-removed-reference","future_reference");
        incoming.properties["nested"]=Json::array({key_only ? Json{{token,true}} : Json{{"opaque_value",token}}});
        variant.apply(ApplyEntityChanges{variant.revision(),{EntityChange::upsert(incoming)}, {},"Retain incoming reference to explicit removal"});
        const auto changed=variant.snapshot();const auto changed_report=physical_wall_room_correspondence(changed,"bottom");
        auto changed_intent=retain_current_rooms(changed,changed_report);changed_intent.removed_reference_ids={dimension.id,constraint.id};
        refuses_unchanged(variant,[&]{(void)prepare_physical_wall_room_review(changed,changed_report,changed_intent);});
    }
    lifecycle(document,source,control);
}
void retained_geometry_proof_tampering_is_bound_by_v2() {
    auto values=fixture();values.push_back(encode_identified_boundary_entity(IdentifiedBoundary{"proof-area","measurement_boundary",{
        {"proof-ab","proof-a","proof-b",{{100,100},{102,100},0}}, {"proof-bc","proof-b","proof-c",{{102,100},{102,102},0}},
        {"proof-cd","proof-c","proof-d",{{102,102},{100,102},0}}, {"proof-da","proof-d","proof-a",{{100,102},{100,100},0}}}}));
    auto document=Document::create(values);
    document.apply(EditBoundaryGeometry{document.revision(),{"proof-area",BoundaryGeometryEditKind::move_vertex,"proof-a",{99,100}}});
    const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    auto intent=review(source,report);assign(intent.fresh.at(0),"proof-reviewed-room",PhysicalWallRoomFreshDisposition::create,report);
    const auto prepared=prepare_physical_wall_room_review(source,report,intent);
    auto changed_source=source;const_cast<std::vector<RevisionRecord>&>(changed_source.history()).at(1).boundary_geometry_edit->target_position={98,100};
    require(changed_source.entities()==source.entities() && document_authoring_source_digest_v1(changed_source)==document_authoring_source_digest_v1(source) &&
        document_authoring_source_digest_v2(changed_source)!=document_authoring_source_digest_v2(source),"geometry proof fixture did not isolate the frozen-v1 omission");
    refuses_unchanged(document,[&]{(void)prepare_physical_wall_room_review(changed_source,report,intent);});
    document.apply(command_for(source,prepared));const auto accepted=document.snapshot();auto forged=accepted;
    const_cast<std::vector<RevisionRecord>&>(forged.history()).at(1).boundary_geometry_edit->target_position={98,100};
    require(forged.entities()==accepted.entities() && forged.revision()==accepted.revision(),"retained proof mutation changed visible head");
    const auto frozen=captured_state(forged);refuses_unchanged(document,[&]{(void)Document::fork(forged);});
    require(captured_state(forged)==frozen,"imported geometry-proof refusal mutated captured history");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { split_and_cancel();merge_and_explicit_references();complete_review_swaps_retained_room_identities_after_wall_edit();
        kept_area_resolves_current_net_clear_quantity();endpoint_constraint_requires_a_decision();
        kept_manual_length_angle_and_chain_map_exact_targets();kept_automatic_lengths_regenerate_fresh_style_and_history();
        explicit_new_and_retire_only_batches();retired_history_identities_cannot_be_reused();
        retained_owner_opaque_child_references_cannot_silently_detach();incoming_removed_reference_identities_cannot_silently_detach();
        opaque_key_only_room_and_child_references_refuse_without_mutation();retained_geometry_proof_tampering_is_bound_by_v2(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
    std::cout<<"physical wall room review tests passed\n";
}
