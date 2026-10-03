#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using sketch::desktop::MainWindow;
using sketch::desktop::PlanCanvas;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T> T& child(QObject& owner,const char* name){auto* result=owner.findChild<T*>(QString::fromLatin1(name));require(result,"component color control exists");return *result;}
PlanCanvas& canvas(MainWindow& window){auto* result=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));require(result,"measurement canvas exists");return *result;}
void display(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,780);window.setMetricUnits(true);window.show();QApplication::processEvents();canvas(window).setOverviewMapEnabled(false);}
void capture(QWidget& widget,const char* name){const auto root=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(root.isEmpty())return;require(QDir().mkpath(root)&&widget.grab().save(QDir(root).filePath(name)),"native colors capture saves");}
const sketch::Entity& owner(const sketch::DocumentSnapshot& snapshot,const QString& id){
    for(const auto& [key,entity]:snapshot.entities())if(entity.type==sketch::kAnnotationEntityType){(void)key;for(const auto& symbol:sketch::decode_annotation_entity(entity).symbols)if(symbol.id==id.toStdString())return entity;}
    throw std::runtime_error("symbol owner exists");
}
sketch::SymbolInstance instance(const sketch::DocumentSnapshot& snapshot,const QString& id){for(const auto& value:sketch::decode_annotation_entity(owner(snapshot,id)).symbols)if(value.id==id.toStdString())return value;throw std::runtime_error("symbol exists");}
void preserved(const sketch::DocumentSnapshot& before,const sketch::DocumentSnapshot& after,const QString& id){
    const auto& previous=owner(before,id);auto expected=previous;
    expected.properties["state"]["version"]=after.entities().at(previous.id).properties.at("state").at("version");
    for(auto& symbol:expected.properties["state"]["symbols"])if(symbol["id"]==id.toStdString()) {
        const auto palette=instance(after,id).svg_palette;
        if(palette) {
            for(const auto& record:after.entities().at(previous.id).properties.at("state").at("symbols"))
                if(record.at("id")==id.toStdString())symbol["svg_palette"]=record.at("svg_palette");
        } else symbol.erase("svg_palette");
    }
    require(after.entities().at(previous.id)==expected,"palette edits retain exact owner, siblings, artwork, geometry and opaque metadata");
    for(const auto& [key,entity]:before.entities())if(key!=previous.id)require(after.entities().at(key)==entity,"palette retains unrelated source and calculation facts");
    require(before.assets()==after.assets(),"palette retains assets");
}
void properties(MainWindow& window,const QString& id,sketch::Vec2 point){
    require(window.selectEntity(id),"select actual component");auto& drawing=canvas(window);drawing.fitView();QApplication::processEvents();
    const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();const auto viewport=QRectF(drawing.rect());
    const QPointF pixel(viewport.center().x()+(point.x-center.x)*scale,viewport.center().y()-(point.y-center.y)*scale);
    QMouseEvent event(QEvent::MouseButtonDblClick,pixel,drawing.mapToGlobal(pixel.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&drawing,&event);QApplication::processEvents();
    require(child<QScrollArea>(window,"contextEditor").isVisible()&&child<QPushButton>(window,"symbolColorsButton").isVisible(),"actual component double click exposes Colors in quick properties");
}
void editor(MainWindow& window,const std::function<void(QDialog&)>& edit){
    std::exception_ptr failure;bool seen=false;QTimer timer;timer.setInterval(1);
    QObject::connect(&timer,&QTimer::timeout,[&]{auto* dialog=window.findChild<QDialog*>("symbolColorsDialog");if(!dialog||!dialog->isVisible())return;
        timer.stop();seen=true;try{require(dialog->testAttribute(Qt::WA_DontShowOnScreen),"color dialog stays offscreen");edit(*dialog);}catch(...){failure=std::current_exception();dialog->reject();}});
    timer.start();child<QPushButton>(window,"symbolColorsButton").click();timer.stop();if(failure)std::rethrow_exception(failure);require(seen,"actual component color dialog opens");
}
void apply(QDialog& dialog){auto* buttons=dialog.findChild<QDialogButtonBox*>();require(buttons,"colors has Apply");buttons->button(QDialogButtonBox::Apply)->click();}
int pixels(const QImage& image,const QColor& expected){int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){const auto color=image.pixelColor(x,y);if(std::abs(color.red()-expected.red())<18&&std::abs(color.green()-expected.green())<18&&std::abs(color.blue()-expected.blue())<18)++count;}return count;}
void workflow(){
    QTemporaryDir dir;MainWindow window({},nullptr,dir.filePath("library.json"));display(window);
    const auto first=window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",{0,0});
    const auto second=window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",{3,0});
    require(!first.isEmpty()&&!second.isEmpty(),"fixture places two real detailed sofas");
    auto source=window.document().snapshot();auto annotated=owner(source,first);annotated.required=true;annotated.extensions["policy"]={{"keep",17}};
    for(auto& symbol:annotated.properties["state"]["symbols"])symbol["style"]["opaque"]=symbol["id"];
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(annotated)},{},"fixture protected palette owner"});
    properties(window,first,{0,0});source=window.document().snapshot();
    editor(window,[](QDialog& dialog){apply(dialog);});require(window.document().revision()==source.revision(),"unchanged inherited Apply is no history");
    editor(window,[](QDialog& dialog){child<QCheckBox>(dialog,"symbolColorsInherit").setChecked(false);child<QLineEdit>(dialog,"symbolColorsSurface").setText("#ff0000");dialog.reject();});
    require(window.document().snapshot().entities()==source.entities(),"Cancel leaves exact source");
    editor(window,[](QDialog& dialog){child<QCheckBox>(dialog,"symbolColorsInherit").setChecked(false);child<QLineEdit>(dialog,"symbolColorsOutline").setText("#2040aa");child<QLineEdit>(dialog,"symbolColorsSurface").setText("#ff0000");capture(dialog,"symbol-colors-native-dialog.png");apply(dialog);});
    auto colored=window.document().snapshot();preserved(source,colored,first);require(colored.revision()==source.revision()+1,"palette is one history command");
    require(instance(colored,first).svg_palette&& !instance(colored,second).svg_palette,"palette belongs only to selected instance");
    require(window.undoCommand()&&window.document().snapshot().entities()==source.entities(),"Undo restores exact absent palette");require(window.redoCommand(),"Redo restores palette");
    properties(window,second,{3,0});require(window.editSymbolSvgPalette(second,sketch::SymbolSvgPalette{"white-outline-2","#aa4020","#0044ff"}),"second instance accepts distinct palette");
    child<QScrollArea>(window,"contextEditor").hide();require(window.selectEntity({}),"clear selection before output");canvas(window).fitView();QApplication::processEvents();
    const auto screen=canvas(window).grab().toImage();require(pixels(screen,QColor("#ff0000"))>20&&pixels(screen,QColor("#0044ff"))>20,"actual screen shows independent palettes sharing exact artwork");capture(canvas(window),"symbol-colors-native-canvas.png");
    QImage image(1100,700,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::white);{QPainter painter(&image);canvas(window).renderScene(painter,QRectF(image.rect()),true,Qt::white);}
    require(pixels(image,QColor("#ff0000"))>20&&pixels(image,QColor("#0044ff"))>20,"shared output renders both palettes");
    const auto pdfpath=dir.filePath("colors.pdf");require(window.exportDraftPdf(pdfpath),"actual sheet PDF exports colored symbols");QPdfDocument pdf;require(pdf.load(pdfpath)==QPdfDocument::Error::None,"color PDF opens");
    const auto pdfimage=pdf.render(0,{1800,1250});require(pixels(pdfimage,QColor("#ff0000"))>8&&pixels(pdfimage,QColor("#0044ff"))>8,"actual sheet PDF retains two instance palettes");
    const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!captures.isEmpty())require(pdfimage.save(QDir(captures).filePath("symbol-colors-native-pdf.png")),"actual PDF capture saves");
    const auto saved=window.document().snapshot();const auto path=dir.filePath("colors.bldproj");require(window.saveProjectAs(path)&&window.createNewProject(),"save palette and release original file owner");
    MainWindow reopened({},nullptr,dir.filePath("reopen.json"));display(reopened);require(reopened.openProject(path),"reopen native colored symbols");require(reopened.document().snapshot().entities()==saved.entities(),"exact palette, source artwork and owners reopen");
    properties(reopened,first,{0,0});const auto before_reset=reopened.document().snapshot();
    editor(reopened,[](QDialog& dialog){child<QCheckBox>(dialog,"symbolColorsInherit").setChecked(true);apply(dialog);});
    preserved(before_reset,reopened.document().snapshot(),first);require(!instance(reopened.document().snapshot(),first).svg_palette&&instance(reopened.document().snapshot(),second).svg_palette,"library reset is scoped and preserves sibling palette");
    require(reopened.undoCommand(),"Undo restores explicit colors after reset");
    source=reopened.document().snapshot();annotated=owner(source,first);annotated.required=false;
    reopened.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(annotated)},{},"fixture clipboard-eligible owner"});
    require(reopened.selectEntity(first)&&reopened.copySelection()&&reopened.pasteSelection(),"colored instance copies and pastes through real clipboard");
    const auto copied=reopened.selectedEntityId();require(copied!=first&&instance(reopened.document().snapshot(),copied).svg_palette==instance(reopened.document().snapshot(),first).svg_palette&&instance(reopened.document().snapshot(),copied).pinned_svg==instance(reopened.document().snapshot(),first).pinned_svg,"clipboard retains palette and exact source bytes with fresh identity");
    require(reopened.selectEntity(first),"restore component selection");
    editor(reopened,[&](QDialog& dialog){child<QCheckBox>(dialog,"symbolColorsInherit").setChecked(false);child<QLineEdit>(dialog,"symbolColorsOutline").setText("red");const auto before=reopened.document().snapshot();apply(dialog);require(dialog.isVisible()&&!child<QLabel>(dialog,"symbolColorsError").text().isEmpty()&&reopened.document().snapshot().entities()==before.entities(),"invalid color refuses without partial changes");dialog.reject();});
    editor(reopened,[&](QDialog& dialog){require(reopened.selectEntity(second),"change selection during modal fixture");const auto before=reopened.document().snapshot();apply(dialog);require(dialog.isVisible()&&reopened.document().snapshot().entities()==before.entities(),"stale selection refuses Apply");dialog.reject();});
    require(reopened.selectEntity(first),"restore selection before stale revision");
    const auto stale=reopened.document().revision();require(reopened.editSymbolSvgPalette(first,{}),"clear palette for stale API check");
    const auto before=reopened.document().snapshot();require(!reopened.editSymbolSvgPalette(first,sketch::SymbolSvgPalette{},stale)&&reopened.document().snapshot().entities()==before.entities(),"stale revision refuses exact palette API");
    properties(reopened,first,{0,0});editor(reopened,[&](QDialog& dialog){child<QCheckBox>(dialog,"symbolColorsInherit").setChecked(false);reopened.document().mark_read_only("fixture");const auto before=reopened.document().snapshot();apply(dialog);require(dialog.isVisible()&&reopened.document().snapshot().entities()==before.entities(),"read-only transition refuses colors");dialog.reject();});
}
void unsupported_artwork(){
    QTemporaryDir dir;MainWindow window({},nullptr,dir.filePath("unsupported-library.json"));display(window);
    const auto id=window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",{0,0});
    require(!id.isEmpty()&&window.editSymbolSvgPalette(id,sketch::SymbolSvgPalette{}),"unsupported fixture starts with real palette");
    const auto source=window.document().snapshot();auto changed=owner(source,id);
    for(auto& symbol:changed.properties["state"]["symbols"])if(symbol.at("id")==id.toStdString()) {
        auto artwork=symbol.at("pinned_svg").get<std::string>();
        const auto profile=artwork.find("white-outline-2");require(profile!=std::string::npos,"fixture source declares known profile");
        artwork.replace(profile,std::string("white-outline-2").size(),"unsupported-profile");symbol["pinned_svg"]=artwork;
    }
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(changed)},{},"fixture unsupported saved artwork"});
    const auto path=dir.filePath("unsupported.bldproj");require(window.saveProjectAs(path)&&window.createNewProject(),"unsupported source preserves native intent");
    MainWindow reopened({},nullptr,dir.filePath("unsupported-reopen.json"));display(reopened);require(reopened.openProject(path),"unsupported source remains recoverable on reopen");
    require(child<QLabel>(reopened,"planGeometryError").text().contains("profile",Qt::CaseInsensitive),"unsupported saved palette exposes canvas diagnostic");
    const auto before=reopened.document().snapshot();
    require(!reopened.editSymbolSvgPalette(id,sketch::SymbolSvgPalette{"white-outline-2","#ffffff","#ff0000"})&&
        reopened.lastError().contains("profile",Qt::CaseInsensitive)&&reopened.document().snapshot().entities()==before.entities(),
        "unsupported artwork refuses color editing without rewriting source");
    const auto output=dir.filePath("existing.pdf");const QByteArray sentinel("preserve existing output");
    {QFile file(output);require(file.open(QIODevice::WriteOnly)&&file.write(sentinel)==sentinel.size(),"create protected output fixture");}
    {QFile file(output+".fingerprint.json");require(file.open(QIODevice::WriteOnly)&&file.write(sentinel)==sentinel.size(),"create protected output sidecar fixture");}
    require(!reopened.exportDraftPdf(output)&&reopened.lastError().contains("Reset colors",Qt::CaseInsensitive),"unsupported saved palette diagnoses and blocks incorrect PDF");
    {QFile file(output);require(file.open(QIODevice::ReadOnly)&&file.readAll()==sentinel,"refused palette export preserves existing destination");}
    {QFile file(output+".fingerprint.json");require(file.open(QIODevice::ReadOnly)&&file.readAll()==sentinel,"refused palette export preserves existing sidecar");}
    require(reopened.editSymbolSvgPalette(id,{})&&reopened.exportDraftPdf(dir.filePath("recovered.pdf")),"library reset recovers source without dropping artwork");
    preserved(before,reopened.document().snapshot(),id);
}
}
int main(int argc,char** argv){sketch::testing::noninteractive_errors();QApplication app(argc,argv);QTemporaryDir settings;QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("SymbolPalette");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    try{require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"test font loads");workflow();unsupported_artwork();std::cout<<"symbol_palette_desktop_tests passed\n";return 0;}catch(const std::exception& failure){std::cerr<<"symbol_palette_desktop_tests: "<<failure.what()<<'\n';return 1;}}
