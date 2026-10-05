#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/text_library_dialog.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QDialog>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {
using sketch::desktop::MainWindow;
using sketch::desktop::PlanCanvas;
using sketch::desktop::TextLibraryStore;
using json=nlohmann::json;

void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
sketch::Boundary square() {
    return {{{0,0},{4,0},0},{{4,0},{4,4},0},{{4,4},{0,4},0},{{0,4},{0,0},0}};
}
sketch::TextLibraryEntry reusable() {
    sketch::TextLibraryEntry entry{"user-text-desktop-fixture","Inspection note","notes","Reusable note\nMeasured on site",{}};
    entry.style.font_family="Inter";
    entry.style.text_height_metres=.15;
    entry.style.stroke_color="#345678";
    entry.style.bold=true;
    entry.style.italic=true;
    entry.style.text_alignment="right";
    return entry;
}
PlanCanvas& canvas(MainWindow& window) {
    auto* value=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(value,"actual measurement canvas must exist");return *value;
}
void display(MainWindow& window) {
    window.setMetricUnits(true);window.resize(1100,780);window.show();QApplication::processEvents();
    canvas(window).setOverviewMapEnabled(false);canvas(window).setSnapEnabled(false);canvas(window).fitView();
}
QPointF screen(const PlanCanvas& canvas,sketch::Vec2 point) {
    return QRectF(canvas.rect()).center()+QPointF((point.x-canvas.viewCenter().x)*canvas.viewScale(),
        -(point.y-canvas.viewCenter().y)*canvas.viewScale());
}
void mouse(PlanCanvas& canvas,QEvent::Type type,QPointF point) {
    const auto button=type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto held=type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),button,held,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
void click(PlanCanvas& canvas,sketch::Vec2 point) {
    const auto position=screen(canvas,point);
    mouse(canvas,QEvent::MouseButtonPress,position);mouse(canvas,QEvent::MouseButtonRelease,position);
}
void escape(PlanCanvas& canvas) {
    QKeyEvent event(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&event);
}
sketch::LabelInstance label(const MainWindow& window,const QString& id) {
    const auto snapshot=window.document().snapshot();
    for(const auto& [owner_id,entity]:snapshot.entities()) {
        (void)owner_id;
        if(entity.type!=sketch::kAnnotationEntityType) continue;
        const auto state=sketch::decode_annotation_entity(entity);
        for(const auto& item:state.labels) if(item.id==id.toStdString()) return item;
    }
    throw std::runtime_error("Placed label is missing from its persisted owner");
}
sketch::desktop::CanvasLabel projected_label(MainWindow& window,const QString& id) {
    const auto& values=canvas(window).labels();
    const auto found=std::find_if(values.begin(),values.end(),[&](const auto& value){return value.id==id;});
    require(found!=values.end(),"placed label must reach retained canvas");return *found;
}
const json& raw_label(const sketch::Entity& owner,const QString& id) {
    for(const auto& item:owner.properties.at("state").at("labels")) if(item.at("id")==id.toStdString()) return item;
    throw std::runtime_error("Raw label record is missing");
}
void capture(QWidget& widget,const QString& filename) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(directory.isEmpty()) return;
    require(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(filename)),"actual widget capture must save");
}
QImage output(PlanCanvas& canvas) {
    QImage image(900,700,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::white);
    QPainter painter(&image);canvas.renderSceneAt(painter,QRectF(image.rect()),100,{2,2},Qt::white);painter.end();return image;
}

