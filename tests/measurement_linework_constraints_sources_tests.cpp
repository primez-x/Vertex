#include "sketch/constraint_authoring.hpp"
#include "sketch/measurement_area_definition.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <set>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void accepted(const ConstraintAuthoringPreview& preview) {
    if (preview.accepted()) return;
    std::string error="Expected accepted measured-source preview";
    for(const auto& diagnostic:preview.diagnostics()) error+="\n"+diagnostic;
    throw std::runtime_error(error);
}
void refused(const std::function<void()>& fn, const char* message) {
    try { fn(); } catch(const std::exception&) { return; }
    throw std::runtime_error(message);
}
struct Temporary {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("vertex-measured-constraints-"+make_stable_id());
    Temporary() {std::filesystem::create_directory(path);}
    ~Temporary() {std::error_code ignored; std::filesystem::remove_all(path,ignored);}
};
Entity stroke(const std::string& id,const std::vector<Vec2>& points,bool closed=false) {
    MeasurementLinework model; model.stroke_id=id; model.anchor=points.front(); model.closed=closed;
    model.extensions={{"vendor",{{"literal",id+":v0"},{"retain",Json::array({7,"raw"})}}}};
    for(std::size_t i=1;i<points.size();++i) {
        ConstructionReceipt receipt; receipt.segment_id=id+":e"+std::to_string(i);
        receipt.kind=BoundaryConstructionKind::line_to_point;receipt.start=points[i-1];receipt.chord_end=points[i];
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(i-1),
            closed&&i+1==points.size()?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"model",encode_measurement_linework_model(model)}},true,{{"opaque","retain"}}};
}
struct Fixture {Document document;std::vector<std::string> areas;};
Fixture fixture(bool grouped=false) {
    auto document=Document::create({
        {"p","property",{{"calculation_workflow","appraisal"},{"appraisal_policy",{{"policy_kind","residential_declared"},
            {"version",1},{"property_kind","detached_single_family"},{"measurement_basis","exterior"}}}},false},
        {"b","building",{{"property_id","p"}},false},
        {"f","floor",{{"building_id","b"},{"appraisal_facts",{{"grade","above"}}}},false},
        {"l","layer",{{"floor_id","f"}},false},
        stroke("outline",{{0,0},{4,0},{4,4},{0,4},{0,0}},true),
        stroke("separator",{{2,-1},{2,5}}),stroke("unrelated",{{10,10},{11,10}})});
    const auto detected=detect_measurement_areas(document.snapshot(),"outline");
    require(detected.graph.faces.size()==2,"fixture must detect actual adjacent measured faces");
    std::vector<MeasurementAreaChoice> choices(2);
    for(auto& choice:choices) {choice.disposition=MeasurementAreaDisposition::define_area;choice.classification="living";
        if(grouped)choice.combine_group=1;}
    auto definition=prepare_measurement_area_definition(document.snapshot(),"outline",choices);
    document.apply(definition.command);
    auto ids=definition.area_ids;std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
    require(ids.size()==(grouped?1:2),"definition must create reviewed independent/grouped identities");
    ApplyEntityChanges facts;facts.expected_revision=document.revision();facts.message="Declare appraisal observations";
    for(const auto& id:ids) {
        auto area=document.snapshot().entities().at(id);
        area.properties["appraisal_facts"]={{"finish","finished"},{"access","direct_interior"},
            {"ceiling_eligibility","standard"},{"area_use","dwelling"},{"boundary_role","measured_area"}};
        area.extensions["vendor"]={{"literal",id},{"retain",true}};
        facts.entity_changes.push_back(EntityChange::upsert(std::move(area)));
    }
    document.apply(facts);return {std::move(document),std::move(ids)};
}
ConstraintAuthoringIntent translate(std::vector<std::string> ids,Vec2 offset) {
    ConstraintAuthoringIntent intent;MeasuredStrokeTransformIntent move;
    for(auto& id:ids)move.targets.push_back({std::move(id),{{},0,false,false,offset}});
    intent.measured_stroke_transform=std::move(move);intent.message="Move measured sources";return intent;
}
void qualification(const DocumentSnapshot& snapshot,const std::vector<std::string>& ids,bool current,
                   const std::set<std::string,std::less<>>* visible=nullptr) {
    const auto checks=measurement_linework_source_checks(snapshot.entities(),visible);
    for(const auto& id:ids) require(checks.at(id).current==current,"derived source qualification must reflect final source graph");
    const auto report=build_appraisal_document_report(snapshot,"p",AreaUnit::square_metre,visible);
    if(current)require(report.qualified&&report.calculation&&std::abs(report.calculation->property.gla().total.square_metres-16)<1e-9,
        "current completed source areas must supply actual sixteen-square-metre GLA");
    else require(!report.qualified&&!report.calculation,"stale source areas must withhold appraisal GLA");
}
void preserve_area_identity(const Entity& before,const Entity& after) {
    const auto a=decode_identified_boundary_entity(before),b=decode_identified_boundary_entity(after);
    require(a.id==b.id&&a.segments.size()==b.segments.size(),"source completion must retain area identity/topology");
    for(std::size_t i=0;i<a.segments.size();++i)require(a.segments[i].segment_id==b.segments[i].segment_id&&
        a.segments[i].start_vertex_id==b.segments[i].start_vertex_id&&a.segments[i].end_vertex_id==b.segments[i].end_vertex_id,
        "source rebuild must retain every area segment and stable vertex ID");
    require(before.properties.at("appraisal_facts")==after.properties.at("appraisal_facts")&&
        before.extensions.at("vendor")==after.extensions.at("vendor"),"source completion must preserve declared facts and opaque metadata");
}
void individual_and_grouped_atomic_completion() {
    Temporary temporary;
    for(bool grouped:{false,true}) {
        auto data=fixture(grouped);auto& document=data.document;const auto before=document.snapshot();
        qualification(before,data.areas,true);
        auto intent=translate({"separator"},{1,0});
        if(!grouped) {BoundaryGeometryEdit edit;edit.boundary_id="separator";edit.target_id="separator:v1";edit.target_position={3,5};
            intent.measured_stroke_transform.reset();intent.measured_stroke_vertex_move=MeasuredStrokeVertexMoveIntent{edit,true};}
        const auto preview=preview_constraint_authoring(before,intent);accepted(preview);
        const auto candidate=preview_constraint_authoring_snapshot(before,preview);
        qualification(candidate,data.areas,true);
        apply_constraint_authoring(document,preview);const auto after=document.snapshot();
        require(after.entities()==candidate.entities()&&after.revision()==before.revision()+1&&after.history().size()==before.history().size()+1,
            "one source edit and all derived completions must form one atomic command");
        require(after.entities().at("unrelated")==before.entities().at("unrelated"),"unrelated source must remain exact");
        for(const auto& id:data.areas)preserve_area_identity(before.entities().at(id),after.entities().at(id));
        if(grouped)require(after.entities().at(data.areas[0]).extensions.at("measurement_linework_group")!=
            before.entities().at(data.areas[0]).extensions.at("measurement_linework_group"),"cancelled seam movement must refresh group member lineage");
        const auto proof=command_to_json(*after.history().back().boundary_constraint_changes);
        require(proof.at("version")==11&&proof.at("measured_source_completion")==true&&!proof.at("measured_stroke_edits").empty(),
            "source completion must retain measured typed command authority");
        require(command_to_json(command_from_json(proof))==proof,"envelope eleven must retain strict exact command roundtrip");
        document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"Undo must restore sources and all source consumers exactly");
        qualification(document.snapshot(),data.areas,true);
        document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"Redo must reproduce the same source completion");
        const auto path=temporary.path/(grouped?"group.bldproj":"individual.bldproj");(void)ProjectStore::save(path,document.snapshot());
        const auto loaded=ProjectStore::load(path);
        require(document_authoring_source_digest_v1(loaded.document.snapshot())==document_authoring_source_digest_v1(document.snapshot()),
            "native reopen must preserve exact IDs/facts/lineage/receipts and command history");
        qualification(loaded.document.snapshot(),data.areas,true);
    }
}

