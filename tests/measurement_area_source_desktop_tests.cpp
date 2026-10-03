#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <sqlite3.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
Entity stroke(const std::string& id,const std::vector<Vec2>& points,bool closed=false){
    MeasurementLinework model;model.stroke_id=id;model.anchor=points.front();model.closed=closed;
    for(std::size_t i=1;i<points.size();++i){
        ConstructionReceipt receipt;receipt.segment_id=id+":e"+std::to_string(i);
        receipt.kind=BoundaryConstructionKind::line_to_point;receipt.start=points[i-1];receipt.chord_end=points[i];
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(i-1),
            closed&&i+1==points.size()?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return{id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},
        {"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
}
std::shared_ptr<Document> fixture(){return std::make_shared<Document>(Document::create({
    {"p","property",{{"name","Measured source review"},{"calculation_workflow","appraisal"},{"appraisal_policy",{
        {"policy_kind","residential_declared"},{"version",1},{"property_kind","detached_single_family"},{"measurement_basis","exterior"}}}},false},
    {"b","building",{{"property_id","p"}},false},
    {"f","floor",{{"building_id","b"},{"appraisal_facts",{{"grade","above"}}}},false},
    {"l","layer",{{"floor_id","f"}},false},
    stroke("outline",{{0,0},{4,0},{4,4},{0,4},{0,0}},true),stroke("divider",{{2,-1},{2,5}})}));}
template<class T>T& widget(QObject& parent,const char* name){auto* found=dynamic_cast<T*>(parent.findChild<QWidget*>(name));require(found,"native control exists");return *found;}
struct Areas{QString left,right;};
Areas define(MainWindow& window){
    require(window.selectEntity("outline"),"select actual measured stroke");
    const auto ids=window.detectRoomBoundariesFromExistingWalls("living");require(ids.size()==2,"define actual source faces");
    const auto source=window.document().snapshot();Areas areas;std::vector<EntityChange> changes;
    for(const auto& id:ids){auto area=source.entities().at(id.toStdString());
        const bool left=boundary_bounds(boundary_geometry(decode_identified_boundary_entity(area))).minimum.x==0;
        (left?areas.left:areas.right)=id;area.properties["name"]=left?"Dwelling":"Garage";
        area.properties["vendor_metadata"]={{"retain",17}};
        area.properties["appraisal_facts"]={{"finish","finished"},{"access","direct_interior"},{"ceiling_eligibility","standard"},
            {"area_use",left?"dwelling":"garage"},{"boundary_role","measured_area"}};changes.push_back(EntityChange::upsert(area));}
    window.document().apply(ApplyEntityChanges{source.revision(),changes,{},"Declare area facts"});return areas;
}
void split(MainWindow& window){window.document().apply(ApplyEntityChanges{window.document().revision(),
    {EntityChange::upsert(stroke("cross-divider",{{-1,2},{2,2}}))},{},"Split measured faces"});}
void capture(MainWindow& window,const char* filename){const auto path=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(path.isEmpty())return;
    require(QDir().mkpath(path),"create capture directory");require(window.grab().save(QDir(path).filePath(filename)),"save native capture");}
int face_at(PlanCanvas& preview,double x,double y){
    for(const auto& entity:preview.entities())if(entity.id=="proposed-area"){
        const auto bounds=boundary_bounds(entity.segments);
        if(x>bounds.minimum.x&&x<bounds.maximum.x&&y>bounds.minimum.y&&y<bounds.maximum.y)return 1;
    }return 0;
}
void review(MainWindow& window,double x,double y,bool apply,bool expect_reject=false){
    std::exception_ptr failure;bool opened=false;
    QTimer::singleShot(0,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        try{require(dialog&&dialog->objectName()=="measuredAreaSourceReview","actual source review dialog opens");opened=true;
            auto& faces=widget<QComboBox>(*dialog,"measuredAreaSourceFace");auto& preview=widget<PlanCanvas>(*dialog,"measuredAreaSourcePreview");
            int chosen=-1;for(int i=1;i<faces.count();++i){faces.setCurrentIndex(i);if(face_at(preview,x,y)){chosen=i;break;}}
            require(chosen>0,"choose geometric face by actual native preview");
            auto& buttons=widget<QDialogButtonBox>(*dialog,"measuredAreaSourceButtons");
            if(expect_reject){require(!buttons.button(QDialogButtonBox::Apply)->isEnabled()&&
                widget<QLabel>(*dialog,"measuredAreaSourceStatus").text().contains("already assigned"),"occupied source face rejects before Apply");dialog->reject();return;}
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"actual candidate is actionable");
            require(widget<QLabel>(*dialog,"measuredAreaSourceStatus").text().contains("GLA"),"candidate calculation status is visible before admission");
            QCoreApplication::processEvents();dialog->layout()->activate();
            const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!directory.isEmpty()){
                QDir().mkpath(directory);require(dialog->grab().save(QDir(directory).filePath(apply?"measured-area-source-preview.png":"measured-area-source-cancel.png")),"save source review capture");}
            if(apply){buttons.button(QDialogButtonBox::Apply)->click();require(dialog->result()==QDialog::Accepted,"valid candidate is accepted without leaving a stalled modal");}else dialog->reject();
        }catch(...){failure=std::current_exception();if(dialog)dialog->reject();}});
    window.showMeasuredAreaSourceReview();if(failure)std::rethrow_exception(failure);require(opened,"modal review exercised");
}
void native_review_and_storage(){
    MainWindow window(fixture());window.setAttribute(Qt::WA_DontShowOnScreen,true);window.setMetricUnits(true);
    window.resize(1400,900);window.show();QCoreApplication::processEvents();const auto areas=define(window);
    require(build_appraisal_document_report(window.document().snapshot(),"p").qualified,"declared original areas qualified");split(window);
    require(window.selectEntity(areas.left),"select stale retained dwelling");const auto before=window.document().snapshot();
    auto& details=widget<AppraisalDetailsPanel>(window,"appraisalDetailsPanel");details.setSelectedBoundary(areas.left);
    require(widget<QPushButton>(details,"appraisalDetailsReviewSources").isEnabled(),"Details has measured-source repair action");
    review(window,1,1,false);require(window.document().revision()==before.revision()&&window.document().snapshot().entities()==before.entities(),"Cancel is immutable");
    review(window,1,1,true);const auto left_repaired=window.document().snapshot();
    require(left_repaired.revision()==before.revision()+1&&measurement_linework_source_checks(left_repaired.entities()).at(areas.left.toStdString()).current,"native Apply admits one exact current face");
    require(left_repaired.entities().at(areas.left.toStdString()).properties.at("appraisal_facts")==before.entities().at(areas.left.toStdString()).properties.at("appraisal_facts"),"native source review preserves facts");
    require(window.undoCommand()&&window.document().snapshot().entities()==before.entities(),"one Undo restores stale state");
    require(ProjectStore::required_format_version(window.document().snapshot())==30,"undone source review remains protected in retained history");
    require(window.redoCommand()&&window.document().snapshot().entities()==left_repaired.entities(),"Redo restores reviewed state");
    require(window.selectEntity(areas.right),"select retained garage with newly split shared edge");review(window,3,1,true);
    const auto repaired=window.document().snapshot();const auto report=build_appraisal_document_report(repaired,"p");
    require(report.qualified&&std::abs(report.calculation->property.gla().total.square_metres-4)<1e-10,"native review recalculates dwelling GLA, excludes garage and leaves unassigned face out");
    review(window,1,1,false,true);require(window.document().snapshot().entities()==repaired.entities(),"duplicate source assignment leaves document unchanged");
    require(ProjectStore::required_format_version(repaired)==30,"typed source replacement requires native format thirty");
    QTemporaryDir temp;const auto filename=temp.filePath("reviewed.bldproj");require(window.saveProjectAs(filename)&&window.openProject(filename),"reviewed project saves and reopens");
    require(document_authoring_source_digest_v1(window.document().snapshot())==document_authoring_source_digest_v1(repaired),"save/reopen preserves all source proofs and retained history");
    const auto exchange=std::filesystem::path(temp.path().toStdString())/"extract";extract_project(window.document().snapshot(),exchange);
    std::ifstream manifest(exchange/"project.json");require(nlohmann::json::parse(manifest).at("exchange_version")==28,"exchange advertises source-review reader");
    auto imported=Document::create([&]{std::vector<Entity> values;for(const auto& [id,entity]:repaired.entities())values.push_back(entity);return values;}());
    require(ProjectStore::required_format_version(imported.snapshot())==30,"imported derivation requires same floor without command history");
    sqlite3* database=nullptr;const auto path=filename.toStdString();require(sqlite3_open(path.c_str(),&database)==SQLITE_OK,"open controlled downgraded native fixture");
    require(sqlite3_exec(database,"PRAGMA user_version=29; UPDATE metadata SET value='29' WHERE key='format_version'",nullptr,nullptr,nullptr)==SQLITE_OK,"lower controlled format markers");sqlite3_close(database);
    std::ifstream bytes_before(path,std::ios::binary);const std::string retained((std::istreambuf_iterator<char>(bytes_before)),{});
    bool rejected=false;try{(void)ProjectStore::load(path);}catch(const StorageError& error){rejected=error.code()==StorageErrorCode::unsupported_format;}
    require(rejected,"lowered format rejected before claiming editable compatibility");
    std::ifstream bytes_after(path,std::ios::binary);require(retained==std::string((std::istreambuf_iterator<char>(bytes_after)),{}),"read rejection preserves original bytes");
    require(window.selectEntity(areas.left),"select repaired area for Details capture");
    widget<QTabWidget>(window,"sidebarTabs").setCurrentIndex(2);capture(window,"measured-area-source-details.png");
}
void stale_review_cannot_apply(){
    MainWindow window(fixture());window.setAttribute(Qt::WA_DontShowOnScreen,true);const auto areas=define(window);split(window);
    require(window.selectEntity(areas.left),"select stale source area");const auto before=window.document().snapshot();std::exception_ptr failure;
    QTimer::singleShot(0,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());try{
        require(dialog&&dialog->objectName()=="measuredAreaSourceReview","stale review dialog opens");auto& faces=widget<QComboBox>(*dialog,"measuredAreaSourceFace");faces.setCurrentIndex(1);
        window.setMetricUnits(!window.metricUnits());widget<QDialogButtonBox>(*dialog,"measuredAreaSourceButtons").button(QDialogButtonBox::Apply)->click();
        require(!widget<QDialogButtonBox>(*dialog,"measuredAreaSourceButtons").button(QDialogButtonBox::Apply)->isEnabled(),"stale modal context disables Apply");dialog->reject();
    }catch(...){failure=std::current_exception();if(dialog)dialog->reject();}});
    window.showMeasuredAreaSourceReview();if(failure)std::rethrow_exception(failure);
    require(window.document().snapshot().entities()==before.entities()&&window.document().revision()==before.revision(),"stale source proposal never changes persisted state");
}
void phase_only_storage_floor(){
    auto document=fixture();require(ProjectStore::required_format_version(document->snapshot())==1,"ordinary v1 strokes retain old format floor");
    const auto phases=ModelPhases::create({"outline","divider"},{"outline"},{{"future","Future",{}, {"divider"}}});
    document->apply(ApplyEntityChanges{document->revision(),{EntityChange::upsert({"phases","model_phases",{{"model",phases.to_json()}},false})},{},"Assign measured strokes to phases"});
    require(ProjectStore::required_format_version(document->snapshot())==30,"measured stroke phase membership requires current reader without any source review");
    QTemporaryDir temp;const auto path=std::filesystem::path(temp.path().toStdString())/"phase-only.bldproj";
    (void)ProjectStore::save(path,document->snapshot());require(ProjectStore::load(path).document.snapshot().entities()==document->snapshot().entities(),"phase-only measured strokes save and reopen");
    document->undo(document->revision());require(ProjectStore::required_format_version(document->snapshot())==30,"undone measured phase membership stays protected");
    auto ordinary=fixture();ordinary->apply(ApplyEntityChanges{ordinary->revision(),{EntityChange::upsert({"vendor","custom",{{"model",phases.to_json()}},false})},{},"Vendor model"});
    require(ProjectStore::required_format_version(ordinary->snapshot())==1,"generic phase-model collision remains opaque");
}
void referenced_area_review(){
    MainWindow window(fixture());window.setAttribute(Qt::WA_DontShowOnScreen,true);const auto areas=define(window);
    auto source=window.document().snapshot();const auto boundary=decode_identified_boundary_entity(source.entities().at(areas.left.toStdString()));
    BoundaryDimension dimension;dimension.id="manual";dimension.boundary_id=boundary.id;dimension.segment_id=boundary.segments.front().segment_id;
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(encode_boundary_dimension_entity(dimension))},{},"Attach manual dimension"});
    split(window);require(window.selectEntity(areas.left),"select referenced stale area");const auto before=window.document().snapshot();
    for(const bool accept:{false,true}){
        std::exception_ptr failure;bool reviewed=false;
        QTimer::singleShot(0,[&]{auto* outer=qobject_cast<QDialog*>(QApplication::activeModalWidget());try{
            require(outer&&outer->objectName()=="measuredAreaSourceReview","referenced source dialog opens");
            auto& faces=widget<QComboBox>(*outer,"measuredAreaSourceFace");auto& preview=widget<PlanCanvas>(*outer,"measuredAreaSourcePreview");
            for(int i=1;i<faces.count();++i){faces.setCurrentIndex(i);if(face_at(preview,1,1))break;}
            auto* choose=widget<QDialogButtonBox>(*outer,"measuredAreaSourceButtons").button(QDialogButtonBox::Apply);
            require(choose->isEnabled()&&choose->text().contains("references"),"dependent reference choice is explicit");
            QTimer::singleShot(0,[&]{auto* inner=qobject_cast<QDialog*>(QApplication::activeModalWidget());try{
                require(inner&&inner->objectName()=="boundaryReferenceReview","existing native reference dialog opens");reviewed=true;
                auto& choices=widget<QTableWidget>(*inner,"boundaryReferenceChoices");require(choices.rowCount()==1,"one actual manual reference is reviewed");
                auto* decision=dynamic_cast<QComboBox*>(choices.cellWidget(0,1));require(decision,"reference decision control exists");decision->setCurrentIndex(2);
                require(widget<QLabel>(*inner,"boundaryReferenceStatus").text().contains("GLA"),"final calculation or withheld state is shown before reference Apply");
                require(window.document().snapshot().entities()==before.entities(),"reference preview is immutable");
                if(accept)widget<QDialogButtonBox>(*inner,"boundaryReferenceButtons").button(QDialogButtonBox::Apply)->click();else inner->reject();
            }catch(...){failure=std::current_exception();if(inner)inner->reject();}});
            choose->click();if(!accept||failure)outer->reject();else require(outer->result()==QDialog::Accepted,"reviewed references and face commit together");
        }catch(...){failure=std::current_exception();if(outer)outer->reject();}});
        window.showMeasuredAreaSourceReview();if(failure)std::rethrow_exception(failure);require(reviewed,"native reference workflow exercised");
        if(!accept)require(window.document().snapshot().entities()==before.entities(),"cancel reference review leaves original dimension and owner untouched");
    }
    require(window.document().revision()==before.revision()+1&&!window.document().snapshot().entities().contains("manual"),"only explicitly removed reference is retired in one source-review revision");
}
}
int main(int argc,char** argv){sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-source-review-"+QUuid::createUuid().toString());
    try{require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"load bundled font");
        native_review_and_storage();stale_review_cannot_apply();phase_only_storage_floor();referenced_area_review();std::cout<<"measured area source desktop tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