void copied_entries_and_raw_metadata_survive_library_changes() {
    QTemporaryDir directory;require(directory.isValid(),"independent projects need a temporary library");
    const auto library_path=directory.filePath(QStringLiteral("text-library.json"));
    TextLibraryStore library(library_path);const auto original_entry=reusable();library.upsert(original_entry);
    MainWindow first({},nullptr,library_path),second({},nullptr,library_path);
    first.setMetricUnits(true);second.setMetricUnits(true);
    require(!first.createBoundary(square()).isEmpty(),"first project needs real geometry");
    const auto unrelated=first.createAnnotationLabel(QStringLiteral("note"),QStringLiteral("Retained unrelated note"),{.5,.5});
    require(!unrelated.isEmpty() && !first.createAnnotationSymbol(QStringLiteral("desk"),{3,3}).isEmpty(),
        "create unrelated text and pinned artwork before adding opaque metadata");
    auto source=first.document().snapshot();auto owner=source.entities().at("annotations-1");
    owner.required=true;owner.extensions["vendor_owner"]={{"retain",17}};
    owner.properties["state"]["vendor_state"]={{"retained",true}};
    for(auto& item:owner.properties["state"]["labels"]) if(item.at("id")==unrelated.toStdString()) {
        item["vendor_label"]="retain";item["placement"]["vendor_placement"]="retain";
        item["style"]["vendor_style"]="retain";
    }
    first.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(owner)}, {}, "seed raw annotation metadata"});
    const auto before=first.document().snapshot();
    const auto first_id=first.createAnnotationLabel(QString::fromStdString(original_entry.id),{}, {1,1});
    const auto second_id=second.createAnnotationLabel(QString::fromStdString(original_entry.id),{}, {2,2});
    require(!first_id.isEmpty() && !second_id.isEmpty(),"custom template IDs must resolve from the injected local library");
    const auto inserted=first.document().snapshot();auto expected=before.entities();
    expected.at(owner.id).properties["state"]["version"]=8;
    expected.at(owner.id).properties["state"]["labels"].push_back(raw_label(inserted.entities().at(owner.id),first_id));
    require(inserted.revision()==before.revision()+1 && inserted.entities()==expected,
        "aligned insertion must append one instance, upgrade the style version and preserve unrelated metadata/artwork");
    require(label(first,first_id).content==original_entry.content && label(second,second_id).content==original_entry.content &&
        projected_label(first,first_id).font_family==QStringLiteral("Inter") && projected_label(first,first_id).bold &&
        projected_label(first,first_id).italic && projected_label(first,first_id).color==QColor(QStringLiteral("#345678")) &&
        projected_label(first,first_id).text_alignment==QStringLiteral("right"),
        "independent project copies must carry chosen content, actual font and style into the canvas");
    require(first.editAnnotation(first_id,QStringLiteral("Independently edited instance"),QStringLiteral("1"),QStringLiteral("1"),
        QStringLiteral("0"),QStringLiteral("1"),true),"placed instance must remain editable");
    const auto edited=first.document().snapshot();expected=inserted.entities();
    for(auto& item:expected.at(owner.id).properties["state"]["labels"])
        if(item.at("id")==first_id.toStdString()) item["content"]="Independently edited instance";
    require(edited.entities()==expected && label(second,second_id).content==original_entry.content,
        "instance editing must preserve raw owner and unrelated child metadata without changing another project's copy");
    // Editing the opaque child itself must retain its admitted nested fields.
    require(first.editAnnotation(unrelated,QStringLiteral("Changed unrelated instance"),QStringLiteral("0.5"),QStringLiteral("0.5"),
        QStringLiteral("0"),QStringLiteral("1"),true),"opaque instance must remain editable");
    const auto opaque_edited=first.document().snapshot();
    auto expected_owner=edited.entities().at(owner.id);
    for(auto& item:expected_owner.properties["state"]["labels"])
        if(item.at("id")==unrelated.toStdString()) item["content"]="Changed unrelated instance";
    require(opaque_edited.entities().at(owner.id)==expected_owner,"editing a raw child must preserve nested placement/style and pinned sibling data");
    auto updated=original_entry;updated.content="Updated library template";updated.style.stroke_color="#987654";library.upsert(updated);
    const auto newer_id=first.createAnnotationLabel(QString::fromStdString(updated.id),{}, {2,1});
    require(!newer_id.isEmpty() && label(first,newer_id).content==updated.content &&
        label(first,newer_id).style.stroke_color==updated.style.stroke_color &&
        label(first,first_id).content=="Independently edited instance" && label(second,second_id).content==original_entry.content,
        "custom template lookup must reload external edits while existing instances remain independent");
    library.remove(original_entry.id);const auto removed=first.document().snapshot();
    require(first.createAnnotationLabel(QString::fromStdString(original_entry.id),{}, {0,0}).isEmpty() &&
        first.document().revision()==removed.revision() && first.document().snapshot().entities()==removed.entities(),
        "deleted templates must refuse future lookup without altering placed copies or history");
    const auto project_path=directory.filePath(QStringLiteral("independent-text.bldproj"));
    require(first.saveProjectAs(project_path) && QFile::remove(library_path),"save independent labels then remove their local library");
    require(first.createNewProject(),"release the saved project's writer ownership before reopening it for editing");
    MainWindow reopened({},nullptr,library_path);
    require(reopened.openProject(project_path) && reopened.document().snapshot().entities()==removed.entities() &&
        label(reopened,first_id).content=="Independently edited instance" && label(reopened,newer_id).content==updated.content &&
        projected_label(reopened,newer_id).font_family==QStringLiteral("Inter"),
        "native project reopen must retain complete copied text/style/metadata with no library present");
    const auto builtin_id=reopened.createAnnotationLabel(QStringLiteral("note"),QStringLiteral("Still available offline"),{3,1});
    if(builtin_id.isEmpty()) std::cerr<<reopened.lastError().toStdString()<<'\n';
    require(!builtin_id.isEmpty(),
        "built-in/free text must remain usable with a missing library");
    QFile malformed(library_path);require(malformed.open(QIODevice::WriteOnly),"fixture must create a malformed library");
    const QByteArray bad_head("{\"version\":99,\"entries\":[]}");
    require(malformed.write(bad_head)==bad_head.size(),"malformed fixture bytes must write");malformed.close();
    require(!reopened.createAnnotationLabel(QStringLiteral("note"),QStringLiteral("Built-in survives corrupt library"),{3,2}).isEmpty(),
        "built-in/free text insertion must also bypass corrupt library files");
    const auto before_bad_lookup=reopened.document().snapshot();
    require(reopened.createAnnotationLabel(QString::fromStdString(original_entry.id),{}, {3,3}).isEmpty() &&
        reopened.document().snapshot().entities()==before_bad_lookup.entities() &&
        reopened.document().revision()==before_bad_lookup.revision(),"future-library custom lookup must refuse mutation");
    require(malformed.open(QIODevice::ReadOnly) && malformed.readAll()==bad_head,"failed lookup must preserve the future library file");
}