void stale_ambiguous_authored_and_phase_sources() {
    for(int mode:{0,1,2,3}) {
        auto data=fixture();auto entities=data.document.snapshot().entities();
        if(mode==0)entities.at("separator")=stroke("separator",{{2.5,-1},{2.5,5}});
        if(mode==2) {
            auto& area=entities.at(data.areas[0]);const auto boundary=decode_identified_boundary_entity(area);
            BoundaryConstructionRecord record;record.schema_version=boundary_receipt_schema_version_v2;
            record.boundary_id=area.id;record.anchor=boundary.segments.front().segment.start;
            for(const auto& edge:boundary.segments) {ConstructionReceipt receipt;receipt.segment_id=edge.segment_id;
                receipt.kind=BoundaryConstructionKind::line_to_point;receipt.start=edge.segment.start;receipt.chord_end=edge.segment.end;
                record.edges.push_back({edge.segment_id,edge.start_vertex_id,edge.end_vertex_id,receipt});}
            area.properties["boundary_authoring"]=encode_boundary_receipt_envelope(record);
        }
        if(mode==3) {
            const auto phases=ModelPhases::create({"outline","separator","unrelated"},{"outline","separator","unrelated"},
                {{"active","Active",{"separator"},{}}},"active");
            entities.emplace("phases",Entity{"phases","model_phases",{{"model",phases.to_json()}},false});
        }
        std::vector<Entity> values;for(const auto& [id,entity]:entities){(void)id;values.push_back(entity);}
        auto document=Document::create(values);const auto before=document.snapshot();
        auto intent=translate(mode==3?std::vector<std::string>{"outline"}:std::vector<std::string>{"separator"},
            mode==1?Vec2{5,0}:Vec2{.5,0});
        const auto preview=preview_constraint_authoring(before,intent);accepted(preview);apply_constraint_authoring(document,preview);
        const auto after=document.snapshot();
        for(const auto& id:data.areas)if(mode!=2||id==data.areas[0])require(after.entities().at(id)==before.entities().at(id),
            "stale, ambiguous, phase-unavailable or authored area must not receive fabricated source completion");
        std::set<std::string,std::less<>> visible;for(const auto& [id,entity]:after.entities()){(void)entity;visible.insert(id);}
        if(mode==3)visible.erase("separator");
        const auto checks=measurement_linework_source_checks(after.entities(),&visible);
        require(!checks.at(data.areas[0]).current,"unqualified consumer must remain explicitly stale");
        const auto report=build_appraisal_document_report(after,"p",AreaUnit::square_metre,&visible);
        require(!report.qualified&&!report.calculation,"unqualified source consumer must withhold property GLA");
        if(mode==3)require(after.entities().at("separator")==before.entities().at("separator"),"inactive source must remain exact");
    }
}

