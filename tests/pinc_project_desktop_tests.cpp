#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/pinc_symbol_counterparts.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPdfDocument>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json=nlohmann::json;
std::string current_stage="startup";
std::string modal_failure;
void progress(const std::string& stage) {
    current_stage=stage;std::cout<<"[pinc-desktop] "<<stage<<std::endl;
    if(!modal_failure.empty())throw std::runtime_error(modal_failure);
}
void require(bool value,const char* message){
    if(!modal_failure.empty())throw std::runtime_error(modal_failure);
    if(!value)throw std::runtime_error(message);
}
class UnexpectedModalDiagnostics {
public:
    UnexpectedModalDiagnostics() {
        poll_.setInterval(100);
        QObject::connect(&poll_,&QTimer::timeout,[this]{
            auto* modal=QApplication::activeModalWidget();
            if(!modal)for(auto* candidate:QApplication::topLevelWidgets())if(candidate->isVisible()&&candidate->windowModality()!=Qt::NonModal){modal=candidate;break;}
            if(!modal||modal->objectName()=="pincImportProgress"||modal->objectName()=="pincImportReview") {
                unexpected_.clear();return;
            }
            if(unexpected_!=modal){unexpected_=modal;deadline_.start();return;}
            if(deadline_.elapsed()<1000)return;
            QStringList texts;
            for(auto* label:modal->findChildren<QLabel*>())if(!label->text().isEmpty())texts.push_back(label->text());
            for(auto* text:modal->findChildren<QPlainTextEdit*>())if(!text->toPlainText().isEmpty())texts.push_back(text->toPlainText());
            const auto description=QString("Unexpected modal during %1: class=%2 object=%3 title=%4 text=%5")
                .arg(QString::fromStdString(current_stage),QString::fromLatin1(modal->metaObject()->className()),modal->objectName(),
                    modal->windowTitle(),texts.join(" | ").left(8192));
            modal_failure=description.toStdString();std::cerr<<"[pinc-desktop] "<<modal_failure<<std::endl;poll_.stop();
            // Reject this fixture-owned unexpected dialog; never approve a
            // dirty transition or silently advance a blocked workflow.
            if(auto* dialog=qobject_cast<QDialog*>(modal))dialog->reject();
        });
        poll_.start();
    }
private:
    QTimer poll_;QPointer<QWidget> unexpected_;QElapsedTimer deadline_;
};
void close(double a,double b,const char* message){require(std::isfinite(a)&&std::abs(a-b)<1e-9,message);}
template<class T>T& widget(QObject& parent,const char* name) {
    auto* found=dynamic_cast<T*>(parent.findChild<QWidget*>(QString::fromLatin1(name)));
    require(found,"Expected real native control is missing");return *found;
}
QByteArray read(const QString& path) {
    QFile input(path);require(input.open(QIODevice::ReadOnly),"Read test artifact");return input.readAll();
}
void write(const QString& path,const QByteArray& bytes) {
    QFile output(path);require(output.open(QIODevice::WriteOnly),"Create source fixture");
    require(output.write(bytes)==bytes.size(),"Write complete source fixture");output.close();
}
void import_without_review(MainWindow& window,const QString& path) {
    progress("import without review: begin "+path.toStdString());
    const auto ok=window.importPinc(path,false);
    progress(std::string("import without review: returned ")+(ok?"true":"false")+" error="+window.lastError().toStdString());
    require(ok,qPrintable(window.lastError()));
}
Json edge(std::string id,double ax,double ay,double bx,double by) {
    return {{"id",id},{"kind","line"},{"a",{{"x",ax},{"y",ay}}},{"b",{{"x",bx},{"y",by}}},
        {"color","#174f90"},{"weight",3},{"lineType","dashdot"},{"dimSize",.9},{"dimFont","Arial"},
        {"dimColor","#ae2525"},{"dimOffset",{{"x",0},{"y",-.6}}}};
}
QByteArray fixture_bytes() {
    const auto page=[](const char* name,const char* note,bool ghost,bool guide) {
        return Json{{"name",name},{"calcWalls",Json::array({edge("a",0,0,8,0),edge("b",8,0,8,8),
            edge("c",8,8,0,8),edge("d",0,8,0,0)})},
            {"interiorWalls",Json::array({edge("inside",2,2,6,2)})},
            {"assignments",{{"a|b|c|d",{{"code","GLA1"},{"name",std::string(name)=="Upper" ? " living Area " : std::string(name)+" room"},
                {"color","#7dc79b"},{"opacity",.37},{"hatch","diagonal"},{"labelColor","#233851"},
                {"nameSize",.8},{"calcSize",.65},{"namePos",{{"x",4},{"y",4}}},
                {"calcPos",{{"x",4},{"y",5.5}}},{"boundaryLineType","dot"},{"boundaryWeight",2.5},{"_area",999}}}}},
            {"symbols",Json::array({{{"id","door"},{"kind","Door - Interior"},{"x",4},{"y",2},
                {"w",3},{"h",2},{"rot",30},{"mirrorX",true},{"mirrorY",true},
                {"wallRef",{{"type","interior"},{"id","inside"},{"t",.5}}}}})},
            {"texts",Json::array({{{"id","note"},{"text",note},{"x",1},{"y",6},{"size",.6},
                {"align","left"},{"color","#8b2255"},{"font","Inter"},{"bold",true},{"italic",true}}})},
            {"ghostPrevious",ghost},{"showPrintGuide",guide}};
    };
    const Json project{{"format","PincSketch"},{"version","4.2"},{"currentPage",0},
        {"pages",{page("Ground","Ground portable note",false,true),page("Upper","Upper portable note",true,false)}}};
    return QByteArray(" \n")+QByteArray::fromStdString(project.dump())+QByteArray("\n ");
}
void configure(MainWindow& window) {
    window.setMetricUnits(false);
    progress("configure native window: begin");
    window.setAttribute(Qt::WA_DontShowOnScreen,true);window.resize(1400,900);window.show();
    auto& canvas=widget<PlanCanvas>(window,"measurementPlanCanvas");canvas.setGridEnabled(false);canvas.setOverviewMapEnabled(false);
    QCoreApplication::processEvents();
    progress("configure native window: settled");
}
void settle(const std::function<bool()>& ready,const char* message) {
    QElapsedTimer deadline;deadline.start();
    while(!ready()&&deadline.elapsed()<5000)QCoreApplication::processEvents(QEventLoop::AllEvents,20);
    require(ready(),message);
}
std::vector<std::byte> bytes(const QByteArray& value) {
    const auto* data=reinterpret_cast<const std::byte*>(value.constData());return {data,data+value.size()};
}
const Entity& page_registry(const DocumentSnapshot& snapshot) {
    const auto found=std::find_if(snapshot.entities().begin(),snapshot.entities().end(),[](const auto& pair){
        return pair.second.type==kSheetViewEntityType&&pair.second.extensions.contains("pinc_import");});
    require(found!=snapshot.entities().end(),"Imported ordered page registry exists");return found->second;
}
const Entity& area_in_layer(const DocumentSnapshot& snapshot,const std::string& layer) {
    const auto found=std::find_if(snapshot.entities().begin(),snapshot.entities().end(),[&](const auto& pair){
        return pair.second.type=="measurement_boundary"&&pair.second.properties.value("layer_id",std::string{})==layer;});
    require(found!=snapshot.entities().end(),"Imported page has an actual measured area");return found->second;
}
const CanvasEntity& canvas_entity(const PlanCanvas& canvas,const QString& id) {
    const auto found=std::find_if(canvas.entities().begin(),canvas.entities().end(),[&](const auto& entity){return entity.id==id;});
    require(found!=canvas.entities().end(),"Actual native geometry is projected to canvas");return *found;
}
const CanvasLabel& label(const PlanCanvas& canvas,const QString& id,const QString& role) {
    const auto found=std::find_if(canvas.labels().begin(),canvas.labels().end(),[&](const auto& item){return item.id==id&&item.callout_role==role;});
    require(found!=canvas.labels().end(),"Actual live callout is projected to canvas");return *found;
}
QImage rendered(PlanCanvas& canvas) {
    QImage image(900,650,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::white);QPainter painter(&image);
    painter.setFont(canvas.font());canvas.renderScene(painter,image.rect(),true,Qt::white);
    require(painter.end(),"Native output painter completes");return image;
}
std::size_t ink_pixels(const QImage& image) {
    std::size_t result=0;
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x) {
        const auto color=image.pixelColor(x,y);
        if(color.alpha()>0&&std::min({color.red(),color.green(),color.blue()})<230)++result;
    }
    return result;
}
void capture(MainWindow& window,const char* name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(directory.isEmpty())return;
    require(QDir().mkpath(directory),"Create native capture directory");
    require(window.grab().save(QDir(directory).filePath(QString::fromLatin1(name))),"Save actual full-feature native screenshot");
}
void assert_original_asset(const DocumentSnapshot& snapshot,const QByteArray& original) {
    std::size_t count=0;
    for(const auto& [id,asset]:snapshot.assets())if(asset.media_type=="application/x-pincsketch") {
        (void)id;++count;require(asset.bytes==bytes(original)&&asset.sha256==sha256_hex(asset.bytes),"Retained source bytes/hash remain exact");
    }
    require(count==1,"Original source retained exactly once");
}
void imported_canvas_pages_persistence_and_live_edit() {
    progress("full-feature case: create source and native window");
    QTemporaryDir temporary;require(temporary.isValid(),"Create isolated desktop fixture directory");
    const auto source_path=temporary.filePath("original.pinc");const auto original=fixture_bytes();write(source_path,original);
    MainWindow window;configure(window);const auto previous_id=window.document().snapshot().document_id();
    import_without_review(window,source_path);
    const auto imported=window.document().snapshot();assert_original_asset(imported,original);
    require(imported.document_id()!=previous_id&&!imported.saved_revision_optional()&&imported.dirty(),"Import creates independent unsaved project");
    require(read(source_path)==original,"Import changed original source file");
    progress("full-feature case: validate imported canvas and presentation");
    const auto& registry=page_registry(imported);const auto& pages=registry.extensions.at("pinc_import").at("pages");
    require(pages.size()==2,"Both source page occurrences retained");const auto sheets=decode_sheet_view_entity(registry);
    require(sheets.sheet_order().size()==2&&sheets.sheet_order()[0]==pages[0].at("sheet_id").get<std::string>()&&
        sheets.sheet_order()[1]==pages[1].at("sheet_id").get<std::string>(),"Source page order reaches actual drawing-set model");
    const auto layer0=pages[0].at("calculation_layer_id").get<std::string>();const auto layer1=pages[1].at("calculation_layer_id").get<std::string>();
    const auto& area0=area_in_layer(imported,layer0);const auto& area1=area_in_layer(imported,layer1);
    const auto area0_id=QString::fromStdString(area0.id);const auto area1_id=QString::fromStdString(area1.id);
    close(signed_area(boundary_geometry(decode_identified_boundary_entity(area0))),5.94579456,"Cached source area must not reach native quantity");
    auto& canvas=widget<PlanCanvas>(window,"measurementPlanCanvas");auto& page_combo=widget<QComboBox>(window,"pincProjectPage");
    settle([&]{return page_combo.count()==2&&!canvas.entities().empty();},"Native import page controls and canvas settle");
    require(window.activeLayerId()==QString::fromStdString(layer0)&&window.entityVisible(area0_id)&&!window.entityVisible(area1_id),"Initial page scopes real canvas visibility");
    const auto& appearance=canvas_entity(canvas,area0_id);
    require(appearance.filled&&appearance.hatch_pattern=="hatch"&&appearance.line_pattern=="dot"&&appearance.fill_opacity.has_value(),"Native area hatch/line/opacity presentation lost");
    close(*appearance.fill_opacity,.37,"Source fill opacity changed");
    std::cout<<"[pinc-desktop] imported callouts: name="<<label(canvas,area0_id,"area_name").text.toStdString()
        <<" calculation="<<label(canvas,area0_id,"area_calculation").text.toStdString()<<std::endl;
    require(label(canvas,area0_id,"area_name").text=="Ground room"&&label(canvas,area0_id,"area_calculation").text.contains("64"),"Source name and geometry-derived live area calculation render");
    window.setMetricUnits(true);
    require(label(canvas,area0_id,"area_calculation").text.contains("5.95"),"Imported geometric area converts to metric without trusting the source cached total");
    window.setMetricUnits(false);
    const auto note=std::find_if(canvas.labels().begin(),canvas.labels().end(),[](const auto& item){return item.text=="Ground portable note";});
    require(note!=canvas.labels().end()&&note->text_alignment=="left"&&note->bold&&note->italic,"Authored portable text alignment/style lost");
    close(note->text_height_metres,.6*.3048,"Source text model height changed");
    const auto symbol=std::find_if(canvas.entities().begin(),canvas.entities().end(),[](const auto& entity){return entity.svg_symbol.has_value();});
    require(symbol!=canvas.entities().end()&&!symbol->svg_symbol->document.isEmpty()&&symbol->svg_symbol->flip_horizontal&&symbol->svg_symbol->flip_vertical,"Imported pinned symbol or source flips lost");
    close(symbol->svg_symbol->width_metres,3*.3048,"Symbol world width changed");close(symbol->svg_symbol->depth_metres,2*.3048,"Symbol world depth changed");
    close(symbol->svg_symbol->rotation_radians,-std::acos(-1.)/6,"Symbol source rotation changed");
    std::string editable_stroke,editable_segment;
    for(const auto& [id,entity]:imported.entities())if(entity.type=="measurement_linework"&&entity.properties.value("layer_id",std::string{})==layer0) {
        const auto model=decode_measurement_linework_model(entity.properties.at("model"));const auto replay=replay_measurement_linework(*model.model);
        if(replay.edges[0].segment.start.y==0&&replay.edges[0].segment.end.y==0) {editable_stroke=id;editable_segment=replay.edges[0].segment_id;break;}
    }
    require(!editable_stroke.empty(),"Find original analytical calculation stroke");
    const auto stroke_id=QString::fromStdString(editable_stroke);const auto dimension_before=label(canvas,stroke_id,{}).text;
    close(label(canvas,stroke_id,{}).text_height_metres,.9*.3048,"Imported dimension must retain model height");
    require(label(canvas,stroke_id,{}).paper_height_mm==0,"Imported dimension model height must not be replaced by paper sizing");
    require(canvas_entity(canvas,stroke_id).line_pattern=="dashdot","Source measured stroke line pattern lost");
    require(canvas.sketchCompositionGuideEnabled()&&canvas.floorGhostEntities().empty(),"First-page screen guide/ghost flags lost");
    progress("full-feature case: capture light and dark native workspace");
    window.setWorkspaceTheme(WorkspaceTheme::light);QCoreApplication::processEvents();const auto light=window.grab().toImage();capture(window,"pinc-import-light.png");
    window.setWorkspaceTheme(WorkspaceTheme::dark);QCoreApplication::processEvents();const auto dark=window.grab().toImage();capture(window,"pinc-import-dark.png");
    require(!light.isNull()&&!dark.isNull()&&light!=dark,"Native light and dark screenshots must differ");
    const auto first_output=rendered(canvas);page_combo.setCurrentIndex(1);
    progress("full-feature case: switch page and inspect ghost/output isolation");
    settle([&]{return window.activeLayerId()==QString::fromStdString(layer1)&&window.entityVisible(area1_id)&&!window.entityVisible(area0_id);},"Page selection prevents source-page geometry bleed");
    require(!canvas.sketchCompositionGuideEnabled()&&!canvas.floorGhostEntities().empty(),"Previous page ghost is screen-only and source guide flag follows page");
    const auto with_ghost=rendered(canvas);const auto ghosts=canvas.floorGhostEntities();const auto ghost_labels=canvas.floorGhostLabels();
    canvas.clearFloorGhost();require(rendered(canvas)==with_ghost,"Screen-only previous-page ghost leaked into printable scene");
    canvas.setFloorGhost(ghosts,.25,{},ghost_labels);
    require(std::none_of(canvas.labels().begin(),canvas.labels().end(),[](const auto& item){return item.text=="Ground portable note";}),"Hidden source-page text leaked into current page");
    require(std::any_of(canvas.labels().begin(),canvas.labels().end(),[](const auto& item){return item.text=="Upper portable note";}),"Second source page did not display its text");
    require(label(canvas,area1_id,"area_name").text==" living Area ","Authored generic-token name retains exact capitalization and spacing");
    require(ink_pixels(first_output)>1000&&ink_pixels(with_ghost)>1000,"Actual imported native output scenes must paint visible content");
    page_combo.setCurrentIndex(0);QCoreApplication::processEvents();
    progress("full-feature case: reject native Save As to source .pinc");
    require(!window.saveProjectAs(source_path)&&read(source_path)==original,"Native Save As must block overwriting the original .pinc source");
    const auto native_path=temporary.filePath("portable.bldproj");progress("full-feature case: native Save As begin");
    require(window.saveProjectAs(native_path),qPrintable(window.lastError()));progress("full-feature case: native Save As returned; reopen begin");
    require(window.openProject(native_path),qPrintable(window.lastError()));assert_original_asset(window.document().snapshot(),original);
    progress("full-feature case: native reopen returned");
    require(document_authoring_source_digest_v1(window.document().snapshot())==document_authoring_source_digest_v1(imported),"Native save/reopen changed retained styles, assets or authoring proofs");
    auto& reopened_canvas=widget<PlanCanvas>(window,"measurementPlanCanvas");widget<QComboBox>(window,"pincProjectPage").setCurrentIndex(0);QCoreApplication::processEvents();
    const auto before_edit=window.document().snapshot();const auto calculation_before=label(reopened_canvas,area0_id,"area_calculation").text;
    progress("full-feature case: connected imported stroke length edit begin");
    require(window.selectEntity(stroke_id)&&window.editSelectedBoundaryEdgeLength(QString::fromStdString(editable_segment),"10 ft",BoundaryFixedEndpoint::start,true),qPrintable(window.lastError()));
    const auto edited=window.document().snapshot();const auto model=decode_measurement_linework_model(edited.entities().at(editable_stroke).properties.at("model"));
    progress("full-feature case: connected length edit returned");
    close(segment_length(replay_measurement_linework(*model.model).edges[0].segment),10*.3048,"Imported stroke must remain editable with exact native quantities");
    require(edited.revision()==before_edit.revision()+1&&measurement_linework_source_checks(edited.entities()).at(area0.id).current,"Connected native edit must refresh imported source area once");
    close(signed_area(boundary_geometry(decode_identified_boundary_entity(edited.entities().at(area0.id)))),72*.09290304,
        "Connected imported corner edit changes the actual enclosed face instead of creating a dangling extension");
    std::cout<<"[pinc-desktop] edit quantities: dimension "<<dimension_before.toStdString()<<" -> "<<label(reopened_canvas,stroke_id,{}).text.toStdString()
        <<"; area "<<calculation_before.toStdString()<<" -> "<<label(reopened_canvas,area0_id,"area_calculation").text.toStdString()<<std::endl;
    require(label(reopened_canvas,stroke_id,{}).text!=dimension_before&&label(reopened_canvas,area0_id,"area_calculation").text!=calculation_before,"Live imported dimensions and area callouts did not recalculate");
    const auto edited_pdf_path=temporary.filePath("connected-edit.pdf");
    require(window.exportDrawingSetPdf(edited_pdf_path),qPrintable(window.lastError()));QPdfDocument edited_pdf;
    require(edited_pdf.load(edited_pdf_path)==QPdfDocument::Error::None && edited_pdf.pageCount()==2 &&
        edited_pdf.getAllText(0).text().contains("72.00") && !edited_pdf.getAllText(1).text().contains("72.00"),
        "Connected corner edit recalculates only its own page in actual PDF output");
    const auto edited_path=temporary.filePath("connected-edit.bldproj");
    require(window.saveProjectAs(edited_path) && window.openProject(edited_path),qPrintable(window.lastError()));
    require(label(reopened_canvas,area0_id,"area_calculation").text.contains("72.00") &&
        window.document().snapshot().entities()==edited.entities(),"Connected imported edit reopens with current native geometry and live quantity");
    progress("full-feature case: native edit Undo/Redo begin");
    require(window.undoCommand()&&window.document().snapshot().entities()==before_edit.entities(),"Native imported geometry edit does not undo exactly");
    require(window.redoCommand()&&window.document().snapshot().entities()==edited.entities(),"Native imported geometry edit does not redo exactly");
    require(window.undoCommand(),"Return to original imported content before import-history check");
    progress("full-feature case: one import Undo begin");
    require(window.undoCommand(),"One Undo removes complete imported content");
    const auto undone_import=window.document().snapshot();
    require(undone_import.assets().empty()&&std::none_of(undone_import.entities().begin(),undone_import.entities().end(),
        [](const auto& pair){return pair.second.type=="measurement_linework"||pair.second.type=="measurement_boundary";}),"Import content/assets were split across active history entries");
    require(window.redoCommand()&&window.document().snapshot().entities()==imported.entities(),"One Redo restores complete original import content");
    progress("full-feature case: import Undo/Redo returned");
    const auto before_add=window.document().snapshot();
    const auto carrier=std::find_if(before_add.entities().begin(),before_add.entities().end(),[&](const auto& pair){
        return pair.second.type==kAnnotationEntityType && pair.second.properties.value("layer_id",std::string{})==layer0;});
    require(carrier!=before_add.entities().end(),"Imported page retains its scoped annotation carrier");
    require(!window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",{1,1}).isEmpty(),qPrintable(window.lastError()));
    require(!window.createAnnotationLabel("note","NATIVE ADDITION ON GROUND PAGE",{0,-1}).isEmpty(),qPrintable(window.lastError()));
    const auto authored=window.document().snapshot();const auto& retained=authored.entities().at(carrier->first);
    for(const char* key:{"property_id","building_id","floor_id","layer_id"})
        require(retained.properties.at(key)==carrier->second.properties.at(key),"New symbol/label stripped imported page context");
    require(retained.extensions==carrier->second.extensions &&
        retained.properties.at("state").at("symbols").at(0)==carrier->second.properties.at("state").at("symbols").at(0),
        "New symbol/label rewrote source provenance or original artwork/visual-wall metadata");
    require(std::any_of(reopened_canvas.labels().begin(),reopened_canvas.labels().end(),[](const auto& value){
        return value.text=="NATIVE ADDITION ON GROUND PAGE";}),"New page-owned label remains on the native canvas");
    const auto authored_path=temporary.filePath("authored.bldproj");
    require(window.saveProjectAs(authored_path) && window.openProject(authored_path) &&
        document_authoring_source_digest_v1(window.document().snapshot())==document_authoring_source_digest_v1(authored),
        "Adding native symbols and labels survives actual native save/reopen");
    const auto draft_path=temporary.filePath("draft.png");const auto pdf_path=temporary.filePath("drawing-set.pdf");
    progress("full-feature case: draft PNG export begin");
    require(window.exportDraftImage(draft_path)&&!QImage(draft_path).isNull(),"Actual draft PNG export failed");
    progress("full-feature case: PNG returned; drawing set PDF export begin");
    require(window.exportDrawingSetPdf(pdf_path)&&read(pdf_path).startsWith("%PDF"),qPrintable(window.lastError()));
    progress("full-feature case: PDF export returned; decode pages");
    QPdfDocument pdf;require(pdf.load(pdf_path)==QPdfDocument::Error::None&&pdf.pageCount()==2,"Actual drawing set PDF must contain exactly two source pages");
    for(int page=0;page<2;++page)require(ink_pixels(pdf.render(page,QSize(600,800)))>1000,"Every actual PDF page must decode and paint visible content");
    for(int page=0;page<2;++page) {
        const auto bounds=pdf.getAllText(page).boundingRectangle();const auto size=pdf.pagePointSize(page);
        require(bounds.left()>=-1&&bounds.top()>=-1&&bounds.right()<=size.width()+1&&bounds.bottom()<=size.height()+1,
            "Actual portrait PDF text, including its title block, must stay inside the physical page");
    }
    const auto first_pdf_text=pdf.getAllText(0).text();const auto second_pdf_text=pdf.getAllText(1).text();
    std::cout<<"[pinc-desktop] first PDF text: "<<first_pdf_text.toStdString()<<"\n[pinc-desktop] second PDF text: "<<second_pdf_text.toStdString()<<std::endl;
    if(const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");!directory.isEmpty())
        write(QDir(directory).filePath("pinc-import-drawing-set.pdf"),read(pdf_path));
    require(first_pdf_text.contains("Ground portable note")&&!first_pdf_text.contains("Upper portable note")&&
        second_pdf_text.contains("Upper portable note")&&!second_pdf_text.contains("Ground portable note"),"Actual PDF output lost source page order or leaked another page's text");
    require(second_pdf_text.contains("living Area"),"Authored generic-token name remains visible in actual PDF output");
    require(first_pdf_text.contains("NATIVE ADDITION ON GROUND PAGE") && !second_pdf_text.contains("NATIVE ADDITION ON GROUND PAGE"),
        "New symbol then label retains page ownership in actual ordered PDF output");
    const auto capture_directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!capture_directory.isEmpty()) {
        require(QDir().mkpath(capture_directory),"Create output artifact capture directory");
        write(QDir(capture_directory).filePath("pinc-import-draft.png"),read(draft_path));
        write(QDir(capture_directory).filePath("pinc-import-drawing-set.pdf"),read(pdf_path));
        for (int page=0;page<2;++page)
            require(pdf.render(page,QSize(900,1200)).save(QDir(capture_directory).filePath(
                QString("pinc-import-output-page-%1.png").arg(page+1))),"Capture actual imported PDF pages");
    }
    const auto hidden_annotations=QString::fromStdString(layer1);
    require(window.setContainerVisible(hidden_annotations,false),"Hide the source page's actual annotation/calculation layer explicitly");
    auto& current_pages=widget<QComboBox>(window,"pincProjectPage");
    current_pages.setCurrentIndex(1);QCoreApplication::processEvents();
    require(std::none_of(reopened_canvas.labels().begin(),reopened_canvas.labels().end(),
        [](const auto& item){return item.text=="Upper portable note";}),
        "Page navigation must not reset an explicit hidden layer");
    current_pages.setCurrentIndex(0);QCoreApplication::processEvents();
    const auto filtered_path=temporary.filePath("explicit-filter.pdf");
    require(window.exportDrawingSetPdf(filtered_path),qPrintable(window.lastError()));QPdfDocument filtered;
    require(filtered.load(filtered_path)==QPdfDocument::Error::None&&filtered.pageCount()==2,"Load explicitly filtered PDF");
    const auto filtered_text=filtered.getAllText(1).text();
    std::cout<<"[pinc-desktop] explicitly filtered second PDF text: "<<filtered_text.toStdString()<<std::endl;
    auto filtered_image=filtered.render(1,QSize(900,1200)).convertToFormat(QImage::Format_ARGB32);
    int blue_ink=0;
    for (int y=0;y<filtered_image.height();++y) {
        const auto* row=reinterpret_cast<const QRgb*>(filtered_image.constScanLine(y));
        for (int x=0;x<filtered_image.width();++x)
            if (qBlue(row[x])>qRed(row[x])+40&&qBlue(row[x])>qGreen(row[x])+20) ++blue_ink;
    }
    require(
        filtered.getAllText(0).text().contains("Ground portable note")&&
        !filtered_text.contains("Upper portable note")&&!filtered_text.contains("64.00")&&blue_ink>50,
        "Drawing-set output must preserve genuine visibility without losing another page's visible interior layer");
    require(window.setContainerVisible(hidden_annotations,true),"Restore explicit source visibility");
    const auto hidden_floor=QString::fromStdString(pages[1].at("floor_id").get<std::string>());
    require(window.setContainerVisible(hidden_floor,false),"Hide an imported floor explicitly");
    current_pages.setCurrentIndex(1);QCoreApplication::processEvents();
    require(!window.entityVisible(area1_id),"Page navigation must not reset an explicit hidden floor");
    require(window.setContainerVisible(hidden_floor,true)&&window.entityVisible(area1_id),
        "Restoring the floor reveals the active page without changing page scope");
    current_pages.setCurrentIndex(0);QCoreApplication::processEvents();
    const auto before_bad_metadata=window.document().snapshot();
    auto bad_registry=page_registry(before_bad_metadata);
    bad_registry.extensions["pinc_import"]["source_asset_id"]="missing-source-for-metadata-check";
    window.document().apply(ApplyEntityChanges{before_bad_metadata.revision(),
        {EntityChange::upsert(bad_registry)}, {},"Exercise invalid imported-page metadata"});
    // Unit changes refresh canvas quantities only. Exercise the complete
    // page-control/navigator refresh through a normal visibility command.
    require(window.setContainerVisible(hidden_floor,false),"Refresh navigation after invalid page metadata");
    QCoreApplication::processEvents();
    require(!current_pages.isVisible()&&window.lastError().contains("Imported pages"),
        "Malformed page metadata must report a diagnostic instead of unwinding canvas/navigation refresh");
    require(!window.exportDrawingSetPdf(temporary.filePath("invalid-metadata.pdf")),
        "Malformed imported-page metadata must still block authoritative sheet output");
    require(window.undoCommand(),"Undo invalid page metadata without losing the imported project");
    require(window.setContainerVisible(hidden_floor,true),"Restore user filter after metadata recovery");
    require(read(source_path)==original,"Native editing/output must preserve original .pinc bytes");
    progress("full-feature case: assertions complete; close window");
}
void cancel_and_failure_keep_active_project(const QString& source_path,bool cancel_review) {
    progress(cancel_review?"review cancellation case: create window":"progress cancellation case: create window");
    MainWindow window;configure(window);const auto before=window.document().snapshot();const auto layer=window.activeLayerId();
    QTimer poll;poll.setInterval(1);QElapsedTimer deadline;deadline.start();bool acted=false,timed_out=false;
    QObject::connect(&poll,&QTimer::timeout,[&]{
        if(cancel_review) {
            for(auto* dialog:window.findChildren<QDialog*>())if(dialog->objectName()=="pincImportReview"&&dialog->isVisible()) {acted=true;poll.stop();dialog->reject();return;}
        } else if(auto* button=window.findChild<QPushButton*>("pincImportCancel");button&&button->isVisible()) {acted=true;poll.stop();button->click();return;}
        if(deadline.elapsed()>45000) {timed_out=true;poll.stop();if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();}
    });
    poll.start();progress(cancel_review?"review cancellation: import begin":"progress cancellation: import begin");
    const auto admitted=window.importPinc(source_path,cancel_review);poll.stop();
    progress(std::string("cancellation import returned ")+(admitted?"true":"false"));
    require(!timed_out&&acted&&!admitted,"Native review/progress cancellation must be exercised before publication");
    const auto after=window.document().snapshot();
    require(after.document_id()==before.document_id()&&after.revision()==before.revision()&&after.entities()==before.entities()&&after.assets()==before.assets()&&
        after.history().size()==before.history().size()&&window.activeLayerId()==layer&&window.lastError().isEmpty(),"Cancellation changed active project or reported a failure");
}
void cancelled_and_invalid_imports() {
    progress("cancellation/failure cases: begin");
    QTemporaryDir temporary;const auto source_path=temporary.filePath("cancel.pinc");const auto original=fixture_bytes();write(source_path,original);
    cancel_and_failure_keep_active_project(source_path,true);cancel_and_failure_keep_active_project(source_path,false);
    MainWindow window;configure(window);const auto before=window.document().snapshot();const auto invalid=temporary.filePath("invalid.pinc");write(invalid,"not a valid project");
    progress("invalid source: import begin");
    require(!window.importPinc(invalid,false)&&!window.lastError().isEmpty(),"Invalid real-worker source must fail explicitly");
    progress("invalid source: import returned");
    require(window.document().snapshot().document_id()==before.document_id()&&window.document().snapshot().entities()==before.entities()&&
        window.document().snapshot().assets()==before.assets()&&window.document().revision()==before.revision(),"Invalid source partially replaced active project");
    require(read(source_path)==original&&read(invalid)=="not a valid project","Cancellation or failure rewrote source bytes");
}
void require_viewport_fits_painted_ink(const CoordinatedView& view,const DrawingSheet& sheet,PlanCanvas& canvas) {
    QString reason;const auto recorded=canvas.recordSketchContent(80,&reason);
    require(recorded.has_value(),qPrintable(reason));require(!view.presentation.crop,"Imported pages must allow dynamic full-content fitting until user authors a crop");
    const auto viewport=std::find_if(sheet.viewports.begin(),sheet.viewports.end(),[&](const auto& value){return value.view_id==view.id;});
    require(viewport!=sheet.viewports.end(),"Imported page has a persisted fitted viewport");
    const auto& ink=recorded->ink_bounds;
    const auto left=recorded->model_center.x+ink.left()/recorded->model_scale;
    const auto right=recorded->model_center.x+ink.right()/recorded->model_scale;
    const auto bottom=recorded->model_center.y-ink.bottom()/recorded->model_scale;
    const auto top=recorded->model_center.y-ink.top()/recorded->model_scale;
    const auto center_x=(left+right)/2,center_y=(bottom+top)/2;
    const auto width=(viewport->bounds.width_mm-6)*viewport->scale_denominator/1000;
    const auto height=(viewport->bounds.height_mm-15)*viewport->scale_denominator/1000;
    constexpr double tolerance=1e-6;
    require(center_x-width/2<=left+tolerance&&center_x+width/2>=right-tolerance&&
        center_y-height/2<=bottom+tolerance&&center_y+height/2>=top-tolerance,
        "Persisted source-page viewport scale clips actual painted text or area callouts");
}
void long_text_and_distant_callouts_fit_real_output() {
    progress("painted-ink case: create source and native window");
    QTemporaryDir temporary;require(temporary.isValid(),"Create painted-ink import fixture directory");
    auto project=Json::parse(fixture_bytes().toStdString());auto& text_page=project["pages"][0];
    text_page["name"]="Text-only alignment page";text_page["calcWalls"]=Json::array();
    text_page["interiorWalls"]=Json::array();text_page["assignments"]=Json::object();text_page["symbols"]=Json::array();
    const std::string left_text="LEFT COMPLETE: Every word in this long left aligned source note must remain visible";
    const std::string right_text="RIGHT COMPLETE: Every word in this long right aligned source note must remain visible";
    const std::string rotated_text="ROTATED COMPLETE: This entire angled source note must reach the printed page";
    const auto text=[](const char* id,const std::string& content,double x,double y,const char* alignment,double rotation) {
        return Json{{"id",id},{"text",content},{"x",x},{"y",y},{"size",1.1},{"font","Inter"},
            {"align",alignment},{"rot",rotation},{"color","#182536"}};
    };
    text_page["texts"]=Json::array({text("left",left_text,0,0,"left",0),text("right",right_text,0,8,"right",0),
        text("angled",rotated_text,5,4,"left",37)});
    auto& area_page=project["pages"][1];area_page["name"]="Distant-callout page";area_page["texts"]=Json::array();
    area_page["symbols"]=Json::array();area_page["ghostPrevious"]=false;
    auto& assignment=area_page["assignments"]["a|b|c|d"];
    assignment["name"]="REMOTE SOURCE AREA NAME";assignment["namePos"]={{"x",70},{"y",-35}};
    assignment["calcPos"]={{"x",-55},{"y",45}};
    const auto original=QByteArray::fromStdString(project.dump());const auto source=temporary.filePath("painted-ink.pinc");write(source,original);
    MainWindow window;configure(window);import_without_review(window,source);
    auto& canvas=widget<PlanCanvas>(window,"measurementPlanCanvas");auto& page_combo=widget<QComboBox>(window,"pincProjectPage");
    const auto imported=window.document().snapshot();const auto sheets=decode_sheet_view_entity(page_registry(imported));
    const auto& records=page_registry(imported).extensions.at("pinc_import").at("pages");
    require(page_combo.count()==2&&sheets.views().size()==2,"Painted-ink source pages remain separate native views");
    progress("painted-ink case: import returned; inspect both fitted viewports");
    for(int page=0;page<2;++page) {
        page_combo.setCurrentIndex(page);QCoreApplication::processEvents();
        const auto id=records[page].at("view_id").get<std::string>();
        const auto view=std::find_if(sheets.views().begin(),sheets.views().end(),[&](const auto& value){return value.id==id;});
        const auto sheet_id=records[page].at("sheet_id").get<std::string>();
        const auto sheet=std::find_if(sheets.sheets().begin(),sheets.sheets().end(),[&](const auto& value){return value.id==sheet_id;});
        require(view!=sheets.views().end()&&sheet!=sheets.sheets().end(),"Painted page has its persisted native view/sheet");
        require_viewport_fits_painted_ink(*view,*sheet,canvas);
        if(page==0) {
            require(canvas.entities().empty()&&canvas.labels().size()>=3,"Text-only source must render without fabricated geometric owners");
            const auto left=std::find_if(canvas.labels().begin(),canvas.labels().end(),[&](const auto& item){return item.text==QString::fromStdString(left_text);});
            const auto right=std::find_if(canvas.labels().begin(),canvas.labels().end(),[&](const auto& item){return item.text==QString::fromStdString(right_text);});
            const auto rotated=std::find_if(canvas.labels().begin(),canvas.labels().end(),[&](const auto& item){return item.text==QString::fromStdString(rotated_text);});
            require(left!=canvas.labels().end()&&right!=canvas.labels().end()&&rotated!=canvas.labels().end()&&
                left->text_alignment=="left"&&right->text_alignment=="right"&&std::abs(rotated->rotation_radians)>.5,
                "Long source text alignment and rotation must reach actual canvas rendering");
        } else {
            const auto layer=records[page].at("calculation_layer_id").get<std::string>();const auto& area=area_in_layer(imported,layer);
            const auto& name=label(canvas,QString::fromStdString(area.id),"area_name");
            const auto& calculation=label(canvas,QString::fromStdString(area.id),"area_calculation");
            require(name.position.x>15&&name.position.y>8&&calculation.position.x<-12&&calculation.position.y<-12,
                "Distant source callout positions must survive without being moved into the room");
        }
    }
    const auto pdf_path=temporary.filePath("painted-ink.pdf");progress("painted-ink case: initial PDF export begin");
    require(window.exportDrawingSetPdf(pdf_path),qPrintable(window.lastError()));progress("painted-ink case: PDF returned; decode full notes");
    QPdfDocument pdf;require(pdf.load(pdf_path)==QPdfDocument::Error::None&&pdf.pageCount()==2,"Painted-ink drawing set must decode with both pages");
    const auto first_text=pdf.getAllText(0).text().simplified();const auto second_text=pdf.getAllText(1).text().simplified();
    require(first_text.contains(QString::fromStdString(left_text))&&first_text.contains(QString::fromStdString(right_text))&&
        first_text.contains(QString::fromStdString(rotated_text)),"Actual text-only PDF clips or omits part of a long aligned/rotated note");
    require(second_text.contains("REMOTE SOURCE AREA NAME")&&second_text.contains("64"),"Actual PDF clips distant live area callouts");
    for(int page=0;page<2;++page)require(ink_pixels(pdf.render(page,QSize(900,1200)))>1000,"Fitted source PDF pages must visibly render complete content");
    const auto native_path=temporary.filePath("painted-ink.bldproj");progress("painted-ink case: native Save As begin");
    require(window.saveProjectAs(native_path),qPrintable(window.lastError()));progress("painted-ink case: Save As returned; reopen begin");
    require(window.openProject(native_path),qPrintable(window.lastError()));progress("painted-ink case: reopen returned");
    const auto restored=decode_sheet_view_entity(page_registry(window.document().snapshot()));
    require(restored.views()==sheets.views()&&restored.sheets()==sheets.sheets(),"Dynamic-fit intent and fitted initial scales must survive native save/reopen");
    progress("painted-ink case: one import Undo/Redo begin");
    require(window.undoCommand()&&window.document().snapshot().assets().empty(),"Painted-ink fitting must preserve one import Undo");
    require(window.redoCommand()&&window.document().snapshot().entities()==imported.entities(),"One Redo must restore fitted original import content");
    widget<QComboBox>(window,"pincProjectPage").setCurrentIndex(0);QCoreApplication::processEvents();
    progress("painted-ink case: add far public annotation");
    const auto far_label=window.createAnnotationLabel("note","POST IMPORT FAR AUTHOR STAYS INSIDE FIT",{80,-45});
    require(!far_label.isEmpty(),qPrintable(window.lastError()));
    const auto after_authoring=window.document().snapshot();const auto later_pdf_path=temporary.filePath("painted-ink-after-edit.pdf");
    progress("painted-ink case: edited PDF export begin");
    require(window.exportDrawingSetPdf(later_pdf_path),qPrintable(window.lastError()));progress("painted-ink case: edited PDF returned");QPdfDocument later_pdf;
    require(later_pdf.load(later_pdf_path)==QPdfDocument::Error::None&&later_pdf.pageCount()==2,"Edited imported pages must still export as a complete drawing set");
    require(later_pdf.getAllText(0).text().contains("POST IMPORT FAR AUTHOR STAYS INSIDE FIT")&&
        !later_pdf.getAllText(1).text().contains("POST IMPORT FAR AUTHOR STAYS INSIDE FIT"),
        "Dynamic imported-page fitting clips later far annotation or leaks it into another source page");
    require(window.document().snapshot().entities()==after_authoring.entities()&&window.document().revision()==after_authoring.revision(),
        "Dynamic output fitting must not rewrite authoring or persist stale crop bounds");
    require(read(source)==original,"Painted-ink fitting or output changed original source");
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!directory.isEmpty()) {
        require(QDir().mkpath(directory),"Create painted-ink capture directory");
        write(QDir(directory).filePath("pinc-painted-ink.pdf"),read(pdf_path));
        write(QDir(directory).filePath("pinc-painted-ink-after-edit.pdf"),read(later_pdf_path));
        for(int page=0;page<2;++page)require(pdf.render(page,QSize(900,1200)).save(QDir(directory).filePath(QString("pinc-painted-ink-page-%1.png").arg(page+1))),"Capture fitted actual PDF page");
    }
}
void empty_text_does_not_block_blank_or_underlay_pages() {
    progress("empty-text/underlay case: begin");
    QTemporaryDir temporary;require(temporary.isValid(),"Create empty-page fixture");
    Json page{{"name","Blank"},{"texts",Json::array({{{"id","empty"},{"text",""},{"x",0},{"y",0}}})}};
    auto image_page=page;image_page["name"]="Reference";
    QImage image(2,2,QImage::Format_RGBA8888);image.fill(Qt::red);
    QByteArray png;QBuffer buffer(&png);require(buffer.open(QIODevice::WriteOnly)&&image.save(&buffer,"PNG"),"Encode empty-page reference");
    image_page["underlay"]={{"data","data:image/png;base64,"+png.toBase64().toStdString()},
        {"x",0},{"y",0},{"width",10},{"opacity",1}};
    const Json source{{"format","PincSketch"},{"version","4.2"},{"currentPage",1},{"pages",{page,image_page}}};
    const auto path=temporary.filePath("empty.pinc");write(path,QByteArray::fromStdString(source.dump()));
    MainWindow window;configure(window);import_without_review(window,path);
    auto& canvas=widget<PlanCanvas>(window,"measurementPlanCanvas");
    require(canvas.references().size()==1,"Empty label must not suppress admitted reference");
    auto& pages=widget<QComboBox>(window,"pincProjectPage");pages.setCurrentIndex(0);
    require(canvas.references().empty(),"Blank page must remain independent from reference page");
}
void known_symbol_fidelity_is_reviewed_before_acceptance() {
    progress("known-symbol fidelity case: create source and native window");
    QTemporaryDir temporary;require(temporary.isValid(),"Create symbol fidelity review fixture directory");
    const auto source=temporary.filePath("fidelity.pinc");const auto original=fixture_bytes();write(source,original);
    const auto counterparts=default_pinc_symbol_counterparts();const auto counterpart=std::find_if(counterparts.begin(),counterparts.end(),[](const auto& value){return value.source_kind=="Door - Interior";});
    require(counterpart!=counterparts.end()&&!counterpart->fidelity_note.empty(),"Known source door has an explicit bundled artwork fidelity note");
    MainWindow window;configure(window);const auto before=window.document().snapshot();QTimer poll;poll.setInterval(1);
    QElapsedTimer deadline;deadline.start();bool reviewed=false,timed_out=false;std::exception_ptr failure;
    QObject::connect(&poll,&QTimer::timeout,[&]{
        for(auto* dialog:window.findChildren<QDialog*>())if(dialog->objectName()=="pincImportReview"&&dialog->isVisible()) {
            poll.stop();try {
                progress("known-symbol fidelity case: real review opened");
                const auto& report=widget<QPlainTextEdit>(*dialog,"pincImportFidelity");
                require(report.toPlainText().contains("symbol_artwork_fidelity")&&report.toPlainText().contains(QString::fromStdString(counterpart->fidelity_note)),
                    "Known bundled symbol's artwork differences must be visible in actual pre-import review");
                require(window.document().snapshot().document_id()==before.document_id()&&window.document().snapshot().entities()==before.entities(),"Review must precede active-project publication");
                const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!directory.isEmpty()) {
                    require(QDir().mkpath(directory),"Create fidelity review capture directory");require(dialog->grab().save(QDir(directory).filePath("pinc-symbol-fidelity-review.png")),"Capture actual known-symbol fidelity review");
                }
                auto* buttons=dialog->findChild<QDialogButtonBox*>();require(buttons&&buttons->button(QDialogButtonBox::Ok),"Actual import review acceptance button exists");
                reviewed=true;buttons->button(QDialogButtonBox::Ok)->click();
            }catch(...){failure=std::current_exception();dialog->reject();}return;
        }
        if(deadline.elapsed()>45000) {timed_out=true;poll.stop();if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();}
    });
    poll.start();progress("known-symbol fidelity case: reviewed import begin");
    const auto admitted=window.importPinc(source,true);poll.stop();progress("known-symbol fidelity case: reviewed import returned");if(failure)std::rethrow_exception(failure);
    require(!timed_out&&reviewed&&admitted,qPrintable(window.lastError()));
    require(window.document().snapshot().document_id()!=before.document_id(),"Accepted real fidelity review must publish independent imported project");
    assert_original_asset(window.document().snapshot(),original);require(read(source)==original,"Accepted symbol review must preserve original source");
}
}
int main(int argc,char** argv) {
    progress("application: initialize Qt");
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-pinc-desktop-"+QUuid::createUuid().toString());
    try {
        UnexpectedModalDiagnostics modal_diagnostics;progress("application: load bundled font");
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"Load bundled native font");
        imported_canvas_pages_persistence_and_live_edit();progress("full-feature case: returned");
        cancelled_and_invalid_imports();progress("cancellation/failure cases: returned");
        long_text_and_distant_callouts_fit_real_output();progress("painted-ink case: returned");
        known_symbol_fidelity_is_reviewed_before_acceptance();progress("known-symbol fidelity case: returned");
        empty_text_does_not_block_blank_or_underlay_pages();progress("empty-text/underlay case: returned");
        std::cout<<"Pinc project desktop tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
