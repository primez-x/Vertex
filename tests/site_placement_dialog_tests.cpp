#include "support/detached_document_snapshot.hpp"
#include "sketch/desktop/site_placement_dialog.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/terrain_surface.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void expect_near(double a,double b) { require(std::abs(a-b)<1e-10,"independent placement expectation"); }
template<class W> W& widget(QDialog& d,const char* name) {
    auto* result=d.findChild<W*>(name);require(result!=nullptr,name);return *result;
}
void text(QDialog& d,const char* name,const char* value) { widget<QLineEdit>(d,name).setText(value); }
void mode(QDialog& d,const char* value) {
    auto& choice=widget<QComboBox>(d,"sitePlacementMode");
    const auto index=choice.findData(value);require(index>=0,"mode exists");choice.setCurrentIndex(index);
}
Entity boundary(std::string id,std::string layer) {
    IdentifiedBoundary b;b.id=id;b.type="boundary";
    const Boundary square{{{0,0},{2,0},0},{{2,0},{2,1},0},{{2,1},{0,1},0},{{0,1},{0,0},0}};
    for(std::size_t i=0;i<square.size();++i)b.segments.push_back({id+"e"+std::to_string(i),id+"v"+std::to_string(i),id+"v"+std::to_string((i+1)%4),square[i]});
    auto e=encode_identified_boundary_entity(b);e.properties["layer_id"]=layer;e.properties["name"]=id+" outline";
    e.properties["authored_expression"]={{"name","Measured width"},{"original_expression","6 ft 6 1/2 in"}};return e;
}
std::vector<Entity> entities() {
    std::vector<Entity> result{{"site","property",{{"name","Surveyed lot"},{"site_frame",{
        {"version",1},{"origin_m",{100.0,200.0,10.0}},{"rotation_radians",std::numbers::pi/2},
        {"vertical_datum",{{"identifier","survey-A"},{"height_at_origin_m",150.0}}}}}}}};
    for(const auto* id:{"a","b"}) {
        const std::string b=id;
        result.push_back({b,"building",{{"name",b=="a"?"Main house":"Guest house"},{"property_id","site"},
            {"site_placement",{{"version",1},{"translation_m",{b=="a"?3.0:30.0,4.0,2.0}},{"rotation_radians",0.0}}}}});
        result.push_back({b+"-floor","floor",{{"building_id",b}}});
        result.push_back({b+"-layer","layer",{{"floor_id",b+"-floor"}}});
        result.push_back(boundary(b+"-outline",b+"-layer"));
    }
    TerrainSurface terrain("Authored survey",{{"p0",0,0,154},{"p1",2,0,155},{"p2",0,2,154}},{TerrainTriangle{{0,1,2}}});
    result.push_back({"terrain","terrain_surface",{{"name","Survey terrain"},{"property_id","site"},{"model",terrain.to_json()},
        {"terrain_elevation_binding",{{"version",1},{"mode","declared_absolute"},{"datum_identifier","survey-A"}}}}});
    AssemblyType type;type.id="post";type.name="Timber post";
    type.profiles={{"solid",{{{0,0},{1,0},0},{{1,0},{1,1},0},{{1,1},{0,1},0},{{0,1},{0,0},0}},{},0,2,std::nullopt}};
    result.push_back({"catalog","assembly_model",{{"model",AssemblyModel::create({}, {type},{}).to_json()}}});
    AssemblyInstance instance;instance.id="assembly";instance.type_id="post";instance.root_transform=AssemblyTransform{{1,2,3},0,1};
    result.push_back(encode_document_assembly_instance({"assembly","assembly_instance",{{"name","Entrance post"},{"layer_id","b-layer"}}},{"catalog",instance}));
    AnnotationState notes;LabelInstance a;a.id="note-a";a.content="House note";a.placement.layer_id="a-layer";notes.labels.push_back(a);
    auto b=a;b.id="note-b";b.content="Guest note";b.placement.layer_id="b-layer";notes.labels.push_back(b);
    result.push_back(make_annotation_entity("notes",notes,AnnotationEntityContext{"site","a","a-floor","a-layer",std::nullopt}));
    return result;
}
const SitePlacementImpact& impact(const SitePlacementDraft& d,const std::string& owner,const std::optional<std::string>& child={}) {
    for(const auto& i:d.impacts)if(i.owner_entity_id==owner && i.annotation_child_id==child)return i;
    throw std::runtime_error("expected affected placement owner");
}
void property_and_detached_guard() {
    auto document=Document::create(entities());auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
    SitePlacementDialog dialog(source,"site",true);text(dialog,"sitePlacementX","110 m");
    require(!widget<QDialogButtonBox>(dialog,"sitePlacementButtons").button(QDialogButtonBox::Save)->isEnabled(),"Save requires a reviewed placement");
    require(dialog.previewUpdate(),"property preview");require(!dialog.candidate(),"preview alone is detached");
    require(widget<QDialogButtonBox>(dialog,"sitePlacementButtons").button(QDialogButtonBox::Save)->isEnabled(),"reviewed placement enables Save");
    require(widget<QTableWidget>(dialog,"sitePlacementPreviewTable").rowCount()>2,"both buildings shown");
    widget<QDialogButtonBox>(dialog,"sitePlacementButtons").button(QDialogButtonBox::Save)->click();
    require(dialog.result()==QDialog::Accepted && dialog.candidate().has_value(),"normal Save accepts reviewed placement");const auto draft=*dialog.candidate();
    require(draft.expected_revision==source.revision() && draft.source_snapshot_digest==digest && draft.target_entity_id=="site","full source authority captured");
    expect_near(impact(draft,"a-outline").before.forward.translation_m.x,96);expect_near(impact(draft,"a-outline").after.forward.translation_m.x,106);
    expect_near(impact(draft,"b-outline").after.forward.translation_m.y,230);
    for(const auto& i:draft.impacts)require(i.owner_entity_id!="notes" && i.owner_entity_id!="assembly","property edit does not enroll legacy independent owners");
    const auto detached=Document::preview_command(source,draft.command);
    require(detached.entities().at("a-outline")==source.entities().at("a-outline"),"geometry and named expression unchanged");
    require(document_snapshot_digest(document.snapshot())==digest,"preview and submit preserve all source including history");
    auto replacement=source.entities().at("site");replacement.properties["name"]="Replacement head";
    // Isolated snapshot tampering simulates an out-of-band same-ID/revision
    // replacement; it never changes the source or the live document.
    sketch::test::DetachedDocumentSnapshotFixture replaced(source);replaced.entities().at("site")=replacement;
    require(replaced.document_id()==source.document_id() && replaced.revision()==source.revision(),"replaced head retains identity and revision");
    require(draft.source_snapshot_digest!=document_snapshot_digest(replaced),"proposal digest rejects same-ID/revision replaced source");
    require(document_snapshot_digest(source)==digest,"replacement fixture owns its independent source copy");
    SitePlacementDialog stale(source,"a",true);text(stale,"sitePlacementX","7 m");require(stale.previewUpdate(),"first preview");
    text(stale,"sitePlacementX","8 m");require(!stale.submit() && !stale.candidate(),"changed input requires current preview");
    require(!widget<QDialogButtonBox>(stale,"sitePlacementButtons").button(QDialogButtonBox::Save)->isEnabled(),"changed placement disables stale Save");
    require(stale.previewUpdate() && stale.submit(),"updated preview accepts");
}
void terrain_datum_and_failures() {
    auto document=Document::create(entities());const auto source=document.snapshot();
    SitePlacementDialog datum(source,"site",true);text(datum,"sitePlacementDatumHeight","152 m");
    require(datum.previewUpdate() && datum.submit(),"explicit datum height edit");
    const auto terrain=impact(*datum.candidate(),"terrain");expect_near(site_transform_point({0,0,154},terrain.after.forward).z,12);
    SitePlacementDialog unknown(source,"terrain",true);text(unknown,"sitePlacementDatumIdentifier","survey-B");
    require(!unknown.previewUpdate() && !unknown.submit() && !unknown.candidate() && !unknown.lastError().isEmpty(),"unknown exact datum rejected");
    SitePlacementDialog relative(source,"terrain",true);mode(relative,"relative_site_origin");
    require(relative.previewUpdate() && relative.submit(),"relative terrain authored explicitly");
    expect_near(site_transform_point({0,0,154},impact(*relative.candidate(),"terrain").after.forward).z,164);
    SitePlacementDialog legacy(source,"terrain",true);mode(legacy,"default");
    require(legacy.previewUpdate() && legacy.submit(),"legacy terrain binding removal");
    require(!legacy.candidate()->command.entity_changes.front().entity.properties.contains("terrain_elevation_binding"),"legacy removes contract");
    for(const auto* input:{"nan","inf","1e999 m"}) {
        SitePlacementDialog invalid(source,"a",true);text(invalid,"sitePlacementX",input);
        require(!invalid.previewUpdate() && !invalid.candidate(),"nonfinite rejected");
    }
    SitePlacementDialog conflict(source,"site",true);text(conflict,"sitePlacementDatumIdentifier","survey-B");
    require(!conflict.previewUpdate(),"property datum edit cannot invalidate absolute terrain");
}
void precision_clear_cancel_and_assembly() {
    auto e=entities();for(auto& item:e)if(item.id=="a")item.properties["site_placement"]["translation_m"][0]=0.12345678901234566;
    auto document=Document::create(e);auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
    SitePlacementDialog untouched(source,"a",false);auto& x=widget<QLineEdit>(untouched,"sitePlacementX");x.setText("  "+x.text()+"  ");
    require(untouched.previewUpdate() && untouched.submit(),"untouched numeric accepts");
    require(untouched.candidate()->command.entity_changes.empty(),"unchanged exact double does not create history");
    SitePlacementDialog clear(source,"a",true);mode(clear,"default");require(clear.previewUpdate() && clear.submit(),"building clear");
    require(impact(*clear.candidate(),"a-outline").after.source_frame.mode==SiteFrameMode::world,"clear returns to legacy world");
    SitePlacementDialog assembly(source,"assembly",false);mode(assembly,"building");
    require(assembly.previewUpdate() && assembly.submit(),"explicit independent assembly enrollment");
    require(impact(*assembly.candidate(),"assembly").after.source_frame.building_id=="b","assembly own layer context");
    require(assembly.candidate()->command.entity_changes.front().entity.properties.at("instance")==source.entities().at("assembly").properties.at("instance"),"assembly expansion source retained");
    SitePlacementDialog cancel(source,"a",true);text(cancel,"sitePlacementX","5 ft 6 in");require(cancel.previewUpdate(),"mixed unit preview");
    widget<QDialogButtonBox>(cancel,"sitePlacementButtons").button(QDialogButtonBox::Cancel)->click();
    require(!cancel.candidate() && document_snapshot_digest(document.snapshot())==digest,"Cancel retains entire source");
    SitePlacementDialog unknown(source,"assembly",true);auto& choice=widget<QComboBox>(unknown,"sitePlacementMode");
    choice.addItem("Unknown","future");choice.setCurrentIndex(choice.count()-1);
    require(!unknown.previewUpdate() && !unknown.candidate(),"unknown explicit frame refuses");
}
void explicit_annotation_children() {
    auto document=Document::create(entities());const auto source=document.snapshot();
    SitePlacementDialog notes(source,"notes",true);mode(notes,"building");require(notes.previewUpdate() && notes.submit(),"explicit annotation owner enrollment");
    const auto draft=*notes.candidate();const auto& edited=draft.command.entity_changes.front().entity;
    require(edited.properties.at("version")==3 && edited.properties.at("state")==source.entities().at("notes").properties.at("state"),"strict v3 with unchanged child state");
    require(impact(draft,"notes","note-a").after.source_frame.building_id=="a" && impact(draft,"notes","note-b").after.source_frame.building_id=="b","each child own context");
    const auto detached=Document::preview_command(source,draft.command);SitePlacementDialog remove(detached,"notes",true);mode(remove,"default");
    require(remove.previewUpdate() && remove.submit(),"explicit annotation clear");
    const auto removed=*remove.candidate();const auto& restored=removed.command.entity_changes.front().entity;
    require(restored.properties.at("version")==2 && !restored.properties.contains("presentation_frame") && restored.properties.at("layer_id")=="a-layer","clear keeps full scoped owner");
    auto plain=entities();for(auto& item:plain)if(item.id=="notes")item=make_annotation_entity("notes",decode_annotation_entity(item));
    auto unscoped_document=Document::create(plain);SitePlacementDialog enroll(unscoped_document.snapshot(),"notes",true);mode(enroll,"building");
    require(enroll.previewUpdate() && enroll.submit(),"unscoped owner enrolls only from each explicit child layer");
    const auto enrolled=*enroll.candidate();const auto unscoped_preview=Document::preview_command(unscoped_document.snapshot(),enrolled.command);
    SitePlacementDialog unscoped_remove(unscoped_preview,"notes",true);mode(unscoped_remove,"default");
    require(unscoped_remove.previewUpdate() && unscoped_remove.submit(),"unscoped explicit frame removal");
    const auto unscoped_removed=*unscoped_remove.candidate();require(unscoped_removed.command.entity_changes.front().entity.properties.at("version")==1,"removal downgrades unscoped v3 to v1");
    auto missing=entities();for(auto& item:missing)if(item.id=="notes") {
        auto state=decode_annotation_entity(item);state.labels[1].placement.layer_id.clear();item=make_annotation_entity("notes",state);
    }
    auto legacy=Document::create(missing);SitePlacementDialog refused(legacy.snapshot(),"notes",true);mode(refused,"site");
    require(!refused.previewUpdate(),"child without explicit layer cannot enroll");
    SitePlacementDialog unscoped(legacy.snapshot(),"notes",true);mode(unscoped,"world");
    // World is still explicit and therefore requires valid child layer IDs.
    require(!unscoped.previewUpdate(),"explicit world never hides malformed child scope");
}
void spatial_capabilities_and_captured_hosts() {
    auto e=entities();
    e.push_back({"host-wall","wall",{{"name","Captured house wall"},{"layer_id","a-layer"}}});
    e.push_back({"door","opening",{{"wall_id","host-wall"},{"presentation_frame",{{"version",1},{"mode","building"}}}}});
    e.push_back({"host-stair","stair",{{"name","Captured guest stair"},{"layer_id","b-layer"}}});
    e.push_back({"rail","railing",{{"host",{{"stair_id","host-stair"}}}}});
    auto document=Document::create(e);const auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
    for(const auto* type:{"assembly_model","model_phases","sheet_view_model","room_relationships","vertical_levels","georeferencing","dxf_source","ifc_source","sheet","view","label","floor","layer","constraint","wall_join","roof_join","future_owner"}) {
        Entity unsupported{"unsupported",type,{{"opaque","retained"}}};
        require(!SitePlacementDialog::supportsTarget(unsupported),"nonspatial metadata is not an editable spatial owner");
    }
    bool refused=false;try {SitePlacementDialog catalog(source,"catalog",true);}catch(const std::invalid_argument&) {refused=true;}
    require(refused,"catalog cannot create a fake placement preview");
    for(const auto* id:{"door","rail"}) {
        SitePlacementDialog dialog(source,id,true);auto& choices=widget<QComboBox>(dialog,"sitePlacementMode");
        require(choices.count()==2 && choices.findData("default")>=0 && choices.findData("building")>=0 && choices.findData("world")<0 && choices.findData("site")<0,"host choices only clear or captured mode");
        const auto help=widget<QLabel>(dialog,"sitePlacementExplanation").text();
        require(help.contains(id==std::string("door")?"Captured house wall":"Captured guest stair") && help.contains(id==std::string("door")?"Main house":"Guest house"),"readable captured physical host and full frame");
        mode(dialog,"building");require(dialog.previewUpdate() && dialog.submit(),"captured host mode validates");
        if(id==std::string("door"))require(dialog.candidate()->command.entity_changes.empty(),"existing canonical host frame stays a no-op");
        SitePlacementDialog cancel(source,id,true);mode(cancel,"default");require(cancel.previewUpdate(),"host clearing preview");cancel.reject();require(!cancel.candidate(),"host Cancel has no candidate");
    }
    SitePlacementDialog boundary_dialog(source,"a-outline",true);mode(boundary_dialog,"building");
    require(boundary_dialog.previewUpdate() && boundary_dialog.submit(),"known spatial owner supports actual placement");
    for(const auto& item:boundary_dialog.candidate()->impacts)require(item.owner_entity_id!="catalog","catalog is absent from spatial preview");
    require(document_snapshot_digest(document.snapshot())==digest,"unsupported, host and spatial drafts preserve source and history");
}
void impact_limit_refuses_atomically() {
    auto e=entities();for(std::size_t i=0;i<4097;++i)e.push_back({"floor-extra-"+std::to_string(i),"floor",{{"building_id","a"}}});
    auto document=Document::create(e);const auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
    SitePlacementDialog dialog(source,"a",true);text(dialog,"sitePlacementZ","5 m");
    require(!dialog.previewUpdate() && !dialog.candidate() && !dialog.lastError().isEmpty(),"oversized impact preview is an atomic refusal");
    require(document_snapshot_digest(document.snapshot())==digest,"impact limit leaves source intact");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);
    try {property_and_detached_guard();terrain_datum_and_failures();precision_clear_cancel_and_assembly();explicit_annotation_children();spatial_capabilities_and_captured_hosts();impact_limit_refuses_atomically();return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