void hard_derived_relation_and_bad_commands_are_atomic() {
    auto data=fixture();auto document=Document::fork(data.document.snapshot());const auto before=document.snapshot();
    const auto preview=preview_constraint_authoring(before,translate({"separator"},{1,0}));accepted(preview);
    const auto candidate=preview_constraint_authoring_snapshot(before,preview);
    const auto proof=command_to_json(*candidate.history().back().boundary_constraint_changes);
    std::vector<Json> bad;
    auto wire=proof;wire["measured_source_completion"]=false;bad.push_back(wire);
    wire=proof;wire["measured_stroke_edits"].push_back(wire["measured_stroke_edits"][0]);bad.push_back(wire);
    wire=proof;wire["measured_stroke_edits"][0]["extra"]=true;bad.push_back(wire);
    wire=proof;wire["measured_stroke_edits"][0]["stroke_id"]="missing";bad.push_back(wire);
    wire=proof;wire["measured_stroke_edits"][0]["authored_edit"]={{"version",1},{"kind","move_vertex"},
        {"boundary_id","separator"},{"vertex_id","separator:v0"},{"position",{3,-1}}};bad.push_back(wire);
    for(const auto& malformed:bad) {
        refused([&]{document.apply(command_from_json(malformed));},"malformed or conflicting measured authority must reject");
        require(document.snapshot().entities()==before.entities()&&document.revision()==before.revision()&&
            document.snapshot().history().size()==before.history().size(),"bad typed command must preserve document and history atomically");
    }
    auto entities=before.entities();bool installed=false;
    for(const auto& id:data.areas) {
        const auto area=decode_identified_boundary_entity(entities.at(id));
        for(const auto& edge:area.segments)if(edge.segment.start.y==0&&edge.segment.end.y==0) {
            PersistentConstraint lock;lock.id="derived-edge-lock";lock.relation=ConstraintRelationKind::fixed_length;
            lock.length=parse_quantity("2 m");lock.bindings={{id,WallEndpointRole::start,edge.segment_id,edge.start_vertex_id},
                {id,WallEndpointRole::end,edge.segment_id,edge.end_vertex_id}};
            entities.emplace(lock.id,encode_constraint_entity(lock));installed=true;break;
        }
        if(installed)break;
    }
    require(installed,"fixture must bind a genuine derived horizontal edge");
    std::vector<Entity> values;for(const auto& [id,entity]:entities){(void)id;values.push_back(entity);}
    auto locked=Document::create(values);const auto original=locked.snapshot();
    const auto rejected=preview_constraint_authoring(original,translate({"separator"},{1,0}));
    require(!rejected.accepted()&&rejected.candidate_entities()==original.entities(),
        "final rebuilt area must honor its persisted hard relation before preview admission");
    refused([&]{apply_constraint_authoring(locked,rejected);},"rejected rebuilt consumer must not apply");
    require(locked.snapshot().entities()==original.entities()&&locked.revision()==original.revision(),"final relation refusal must be atomic");
}

