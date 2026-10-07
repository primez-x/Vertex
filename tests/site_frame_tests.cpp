#include "sketch/site_frame.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/constraint_entity.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void expect_near(double actual, double expected) {
    require(std::abs(actual - expected) < 1e-9, "independent coordinate expectation differs");
}
void point(Vec3 actual, Vec3 expected) { expect_near(actual.x, expected.x); expect_near(actual.y, expected.y); expect_near(actual.z, expected.z); }
template<class F> void refuses(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid contract must refuse rather than become identity");
}
Entity entity(std::string id, std::string type, json p = json::object()) {
    return {std::move(id), std::move(type), std::move(p), false, json::object()};
}
json frame() {
    return {{"version",1}, {"origin_m",{100,200,10}}, {"rotation_radians",std::numbers::pi/2},
        {"vertical_datum",{{"identifier","survey-A"},{"height_at_origin_m",150}}}};
}
SiteFrameEntities fixture() {
    SiteFrameEntities result;
    const auto add = [&](Entity e) { result.emplace(e.id, std::move(e)); };
    add(entity("site","property",{{"site_frame",frame()}}));
    for (const auto* b : {"a","b"}) {
        add(entity(b,"building",{{"property_id","site"},{"site_placement",{
            {"version",1},{"translation_m",{b[0]=='a'?3:30,4,2}}, {"rotation_radians",0}}}}));
        add(entity(std::string(b)+"-floor","floor",{{"building_id",b}}));
        add(entity(std::string(b)+"-layer","layer",{{"floor_id",std::string(b)+"-floor"}}));
        add(entity(std::string(b)+"-wall","wall",{{"layer_id",std::string(b)+"-layer"},
            {"elevation_m",2.5},{"length_m",3},{"area_m2",7.5}}));
    }
    return result;
}
void composition_and_snapshot_purity() {
    auto entities = fixture(); const auto before = entities;
    validate_document_site_frames(entities);
    const auto a = resolve_site_presentation(entities,"a-wall");
    const auto b = resolve_site_presentation(entities,"b-wall");
    require(a.source_frame == SiteFrameIdentity{SiteFrameMode::building,"site","a"}, "typed source building identity");
    point(site_transform_point({1,2,3},a.forward),{94,204,15});
    point(site_transform_point({1,2,3},b.forward),{94,231,15});
    point(site_transform_point({94,204,15},a.inverse),{1,2,3});
    point(site_transform_delta({1,2,3},a.forward),{-2,1,3});
    require(a.dependency_digest.size()==64 && a.dependency_digest!=b.dependency_digest, "captured owner-specific digest");
    require(entities==before && entities.at("a-wall").properties.at("area_m2")==7.5,
        "presentation leaves authored coordinates and measurement facts unchanged");
    entities.at("a-floor").extensions["source_witness"]="changed";
    require(resolve_site_presentation(entities,"a-wall").dependency_digest!=a.dependency_digest,
        "source container content participates in dependency guard");
    entities=before; entities.emplace("unrelated",entity("unrelated","opaque",{{"data",5}}));
    require(resolve_site_presentation(entities,"a-wall").dependency_digest==a.dependency_digest,
        "unrelated source is outside placement dependency scope");
}
void snapshot_capture_matches_map_without_mutation() {
    const auto document=Document::create({entity("site","property",{{"site_frame",frame()}})});
    const auto source=document.snapshot();
    const auto placed=resolve_site_presentation(source,"site");
    require(placed.snapshot_document_id==source.document_id() && placed.snapshot_revision==source.revision(),
        "snapshot placement retains captured source identity and revision");
    const std::vector<std::string> ids{"site"};
    const auto batch=resolve_site_presentations(source,ids);
    require(batch.at("site").dependency_digest==placed.dependency_digest &&
        batch.at("site").snapshot_document_id==placed.snapshot_document_id,
        "snapshot singular/batch captures agree");
    point(site_transform_point({1,2,3},placed.forward),{98,201,13});
    require(document.snapshot().entities()==source.entities(),"snapshot resolution preserves authored maps");
}
void world_gesture_conjugation_and_curves() {
    const auto composed=compose_site_transforms({{10,20,3},std::numbers::pi/2},
        {{3,4,2},std::numbers::pi/2});
    point(site_transform_point({1,2,3},composed),{5,21,8});
    const SiteRigidTransform f{{10,20,3},std::numbers::pi/2};
    const SiteEditTransform world{{2,-1,5},0.125,1.25};
    const auto local=conjugate_site_edit(world,f);
    const Vec3 p{2,4,6};
    const auto w=site_transform_point(p,f);
    // Independent F^-1 * (scale * R_world * F(p) + translation) expectation.
    const auto c=std::cos(0.125),s=std::sin(0.125);
    const Vec3 moved{1.25*(c*w.x-s*w.y)+2,1.25*(s*w.x+c*w.y)-1,1.25*w.z+5};
    const Vec3 expected{moved.y-20,10-moved.x,moved.z-3};
    point(site_edit_point(p,local),expected);
    expect_near(local.rotation_radians,0.125); expect_near(local.scale,1.25);
    const Boundary arc{{{0,0},{2,0},0.5}};
    const auto placed=site_transform_boundary(arc,f);
    expect_near(placed.at(0).start.x,10); expect_near(placed.at(0).end.y,22);
    expect_near(placed.at(0).sweep_radians,0.5);
    const auto restored=site_transform_boundary(placed,inverse_site_transform(f));
    expect_near(restored.at(0).end.x,2); expect_near(restored.at(0).end.y,0);
    refuses([&]{ (void)conjugate_site_edit({{},0,0},f); });
    refuses([&]{ (void)site_transform_point({std::numeric_limits<double>::infinity(),0,0},f); });
}
void batch_matches_singular_and_refuses_invalid_sources() {
    auto entities=fixture();
    const auto before=entities;
    const std::vector<std::string> ids{"b-wall","a-wall"};
    const auto batch=resolve_site_presentations(entities,ids);
    require(batch.size()==2,"batch returns exactly requested owners");
    for (const auto& id:ids) {
        const auto single=resolve_site_presentation(entities,id);
        const auto& combined=batch.at(id);
        require(single.source_frame==combined.source_frame &&
            single.dependency_ids==combined.dependency_ids &&
            single.dependency_digest==combined.dependency_digest &&
            single.drawing_context==combined.drawing_context,"batch and singular captures are identical");
        point(site_transform_point({1,2,3},combined.forward),id=="a-wall" ? Vec3{94,204,15} : Vec3{94,231,15});
    }
    require(entities==before,"batch never mutates authored source");
    require(resolve_vertical_placement(entities,entities.at("a-wall"))==entities.at("a-wall"),
        "legacy vertical resolver remains independent of presentation pose");
    const std::vector<std::string> duplicate{"a-wall","a-wall"};
    refuses([&]{ (void)resolve_site_presentations(entities,duplicate); });
    const std::vector<std::string> missing{"a-wall","missing"};
    refuses([&]{ (void)resolve_site_presentations(entities,missing); });
    entities.at("b").properties["site_placement"]["rotation_radians"]=nullptr;
    refuses([&]{ (void)resolve_site_presentations(entities,ids); });
    SiteFrameLimits limits; limits.maximum_dependencies=2;
    refuses([&]{ (void)resolve_site_presentations(before,ids,limits); });
    limits={}; limits.maximum_cached_dependency_entries=9;
    refuses([&]{ (void)resolve_site_presentations(before,ids,limits); });
    require(entities.at("a-wall")==before.at("a-wall"),"failed batches expose no partial source mutation");
    limits.maximum_cached_dependency_entries=10;
    require(resolve_site_presentations(before,ids,limits).size()==2,"exact aggregate cache budget is admitted");
}
void terrain_datum_is_explicit_and_never_scales_grade() {
    auto entities=fixture();
    entities.emplace("terrain",entity("terrain","terrain_surface",{{"property_id","site"},
        {"terrain_elevation_binding",{{"version",1},{"mode","relative_site_origin"}}}}));
    auto placed=resolve_site_presentation(entities,"terrain");
    point(site_transform_point({1,2,4},placed.forward),{98,201,14});
    entities.at("terrain").properties["terrain_elevation_binding"]={
        {"version",1},{"mode","declared_absolute"},{"datum_identifier","survey-A"}};
    placed=resolve_site_presentation(entities,"terrain");
    point(site_transform_point({1,2,154},placed.forward),{98,201,14});
    entities.at("terrain").properties["terrain_elevation_binding"]["datum_identifier"]="survey-B";
    refuses([&]{ (void)resolve_site_presentation(entities,"terrain"); });
    entities.at("terrain").properties["terrain_elevation_binding"]["datum_identifier"]="survey-A";
    entities.at("site").properties.erase("site_frame");
    refuses([&]{ (void)resolve_site_presentation(entities,"terrain"); });
    entities.at("terrain").properties.erase("terrain_elevation_binding");
    point(site_transform_point({1,2,154},resolve_site_presentation(entities,"terrain").forward),{1,2,154});
}
void hosted_clusters_and_join_frames() {
    auto entities=fixture();
    entities.emplace("door",entity("door","opening",{{"wall_id","a-wall"}}));
    entities.at("door").properties["property_id"]="site";
    entities.emplace("stair",entity("stair","stair",{{"layer_id","a-layer"}}));
    entities.emplace("rail",entity("rail","railing",{{"host",{{"stair_id","stair"}}}}));
    entities.emplace("wall2",entity("wall2","wall",{{"layer_id","a-layer"}}));
    entities.emplace("join",entity("join","wall_join",{{"wall_ids",{"a-wall","wall2"}}}));
    for (const auto* id : {"door","rail","join"})
        point(site_transform_point({1,2,3},resolve_site_presentation(entities,id).forward),{94,204,15});
    require(resolve_site_presentation(entities,"door").drawing_context.layer_id=="a-layer",
        "redundant host property reference retains inherited drawing layer");
    entities.at("join").properties["wall_ids"]={"a-wall","b-wall"};
    refuses([&]{ validate_document_site_frames(entities); });
    entities.erase("join");
    entities.emplace("r1",entity("r1","roof",{{"layer_id","a-layer"}}));
    entities.emplace("r2",entity("r2","roof",{{"layer_id","b-layer"}}));
    entities.emplace("roof-join",entity("roof-join","roof_join",{{"roof_ids",{"r1","r2"}}}));
    refuses([&]{ validate_document_site_frames(entities); });
    entities.erase("roof-join");
    entities.at("rail").properties["layer_id"]="b-layer";
    refuses([&]{ (void)resolve_site_presentation(entities,"rail"); });
    entities.at("door").properties["presentation_frame"]={{"version",1},{"mode","world"}};
    refuses([&]{ (void)resolve_site_presentation(entities,"door"); });
}
void independent_and_annotation_contexts() {
    auto entities=fixture();
    entities.emplace("assembly",entity("assembly","assembly_instance",{{"layer_id","a-layer"},
        {"instance",{{"root_transform",{{"translation_m",{10,20,3}}}}}}}));
    entities.emplace("note",entity("note","annotation",{{"layer_id","b-layer"},{"wall_id","a-wall"}}));
    // Annotation references are witnesses, not organizational/placement hosts.
    for (const auto* id : {"assembly","note"})
        point(site_transform_point({10,20,3},resolve_site_presentation(entities,id).forward),{10,20,3});
    entities.at("assembly").properties["presentation_frame"]={{"version",1},{"mode","building"}};
    point(site_transform_point({1,2,3},resolve_site_presentation(entities,"assembly").forward),{94,204,15});
    entities.at("note").properties["presentation_frame"]={{"version",1},{"mode","building"}};
    point(site_transform_point({1,2,3},resolve_site_presentation(entities,"note").forward),{94,231,15});
    entities.at("assembly").properties["presentation_frame"]["mode"]="site";
    point(site_transform_point({1,2,3},resolve_site_presentation(entities,"assembly").forward),{98,201,13});
    auto legacy=fixture(); legacy.at("a").properties.erase("site_placement");
    point(site_transform_point({1,2,3},resolve_site_presentation(legacy,"a-wall").forward),{1,2,3});
}
void annotation_children_keep_their_own_context() {
    auto entities=fixture();
    AnnotationState state;
    auto first=instantiate_label(default_label_templates().front(),"child:a");
    first.placement={{1,2},0,1,"a-layer"};
    auto second=first; second.id="child:b"; second.placement.layer_id="b-layer";
    state.labels={first,second};
    auto owner=make_annotation_entity("owner:a",state,
        AnnotationEntityContext{"site","a","a-floor","a-layer",std::nullopt});
    // A witness in the real annotation state must not transfer host context.
    PresentationOverride witness; witness.target_kind="object"; witness.target_id="a-wall";
    state.overrides.push_back(witness);
    owner.properties["state"]=encode_annotation_state(state,default_symbol_catalog());
    entities.emplace(owner.id,owner);
    const std::vector<SiteAnnotationTarget> targets{{owner.id,first.id},{owner.id,second.id}};
    const auto world=resolve_site_annotation_presentations(entities,targets);
    point(site_transform_point({1,2,3},world.at(targets[1]).forward),{1,2,3});
    require(world.at(targets[1]).drawing_context.layer_id=="b-layer",
        "world default still reports child's own layer");
    entities.at(owner.id).properties["presentation_frame"]={{"version",1},{"mode","building"}};
    entities.at(owner.id).properties["version"]=3;
    const auto before=entities;
    const auto batch=resolve_site_annotation_presentations(entities,targets);
    validate_document_site_frames(entities);
    point(site_transform_point({1,2,3},batch.at(targets[0]).forward),{94,204,15});
    point(site_transform_point({1,2,3},batch.at(targets[1]).forward),{94,231,15});
    point(site_transform_point({94,231,15},batch.at(targets[1]).inverse),{1,2,3});
    require(batch.at(targets[1]).source_frame==SiteFrameIdentity{SiteFrameMode::building,"site","b"} &&
        batch.at(targets[1]).dependency_ids==std::vector<std::string>{"b","b-floor","b-layer","owner:a","site"},
        "child dependency binds owner and own containers only");
    require(entities==before,"annotation resolution and validation preserve source");
    entities.at(owner.id).properties["state"]["labels"][1]["placement"]["x"]=9;
    require(resolve_site_annotation_presentations(entities,targets).at(targets[1]).dependency_digest!=
        batch.at(targets[1]).dependency_digest,"child placement edits invalidate capture");
    entities=before;
    SiteFrameLimits limits; limits.maximum_cached_dependency_entries=9;
    refuses([&]{ (void)resolve_site_annotation_presentations(entities,targets,limits); });
    limits.maximum_cached_dependency_entries=10;
    require(resolve_site_annotation_presentations(entities,targets,limits).size()==2,"shared exact child cache budget");
    limits={}; limits.maximum_dependencies=4;
    refuses([&]{ (void)resolve_site_annotation_presentations(entities,targets,limits); });
    limits={}; limits.maximum_dependency_bytes=1;
    refuses([&]{ (void)resolve_site_annotation_presentations(entities,targets,limits); });
    const std::vector<SiteAnnotationTarget> duplicate{targets[0],targets[0]};
    refuses([&]{ (void)resolve_site_annotation_presentations(entities,duplicate); });
    const std::vector<SiteAnnotationTarget> missing{targets[0],{owner.id,"missing"}};
    refuses([&]{ (void)resolve_site_annotation_presentations(entities,missing); });
    for (const auto layer: {"missing",""}) {
        entities=before;
        entities.at(owner.id).properties["state"]["labels"][1]["placement"]["layer_id"]=layer;
        refuses([&]{ validate_document_site_frames(entities); });
        refuses([&]{ (void)resolve_site_annotation_presentations(entities,targets); });
    }
    entities=before; entities.at(owner.id).properties["state"]["version"]=100;
    refuses([&]{ validate_document_site_frames(entities); });
    entities=before; entities.at(owner.id).properties["presentation_frame"]["version"]=2;
    refuses([&]{ validate_document_site_frames(entities); });
    entities=before; entities.at(owner.id).properties["state"]["labels"][1]["placement"]["x"]=nullptr;
    refuses([&]{ validate_document_site_frames(entities); });
    require(before.at(owner.id).properties["state"]==owner.properties["state"],
        "failure fixtures retain captured immutable annotation state");
}
void annotation_typed_targets_and_snapshot_capture() {
    auto entities=fixture();
    entities.erase("a-wall"); entities.erase("b-wall");
    AnnotationState state;
    auto child=instantiate_label(default_label_templates().front(),"b:c");
    child.placement.layer_id="a-layer"; state.labels={child};
    auto owner=make_annotation_entity("a",state);
    // The building IDs in fixture remain intact; use collision pairs with a
    // separator-rich owner namespace disjoint from them.
    owner.id="note:a"; entities.emplace(owner.id,owner);
    SymbolInstance symbol; symbol.id="c"; symbol.symbol_id=default_symbol_catalog().front().id;
    symbol.placement.layer_id="b-layer";
    state.labels.clear(); state.symbols={symbol};
    owner=make_annotation_entity("note:a:b",state); entities.emplace(owner.id,owner);
    const std::vector<SiteAnnotationTarget> targets{{"note:a","b:c"},{"note:a:b","c"}};
    const auto batch=resolve_site_annotation_presentations(entities,targets);
    require(batch.size()==2 && batch.at(targets[0]).dependency_digest!=batch.at(targets[1]).dependency_digest,
        "raw separator IDs never collide in typed target results");
    require(batch.at(targets[1]).drawing_context.layer_id=="b-layer",
        "symbol children also resolve their own placement layer");
    std::vector<Entity> input;
    for (const auto& [id,value]:entities) { (void)id; input.push_back(value); }
    const auto document=Document::create(input); const auto source=document.snapshot();
    const auto captured=resolve_site_annotation_presentations(source,targets);
    require(captured.at(targets[0]).snapshot_document_id==source.document_id() &&
        captured.at(targets[0]).snapshot_revision==source.revision() &&
        captured.at(targets[0]).dependency_digest==batch.at(targets[0]).dependency_digest,
        "annotation snapshot capture retains identity revision and source digest");
    require(document.snapshot().entities()==source.entities(),"annotation snapshot source is unchanged");
}
void geometric_constraints_require_one_enrolled_frame() {
    auto entities=fixture();
    entities.emplace("a-wall2",entity("a-wall2","wall",{{"layer_id","a-layer"}}));
    const auto relation=[&](ConstraintRelationKind kind,std::string second) {
        PersistentConstraint value; value.id="relation"; value.relation=kind;
        if (kind==ConstraintRelationKind::coincident)
            value.bindings={{"a-wall",WallEndpointRole::end},{second,WallEndpointRole::start}};
        else value.bindings={{"a-wall",WallEndpointRole::start},{"a-wall",WallEndpointRole::end},
            {second,WallEndpointRole::start},{second,WallEndpointRole::end}};
        return encode_constraint_entity(value);
    };
    for (const auto kind:{ConstraintRelationKind::coincident,ConstraintRelationKind::parallel,
                         ConstraintRelationKind::perpendicular}) {
        entities.insert_or_assign("relation",relation(kind,"b-wall"));
        const auto before=entities;
        refuses([&]{ validate_document_site_frames(entities); });
        require(entities==before,"cross-frame constraint refusal preserves source atomically");
        entities.insert_or_assign("relation",relation(kind,"a-wall2"));
        entities.at("relation").extensions["opaque_owner_id"]="missing";
        entities.at("relation").properties["opaque_geometry"]={{"owner_id","b-wall"}};
        const auto same=entities; validate_document_site_frames(entities);
        require(entities==same,"same-building bindings ignore opaque metadata and preserve source");
        entities.insert_or_assign("relation",relation(kind,"b-wall"));
        entities.at("b").properties.erase("site_placement");
        refuses([&]{ validate_document_site_frames(entities); });
        entities.at("a").properties.erase("site_placement");
        const auto legacy=entities; validate_document_site_frames(entities);
        require(entities==legacy,"all-world legacy multi-owner constraints remain unchanged");
        entities=fixture();
        entities.emplace("a-wall2",entity("a-wall2","wall",{{"layer_id","a-layer"}}));
    }
    // Typed identity matters even when different buildings happen to have
    // exactly the same numerical pose.
    entities.at("b").properties["site_placement"]=entities.at("a").properties["site_placement"];
    entities.insert_or_assign("relation",relation(ConstraintRelationKind::coincident,"b-wall"));
    refuses([&]{ validate_document_site_frames(entities); });
    entities.at("relation").properties["version"]=999;
    entities.at("relation").properties["relation"]="future_relation";
    entities.at("relation").properties["bindings"][0]["owner_id"]="missing";
    entities.at("relation").properties["wall_ids"]={"b-wall","missing"};
    const auto opaque=entities; validate_document_site_frames(entities);
    require(entities==opaque,"unsupported constraint bindings acquire no site-frame authority");
    PersistentConstraint single; single.id="relation"; single.relation=ConstraintRelationKind::horizontal;
    single.bindings={{"a-wall",WallEndpointRole::start},{"a-wall",WallEndpointRole::end}};
    entities.insert_or_assign("relation",encode_constraint_entity(single));
    validate_document_site_frames(entities);
}
void document_admission_preserves_unenrolled_legacy_context() {
    SiteFrameEntities legacy;
    legacy.emplace("legacy-floor",entity("legacy-floor","floor"));
    legacy.emplace("legacy-layer",entity("legacy-layer","layer",{{"floor_id","legacy-floor"}}));
    legacy.emplace("missing-parent-layer",entity("missing-parent-layer","layer",{{"floor_id","missing-floor"}}));
    legacy.emplace("opaque",entity("opaque","future_object",{{"layer_id","missing-layer"},
        {"retained",{{"future_version",99}}}}));
    const auto before=legacy;
    validate_document_site_frames(legacy);
    SiteFrameLimits legacy_limits; legacy_limits.maximum_cached_dependency_entries=1;
    validate_document_site_frames(legacy,legacy_limits);
    // General document admission still rejects dangling declared references.
    // Exercise incomplete but internally connected legacy hierarchy here.
    auto admitted=legacy;admitted.erase("missing-parent-layer");
    admitted.at("opaque").properties.erase("layer_id");
    std::vector<Entity> initial;
    for (const auto& [id,value]:admitted) { (void)id; initial.push_back(value); }
    auto document=Document::create(initial);
    document.mark_saved(document.revision());
    const auto saved=document.snapshot();
    const auto restored=Document::fork(saved);
    require(saved.entities()==admitted && restored.snapshot().entities()==admitted && legacy==before && !saved.dirty(),
        "incomplete legacy hierarchy and opaque metadata remain admitted and saved without repair");
    refuses([&]{ (void)resolve_site_presentation(legacy,"legacy-layer"); });
    refuses([&]{ (void)resolve_site_presentation(legacy,"missing-parent-layer"); });
    const std::vector<std::string> requested{"legacy-layer"};
    refuses([&]{ (void)resolve_site_presentations(legacy,requested); });

    legacy.emplace("legacy-wall",entity("legacy-wall","wall",{{"layer_id","legacy-layer"}}));
    legacy.emplace("legacy-opening",entity("legacy-opening","opening",{{"wall_id","missing-wall"}}));
    legacy.emplace("legacy-join",entity("legacy-join","wall_join",{{"wall_ids",{"legacy-wall","missing-wall"}}}));
    AnnotationState state;
    auto child=instantiate_label(default_label_templates().front(),"legacy-child");
    child.placement.layer_id="missing-layer"; state.labels={child};
    auto annotation=make_annotation_entity("legacy-note",state);
    legacy.emplace(annotation.id,annotation);
    validate_document_site_frames(legacy);
    refuses([&]{ (void)resolve_site_annotation_presentations(legacy,
        std::vector<SiteAnnotationTarget>{{annotation.id,child.id}}); });
    legacy.at("legacy-wall").properties["presentation_frame"]={{"version",1},{"mode","world"}};
    refuses([&]{ validate_document_site_frames(legacy); });
    legacy.at("legacy-wall").properties.erase("presentation_frame");
    legacy.at(annotation.id).properties["presentation_frame"]={{"version",1},{"mode","world"}};
    refuses([&]{ validate_document_site_frames(legacy); });

    auto framed=fixture();
    framed.insert(before.begin(),before.end());
    framed.emplace("framed-opaque",entity("framed-opaque","future_object",{{"layer_id","a-layer"},
        {"floor_id","missing-floor"}}));
    validate_document_site_frames(framed);
    auto world=fixture();
    world.at("a").properties.erase("site_placement");
    world.at("b").properties.erase("site_placement");
    world.at("a-floor").properties.erase("building_id");
    validate_document_site_frames(world); // Property frame alone never enrolls legacy building geometry.
    world.at("a").properties["site_placement"]=fixture().at("a").properties.at("site_placement");
    world.at("a-floor").properties["building_id"]="a";
    world.at("a-wall").properties["floor_id"]="missing-floor";
    refuses([&]{ validate_document_site_frames(world); });
}
void affected_relationships_resolve_all_owners_strictly() {
    auto entities=fixture();
    entities.emplace("legacy-wall",entity("legacy-wall","wall",{{"layer_id","missing-layer"}}));
    PersistentConstraint relation; relation.id="relation"; relation.relation=ConstraintRelationKind::coincident;
    relation.bindings={{"legacy-wall",WallEndpointRole::end},{"missing-wall",WallEndpointRole::start}};
    entities.emplace("relation",encode_constraint_entity(relation));
    validate_document_site_frames(entities);
    relation.bindings[1].owner_id="a-wall";
    entities.insert_or_assign("relation",encode_constraint_entity(relation));
    refuses([&]{ validate_document_site_frames(entities); });
    entities.erase("relation");
    entities.emplace("join",entity("join","wall_join",{{"wall_ids",{"legacy-wall","a-wall"}}}));
    refuses([&]{ validate_document_site_frames(entities); });
    entities.erase("join");
    entities.emplace("opening",entity("opening","opening",{{"wall_id","a-wall"},{"layer_id","missing-layer"}}));
    refuses([&]{ validate_document_site_frames(entities); });
    entities.erase("opening");
    entities.emplace("stair",entity("stair","stair",{{"layer_id","a-layer"}}));
    entities.emplace("rail",entity("rail","railing",{{"host",{{"stair_id","stair"}}},{"floor_id","missing-floor"}}));
    refuses([&]{ validate_document_site_frames(entities); });
    entities.erase("rail");
    // An invalid cycle must still refuse when an independently known enrolled
    // context reaches it; the admission scan must terminate without recursion.
    entities.at("a-layer").properties["floor_id"]="a-layer";
    entities.at("a-layer").properties["building_id"]="a";
    refuses([&]{ validate_document_site_frames(entities); });
}
void strict_decoding_and_resource_budgets() {
    const auto original=fixture();
    const std::vector<std::string> wall_ids{"a-wall"};
    for (const auto* field : {"origin_m","vertical_datum","rotation_radians"}) {
        auto e=original; e.at("site").properties["site_frame"].erase(field);
        refuses([&]{ validate_document_site_frames(e); });
    }
    for (const auto v : {0,2}) {
        auto e=original; e.at("site").properties["site_frame"]["version"]=v;
        refuses([&]{ validate_document_site_frames(e); });
    }
    auto e=original; e.at("a").properties["site_placement"]["scale"]=2;
    refuses([&]{ validate_document_site_frames(e); });
    e=original; e.at("a-wall").properties["layer_id"]="missing";
    refuses([&]{ (void)resolve_site_presentation(e,"a-wall"); });
    refuses([&]{ (void)resolve_site_presentations(e,wall_ids); });
    e.at("a-wall").properties["building_id"]="a";
    refuses([&]{ validate_document_site_frames(e); });
    e=original; e.at("site").properties["site_frame"]["origin_m"]={1,2};
    refuses([&]{ validate_document_site_frames(e); });
    e=original; e.at("site").properties["site_frame"]["version"]=1.0;
    refuses([&]{ validate_document_site_frames(e); });
    e=original; e.at("a-wall").properties["layer_id"]=nullptr;
    refuses([&]{ (void)resolve_site_presentation(e,"a-wall"); });
    refuses([&]{ (void)resolve_site_presentations(e,wall_ids); });
    e.at("a-wall").properties["building_id"]="a";
    refuses([&]{ validate_document_site_frames(e); });
    e=original; e.at("a-wall").properties["presentation_frame"]={{"version",1},{"mode","future"}};
    refuses([&]{ validate_document_site_frames(e); });
    SiteFrameLimits limits; limits.maximum_entities=1;
    refuses([&]{ validate_document_site_frames(original,limits); });
    limits={}; limits.maximum_dependency_bytes=1;
    refuses([&]{ (void)resolve_site_presentation(original,"a-wall",limits); });
    limits={}; limits.maximum_dependencies=1;
    refuses([&]{ (void)resolve_site_presentation(original,"a-wall",limits); });
    e=original; e.at("site").properties["site_frame"]["vertical_datum"]["identifier"]="   ";
    refuses([&]{ validate_document_site_frames(e); });
    e=original; e.at("a").properties.erase("site_placement");
    e.at("site").properties["site_frame"]["version"]=2;
    refuses([&]{ (void)resolve_site_presentation(e,"a-wall"); });
    refuses([&]{ (void)site_transform_point({std::numeric_limits<double>::max(),0,0},
        {{std::numeric_limits<double>::max(),0,0},0}); });
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        composition_and_snapshot_purity(); world_gesture_conjugation_and_curves();
        terrain_datum_is_explicit_and_never_scales_grade(); hosted_clusters_and_join_frames();
        independent_and_annotation_contexts(); strict_decoding_and_resource_budgets();
        batch_matches_singular_and_refuses_invalid_sources();
        snapshot_capture_matches_map_without_mutation();
        annotation_children_keep_their_own_context(); annotation_typed_targets_and_snapshot_capture();
        geometric_constraints_require_one_enrolled_frame();
        document_admission_preserves_unenrolled_legacy_context();
        affected_relationships_resolve_all_owners_strictly();
        std::cout << "site_frame_tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