void actual_dialog_and_canvas_placement_obey_input_and_revision_fences() {
    QTemporaryDir directory;require(directory.isValid(),"placement fixture needs a temporary library");
    const auto library_path=directory.filePath(QStringLiteral("placement-library.json"));
    TextLibraryStore store(library_path);const auto entry=reusable();store.upsert(entry);
    MainWindow window({},nullptr,library_path);const auto boundary=window.createBoundary(square(),QStringLiteral("garage"));
    require(!boundary.isEmpty(),"placement fixture needs real selectable geometry");display(window);
    QString dialog_failure;
    QTimer::singleShot(0,&window,[&]{
        auto* dialog=dynamic_cast<QDialog*>(QApplication::activeModalWidget());
        try {
            require(dialog && dialog->objectName()==QStringLiteral("textLibraryDialog"),"Add text must open actual local text library");
            auto* list=dialog->findChild<QListWidget*>(QStringLiteral("textLibraryList"));
            require(list,"actual library needs entry selection");bool found=false;
            for(int row=0;row<list->count();++row) if(list->item(row)->data(Qt::UserRole).toString()==QString::fromStdString(entry.id)) {
                list->setCurrentRow(row);found=true;break;
            }
            require(found,"actual library must show the saved custom entry");capture(*dialog,QStringLiteral("text-library-dialog.png"));
            auto* insert=dialog->findChild<QPushButton*>(QStringLiteral("textLibraryInsert"));require(insert,"library needs actual Insert button");insert->click();
        } catch(const std::exception& error){dialog_failure=QString::fromUtf8(error.what());if(dialog)dialog->reject();}
    });
    const auto initial=window.document().snapshot();window.showTextLibrary();
    require(dialog_failure.isEmpty() && window.document().snapshot().entities()==initial.entities(),"dialog Insert must arm placement without mutating the project");
    auto& drawing=canvas(window);const auto center=drawing.viewCenter();
    const auto start=QRectF(drawing.rect()).center();const auto moved=start+QPointF(90,35);
    mouse(drawing,QEvent::MouseButtonPress,start);mouse(drawing,QEvent::MouseMove,moved);mouse(drawing,QEvent::MouseButtonRelease,moved);
    require(window.document().snapshot().entities()==initial.entities() && window.document().revision()==initial.revision() &&
        (std::abs(drawing.viewCenter().x-center.x)>1e-6 || std::abs(drawing.viewCenter().y-center.y)>1e-6),
        "armed placement left-drag must pan without inserting text or changing geometry");
    click(drawing,{2,2});const auto placed=window.document().snapshot();const auto placed_id=window.selectedEntityId();
    require(placed.revision()==initial.revision()+1 && placed.entities().at(boundary.toStdString())==initial.entities().at(boundary.toStdString()) &&
        label(window,placed_id).content==entry.content && std::abs(label(window,placed_id).placement.position.x-2)<1e-9 &&
        std::abs(label(window,placed_id).placement.position.y-2)<1e-9 && !drawing.boundaryDraftPreview(),
        "stationary placement click must insert exactly one text instance without starting drawing or moving selected geometry");
    capture(drawing,QStringLiteral("placed-library-text-canvas.png"));
    require(window.undoCommand() && window.document().snapshot().entities()==initial.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==placed.entities(),"actual placement must use normal undo/redo");
    require(label(window,placed_id).model_plan && window.selectEntity(boundary) && window.setSelectedPlanLabelPosition({5,5}) &&
        label(window,placed_id).model_plan && window.selectEntity(placed_id),
        "area-label presentation edits must preserve the newer reusable text anchor schema");
    const auto inter_image=output(drawing);
    // Offscreen QPA does not enumerate Windows fonts like the native plugin.
    // Register an actual installed second face so this tests a real change,
    // rather than comparing Inter with an unavailable family's fallback.
    const auto fixed_id=QFontDatabase::addApplicationFont(
        QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/consola.ttf")));
    require(fixed_id>=0,"Windows verification needs the installed Consolas face");
    const auto fixed_families=QFontDatabase::applicationFontFamilies(fixed_id);
    require(!fixed_families.isEmpty(),"installed comparison font must expose its family");
    const auto fixed_family=fixed_families.front();
    require(fixed_family!=QStringLiteral("Inter") && window.editAnnotation(placed_id,QString::fromStdString(entry.content),
        QStringLiteral("2"),QStringLiteral("2"),QStringLiteral("0"),QStringLiteral("1"),true,fixed_family,QStringLiteral("150"),
        QStringLiteral("#345678"),QStringLiteral("#FFFFFF"),true,true,true),"placed text supports an independent font edit");
    require(projected_label(window,placed_id).font_family==fixed_family && output(drawing)!=inter_image,
        "chosen font must change the actual output renderer, including cached text layout");
    require(window.undoCommand(),"restore original reusable style before PDF output");
    const auto pdf_path=directory.filePath(QStringLiteral("placed-library-text.pdf"));
    require(window.exportDraftPdf(pdf_path),"copied library text must export through real plan PDF output");
    QPdfDocument pdf;require(pdf.load(pdf_path)==QPdfDocument::Error::None && pdf.pageCount()>0 &&
        pdf.getAllText(0).text().simplified().contains(QStringLiteral("Reusable note")),"actual plan PDF must retain copied reusable text");
    const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!captures.isEmpty()) require(QFile::copy(pdf_path,QDir(captures).filePath(QStringLiteral("placed-library-text.pdf"))) &&
        pdf.render(0,QSize(1400,1000)).save(QDir(captures).filePath(QStringLiteral("placed-library-text-pdf.png"))),"retain actual text PDF output");
    const auto cancel_source=window.document().snapshot();const auto selected=window.selectedEntityId();
    require(window.beginTextPlacement(entry),"begin cancellable text placement");escape(drawing);
    require(window.document().revision()==cancel_source.revision() && window.document().snapshot().entities()==cancel_source.entities() &&
        window.selectedEntityId()==selected,"Escape must cancel text placement while preserving document and selection");
    require(window.beginTextPlacement(entry),"begin stale placement fixture");
    auto newer=window.document().snapshot().entities().at("property-1");newer.properties["external_text_test"]="retain";
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),{sketch::EntityChange::upsert(newer)}, {}, "external property edit"});
    const auto external=window.document().snapshot();click(drawing,{1,1});
    require(window.document().revision()==external.revision() && window.document().snapshot().entities()==external.entities() &&
        !window.lastError().isEmpty(),"stale placement must refuse insertion and preserve the newer project head");
    require(window.beginTextPlacement(entry),"begin placement before editability changes");
    window.document().mark_read_only("text placement read-only fixture");click(drawing,{1,1});
    require(!window.beginTextPlacement(entry) && window.createAnnotationLabel(QStringLiteral("note"),QStringLiteral("Forbidden"),{1,1}).isEmpty() &&
        window.document().revision()==external.revision() && window.document().snapshot().entities()==external.entities(),
        "read-only transition and direct commands must refuse text mutation");
}