void durable_reader_floors() {
    Temporary temporary;auto data=fixture();auto& document=data.document;
    const auto preview=preview_constraint_authoring(document.snapshot(),translate({"separator"},{1,0}));accepted(preview);
    apply_constraint_authoring(document,preview);const auto current=document.snapshot();
    document.undo(document.revision());const auto undone=document.snapshot();document.redo(document.revision());
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::erase("separator")},.message="Delete source"});
    const std::vector<DocumentSnapshot> snapshots{current,undone,document.snapshot()};
    for(std::size_t i=0;i<snapshots.size();++i) {
        require(ProjectStore::required_format_version(snapshots[i])==35,"current, undone and deleted envelope-eleven evidence must retain native reader floor");
        const auto file=temporary.path/("floor-"+std::to_string(i)+".bldproj");(void)ProjectStore::save(file,snapshots[i]);
        const auto reopened=ProjectStore::load(file);
        require(document_authoring_source_digest_v1(reopened.document.snapshot())==document_authoring_source_digest_v1(snapshots[i]),
            "retained typed source history must survive native reopen");
        const auto extract=temporary.path/("extract-"+std::to_string(i));extract_project(snapshots[i],extract);
        std::ifstream input(extract/"project.json");const auto manifest=Json::parse(input);
        require(manifest.at("exchange_version")==33,"retained measured completion proof must advertise logical reader floor");
        sqlite3* db=nullptr;const auto name=file.u8string();const std::string utf8(reinterpret_cast<const char*>(name.data()),name.size());
        require(sqlite3_open_v2(utf8.c_str(),&db,SQLITE_OPEN_READWRITE,nullptr)==SQLITE_OK,"controlled native database must open");
        const auto status=sqlite3_exec(db,"PRAGMA user_version=34; UPDATE metadata SET value='34' WHERE key='format_version'",nullptr,nullptr,nullptr);
        sqlite3_close(db);require(status==SQLITE_OK,"controlled downgrade markers must write");
        const auto fingerprint=ProjectStore::file_sha256(file);
        bool rejected=false;try{(void)ProjectStore::load(file);}catch(const StorageError& error){rejected=error.code()==StorageErrorCode::unsupported_format;}
        require(rejected&&ProjectStore::file_sha256(file)==fingerprint,"downgraded reader marker must refuse without changing native bytes");
    }
    // A satisfied relation-only entity binding must protect readers even with
    // no v5 model or command-eleven geometry event in the current document.
    auto line=stroke("line",{{0,0},{2,0}});PersistentConstraint relation;relation.id="line-horizontal";
    relation.relation=ConstraintRelationKind::horizontal;relation.bindings={{"line",WallEndpointRole::start,"line:e1","line:v0"},
        {"line",WallEndpointRole::end,"line:e1","line:v1"}};
    auto entities=fixture().document.snapshot().entities();entities.emplace(line.id,line);entities.emplace(relation.id,encode_constraint_entity(relation));
    std::vector<Entity> values;for(const auto& [id,entity]:entities){(void)id;values.push_back(entity);}
    const auto relation_only=Document::create(values).snapshot();
    require(ProjectStore::required_format_version(relation_only)==35,"entity-only measured constraint binding must protect its reader floor");
    entities.erase(relation.id);values.clear();for(const auto& [id,entity]:entities){(void)id;values.push_back(entity);}
    auto satisfied=Document::create(values);const auto relation_before=satisfied.snapshot();
    ConstraintAuthoringIntent relation_intent;relation_intent.relation_mutations={ConstraintRelationMutation::upsert(relation)};
    const auto relation_preview=preview_constraint_authoring(relation_before,relation_intent);accepted(relation_preview);
    apply_constraint_authoring(satisfied,relation_preview);const auto satisfied_snapshot=satisfied.snapshot();
    require(satisfied_snapshot.entities().at("line")==relation_before.entities().at("line"),
        "satisfied relation-only admission must preserve measured geometry exactly");
    const auto relation_proof=command_to_json(*satisfied_snapshot.history().back().boundary_constraint_changes);
    require(relation_proof.at("version")==11&&relation_proof.at("measured_source_completion")==true&&
        relation_proof.at("measured_stroke_edits").empty(),"relation-only measured authority must retain envelope eleven without fabricated geometry edits");
    require(command_to_json(command_from_json(relation_proof))==relation_proof,"empty measured lane must preserve relation-only proof roundtrip");
    auto model=*decode_measurement_linework_model(line.properties.at("model")).model;
    BoundaryGeometryEdit a;a.boundary_id="line";a.target_id="line:v0";a.target_position={2,0};
    auto b=a;b.target_id="line:v1";b.target_position={0,0};
    line.properties["model"]=encode_measurement_linework_model(edited_measurement_linework_vertices(model,{a,b}));
    entities=fixture().document.snapshot().entities();entities.emplace(line.id,line);values.clear();
    for(const auto& [id,entity]:entities){(void)id;values.push_back(entity);}
    require(ProjectStore::required_format_version(Document::create(values).snapshot())==35,"batch-v5 model alone must retain its native reader floor");
}

