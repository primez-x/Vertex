#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFile>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using sketch::desktop::MainWindow;
using sketch::desktop::PlanCanvas;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T& child(QObject& owner,const char* name) {
    auto* found=owner.findChild<T*>(QString::fromLatin1(name));
    require(found,"actual output-view appearance control must exist");return *found;
}
PlanCanvas& canvas(MainWindow& window) {
    auto* found=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    require(found,"architectural canvas must exist");return *found;
}
void display(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,780);window.setMetricUnits(true);
    window.setWorkspace(sketch::desktop::Workspace::architectural);window.show();QApplication::processEvents();
    canvas(window).setOverviewMapEnabled(false);
}
void capture(QWidget& widget,const char* name) {
    const auto path=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(path.isEmpty())return;
    require(QDir().mkpath(path) && widget.grab().save(QDir(path).filePath(QString::fromLatin1(name))),"actual view appearance capture saves");
}
void select_view(MainWindow& window,const char* id,const QString& owner={}) {
    auto& combo=child<QComboBox>(window,"architecturalView");
    int index=-1;for (int i=0;i<combo.count();++i)
        if (combo.itemData(i,Qt::UserRole+1).toString()==QString::fromLatin1(id) &&
            (owner.isEmpty() || combo.itemData(i,Qt::UserRole+2).toString()==owner)) index=i;
    require(index>=3,"saved view remains recoverable through actual selector");combo.setCurrentIndex(index);QApplication::processEvents();
}
void editor(MainWindow& window,const std::function<void(QDialog&)>& edit) {
    QAction* action=nullptr;for (auto* value:window.findChildren<QAction*>())
        if (value->text()=="Architectural view settings") action=value;
    require(action,"actual architectural view settings action exists");
    std::exception_ptr failure;bool seen=false;
    QTimer timer;timer.setInterval(1);
    QObject::connect(&timer,&QTimer::timeout,[&] {
        auto* settings=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!settings || settings->windowTitle()!="Architectural view settings") return;
        timer.stop();
        try {
            auto* button=settings->findChild<QPushButton*>("viewAppearanceButton");
            require(button && button->isEnabled(),"saved-view settings exposes drawing appearance editor");
            QTimer nested;nested.setInterval(1);
            QObject::connect(&nested,&QTimer::timeout,[&] {
                auto* dialog=window.findChild<QDialog*>("viewAppearanceDialog");
                if (!dialog || !dialog->isVisible()) return;
                nested.stop();seen=true;
                try { require(dialog->testAttribute(Qt::WA_DontShowOnScreen),"view appearance dialog remains offscreen");edit(*dialog); }
                catch (...) { failure=std::current_exception();dialog->reject(); }
            });
            nested.start();button->click();settings->reject();
        } catch (...) { failure=std::current_exception();settings->reject(); }
    });
    timer.start();action->trigger();timer.stop();if(failure)std::rethrow_exception(failure);
    require(seen,"actual saved-view appearance dialog opened");
}
void apply(QDialog& dialog) { auto* buttons=dialog.findChild<QDialogButtonBox*>();require(buttons,"appearance has actual Apply button");buttons->button(QDialogButtonBox::Apply)->click(); }
void scope(QDialog& dialog,const QString& id) {
    auto& control=child<QComboBox>(dialog,"viewAppearanceScope");const auto index=control.findData(id);
    require(index>=0,"scope lists physical source identity including hidden objects");control.setCurrentIndex(index);
}
void style(MainWindow& window,const QString& id,const char* outline,const char* fill) {
    editor(window,[&](QDialog& dialog) {
        scope(dialog,id);child<QCheckBox>(dialog,"viewAppearanceInheritStyle").setChecked(false);
        child<QLineEdit>(dialog,"viewOutlineColor").setText(outline);child<QLineEdit>(dialog,"viewFillColor").setText(fill);
        auto& pattern=child<QComboBox>(dialog,"viewFillPattern");pattern.setCurrentIndex(pattern.findData("solid"));
        child<QDoubleSpinBox>(dialog,"viewLineWidthMm").setValue(.8);child<QDoubleSpinBox>(dialog,"viewAppearanceHatchScale").setValue(1.7);apply(dialog);
    });
}
void styled(MainWindow& window,const QString& id,const char* outline) {
    bool seen=false;for(const auto& entity:canvas(window).entities())if(entity.id==id) {
        seen=true;require(entity.stroke_color==QColor(outline) && entity.fill_color==QColor("#31ba69") &&
            entity.filled && entity.hatch_pattern=="solid" && entity.output_stroke_width_mm==.8 &&
            entity.paper_stroke_width_on_screen && entity.hatch_scale==1.7,"saved-view style reaches every physical projected stroke");
    }require(seen,"styled source has retained projected geometry");
}
bool has(MainWindow& window,const QString& id) {
    return std::any_of(canvas(window).entities().begin(),canvas(window).entities().end(),[&](const auto& value){return value.id==id;});
}
void click_model(MainWindow& window,sketch::Vec2 point) {
    auto& drawing=canvas(window);drawing.setTool(sketch::desktop::CanvasTool::select);drawing.fitView();QApplication::processEvents();
    const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();const auto viewport=QRectF(drawing.rect());
    const QPointF pixel(viewport.center().x()+(point.x-center.x)*scale,viewport.center().y()-(point.y-center.y)*scale);
    for(const auto type:{QEvent::MouseMove,QEvent::MouseButtonPress,QEvent::MouseButtonRelease}) {
        QMouseEvent event(type,pixel,drawing.mapToGlobal(pixel.toPoint()),type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
            type==QEvent::MouseButtonPress?Qt::LeftButton:Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&drawing,&event);QApplication::processEvents();
    }
}
int pixels(const QImage& image,const QColor& expected) {
    int result=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x) {
        const auto color=image.pixelColor(x,y);if(std::abs(color.red()-expected.red())<20 &&
            std::abs(color.green()-expected.green())<20 && std::abs(color.blue()-expected.blue())<20)++result;
    }return result;
}
void source_preserved(const sketch::DocumentSnapshot& before,const sketch::DocumentSnapshot& after) {
    for(const auto& [id,entity]:before.entities())if(entity.type!=sketch::kSheetViewEntityType)
        require(after.entities().at(id)==entity,"view appearance preserves exact geometry, quantities, appraisal facts, annotations and assets");
    require(before.assets()==after.assets(),"view appearance preserves source assets");
}
void saved_view_constraint_previews() {
    QTemporaryDir dir;MainWindow window({},nullptr,dir.filePath("constraint-library.json"));display(window);
    const auto first=window.createStraightWall({0,0},{3,0});
    const auto neighbor=window.createStraightWall({3,0},{3,2});
    require(!first.isEmpty() && !neighbor.isEmpty(),"constraint fixture creates connected physical walls");
    const auto opening=window.createHostedOpening("window",".2 m",".5 m",".8 m","1 m");
    require(!opening.isEmpty(),"constraint fixture creates real hosted window outside crop");
    auto source=window.document().snapshot();sketch::Entity owner;
    for(const auto& [id,entity]:source.entities())if(entity.type==sketch::kSheetViewEntityType){(void)id;owner=entity;break;}
    require(!owner.id.empty(),"constraint fixture has saved-view owner");
    sketch::CoordinatedView view;view.id="constraint-plan";view.name="Constraint plan";
    view.presentation.crop=sketch::ViewCrop{-1,2.8,-1,3};
    owner.properties=sketch::make_sheet_view_entity(owner.id,sketch::SheetViewModel::create({view},{})).properties;
    auto duplicate=sketch::make_sheet_view_entity("zz-constraint-owner",sketch::SheetViewModel::create({view},{}));
    sketch::AnnotationState annotations;sketch::PresentationOverride global;
    global.target_kind="object";global.target_id=neighbor.toStdString();global.visible=false;
    annotations.overrides.push_back(global);
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(owner),
        sketch::EntityChange::upsert(duplicate),sketch::EntityChange::upsert(sketch::make_annotation_entity("constraint-global-hidden",annotations))}, {},"seed saved-view constraint presentation"});
    require(window.selectEntity({}),"refresh constraint fixture");select_view(window,"constraint-plan",QString::fromStdString(owner.id));
    style(window,{},"#b12d73","#31ba69");style(window,neighbor,"#d26218","#31ba69");
    editor(window,[&](QDialog& dialog){scope(dialog,neighbor);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(true);apply(dialog);});
    require(has(window,first) && !has(window,neighbor) && !has(window,opening),"recovered neighbor and child begin wholly outside crop");
    auto& drawing=canvas(window);
    const auto mouse=[&](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
        QMouseEvent event(type,point,drawing.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);
        QApplication::sendEvent(&drawing,&event);QApplication::processEvents();
    };
    const auto wait=[&](const std::function<bool()>& ready) {
        QElapsedTimer timer;timer.start();
        do {QApplication::processEvents(QEventLoop::AllEvents,20);if(ready())return true;QThread::msleep(1);}while(timer.elapsed()<5000);
        return false;
    };
    const auto find=[](const std::vector<sketch::desktop::CanvasEntity>& entities,const QString& id) -> const sketch::desktop::CanvasEntity* {
        const auto it=std::find_if(entities.begin(),entities.end(),[&](const auto& entity){return entity.id==id;});
        return it==entities.end()?nullptr:&*it;
    };
    const auto start_drag=[&] {
        require(window.selectEntity(first),"select retained wall for real saved-view drag");drawing.fitView();QApplication::processEvents();
        drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);
        const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();const auto viewport=QRectF(drawing.rect());
        const QPointF midpoint(viewport.center().x()+(1.5-center.x)*scale,viewport.center().y()+center.y*scale);
        const auto* wall=find(drawing.entities(),first);require(wall,"selected wall remains visible");
        const auto frame=drawing.selectionBounds();require(frame.has_value(),"selected wall exposes actual move frame");
        auto grab=midpoint+QPointF(26,wall->thickness_metres*scale*.5+3);
        if(!frame->contains(grab))grab.setY(midpoint.y()-wall->thickness_metres*scale*.5-3);
        require(frame->contains(grab),"real move gesture starts in wall frame");
        const auto target=grab+QPointF(-scale,0);
        mouse(QEvent::MouseButtonPress,grab,Qt::LeftButton,Qt::LeftButton);
        mouse(QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);
        require(wait([&]{return !drawing.entitiesMovePreviewPending() && !drawing.entitiesMovePreview().empty();}),"saved-view exact constraint preview completes");
        return target;
    };
    const auto before=window.document().snapshot();const auto target=start_drag();const auto preview=drawing.entitiesMovePreview();
    const auto* proposed_neighbor=find(preview,neighbor);const auto* proposed_opening=find(preview,opening);
    require(proposed_neighbor && !proposed_neighbor->segments.empty(),"locally recovered connected wall entering crop appears in exact constraint preview");
    require(proposed_opening && !proposed_opening->segments.empty(),"recovered host restores inherited window entering crop in preview");
    require(proposed_neighbor->stroke_color==QColor("#d26218") && proposed_opening->stroke_color==QColor("#b12d73") &&
        proposed_neighbor->output_stroke_width_mm==.8 && proposed_opening->paper_stroke_width_on_screen,
        "newly eligible preview sources resolve exact saved owner view and object styles");
    require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),"exact appearance preview preserves document and history");
    capture(drawing,"output-view-constraint-crop-entry-preview.png");
    mouse(QEvent::MouseButtonRelease,target,Qt::LeftButton,Qt::NoButton);
    require(wait([&]{return window.document().revision()==before.revision()+1 && !drawing.entitiesMovePreviewPending();}),"exact release commits connected move once");
    for(const auto& proposed:preview) {
        const auto* committed=find(drawing.entities(),proposed.id);
        require(committed && committed->segments.size()==proposed.segments.size() && committed->stroke_color==proposed.stroke_color &&
            committed->fill_color==proposed.fill_color && committed->output_stroke_width_mm==proposed.output_stroke_width_mm,
            "committed scene agrees with candidate geometry and saved-view appearance");
        for(std::size_t i=0;i<proposed.segments.size();++i) {
            const auto& a=proposed.segments[i];const auto& b=committed->segments[i];
            require(std::abs(a.start.x-b.start.x)<1e-7 && std::abs(a.start.y-b.start.y)<1e-7 &&
                std::abs(a.end.x-b.end.x)<1e-7 && std::abs(a.end.y-b.end.y)<1e-7,"committed crop strokes match exact constraint preview");
        }
    }
    // The same local view ID in another graph retains global host hiding.
    select_view(window,"constraint-plan",QString::fromStdString(duplicate.id));
    require(!has(window,neighbor) && !has(window,opening),"duplicate owner never borrows primary recovered visibility");
    start_drag();const auto hidden_preview=drawing.entitiesMovePreview();
    require(!find(hidden_preview,neighbor) && !find(hidden_preview,opening),"globally hidden sources do not leak into another owner preview");
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&drawing,&escape);QApplication::processEvents();
    select_view(window,"constraint-plan",QString::fromStdString(owner.id));
    // Numeric constraints remain detached until actual Apply, even on a recovered source.
    require(window.selectEntity(neighbor),"numeric constraint selects recovered wall");
    const auto numeric_before=window.document().snapshot();
    const auto numeric=[&](bool accept) {
        std::exception_ptr failure;bool seen=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QDialog*>("constraintDialog");
            try {
                require(dialog && dialog->testAttribute(Qt::WA_DontShowOnScreen),"actual numeric constraint dialog stays offscreen");seen=true;
                child<QLineEdit>(*dialog,"constraintLength").setText("2.5 m");
                child<QComboBox>(*dialog,"constraintAnchor").setCurrentIndex(0);
                child<QPushButton>(*dialog,"constraintPreviewButton").click();
                auto& submit=child<QPushButton>(*dialog,"constraintApplyButton");require(submit.isEnabled(),"numeric recovered-wall length proposal accepted");
                auto& changes=child<QTableWidget>(*dialog,"constraintChanges");
                bool reports_length=false;
                for(int row=0;row<changes.rowCount();++row)
                    if(changes.item(row,0) && changes.item(row,0)->toolTip()==neighbor && changes.item(row,2))
                        reports_length=changes.item(row,2)->text()==QStringLiteral("2.5 m");
                require(reports_length,"actual numeric constraint preview reports requested physical length for the selected wall");
                require(window.document().snapshot().entities()==numeric_before.entities(),"numeric Preview leaves recovered geometry and presentation detached");
                capture(*dialog,"output-view-recovered-wall-numeric-constraint-preview.png");
                if(accept)submit.click();else dialog->reject();
            }catch(...){failure=std::current_exception();if(dialog)dialog->reject();}
        });
        window.showConstraintEditor();if(failure)std::rethrow_exception(failure);require(seen,"numeric constraint control workflow exercised");
    };
    numeric(false);require(window.document().revision()==numeric_before.revision(),"numeric Cancel adds no history");numeric(true);
    require(window.document().revision()==numeric_before.revision()+1 && has(window,neighbor),"numeric Apply commits once and retains local recovery");
    const auto baseline=window.document().snapshot().entities().at(neighbor.toStdString()).properties.at("baseline");
    require(std::abs(std::hypot(baseline.at("end")[0].get<double>()-baseline.at("start")[0].get<double>(),
        baseline.at("end")[1].get<double>()-baseline.at("start")[1].get<double>())-2.5)<1e-7,
        "numeric Apply agrees with preview physical length");
    styled(window,neighbor,"#d26218");
    require(window.undoCommand() && window.document().snapshot().entities()==numeric_before.entities(),"numeric Undo restores exact source and appearance");
    // Hidden child intent remains authoritative after a recovered host moves.
    editor(window,[&](QDialog& dialog){scope(dialog,opening);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(false);apply(dialog);});
    const auto hidden_child_source=window.document().snapshot();start_drag();
    const auto child_preview=drawing.entitiesMovePreview();
    require(find(child_preview,neighbor) && !find(child_preview,opening),"recovered-host preview respects explicit child hiding");
    QApplication::sendEvent(&drawing,&escape);QApplication::processEvents();
    require(window.document().snapshot().entities()==hidden_child_source.entities(),"Escape preserves hidden child intent and exact geometry");
    // Locally hidden sources must not use the globally visible eligibility path.
    source=window.document().snapshot();auto visible_global=source.entities().at("constraint-global-hidden");
    auto state=sketch::decode_annotation_entity(visible_global);state.overrides[0].visible=true;
    visible_global.properties=sketch::make_annotation_entity(visible_global.id,state).properties;
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(visible_global)}, {},"fixture globally visible locally hidden source"});
    require(window.selectEntity({}),"refresh globally visible guard");
    editor(window,[&](QDialog& dialog){scope(dialog,neighbor);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(false);apply(dialog);});
    require(!has(window,neighbor) && !has(window,opening),"local host hiding suppresses retained physical sources");
    const auto hidden_source=window.document().snapshot();start_drag();const auto locally_hidden=drawing.entitiesMovePreview();
    require(!find(locally_hidden,neighbor) && !find(locally_hidden,opening),"locally hidden connected sources do not leak into exact preview");
    QApplication::sendEvent(&drawing,&escape);QApplication::processEvents();
    require(window.document().snapshot().entities()==hidden_source.entities(),"hidden-source preview and cancellation do not mutate source");
    // Explicit recovery cannot bypass the view's restricted source list.
    source=window.document().snapshot();auto restricted=source.entities().at(owner.id);
    auto model=sketch::decode_sheet_view_entity(restricted);auto views=model.views();
    views[0].restrict_to_objects=true;views[0].object_ids={first.toStdString()};
    restricted.properties=sketch::make_sheet_view_entity(restricted.id,sketch::SheetViewModel::create(views,model.sheets())).properties;
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(restricted)}, {},"fixture restricted preview source list"});
    require(window.selectEntity({}),"refresh restricted view");
    editor(window,[&](QDialog& dialog){scope(dialog,neighbor);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(true);apply(dialog);});
    start_drag();const auto restricted_preview=drawing.entitiesMovePreview();
    require(!find(restricted_preview,neighbor) && !find(restricted_preview,opening),"local recovery cannot bypass exact preview source restriction");
    QApplication::sendEvent(&drawing,&escape);QApplication::processEvents();
    editor(window,[](QDialog& dialog){child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(false);apply(dialog);});
    require(drawing.entities().empty() && drawing.labels().empty(),"whole-view hiding suppresses recovered constraint scene");
}
void workflow() {
    QTemporaryDir dir;MainWindow window({},nullptr,dir.filePath("library.json"));display(window);
    const auto wall=window.createStraightWall({0,0},{4,0});require(!wall.isEmpty(),"fixture creates actual wall");
    const auto opening=window.createHostedOpening("window",".6 m",".8 m",".8 m","1 m");
    require(!opening.isEmpty(),"fixture creates actual hosted opening");
    require(window.selectEntity(wall),"select host for real door");
    const auto door=window.createHostedOpening("door","2 m",".8 m","0 m","2 m",std::nullopt,sketch::DoorOperation{false,true,90.0});
    require(!door.isEmpty(),"fixture creates actual hosted door detail geometry");
    require(!window.createAnnotationLabel("note","Visible reference annotation",{1,2}).isEmpty(),"fixture creates real annotation content");
    QImage reference_image(40,30,QImage::Format_RGB32);reference_image.fill(QColor("#edac32"));
    QByteArray png;QBuffer png_buffer(&png);
    require(png_buffer.open(QIODevice::WriteOnly) && reference_image.save(&png_buffer,"PNG"),"fixture encodes real retained reference PNG");
    std::vector<std::byte> bytes;bytes.reserve(static_cast<std::size_t>(png.size()));
    for(const auto value:png)bytes.push_back(static_cast<std::byte>(value));
    auto source_asset=sketch::Asset::create("appearance-reference-source","image/png",bytes,
        {{"source_path","reference.png"},{"width_px",40},{"height_px",30},{"content","raster-reference"}});
    auto preview_asset=sketch::Asset::create("appearance-reference-preview","image/png",bytes,
        {{"content","raster-preview"},{"page_index",0},{"page_count",1}});
    auto reference=sketch::Entity::create("reference_asset",{{"asset_id",source_asset.id},
        {"render_asset_id",preview_asset.id},{"source_path","reference.png"},{"mime_type","image/png"},
        {"position_m",{0,0}},{"metres_per_source_unit",.01},{"scale",1.0},{"rotation_degrees",0.0},
        {"flip_horizontal",false},{"flip_vertical",false},{"intensity",.72},{"visible",true}});
    reference.id="appearance-reference";
    auto retained_source=window.document().snapshot();
    window.document().apply(sketch::ApplyEntityChanges{retained_source.revision(),{sketch::EntityChange::upsert(reference)},
        {sketch::AssetChange::upsert(source_asset),sketch::AssetChange::upsert(preview_asset)},"fixture retains real PNG reference content"});
    auto source=window.document().snapshot();sketch::Entity owner;
    for(const auto& [id,entity]:source.entities())if(entity.type==sketch::kSheetViewEntityType){(void)id;owner=entity;break;}
    require(!owner.id.empty(),"fixture has typed sheet graph");
    sketch::CoordinatedView a;a.id="appearance-a";a.name="Appearance A";
    auto b=a;b.id="appearance-b";b.name="Appearance B";
    sketch::DrawingSheet sheet;sheet.id="appearance-sheet";sheet.number="A-001";
    sheet.viewports={{"appearance-va",a.id,{10,10,195,180},50},{"appearance-vb",b.id,{215,10,195,180},50}};
    owner.properties=sketch::make_sheet_view_entity(owner.id,sketch::SheetViewModel::create({a,b},{sheet})).properties;
    owner.required=true;owner.extensions["opaque_policy"]={{"retain",17}};
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(owner)}, {},"seed two independent saved views"});
    require(window.selectEntity({}),"refresh fixture");select_view(window,"appearance-a");
    const auto before=window.document().snapshot();
    editor(window,[](QDialog& dialog){apply(dialog);});
    require(window.document().revision()==before.revision() && window.document().snapshot().entities()==before.entities(),"unchanged Apply preserves absent legacy appearance and history");
    editor(window,[](QDialog& dialog){child<QLineEdit>(dialog,"viewOutlineColor").setText("#112233");dialog.reject();});
    require(window.document().snapshot().entities()==before.entities(),"Cancel preserves exact source");
    style(window,{},"#b12d73","#31ba69");styled(window,wall,"#b12d73");styled(window,opening,"#b12d73");
    styled(window,door,"#b12d73");
    const auto after=window.document().snapshot();source_preserved(before,after);
    require(after.revision()==before.revision()+1,"view appearance is one history command");
    require(after.entities().at(owner.id).required && after.entities().at(owner.id).extensions==owner.extensions,"view edit preserves required and opaque owner policies");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"one Undo restores exact appearance absence");
    require(window.redoCommand(),"one Redo restores authored appearance");styled(window,wall,"#b12d73");
    select_view(window,"appearance-b");style(window,{},"#126acc","#31ba69");styled(window,wall,"#126acc");
    style(window,opening,"#8144dd","#31ba69");styled(window,opening,"#8144dd");styled(window,wall,"#126acc");
    canvas(window).fitView();QApplication::processEvents();capture(canvas(window),"output-view-b-native-screen.png");
    select_view(window,"appearance-a");styled(window,wall,"#b12d73");styled(window,opening,"#b12d73");
    canvas(window).fitView();QApplication::processEvents();require(pixels(canvas(window).grab().toImage(),QColor("#b12d73"))>8,"actual named-view screen paints independent outline");
    capture(canvas(window),"output-view-a-native-screen.png");
    editor(window,[](QDialog& dialog){capture(dialog,"output-view-appearance-native-dialog.png");dialog.reject();});
    QImage image(1100,780,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::white);
    {QPainter painter(&image);canvas(window).renderScene(painter,QRectF(image.rect()),true,Qt::white);}
    require(pixels(image,QColor("#b12d73"))>8 && pixels(image,QColor(203,238,217))>8,"shared canvas output renders saved-view outline and fill");
    require(window.selectOutputSheet("appearance-sheet"),"select shared sheet");
    const auto pdfpath=dir.filePath("view-appearance.pdf");require(window.exportDraftPdf(pdfpath),"shared sheet PDF exports");
    QPdfDocument pdf;require(pdf.load(pdfpath)==QPdfDocument::Error::None,"shared sheet PDF reopens");
    const auto pdfimage=pdf.render(0,QSize(1800,1250));
    require(pixels(pdfimage,QColor("#b12d73"))>8 && pixels(pdfimage,QColor("#126acc"))>8 && pixels(pdfimage,QColor("#8144dd"))>8,"actual same-sheet PDF contains independent views and hosted-opening override");
    const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!captures.isEmpty()) {
        require(pdfimage.save(QDir(captures).filePath("output-view-appearance-native-pdf.png")),"actual PDF page capture saves");
        const auto destination=QDir(captures).filePath("output-view-appearance-native.pdf");
        if(QFile::exists(destination))require(QFile::remove(destination),"replace task owned capture");
        require(QFile::copy(pdfpath,destination),"actual PDF capture saves");
    }
    select_view(window,"appearance-b");
    editor(window,[&](QDialog& dialog){scope(dialog,opening);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(false);apply(dialog);});
    require(!has(window,opening) && has(window,wall),"object hide is scoped to chosen view and opening");
    select_view(window,"appearance-a");require(has(window,opening),"other saved view retains same opening");
    select_view(window,"appearance-b");editor(window,[](QDialog& dialog){child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(false);apply(dialog);});
    require(canvas(window).entities().empty() && canvas(window).labels().empty() && canvas(window).references().empty(),"whole-view hide suppresses physical, actual annotation and actual reference content");
    const auto saved=window.document().snapshot();source_preserved(before,saved);
    const auto path=dir.filePath("views.bldproj");require(window.saveProjectAs(path) && window.createNewProject(),"native save releases project ownership");
    MainWindow reopened({},nullptr,dir.filePath("reopen-library.json"));display(reopened);require(reopened.openProject(path),"native reopen restores views");
    require(reopened.document().snapshot().entities()==saved.entities(),"native reopen retains exact authored state");
    select_view(reopened,"appearance-b");require(canvas(reopened).entities().empty(),"hidden saved view reopens hidden and selectable");
    editor(reopened,[](QDialog& dialog){child<QPushButton>(dialog,"resetViewAppearance").click();apply(dialog);});
    require(has(reopened,wall) && !has(reopened,opening),"whole-view reset preserves object-scoped hidden override");
    editor(reopened,[&](QDialog& dialog){scope(dialog,opening);child<QPushButton>(dialog,"resetViewAppearance").click();apply(dialog);});
    require(has(reopened,opening),"object reset restores inherited visibility");
    const auto reset_before=reopened.document().snapshot();
    editor(reopened,[&](QDialog& dialog){scope(dialog,opening);child<QPushButton>(dialog,"resetViewAppearance").click();apply(dialog);});
    require(reopened.document().revision()==reset_before.revision() && reopened.document().snapshot().entities()==reset_before.entities(),"repeated scoped reset is a true no-op");
    select_view(reopened,"appearance-a");styled(reopened,wall,"#b12d73");
    require(!canvas(reopened).labels().empty() && !canvas(reopened).references().empty(),"visible saved view restores real annotations and references");
    editor(reopened,[](QDialog& dialog){child<QCheckBox>(dialog,"viewAppearanceInheritStyle").setChecked(false);child<QLineEdit>(dialog,"viewOutlineColor").setText("red");apply(dialog);
        require(dialog.isVisible() && !child<QLabel>(dialog,"viewAppearanceError").text().isEmpty(),"invalid color keeps appearance editor open with error");dialog.reject();});
    // Explicit local object visibility recovers global presentation hiding,
    // while the organization mask remains authoritative.
    auto snapshot=reopened.document().snapshot();sketch::AnnotationState annotations;
    sketch::PresentationOverride global;global.target_kind="object";global.target_id=wall.toStdString();global.visible=false;
    annotations.overrides.push_back(global);
    auto global_owner=sketch::make_annotation_entity("appearance-global-hidden",annotations);
    reopened.document().apply(sketch::ApplyEntityChanges{snapshot.revision(),{sketch::EntityChange::upsert(global_owner)}, {},"fixture global presentation visibility"});
    require(reopened.selectEntity({}),"refresh global visibility");select_view(reopened,"appearance-a");
    require(!has(reopened,wall),"absent view visibility inherits global object hiding");
    editor(reopened,[&](QDialog& dialog){scope(dialog,wall);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(true);apply(dialog);});
    require(has(reopened,wall),"explicit local visibility recovers globally hidden physical geometry");
    require(reopened.selectEntity({}),"clear selection before actual revealed-wall click");click_model(reopened,{3.5,0});
    require(reopened.selectedEntityId()==wall,"actual canvas click selects locally recovered wall");
    select_view(reopened,"appearance-b");require(!has(reopened,wall),"other saved view retains inherited global hiding");
    select_view(reopened,"appearance-a");QString floor;
    snapshot=reopened.document().snapshot();
    for(const auto& [id,entity]:snapshot.entities())if(entity.type=="floor"){floor=QString::fromStdString(id);break;}
    require(!floor.isEmpty() && reopened.setContainerVisible(floor,false),"fixture hides actual source floor");
    require(!has(reopened,wall),"local visibility cannot bypass hidden floor");
    require(reopened.setContainerVisible(floor,true) && has(reopened,wall),"restored floor recovers local object visibility");
    const auto before_delete=reopened.document().snapshot();
    require(reopened.selectEntity(wall) && reopened.deleteSelection(),"physical deletion repairs appearance references atomically");
    const auto deleted=reopened.document().snapshot();
    for(const auto& [id,entity]:deleted.entities())if(entity.type==sketch::kSheetViewEntityType) {
        (void)id;const auto model=sketch::decode_sheet_view_entity(entity);for(const auto& view:model.views())if(view.presentation.appearance)
            for(const auto& object:view.presentation.appearance->objects)
                require(object.object_id!=wall.toStdString() && object.object_id!=opening.toStdString() && object.object_id!=door.toStdString(),"deletion removes source and hosted-child appearance references");
    }
    require(reopened.undoCommand() && reopened.document().snapshot().entities()==before_delete.entities(),"one Undo restores source and exact view appearance references");
    // Legal graph-local view IDs must not address another graph's cache/editor.
    snapshot=reopened.document().snapshot();auto duplicate=snapshot.entities().at(owner.id);duplicate.id="zz-appearance-owner";
    duplicate.extensions["duplicate_graph"]=true;
    for(auto& view:duplicate.properties["model"]["views"])if(view["id"]=="appearance-a")
        view["presentation"]["appearance"]["style"]["outline_color"]="#23b98a";
    reopened.document().apply(sketch::ApplyEntityChanges{snapshot.revision(),{sketch::EntityChange::upsert(duplicate)}, {},"fixture graph-local duplicate view IDs"});
    require(reopened.selectEntity({}),"refresh duplicate graphs");
    select_view(reopened,"appearance-a",QString::fromStdString(owner.id));styled(reopened,wall,"#b12d73");
    select_view(reopened,"appearance-a",QString::fromStdString(duplicate.id));styled(reopened,wall,"#23b98a");
    const auto primary_before=reopened.document().snapshot().entities().at(owner.id);
    style(reopened,{},"#dd5588","#31ba69");styled(reopened,wall,"#dd5588");
    require(reopened.document().snapshot().entities().at(owner.id)==primary_before,"appearance controls address exact selected graph owner");
    require(!reopened.editArchitecturalViewPresentation("appearance-a","1.2","100",".5",".18",true,"solid","1","medium"),"legacy view-ID-only edit refuses ambiguous identity");
    select_view(reopened,"appearance-a",QString::fromStdString(owner.id));styled(reopened,wall,"#b12d73");
    editor(reopened,[&](QDialog& dialog){child<QCheckBox>(dialog,"viewAppearanceInheritStyle").setChecked(false);child<QLineEdit>(dialog,"viewOutlineColor").setText("#111111");
        auto snapshot=reopened.document().snapshot();auto entity=snapshot.entities().at(wall.toStdString());entity.extensions["stale_fixture"]=true;
        reopened.document().apply(sketch::ApplyEntityChanges{snapshot.revision(),{sketch::EntityChange::upsert(entity)}, {},"stale modal fixture"});
        const auto stale=reopened.document().snapshot();apply(dialog);require(dialog.isVisible() && !child<QLabel>(dialog,"viewAppearanceError").text().isEmpty(),"stale modal refuses Apply");
        require(reopened.document().snapshot().entities()==stale.entities(),"stale Apply preserves newer work");dialog.reject();});
    editor(reopened,[&](QDialog& dialog){child<QLineEdit>(dialog,"viewOutlineColor").setText("#111111");reopened.document().mark_read_only("fixture policy");
        const auto snapshot=reopened.document().snapshot();apply(dialog);require(dialog.isVisible() && child<QLabel>(dialog,"viewAppearanceError").text().contains("read-only"),"read-only transition refuses Apply");
        require(reopened.document().snapshot().entities()==snapshot.entities(),"read-only Apply preserves source");dialog.reject();});
}
void saved_view_clipboard() {
    QTemporaryDir dir;MainWindow window({},nullptr,dir.filePath("clipboard-library.json"));display(window);
    const auto wall=window.createStraightWall({0,0},{4,0});require(!wall.isEmpty(),"clipboard fixture creates source wall");
    const auto original=window.document().snapshot();
    for(const int version:{1,2,7}) {
        sketch::CoordinatedView view;view.id="clipboard-view";view.name="Clipboard view";
        view.object_ids={wall.toStdString()};
        if(version==7) {
            view.presentation.appearance=sketch::ViewAppearance{};
            view.presentation.appearance->objects.push_back({wall.toStdString(),sketch::ViewAppearanceStyle{},false});
        }
        auto graph=sketch::make_sheet_view_entity("clipboard-owner",sketch::SheetViewModel::create({view},{ }));
        graph.extensions["opaque_identity"]=wall.toStdString();
        auto& model=graph.properties["model"];model["version"]=version;
        if(version<7) {
            model.erase("sheet_order");
            for(auto& item:model["views"]) {
                item.erase("overlays");item.erase("restrict_to_objects");item["presentation"].erase("crop");
                if(version==1)item.erase("object_ids");
            }
        }
        const auto record=[](const sketch::Entity& entity) {
            return nlohmann::json{{"id",entity.id},{"type",entity.type},{"properties",entity.properties},
                {"required",false},{"extensions",entity.extensions}};
        };
        const nlohmann::json payload{{"format","sketch.document.clipboard"},{"version",1},
            {"root_ids",{graph.id}},{"entities",{record(graph),record(original.entities().at(wall.toStdString()))}}};
        QGuiApplication::clipboard()->setText(QString::fromStdString(payload.dump()));
        const auto before=window.document().snapshot();
        if(!window.pasteSelection())throw std::runtime_error("saved-view clipboard paste: "+window.lastError().toStdString());
        const auto after=window.document().snapshot();const auto pasted=after.entities().at(window.selectedEntityId().toStdString());
        require(pasted.type==sketch::kSheetViewEntityType && pasted.extensions==graph.extensions,"clipboard preserves opaque graph identity data");
        const auto decoded=sketch::decode_sheet_view_entity(pasted);
        if(version==1)require(decoded.views()[0].object_ids.empty(),"version 1 clipboard retains absent source list");
        else {
            const auto target=decoded.views()[0].object_ids.at(0);
            require(target!=wall.toStdString() && after.entities().at(target).type=="wall","clipboard remaps graph source to pasted wall");
            if(version==7)require(decoded.views()[0].presentation.appearance->objects[0].object_id==target,
                "clipboard remaps view appearance source without rewriting opaque strings");
        }
        if(version<3)require(!pasted.properties.at("model").at("views")[0].contains("overlays"),"legacy clipboard does not invent absent fields");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"one Undo restores exact pre-paste source");
    }
}
void hosted_visibility_and_section_identity() {
    QTemporaryDir dir;MainWindow window({},nullptr,dir.filePath("sections-library.json"));display(window);
    const auto wall=window.createStraightWall({0,0},{4,0});require(!wall.isEmpty(),"solid-view fixture creates actual wall");
    const auto opening=window.createHostedOpening("window",".6 m",".8 m",".8 m","1 m");
    require(!opening.isEmpty() && window.selectEntity(wall),"solid-view fixture creates actual hosted window");
    const auto door=window.createHostedOpening("door","2 m",".8 m","0 m","2 m",std::nullopt,sketch::DoorOperation{false,true,90.0});
    require(!door.isEmpty(),"solid-view fixture creates actual hosted door");
    auto source=window.document().snapshot();sketch::Entity owner;
    for(const auto& [id,entity]:source.entities())if(entity.type==sketch::kSheetViewEntityType){(void)id;owner=entity;break;}
    sketch::CoordinatedView elevation;elevation.id="shared-elevation";elevation.name="Primary elevation";
    elevation.kind=sketch::CoordinatedViewKind::elevation;elevation.direction={0,-1,0};elevation.up={0,0,1};
    sketch::CoordinatedView section;section.id="shared-section";section.name="Primary section";
    section.kind=sketch::CoordinatedViewKind::section;section.origin_m={0,0,2.4};
    sketch::SectionOverlay primary_note;primary_note.id="actual-note";primary_note.text="Canonical section note";
    primary_note.start_m={1,1};section.overlays={primary_note};
    sketch::DrawingSheet sheet;sheet.id="section-appearance-sheet";sheet.number="S-001";
    sheet.viewports={{"section-elevation",elevation.id,{10,10,195,180},50},{"section-cut",section.id,{215,10,195,180},50}};
    owner.properties=sketch::make_sheet_view_entity(owner.id,sketch::SheetViewModel::create({elevation,section},{sheet})).properties;
    auto hidden_section=section;hidden_section.name="Secondary hidden section";
    hidden_section.overlays[0].text="Hidden secondary section note";
    sketch::SectionOverlay extreme_dimension;extreme_dimension.id="hidden-extreme-dimension";
    extreme_dimension.kind=sketch::SectionOverlayKind::dimension;
    extreme_dimension.dimension_binding=sketch::SectionDimensionBinding{wall.toStdString(),sketch::SectionDimensionAxis::horizontal,1e6};
    hidden_section.overlays.push_back(extreme_dimension);hidden_section.presentation.appearance=sketch::ViewAppearance{};
    hidden_section.presentation.appearance->visible=false;
    auto secondary=sketch::make_sheet_view_entity("zz-secondary-section-owner",sketch::SheetViewModel::create({elevation,hidden_section},{sheet}));
    sketch::AnnotationState annotations;sketch::PresentationOverride global;
    global.target_kind="object";global.target_id=wall.toStdString();global.visible=false;annotations.overrides.push_back(global);
    const auto global_owner=sketch::make_annotation_entity("section-global-hidden-wall",annotations);
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(owner),
        sketch::EntityChange::upsert(secondary),sketch::EntityChange::upsert(global_owner)}, {},"fixture solid host masks and graph-local section annotations"});
    require(window.selectEntity({}),"refresh solid-view fixture");
    for(const auto* id:{"shared-elevation","shared-section"}) {
        select_view(window,id,QString::fromStdString(owner.id));
        require(!has(window,wall) && !has(window,opening) && !has(window,door),"globally hidden host suppresses inherited door/window solid glyphs");
        editor(window,[&](QDialog& dialog){scope(dialog,opening);child<QCheckBox>(dialog,"viewAppearanceInheritVisibility").setChecked(false);
            child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(true);apply(dialog);});
        require(!has(window,opening),"explicit child visibility cannot bypass hidden host");
        editor(window,[&](QDialog& dialog){scope(dialog,wall);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(true);apply(dialog);});
        require(has(window,wall) && has(window,opening) && has(window,door),"local host recovery restores inherited real door and window geometry");
        editor(window,[&](QDialog& dialog){scope(dialog,opening);child<QCheckBox>(dialog,"viewAppearanceVisible").setChecked(false);apply(dialog);});
        require(has(window,wall) && has(window,door) && !has(window,opening),"local host recovery respects child's own hidden intent");
        editor(window,[&](QDialog& dialog){scope(dialog,opening);child<QPushButton>(dialog,"resetViewAppearance").click();apply(dialog);});
        require(has(window,opening),"child reset inherits visible host in solid projection");
    }
    auto& views=child<QComboBox>(window,"architecturalView");views.setCurrentIndex(2);QApplication::processEvents();
    const auto has_note=[&](const char* text){return std::any_of(canvas(window).labels().begin(),canvas(window).labels().end(),
        [&](const auto& label){return label.text==QString::fromLatin1(text);});};
    require(has_note("Canonical section note") && !has_note("Hidden secondary section note"),"built-in section resolves notes from same canonical owner/view as geometry");
    capture(canvas(window),"output-view-canonical-section-native-screen.png");
    select_view(window,"shared-section",QString::fromStdString(secondary.id));
    require(canvas(window).entities().empty() && canvas(window).labels().empty(),"hidden secondary named section suppresses its actual notes and dimension overlays");
    select_view(window,"shared-section",QString::fromStdString(owner.id));
    require(has_note("Canonical section note") && !has_note("Hidden secondary section note"),"named section resolves exact graph-local annotation identity");
    require(window.selectOutputSheet("section-appearance-sheet"),"select visible solid-view sheet");
    if(!window.exportDraftPdf(dir.filePath("hidden-section-dimension.pdf")))
        throw std::runtime_error("hidden presentation dimension must not block visible sheet output: "+window.lastError().toStdString());
    source=window.document().snapshot();auto revealed=source.entities().at(secondary.id);
    for(auto& view:revealed.properties["model"]["views"])if(view["id"]=="shared-section")view["presentation"]["appearance"]["visible"]=true;
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(revealed)}, {},"fixture visible unsupported dimension placement"});
    require(window.selectEntity({}),"refresh visible dimension failure");
    require(!window.exportDraftPdf(dir.filePath("visible-section-dimension.pdf")),"visible unresolved dimension retains output refusal");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("OutputViewAppearance");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    try {require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"bundled test font loads");
        saved_view_clipboard();
        if(app.arguments().contains("--constraint-only"))saved_view_constraint_previews();
        else if(!app.arguments().contains("--clipboard-only")){workflow();hosted_visibility_and_section_identity();saved_view_constraint_previews();}
        std::cout<<"output_view_appearance_desktop_tests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<"output_view_appearance_desktop_tests: "<<error.what()<<'\n';return 1;}
}