void named_horizontal_plan_inverts_placement() {
    for (const auto direction_z : {-1.0,1.0}) {
        QTemporaryDir directory;
        MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
        require(!window.createBoundary(square()).isEmpty(),"named plan text needs real geometry");
        display(window);
        auto sheet=window.document().snapshot().entities().at("sheet-view-1");
        const auto model=sketch::decode_sheet_view_entity(sheet);
        auto views=model.views();
        auto view=std::find_if(views.begin(),views.end(),[](const auto& item){return item.id=="view-plan";});
        require(view!=views.end(),"named horizontal text fixture needs a saved plan");
        const auto angle=std::numbers::pi/6;
        view->origin_m={10,-4,direction_z<0 ? 10.0 : -10.0};
        view->direction={0,0,direction_z};view->up={-std::sin(angle),std::cos(angle),0};
        sheet.properties["model"]=sketch::SheetViewModel::create(views,model.sheets(),model.schedule_ids(),model.sheet_order()).to_json();
        window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
            {sketch::EntityChange::upsert(sheet)}, {}, "named text placement frame"});
        window.setWorkspace(sketch::desktop::Workspace::architectural);
        auto* choices=window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
        require(choices,"named text fixture needs actual view chooser");
        const auto index=choices->findData(QStringLiteral("view-plan"),Qt::UserRole+1);
        require(index>=3,"named horizontal text plan must be available");choices->setCurrentIndex(index);
        auto* drawing=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
        require(drawing,"named text fixture needs actual plan canvas");
        drawing->setOverviewMapEnabled(false);drawing->setSnapEnabled(false);drawing->fitView();QApplication::processEvents();
        const auto source=window.document().snapshot();
        const sketch::Vec2 world{2.5,1.5};
        const auto x=world.x-view->origin_m[0],y=world.y-view->origin_m[1];
        const sketch::Vec2 projected{-direction_z*(x*std::cos(angle)+y*std::sin(angle)),
                                    -x*std::sin(angle)+y*std::cos(angle)};
        require(window.beginTextPlacement(reusable()),"named horizontal plan must accept reusable text placement");
        click(*drawing,projected);
        const auto placed_id=window.selectedEntityId();const auto placed=label(window,placed_id);
        require(placed.model_plan && std::abs(placed.placement.position.x-world.x)<1e-8 &&
            std::abs(placed.placement.position.y-world.y)<1e-8 && window.document().revision()==source.revision()+1,
            "rotated/reflected named plan clicks must retain world-space annotation positions");
        const auto retained=std::find_if(drawing->labels().begin(),drawing->labels().end(),
            [&](const auto& item){return item.id==placed_id;});
        require(retained!=drawing->labels().end() && std::abs(retained->position.x-projected.x)<1e-8 &&
            std::abs(retained->position.y-projected.y)<1e-8 && retained->font_family==QStringLiteral("Inter"),
            "placed text and selected style must project back into the named plan");
        const auto after=window.document().snapshot();
        for(const auto& [id,entity]:source.entities()) if(entity.type!=sketch::kAnnotationEntityType)
            require(after.entities().at(id)==entity,"named text placement must preserve original geometry and view records");
        const sketch::Vec2 delta{.4,-.3};
        const sketch::Vec2 projected_delta{-direction_z*(delta.x*std::cos(angle)+delta.y*std::sin(angle)),
                                          -delta.x*std::sin(angle)+delta.y*std::cos(angle)};
        drawing->setSnapEnabled(false); // Commit refresh restores the workspace preference.
        // Right alignment places the insertion anchor at the text edge. Drag
        // the painted selection interior instead of an edge/control target.
        const auto frame=drawing->selectionBounds();
        require(frame && !frame->isEmpty(),"placed named-plan text must retain a painted selection frame");
        const auto start=frame->center();
        const auto end=start+QPointF(projected_delta.x*drawing->viewScale(),
                                    -projected_delta.y*drawing->viewScale());
        require(retained->selected && window.selectedEntityId()==placed_id,
            "named-plan label movement must retain the authored label selection");
        require((end-start).manhattanLength()>=QApplication::startDragDistance(),
            "named-plan label movement must cross the drag threshold");
        const auto move_revision=window.document().revision();
        const auto preview_serial=drawing->entitiesMovePreviewSerial();
        mouse(*drawing,QEvent::MouseButtonPress,start);mouse(*drawing,QEvent::MouseMove,end);
        require(drawing->entitiesMovePreviewSerial()>preview_serial && drawing->entitiesMovePreviewPending() &&
            window.document().revision()==move_revision,
            "named-plan label drag must enqueue an exact move proposal before mutating the document");
        mouse(*drawing,QEvent::MouseButtonRelease,end);
        // Exact movement is projected on a worker, then admitted and committed
        // by the UI event loop after release. Wait on that transition only.
        QElapsedTimer completion;completion.start();
        while(drawing->entitiesMovePreviewPending() && completion.elapsed()<5000)
            QApplication::processEvents(QEventLoop::AllEvents,20);
        if(drawing->entitiesMovePreviewPending() || window.document().revision()!=move_revision+1)
            std::cerr<<"Named label move completion: pending="<<drawing->entitiesMovePreviewPending()
                     <<"; revision="<<window.document().revision()<<"; expected="<<move_revision+1
                     <<"; elapsed_ms="<<completion.elapsed()<<"; "<<window.lastError().toStdString()<<'\n';
        require(!drawing->entitiesMovePreviewPending(),"named-plan exact label move must finish within five seconds");
        require(window.document().revision()==move_revision+1,
            "named-plan label movement must commit exactly one document revision");
        const auto moved=label(window,placed_id);
        if(std::abs(moved.placement.position.x-world.x-delta.x)>=1e-8 ||
            std::abs(moved.placement.position.y-world.y-delta.y)>=1e-8)
            std::cerr<<"Named label moved to "<<moved.placement.position.x<<","<<moved.placement.position.y
                     <<"; expected "<<world.x+delta.x<<","<<world.y+delta.y<<"; "<<window.lastError().toStdString()<<'\n';
        require(std::abs(moved.placement.position.x-world.x-delta.x)<1e-8 &&
            std::abs(moved.placement.position.y-world.y-delta.y)<1e-8 && moved.model_plan,
            "named-plan label drag must convert the canvas delta back into world XY");
        require(window.undoCommand() && std::abs(label(window,placed_id).placement.position.x-world.x)<1e-8 && window.redoCommand(),
            "named-plan anchored movement must support undo and redo");
        choices->setCurrentIndex(1);
        require(std::none_of(drawing->labels().begin(),drawing->labels().end(),[&](const auto& item){return item.id==placed_id;}),
            "plan-anchored text must stay out of unrelated elevation and section overlays");
        const auto path=directory.filePath(QStringLiteral("named-text.bldproj"));
        require(window.saveProjectAs(path) && window.openProject(path) && label(window,placed_id).model_plan &&
            std::abs(label(window,placed_id).placement.position.x-world.x-delta.x)<1e-8,
            "versioned plan anchors and exact world positions must survive native save/reopen");
    }
}
}

int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);
    try {
        const auto font_id=QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        require(font_id>=0,"bundled Inter resource must load for real font rendering tests");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        copied_entries_and_raw_metadata_survive_library_changes();
        actual_dialog_and_canvas_placement_obey_input_and_revision_fences();
        named_horizontal_plan_inverts_placement();
        std::cout<<"text_library_desktop_tests passed\n";return 0;
    } catch(const std::exception& error){std::cerr<<"text_library_desktop_tests: "<<error.what()<<'\n';return 1;}
}
