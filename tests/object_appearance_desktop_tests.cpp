#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using sketch::desktop::MainWindow;
using sketch::desktop::PlanCanvas;
using sketch::desktop::CanvasEntity;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T& child(QObject& owner,const char* name) {
    auto* result=owner.findChild<T*>(QString::fromLatin1(name));
    require(result,"actual object appearance control must exist");return *result;
}
PlanCanvas& canvas(MainWindow& window,const char* name="measurementPlanCanvas") {
    auto* result=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QString::fromLatin1(name)));
    require(result,"actual physical plan canvas must exist");return *result;
}
void display(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,780);window.setMetricUnits(true);
    window.show();QApplication::processEvents();canvas(window).setOverviewMapEnabled(false);
}
void hierarchy_properties(MainWindow& window,const QString& id) {
    auto& tree=child<QTreeWidget>(window,"projectNavigator");tree.expandAll();
    QTreeWidgetItem* item=nullptr;
    for (QTreeWidgetItemIterator it(&tree);*it;++it) if ((*it)->data(0,Qt::UserRole).toString()==id) {item=*it;break;}
    require(item,"hidden physical object must remain in actual project hierarchy");
    tree.scrollToItem(item);QApplication::processEvents();
    const auto position=QPointF(tree.visualItemRect(item).center());
    require(tree.viewport()->rect().contains(position.toPoint()),"hidden object hierarchy row must be reachable");
    const auto send=[&](QEvent::Type type) {
        QMouseEvent event(type,position,tree.viewport()->mapToGlobal(position.toPoint()),Qt::LeftButton,
            type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(),&event);
    };
    send(QEvent::MouseButtonPress);send(QEvent::MouseButtonRelease);
    send(QEvent::MouseButtonDblClick);send(QEvent::MouseButtonRelease);QApplication::processEvents();
    require(window.selectedEntityId()==id && child<QScrollArea>(window,"contextEditor").isVisible(),
        "actual hidden-object hierarchy double click must open quick properties");
    require(child<QPushButton>(window,"objectAppearanceButton").isVisible() &&
        child<QPushButton>(window,"objectAppearanceButton").isEnabled(),"hierarchy quick properties exposes hidden-object drawing appearance");
}
void editor(MainWindow& window,const std::function<void(QDialog&)>& edit) {
    auto& button=child<QPushButton>(window,"objectAppearanceButton");
    require(button.isEnabled() && !button.isHidden(),"selected physical object must offer enabled appearance editor");
    std::exception_ptr failure;bool seen=false;
    QTimer timer;timer.setInterval(1);
    QObject::connect(&timer,&QTimer::timeout,[&] {
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("objectAppearanceDialog"));
        if (!dialog || !dialog->isVisible()) return;
        timer.stop();seen=true;
        try { require(dialog->testAttribute(Qt::WA_DontShowOnScreen),"appearance test dialog must remain offscreen");edit(*dialog); }
        catch (...) { failure=std::current_exception();dialog->reject(); }
    });
    timer.start();button.click();timer.stop();
    if (failure) std::rethrow_exception(failure);
    require(seen,"actual object appearance dialog must open");
}
void apply(QDialog& dialog) { child<QDialogButtonBox>(dialog,"").button(QDialogButtonBox::Apply)->click(); }
void capture(QWidget& widget,const QString& name) {
    const auto path=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if (path.isEmpty()) return;
    require(QDir().mkpath(path) && widget.grab().save(QDir(path).filePath(name)),"actual object appearance capture saves");
}
void style(MainWindow& window,const char* outline="#B12D73",const char* fill="#31BA69") {
    editor(window,[&](QDialog& dialog) {
        child<QLineEdit>(dialog,"objectOutlineColor").setText(QString::fromLatin1(outline));
        child<QLineEdit>(dialog,"objectFillColor").setText(QString::fromLatin1(fill));
        auto& pattern=child<QComboBox>(dialog,"objectFillPattern");pattern.setCurrentIndex(pattern.findData("solid"));
        child<QDoubleSpinBox>(dialog,"objectHatchScale").setValue(1.7);
        child<QDoubleSpinBox>(dialog,"objectLineWidthMm").setValue(.8);
        capture(dialog,"object-appearance-dialog.png");apply(dialog);
    });
}
std::vector<CanvasEntity> shapes(PlanCanvas& drawing,const QString& id) {
    std::vector<CanvasEntity> result;
    for (const auto& shape:drawing.entities()) if (shape.id==id) result.push_back(shape);
    return result;
}
void styled_shapes(PlanCanvas& drawing,const QString& id,const char* outline="#B12D73") {
    const auto values=shapes(drawing,id);require(!values.empty(),"physical shape must remain projected");
    for (const auto& value:values) require(value.stroke_color==QColor(QString::fromLatin1(outline)) &&
        value.fill_color==QColor("#31BA69") && value.filled && value.hatch_pattern=="solid" &&
        value.hatch_scale==1.7 && value.output_stroke_width_mm==.8 && value.paper_stroke_width_on_screen,
        "every projected physical shape and opening detail must receive object presentation");
}
void model_unchanged(const sketch::DocumentSnapshot& before,const sketch::DocumentSnapshot& after) {
    for (const auto& [id,entity]:before.entities()) if (entity.type!=sketch::kAnnotationEntityType)
        require(after.entities().at(id)==entity,"appearance must preserve physical source, receipts, relationships and GLA facts");
    require(before.assets()==after.assets(),"appearance must preserve exact project assets");
}
int colored_pixels(const QImage& image,const QColor& expected) {
    int count=0;
    for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) {
        const auto color=image.pixelColor(x,y);
        if (std::abs(color.red()-expected.red())<20 && std::abs(color.green()-expected.green())<20 &&
            std::abs(color.blue()-expected.blue())<20) ++count;
    }
    return count;
}
void source_preserving_editor() {
    QTemporaryDir dir;MainWindow window({},nullptr,dir.filePath("library.json"));display(window);
    const auto wall=window.createStraightWall({0,0},{4,0});
    const auto opening=window.createHostedOpening("window",".6 m",".8 m",".8 m","1 m");
    require(!wall.isEmpty() && !opening.isEmpty() && window.selectEntity(wall),"raw fixture needs actual wall and window");
    style(window);
    auto source=window.document().snapshot();sketch::Entity owner;
    for (const auto& [id,entity]:source.entities()) if (entity.type==sketch::kAnnotationEntityType &&
        !entity.properties.at("state").at("overrides").empty()) { (void)id;owner=entity;break; }
    require(!owner.id.empty(),"appearance must have a persisted owner");
    owner.required=true;owner.extensions["opaque_owner"]={{"retain",17}};
    auto& state=owner.properties["state"];state["opaque_state"]="retain";
    auto& record=state["overrides"][0];record["opaque_record"]="retain";record["style"]["opaque_style"]="retain";
    record.erase("paper_line_width_mm");record.erase("hatch_scale");record["style"]["stroke_width_metres"]=.003;
    auto sibling=record;sibling["target_id"]=opening.toStdString();state["overrides"].push_back(sibling);
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(owner)}, {}, "seed opaque and optional-free appearance"});
    require(window.selectEntity(wall),"refresh raw provider");const auto legacy=window.document().snapshot();
    editor(window,[](QDialog& dialog){apply(dialog);});
    require(window.document().snapshot().entities()==legacy.entities() && window.document().revision()==legacy.revision(),
        "unchanged legacy Apply preserves absent optional paper width/hatch and command history");
    editor(window,[](QDialog& dialog){child<QLineEdit>(dialog,"objectOutlineColor").setText("#112233");dialog.reject();});
    require(window.document().snapshot().entities()==legacy.entities(),"Cancel discards object appearance edits");
    editor(window,[](QDialog& dialog){child<QLineEdit>(dialog,"objectOutlineColor").setText("invalid");apply(dialog);
        require(dialog.isVisible() && !child<QLabel>(dialog,"objectAppearanceError").text().isEmpty(),"invalid appearance keeps dialog open with error");dialog.reject();});
    require(window.document().snapshot().entities()==legacy.entities(),"invalid color must not write records");
    editor(window,[](QDialog& dialog){child<QLineEdit>(dialog,"objectOutlineColor").setText("#112233");apply(dialog);});
    auto expected=owner;expected.properties["state"]["overrides"][0]["style"]["stroke_color"]="#112233";
    require(window.document().snapshot().entities().at(owner.id)==expected,
        "editing one field preserves exact owner policies, unknown fields, sibling records and absent legacy optionals");
    require(window.selectEntity(opening),"select hosted window");style(window);styled_shapes(canvas(window),opening);
    require(window.selectEntity(wall),"select wall for visibility independence");
    const auto labels=canvas(window).labels();
    editor(window,[](QDialog& dialog){child<QCheckBox>(dialog,"objectVisible").setChecked(false);apply(dialog);});
    require(shapes(canvas(window),wall).empty(),"physical wall visibility hides wall shapes");
    for (const auto& label:labels) if (label.id==wall) {
        const auto found=std::find_if(canvas(window).labels().begin(),canvas(window).labels().end(),[&](const auto& value){return value.id==wall;});
        require(found!=canvas(window).labels().end() && found->text==label.text && found->color==label.color,
            "hiding physical wall leaves independently derived measurement visible and unchanged");
    }
    hierarchy_properties(window,wall);
    const auto before_reset=window.document().snapshot();auto reset_owner=before_reset.entities().at(owner.id);
    auto& reset_records=reset_owner.properties["state"]["overrides"];
    reset_records.erase(reset_records.begin());
    editor(window,[](QDialog& dialog){child<QPushButton>(dialog,"resetObjectAppearance").click();apply(dialog);});
    require(window.document().snapshot().entities().at(owner.id)==reset_owner && !shapes(canvas(window),wall).empty(),
        "reset removes only chosen object record, retaining opaque owner policies and other object appearance");
    require(window.undoCommand() && window.document().snapshot().entities()==before_reset.entities() && window.redoCommand(),
        "Reset undo/redo restores exact override source including hidden state");
    // Two providers must refuse editing rather than silently choose an owner.
    const auto before_duplicate=window.document().snapshot();
    auto first=before_duplicate.entities().at(owner.id);
    auto wall_record=first.properties["state"]["overrides"][0];wall_record["target_id"]=wall.toStdString();
    first.properties["state"]["overrides"].push_back(wall_record);
    auto duplicate=sketch::make_annotation_entity("duplicate-object-provider",sketch::AnnotationState{});
    duplicate.properties["state"]["overrides"].push_back(wall_record);
    window.document().apply(sketch::ApplyEntityChanges{before_duplicate.revision(),{sketch::EntityChange::upsert(first),
        sketch::EntityChange::upsert(duplicate)}, {}, "seed duplicate appearance providers"});
    require(window.selectEntity(wall),"select duplicate-provider object");const auto duplicates=window.document().snapshot();
    child<QPushButton>(window,"objectAppearanceButton").click();
    require(window.lastError().contains("multiple annotation groups") && window.document().snapshot().entities()==duplicates.entities(),
        "duplicate providers refuse without mutation or inaccessible modal errors");
}
void physical_workflow() {
    QTemporaryDir dir;require(dir.isValid(),"fixture needs temporary project directory");
    MainWindow window({},nullptr,dir.filePath("text-library.json"));display(window);
    const auto wall=window.createStraightWall({0,0},{4,0});require(!wall.isEmpty(),"fixture needs straight wall");
    const auto door=window.createHostedOpening("door",".6 m",".8 m","0 m","2 m",
        std::nullopt,sketch::DoorOperation{false,true,90.0});
    require(!door.isEmpty(),"fixture needs actual hosted door");
    require(window.selectEntity(wall),"select host for second opening");
    const auto hosted_window=window.createHostedOpening("window","2 m",".8 m",".8 m","1 m");
    require(!hosted_window.isEmpty(),"fixture needs actual hosted window");
    require(!window.createAnnotationLabel("note","Unrelated retained text",{.5,2}).isEmpty(),"fixture needs unrelated annotation");
    require(window.selectEntity(wall),"select straight wall");
    const auto untouched=window.document().snapshot();
    editor(window,[](QDialog& dialog) { apply(dialog); });
    require(window.document().revision()==untouched.revision() && window.document().snapshot().entities()==untouched.entities(),
        "unchanged Apply must be a true no-op without a default override");
    const auto measurements=canvas(window).labels();
    style(window);const auto styled=window.document().snapshot();model_unchanged(untouched,styled);
    require(styled.revision()==untouched.revision()+1,"appearance must be one history command");
    styled_shapes(canvas(window),wall);
    for (const auto& label:measurements) if (label.id==wall) {
        const auto found=std::find_if(canvas(window).labels().begin(),canvas(window).labels().end(),[&](const auto& v){return v.id==wall;});
        require(found!=canvas(window).labels().end() && found->text==label.text && found->color==label.color &&
            found->paper_height_mm==label.paper_height_mm,"physical appearance must leave derived measurements unchanged");
    }
    require(window.undoCommand(),"object appearance must undo");
    require(window.document().snapshot().entities()==untouched.entities(),"Undo restores exact source state");
    require(window.redoCommand(),"object appearance must redo");styled_shapes(canvas(window),wall);
    // Architecture projection is built after the retained plan; test fresh solid projections too.
    window.setWorkspace(sketch::desktop::Workspace::architectural);
    auto& views=child<QComboBox>(window,"architecturalView");
    views.setCurrentIndex(1);styled_shapes(canvas(window,"architecturalPlanCanvas"),wall);
    views.setCurrentIndex(2);styled_shapes(canvas(window,"architecturalPlanCanvas"),wall);
    window.setWorkspace(sketch::desktop::Workspace::measurement);
    require(window.selectEntity(door),"select actual hosted door");style(window);styled_shapes(canvas(window),door);
    require(window.selectEntity(hosted_window),"select actual hosted window");style(window,"#126ACC");
    styled_shapes(canvas(window),hosted_window,"#126ACC");styled_shapes(canvas(window),wall);styled_shapes(canvas(window),door);
    const auto both=window.document().snapshot();model_unchanged(untouched,both);
    require(window.selectEntity({}),"clear selection for actual screen render");
    canvas(window).fitView();QApplication::processEvents();
    capture(canvas(window),"object-appearance-unselected-plan.png");
    require(colored_pixels(canvas(window).grab().toImage(),QColor("#B12D73"))>8,"actual screen paints authored object outline");
    require(colored_pixels(canvas(window).grab().toImage(),QColor("#126ACC"))>8,"actual screen paints distinct hosted-window detail strokes");
    QImage output(1100,780,QImage::Format_ARGB32_Premultiplied);output.fill(Qt::white);
    {QPainter painter(&output);canvas(window).renderScene(painter,QRectF(output.rect()),true,Qt::white);}
    // The shared renderer intentionally draws fills at alpha 64 on white.
    require(colored_pixels(output,QColor("#B12D73"))>8 && colored_pixels(output,QColor(203,238,217))>8,
        "shared print renderer paints authored physical outline and fill");
    require(colored_pixels(output,QColor("#126ACC"))>8,"shared print renderer paints distinct hosted-window detail strokes");
    const auto pdf_path=dir.filePath("object-appearance.pdf");require(window.exportDraftPdf(pdf_path),"physical appearance PDF exports");
    QPdfDocument pdf;require(pdf.load(pdf_path)==QPdfDocument::Error::None,"appearance PDF reopens");
    const auto pdf_image=pdf.render(0,QSize(1400,1000));
    require(colored_pixels(pdf_image,QColor("#B12D73"))>8,"actual PDF paints authored object outline");
    require(colored_pixels(pdf_image,QColor("#126ACC"))>8,"actual PDF paints distinct hosted-window detail strokes");
    const auto capture_path=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture_path.isEmpty()) {
        require(QDir().mkpath(capture_path) && pdf_image.save(QDir(capture_path).filePath("object-appearance-native-pdf.png")),"actual PDF render capture saves");
        const auto copy=QDir(capture_path).filePath("object-appearance-native.pdf");
        if (QFile::exists(copy)) require(QFile::remove(copy),"replace task-owned PDF capture");
        require(QFile::copy(pdf_path,copy),"actual PDF capture saves");
    }
    require(window.selectEntity(door),"select opening before hiding");
    editor(window,[](QDialog& dialog){child<QCheckBox>(dialog,"objectVisible").setChecked(false);apply(dialog);});
    require(shapes(canvas(window),door).empty() && !shapes(canvas(window),wall).empty(),"hiding opening removes only its physical glyph");
    require(window.selectEntity(door),"hidden object remains selectable by document identity");
    require(child<QPushButton>(window,"objectAppearanceButton").isEnabled(),"hidden object retains recovery control");
    const auto saved=window.document().snapshot();const auto path=dir.filePath("appearance.bldproj");
    require(window.saveProjectAs(path),"appearance saves in native project");
    require(window.createNewProject(),"release native project ownership before reopening");
    MainWindow reopened({},nullptr,dir.filePath("reopened-library.json"));display(reopened);
    require(reopened.openProject(path),"appearance native project reopens");
    require(reopened.document().snapshot().entities()==saved.entities(),"native reopen retains exact model and appearance records");
    styled_shapes(canvas(reopened),hosted_window,"#126ACC");styled_shapes(canvas(reopened),wall);
    require(shapes(canvas(reopened),door).empty(),"reopened hidden object stays hidden");
    hierarchy_properties(reopened,door);
    editor(reopened,[](QDialog& dialog){child<QPushButton>(dialog,"resetObjectAppearance").click();apply(dialog);});
    require(!shapes(canvas(reopened),door).empty(),"reset restores hosted glyph visibility");styled_shapes(canvas(reopened),wall);
    require(reopened.selectEntity(wall),"select wall for stale revision guard");
    editor(reopened,[&](QDialog& dialog){
        child<QLineEdit>(dialog,"objectOutlineColor").setText("#111111");
        auto source=reopened.document().snapshot();auto entity=source.entities().at(wall.toStdString());entity.extensions["stale_fixture"]=true;
        reopened.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(entity)}, {}, "stale modal fixture"});
        const auto stale=reopened.document().snapshot();apply(dialog);
        require(dialog.isVisible() && !child<QLabel>(dialog,"objectAppearanceError").text().isEmpty(),"stale appearance refuses Apply");
        require(reopened.document().snapshot().entities()==stale.entities(),"stale dialog must not overwrite newer work");dialog.reject();
    });
    require(reopened.selectEntity(wall),"select object before read-only fence");
    editor(reopened,[&](QDialog& dialog){
        child<QLineEdit>(dialog,"objectOutlineColor").setText("#111111");
        reopened.document().mark_read_only("fixture policy");const auto before=reopened.document().snapshot();apply(dialog);
        require(dialog.isVisible() && child<QLabel>(dialog,"objectAppearanceError").text().contains("read-only"),"read-only transition refuses Apply");
        require(reopened.document().snapshot().entities()==before.entities(),"read-only appearance must preserve source");dialog.reject();
    });
    require(reopened.selectEntity(wall) && !child<QPushButton>(reopened,"objectAppearanceButton").isEnabled(),"read-only object appearance is disabled");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("ObjectAppearance");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    try {
        require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"bundled test font loads");
        physical_workflow();source_preserving_editor();std::cout<<"object_appearance_desktop_tests passed\n";return 0;
    } catch (const std::exception& error) {std::cerr<<"object_appearance_desktop_tests: "<<error.what()<<'\n';return 1;}
}