void measured_only_annotation_completion_and_supplements() {
    auto entities=fixture().document.snapshot().entities();
    for(auto it=entities.begin();it!=entities.end();) {
        if(it->second.type=="measurement_boundary"||it->first=="outline"||it->first=="separator")it=entities.erase(it);else ++it;
    }
    entities.emplace("a",stroke("a",{{0,0},{2,0}}));entities.emplace("b-stroke",stroke("b-stroke",{{0,3},{2,3}}));
    AnnotationState state;
    for(const auto& [id,offset]:std::vector<std::pair<std::string,Vec2>>{{"a",{1,2}},{"b-stroke",{-3,4}},{"unrelated",{5,6}}}) {
        PresentationOverride record;record.target_kind="area";record.target_id=id;record.plan_label_offset=offset;
        record.style.stroke_color="#315a8c";state.overrides.push_back(record);
    }
    auto annotation=make_annotation_entity("stroke-labels",state);annotation.extensions={{"vendor",{{"literal","a"},{"retain",true}}}};
    entities.emplace(annotation.id,annotation);std::vector<Entity> values;
    for(const auto& [id,entity]:entities){(void)id;values.push_back(entity);}
    auto document=Document::create(values);const auto before=document.snapshot();
    ConstraintAuthoringIntent intent;MeasuredStrokeTransformIntent rigid;
    const PlanarTransform transform{{},std::numbers::pi/2,true,false,{7,9}};
    rigid.targets={{"a",transform},{"b-stroke",transform}};intent.measured_stroke_transform=rigid;
    const auto preview=preview_constraint_authoring(before,intent);accepted(preview);
    const auto candidate=preview_constraint_authoring_snapshot(before,preview);
    auto command=*candidate.history().back().boundary_constraint_changes;
    // Retained source offsets, rather than preview offsets, are the input to
    // completion. A supplemental color edit may merge into that same owner.
    auto styled=annotation;auto changed=state;changed.overrides[0].style.stroke_color="#824725";
    styled.properties["state"]=make_annotation_entity(annotation.id,changed).properties.at("state");
    styled.extensions["style-review"]=true;
    command.supplemental_source_completion=true;
    command.supplemental_entity_changes={EntityChange::upsert(styled)};
    const auto asset=Asset::create("annotation-proof","application/octet-stream",{std::byte{1},std::byte{2}});
    command.supplemental_asset_changes={AssetChange::upsert(asset)};
    const auto wire=command_to_json(command);
    require(wire.at("version")==11&&wire.at("source_completion")==false&&wire.at("exterior_source_edits").empty(),
        "measured-only metadata/assets must retain envelope eleven without borrowing exterior authority");
    require(command_to_json(command_from_json(wire))==wire,"measured-only supplement proof must roundtrip exactly");
    document.apply(command_from_json(wire));const auto after=document.snapshot();
    const auto result=decode_annotation_entity(after.entities().at(annotation.id));
    const auto offset_matches=[](Vec2 value,Vec2 expected){return std::hypot(value.x-expected.x,value.y-expected.y)<1e-12;};
    require(result.overrides.size()==3&&result.overrides[0].plan_label_offset&&result.overrides[1].plan_label_offset&&
        offset_matches(*result.overrides[0].plan_label_offset,{2,1})&&offset_matches(*result.overrides[1].plan_label_offset,{4,-3}),
        "both selected stroke offsets must rotate then reflect exactly once in the shared annotation owner");
    require(result.overrides[0].style.stroke_color=="#824725"&&
        after.entities().at(annotation.id).extensions==styled.extensions&&
        after.entities().at(annotation.id).properties.at("state").at("overrides")[2]==
            annotation.properties.at("state").at("overrides")[2]&&after.entities().at("unrelated")==before.entities().at("unrelated")&&
        after.assets().at(asset.id)==asset,"supplement merge must preserve unrelated row/source, vendor metadata and exact asset bytes");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities()&&document.snapshot().assets()==before.assets(),
        "Undo must restore both placements, style and supplementary assets together");
    document.redo(document.revision());require(document.snapshot().entities()==after.entities()&&document.snapshot().assets()==after.assets(),
        "Redo must reproduce placements without double-transforming offsets");
    Temporary temporary;const auto file=temporary.path/"label-completion.bldproj";(void)ProjectStore::save(file,document.snapshot());
    require(document_authoring_source_digest_v1(ProjectStore::load(file).document.snapshot())==document_authoring_source_digest_v1(document.snapshot()),
        "native reopen must retain exact measured placement completion and mixed metadata/assets proof");
    for(int fault:{0,1,2,3,4,5}) {
        auto forged=command;forged.supplemental_asset_changes.clear();
        if(fault<3) {
            auto raw=before.entities().at("a");raw.extensions["smuggled"]=true;
            if(fault==2){raw.type="label";raw.required=false;raw.properties={{"text","type replacement"}};}
            forged.supplemental_entity_changes={fault==1?EntityChange::erase("a"):EntityChange::upsert(raw)};
        } else {
            auto overlap=styled;
            if(fault==3)overlap.properties["state"]["overrides"][0]["plan_label_offset_m"]=Json::array({9,9});
            if(fault==4)overlap.properties["state"]["overrides"].erase(overlap.properties["state"]["overrides"].begin());
            forged.supplemental_entity_changes={fault==5?EntityChange::erase(annotation.id):EntityChange::upsert(overlap)};
        }
        auto fresh=Document::fork(before);refused([&]{fresh.apply(forged);},"raw measured owner or owned-placement supplement overlap must reject");
        require(fresh.snapshot().entities()==before.entities()&&fresh.snapshot().assets()==before.assets()&&fresh.revision()==before.revision()&&
            fresh.snapshot().history().size()==before.history().size(),"supplement refusal must preserve complete document/history atomically");
    }
    // Pure translation cannot alter a model-relative label offset.
    auto translated=Document::fork(before);const auto translation=preview_constraint_authoring(before,translate({"a","b-stroke"},{3,5}));accepted(translation);
    apply_constraint_authoring(translated,translation);
    require(translated.snapshot().entities().at(annotation.id)==annotation,"translation must preserve all owned offsets exactly");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {individual_and_grouped_atomic_completion();stale_ambiguous_authored_and_phase_sources();
        hard_derived_relation_and_bad_commands_are_atomic();durable_reader_floors();measured_only_annotation_completion_and_supplements();}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    std::cout<<"Measured constraints and sources tests passed\n";return 0;
}
