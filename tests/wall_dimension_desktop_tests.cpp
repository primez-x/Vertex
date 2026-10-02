#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/geometry.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>

namespace {
using sketch::desktop::MainWindow;
using sketch::desktop::PlanCanvas;

void require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
PlanCanvas& canvas(MainWindow& window,const char* name="measurementPlanCanvas") {
    auto* value=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QString::fromLatin1(name)));
    require(value,"wall presentation needs actual plan canvas");return *value;
}
void display(MainWindow& window) {
    window.setMetricUnits(true);window.resize(1100,780);window.show();QApplication::processEvents();
    auto& drawing=canvas(window);drawing.setOverviewMapEnabled(false);drawing.setSnapEnabled(false);drawing.fitView();
}
std::optional<sketch::desktop::CanvasLabel> label(PlanCanvas& drawing,const QString& id) {
    for(const auto& value:drawing.labels()) if(value.id==id && value.plan_only) return value;
    return std::nullopt;
}
sketch::desktop::CanvasLabel visible_label(MainWindow& window,const QString& id) {
    const auto value=label(canvas(window),id);require(value.has_value(),"wall measurement must remain visible");return *value;
}
sketch::PresentationOverride presentation(const MainWindow& window,const QString& id) {
    const auto snapshot=window.document().snapshot();
    for(const auto& [owner_id,entity]:snapshot.entities()) {
        (void)owner_id;if(entity.type!=sketch::kAnnotationEntityType) continue;
        const auto state=sketch::decode_annotation_entity(entity);
        for(const auto& value:state.overrides)
            if(value.target_kind=="wall_dimension" && value.target_id==id.toStdString()) return value;
    }
    throw std::runtime_error("saved wall dimension presentation is missing");
}
QPointF screen(const PlanCanvas& drawing,sketch::Vec2 point) {
    return QRectF(drawing.rect()).center()+QPointF((point.x-drawing.viewCenter().x)*drawing.viewScale(),
        -(point.y-drawing.viewCenter().y)*drawing.viewScale());
}
void mouse(PlanCanvas& drawing,QEvent::Type type,QPointF point) {
    QMouseEvent event(type,point,drawing.mapToGlobal(point.toPoint()),
        type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&drawing,&event);
}
void middle_pan(PlanCanvas& drawing,QPointF start,QPointF end) {
    const auto send=[&](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
        QMouseEvent event(type,point,drawing.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);
        QApplication::sendEvent(&drawing,&event);
    };
    send(QEvent::MouseButtonPress,start,Qt::MiddleButton,Qt::MiddleButton);
    send(QEvent::MouseMove,end,Qt::NoButton,Qt::MiddleButton);
    send(QEvent::MouseButtonRelease,end,Qt::MiddleButton,Qt::NoButton);
}
void click(PlanCanvas& drawing,sketch::Vec2 point) {
    const auto position=screen(drawing,point);mouse(drawing,QEvent::MouseButtonPress,position);mouse(drawing,QEvent::MouseButtonRelease,position);
}
void escape(PlanCanvas& drawing) {
    QKeyEvent event(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&drawing,&event);
}
QPushButton& button(MainWindow& window,const char* name) {
    auto* value=window.findChild<QPushButton*>(QString::fromLatin1(name));require(value,"wall measurement needs actual button");return *value;
}
QLineEdit& field(MainWindow& window,const char* name) {
    auto* value=window.findChild<QLineEdit*>(QString::fromLatin1(name));require(value,"wall measurement needs actual field");return *value;
}
QCheckBox& flag(MainWindow& window,const char* name) {
    auto* value=window.findChild<QCheckBox*>(QString::fromLatin1(name));require(value,"wall measurement needs actual checkbox");return *value;
}
void assert_model_unchanged(const sketch::DocumentSnapshot& before,const sketch::DocumentSnapshot& after) {
    for(const auto& [id,entity]:before.entities()) if(entity.type!=sketch::kAnnotationEntityType)
        require(after.entities().at(id)==entity,"wall measurement presentation must preserve model geometry, openings and organization");
    require(after.entities().size()==before.entities().size(),"wall measurement must not create model entities");
}
void capture(QWidget& widget,const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(directory.isEmpty()) return;
    require(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(name)),"actual wall dimension capture must save");
}
QString pdf_text(MainWindow& window,const QString& path,const QString& capture_name={},QColor expected_color={}) {
    require(window.exportDraftPdf(path),"wall measurement must export through actual plan PDF");
    QPdfDocument pdf;require(pdf.load(path)==QPdfDocument::Error::None && pdf.pageCount()>0,"actual wall dimension PDF must reopen");
    const auto rendered=pdf.render(0,QSize(1400,1000));
    if(expected_color.isValid()) {
        int pixels=0;
        for(int y=0;y<rendered.height();++y) for(int x=0;x<rendered.width();++x) {
            const auto color=rendered.pixelColor(x,y);
            if(std::abs(color.red()-expected_color.red())<35 && std::abs(color.green()-expected_color.green())<35 &&
                std::abs(color.blue()-expected_color.blue())<35) ++pixels;
        }
        require(pixels>8,"actual PDF must render the independent wall-callout text color");
    }
    if(!capture_name.isEmpty()) {
        const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if(!directory.isEmpty()) require(QDir().mkpath(directory) && QFile::copy(path,QDir(directory).filePath(capture_name+".pdf")) &&
            rendered.save(QDir(directory).filePath(capture_name+".png")),"retain actual wall dimension PDF for style review");
    }
    return pdf.getAllText(0).text().simplified();
}

QString curved_exterior_dimension_id(const sketch::DocumentSnapshot& snapshot,const QString& boundary_id) {
    const auto boundary=sketch::decode_identified_boundary_entity(
        snapshot.entities().at(boundary_id.toStdString()));
    for(const auto& [id,entity]:snapshot.entities()) {
        if(!sketch::can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded=sketch::decode_boundary_dimension_entity(entity);
        if(!decoded.dimension || decoded.dimension->boundary_id!=boundary_id.toStdString() ||
            decoded.dimension->kind!=sketch::BoundaryDimensionKind::segment_length) continue;
        const auto edge=std::find_if(boundary.segments.begin(),boundary.segments.end(),[&](const auto& value) {
            return value.segment_id==decoded.dimension->segment_id;
        });
        if(edge!=boundary.segments.end() && std::abs(edge->segment.sweep_radians)>1e-9)
            return QString::fromStdString(id);
    }
    throw std::runtime_error("curved exterior must retain an analytical arc dimension");
}

std::optional<sketch::desktop::CanvasLabel> any_canvas_label(PlanCanvas& drawing,const QString& id) {
    for(const auto& value:drawing.labels()) if(value.id==id) return value;
    return std::nullopt;
}

sketch::desktop::CanvasLabel visible_canvas_label(MainWindow& window,const QString& id) {
    const auto value=any_canvas_label(canvas(window),id);
    require(value.has_value(),"curved physical wall and exterior dimensions remain visible in the plan");
    return *value;
}

void require_label_content_and_presentation_unchanged(
    const sketch::desktop::CanvasLabel& before,const sketch::desktop::CanvasLabel& after) {
    require(before.id==after.id && before.position.x==after.position.x && before.position.y==after.position.y &&
        before.text==after.text && before.rotation_radians==after.rotation_radians &&
        before.paper_height_mm==after.paper_height_mm && before.color==after.color &&
        before.bold==after.bold && before.italic==after.italic,
        "selecting and navigating the outline must not change dimension value or presentation");
}
bool same_optional_point(const std::optional<sketch::Vec2>& left,const std::optional<sketch::Vec2>& right) {
    return left.has_value()==right.has_value() && (!left ||
        (left->x==right->x && left->y==right->y));
}

void actual_wall_controls_preserve_geometry_and_raw_siblings() {
    QTemporaryDir directory;require(directory.isValid(),"wall measurement fixture needs temporary project directory");
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));window.setMetricUnits(true);
    const auto wall=window.createStraightWall({0,0},{4,0});
    require(!wall.isEmpty(),"wall presentation fixture needs a real wall");
    const auto opening=window.createHostedOpening(QStringLiteral("door"),QStringLiteral("0.6 m"),QStringLiteral("0.8 m"),
        QStringLiteral("0 m"),QStringLiteral("2 m"));
    require(!opening.isEmpty(),"wall presentation fixture needs a hosted opening");
    require(!window.createAnnotationLabel(QStringLiteral("note"),QStringLiteral("Unrelated retained annotation"),{.5,1.5}).isEmpty() &&
        !window.createAnnotationSymbol(QStringLiteral("desk"),{3,2}).isEmpty(),"seed unrelated text and pinned artwork before metadata");
    auto source=window.document().snapshot();auto owner=source.entities().at("annotations-1");
    owner.required=true;owner.extensions["vendor_wall_dimensions"]={{"retain",17}};
    owner.properties["state"]["vendor_state"]="retain";owner.properties["state"]["labels"][0]["vendor_label"]="retain";
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),{sketch::EntityChange::upsert(owner)}, {}, "seed raw dimension siblings"});
    display(window);require(window.selectEntity(wall),"select wall for actual Dimension controls");
    mouse(canvas(window),QEvent::MouseButtonDblClick,screen(canvas(window),{2.5,0}));
    QApplication::processEvents();
    auto* group=window.findChild<QGroupBox*>(QStringLiteral("dimensionProperties"));
    require(group && group->isVisible() && button(window,"applyBoundaryDimension").isEnabled(),"selected wall must offer enabled Dimension controls");
    const auto defaults=visible_label(window,wall);require(defaults.text==QStringLiteral("4.000 m"),"default wall value must derive its analytical length");
    const auto untouched=window.document().snapshot();button(window,"applyBoundaryDimension").click();
    require(window.document().revision()==untouched.revision() && window.document().snapshot().entities()==untouched.entities() &&
        visible_label(window,wall).automatic_linear_placement,"untouched Apply must preserve automatic placement without creating a presentation record");
    field(window,"dimensionPositionX").setText(QStringLiteral("1 m"));field(window,"dimensionPositionY").setText(QStringLiteral("1 m"));
    field(window,"dimensionTextHeightMm").setText(QStringLiteral("5.5"));field(window,"dimensionColor").setText(QStringLiteral("#A12B34"));
    field(window,"dimensionRotationDegrees").setText(QStringLiteral("25"));
    flag(window,"dimensionBold").setChecked(true);flag(window,"dimensionItalic").setChecked(true);flag(window,"dimensionVisible").setChecked(true);
    const auto before=window.document().snapshot();button(window,"applyBoundaryDimension").click();auto styled=window.document().snapshot();
    assert_model_unchanged(before,styled);
    require(styled.revision()==before.revision()+1,"Apply wall measurement must be one presentation history command");
    const auto styled_owner=styled.entities().at(owner.id);auto stripped=styled_owner;
    stripped.properties["state"]["overrides"]=owner.properties.at("state").at("overrides");
    stripped.properties["state"]["version"]=owner.properties.at("state").at("version");
    require(stripped==owner,"wall dimension Apply must preserve exact raw owner, unrelated text and pinned artwork records");
    const auto styled_label=visible_label(window,wall);const auto record=presentation(window,wall);
    require(std::abs(styled_label.position.x-1)<1e-9 && std::abs(styled_label.position.y-1)<1e-9 &&
        styled_label.paper_height_mm==5.5 && styled_label.color==QColor(QStringLiteral("#A12B34")) && styled_label.bold && styled_label.italic &&
        std::abs(styled_label.rotation_radians-25*std::numbers::pi/180)<1e-12 && !styled_label.automatic_linear_placement &&
        record.plan_label_offset && std::abs(record.plan_label_offset->x+1)<1e-9 && std::abs(record.plan_label_offset->y-1)<1e-9,
        "actual Apply must change only derived wall measurement position and presentation");
    QApplication::processEvents();
    auto* editor=window.findChild<QScrollArea*>(QStringLiteral("contextEditor"));require(editor,"actual properties must be scrollable");
    editor->ensureWidgetVisible(group,0,8);QApplication::processEvents();
    require(button(window,"applyBoundaryDimension").height()>=28 && button(window,"placeWallMeasurement").height()>=28,
        "measurement buttons must retain usable heights in the compact scrolling editor");
    capture(window,QStringLiteral("wall-measurement-controls.png"));
    capture(*group,QStringLiteral("wall-measurement-control-group.png"));
    auto raw_owner=styled.entities().at(owner.id);
    for(auto& value:raw_owner.properties["state"]["overrides"]) if(value.at("target_kind")=="wall_dimension") {
        value["vendor_dimension"]="retain";value["style"]["vendor_dimension_style"]="retain";
    }
    window.document().apply(sketch::ApplyEntityChanges{styled.revision(),{sketch::EntityChange::upsert(raw_owner)}, {}, "seed raw wall dimension metadata"});
    require(window.selectEntity(wall),"reselect wall after raw metadata seed");styled=window.document().snapshot();
    sketch::TextLibraryEntry reusable{"user-text-wall-compatibility","Wall annotation","notes","Authored after wall presentation",{}};
    require(window.beginTextPlacement(reusable),"v6 presentation must coexist with authored plan text placement");click(canvas(window),{1,2});
    const auto text_added=window.document().snapshot();assert_model_unchanged(styled,text_added);
    require(text_added.entities().at(owner.id).properties.at("state").at("version")==6 &&
        presentation(window,wall).paper_text_height_mm==5.5 && window.undoCommand() &&
        window.document().snapshot().entities()==styled.entities() && window.selectEntity(wall),
        "authored plan text must preserve raw v6 wall presentation without downgrading the shared annotation owner");
    capture(canvas(window),QStringLiteral("styled-wall-measurement.png"));
    capture(window,QStringLiteral("wall-measurement-properties.png"));
    const auto visible_pdf=pdf_text(window,directory.filePath(QStringLiteral("visible-wall.pdf")),QStringLiteral("styled-wall-measurement-pdf"),styled_label.color);
    require(visible_pdf.contains(QStringLiteral("4.000 m")),"styled wall value must remain extractable in actual PDF");
    button(window,"automaticWallMeasurement").click();const auto automatic_style=visible_label(window,wall);
    require(automatic_style.automatic_linear_placement && automatic_style.paper_height_mm==styled_label.paper_height_mm &&
        automatic_style.color==styled_label.color && automatic_style.bold==styled_label.bold && automatic_style.italic==styled_label.italic &&
        automatic_style.rotation_radians==styled_label.rotation_radians && window.undoCommand() &&
        window.document().snapshot().entities()==styled.entities(),"Automatic must clear position only while preserving explicit callout style and rotation");
    flag(window,"dimensionVisible").setChecked(false);const auto before_hide=window.document().snapshot();button(window,"applyBoundaryDimension").click();
    const auto hidden=window.document().snapshot();assert_model_unchanged(before_hide,hidden);
    auto expected_hidden=before_hide.entities();
    for(auto& value:expected_hidden.at(owner.id).properties["state"]["overrides"])
        if(value.at("target_kind")=="wall_dimension") value["visible"]=false;
    require(hidden.entities()==expected_hidden,"visibility edit must preserve exact raw dimension fields and all unrelated records");
    require(hidden.revision()==before_hide.revision()+1 && !label(canvas(window),wall) &&
        std::any_of(canvas(window).entities().begin(),canvas(window).entities().end(),[&](const auto& value){return value.id==wall;}) &&
        hidden.entities().contains(opening.toStdString()),"hide must remove only the callout and retain wall plus opening");
    require(!pdf_text(window,directory.filePath(QStringLiteral("hidden-wall.pdf"))).contains(QStringLiteral("4.000 m")),
        "hidden wall callout must also be absent from actual PDF output");
    require(window.undoCommand() && window.document().snapshot().entities()==styled.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==hidden.entities() && window.undoCommand(),"wall measurement visibility must undo/redo independently of wall geometry");
    flag(window,"dimensionVisible").setChecked(false);button(window,"applyBoundaryDimension").click();
    require(!label(canvas(window),wall) && window.selectEntity(wall),"a hidden callout must leave its wall selectable for restoration");
    flag(window,"dimensionVisible").setChecked(true);button(window,"applyBoundaryDimension").click();
    require(label(canvas(window),wall) && window.document().snapshot().entities()==styled.entities(),
        "actual Visible control must restore a hidden measurement without changing its style or geometry");
    require(window.selectEntity(wall),"reselect wall before measured length edit");
    const auto before_length=window.document().snapshot();require(window.editSelectedLength(QStringLiteral("6 m")),"wall must support ordinary length edit");
    const auto resized=window.document().snapshot();const auto resized_label=visible_label(window,wall);
    const auto resized_presentation=presentation(window,wall);
    require(resized_label.text==QStringLiteral("6.000 m") && resized_label.paper_height_mm==styled_label.paper_height_mm &&
        resized_label.color==styled_label.color && resized_label.bold==styled_label.bold && resized_label.italic==styled_label.italic &&
        resized_presentation.plan_label_offset && record.plan_label_offset &&
        resized_presentation.plan_label_offset->x==record.plan_label_offset->x &&
        resized_presentation.plan_label_offset->y==record.plan_label_offset->y &&
        resized.entities().at(owner.id)==before_length.entities().at(owner.id),
        "length edits must derive a new numerical value while preserving independent callout style and midpoint offset");
    require(window.undoCommand() && window.document().snapshot().entities()==before_length.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==resized.entities(),"ordinary wall edit undo/redo must restore derived measurement values");
    const auto path=directory.filePath(QStringLiteral("wall-measurement.bldproj"));require(window.saveProjectAs(path),"styled wall project must save");
    MainWindow reopened({},nullptr,directory.filePath(QStringLiteral("missing-library.json")));
    const auto opened=reopened.openProject(path);
    if(!opened) std::cerr<<"Reopen failed: "<<reopened.lastError().toStdString()<<'\n';
    require(opened,"styled wall project must reopen");
    require(reopened.document().snapshot().entities()==resized.entities(),"native reopen must preserve exact wall presentation entities");
    // Display units belong to the workspace, not a cached numerical annotation.
    reopened.setMetricUnits(true);
    const auto reopened_label=visible_label(reopened,wall);
    if(reopened_label.text!=QStringLiteral("6.000 m")) std::cerr<<"Reopened value: "<<reopened_label.text.toStdString()<<'\n';
    require(
        visible_label(reopened,wall).text==QStringLiteral("6.000 m") && visible_label(reopened,wall).paper_height_mm==5.5 &&
        visible_label(reopened,wall).color==styled_label.color,"native reopen must restore wall measurement style and current derived quantity");
}

void place_pan_cancel_automatic_and_fences() {
    QTemporaryDir directory;MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
    window.setMetricUnits(true);const auto wall=window.createStraightWall({0,0},{4,0});require(!wall.isEmpty(),"placement fixture needs wall");
    display(window);require(window.selectEntity(wall),"select wall for placement controls");auto& drawing=canvas(window);
    const auto original=window.document().snapshot();const auto default_label=visible_label(window,wall);
    require(button(window,"placeWallMeasurement").isEnabled() && button(window,"automaticWallMeasurement").isEnabled(),"wall needs actual Place and Automatic controls");
    button(window,"placeWallMeasurement").click();const auto center=drawing.viewCenter();const auto start=QRectF(drawing.rect()).center();
    const auto moved=start+QPointF(80,35);mouse(drawing,QEvent::MouseButtonPress,start);mouse(drawing,QEvent::MouseMove,moved);mouse(drawing,QEvent::MouseButtonRelease,moved);
    require(window.document().snapshot().entities()==original.entities() &&
        (std::abs(drawing.viewCenter().x-center.x)>1e-6 || std::abs(drawing.viewCenter().y-center.y)>1e-6),
        "armed wall callout left-drag must pan without changing wall or inserting presentation");
    click(drawing,{2,1});const auto placed=window.document().snapshot();assert_model_unchanged(original,placed);
    const auto placed_label=visible_label(window,wall);
    require(placed.revision()==original.revision()+1 && std::abs(placed_label.position.x-2)<1e-9 && std::abs(placed_label.position.y-1)<1e-9 &&
        !placed_label.automatic_linear_placement && presentation(window,wall).inherit_appearance &&
        placed_label.color==default_label.color && placed_label.bold==default_label.bold && !drawing.boundaryDraftPreview(),
        "stationary Place click must save one theme-safe offset and consume selected-wall input before geometry gestures");
    capture(drawing,QStringLiteral("placed-wall-measurement.png"));
    button(window,"automaticWallMeasurement").click();const auto automatic=visible_label(window,wall);
    require(automatic.automatic_linear_placement && std::abs(automatic.position.x-default_label.position.x)<1e-9 &&
        std::abs(automatic.position.y-default_label.position.y)<1e-9 && window.undoCommand() && window.document().snapshot().entities()==placed.entities(),
        "Automatic must reinstate derived placement and undo must restore the exact saved offset");
    button(window,"placeWallMeasurement").click();const auto cancel_source=window.document().snapshot();escape(drawing);
    require(window.document().revision()==cancel_source.revision() && window.document().snapshot().entities()==cancel_source.entities() &&
        window.selectedEntityId()==wall,"Escape must cancel wall measurement placement without deselecting or changing history");
    button(window,"placeWallMeasurement").click();const auto stale_revision=window.document().revision();
    auto property=window.document().snapshot().entities().at("property-1");property.properties["external_dimension_test"]="retain";
    window.document().apply(sketch::ApplyEntityChanges{stale_revision,{sketch::EntityChange::upsert(property)}, {}, "external wall measurement fixture edit"});
    const auto newer=window.document().snapshot();click(drawing,{1,1});
    require(window.document().revision()==newer.revision() && window.document().snapshot().entities()==newer.entities() && !window.lastError().isEmpty(),
        "stale wall measurement placement must preserve newer project data");
    require(!window.editSelectedWallDimension(QStringLiteral("1 m"),QStringLiteral("1 m"),QStringLiteral("4"),QStringLiteral("#123456"),
        false,false,true,QStringLiteral("0"),stale_revision) && window.document().snapshot().entities()==newer.entities(),
        "revision-fenced direct presentation edits must refuse stale commands");
    require(window.selectEntity(wall) && window.beginSelectedPlanLabelPlacement(),"arm placement before read-only transition");
    window.document().mark_read_only("wall measurement read-only fixture");click(drawing,{1,1});
    require(!window.beginSelectedPlanLabelPlacement() && !window.setSelectedPlanLabelPosition({1,1}) &&
        !window.resetSelectedPlanLabelPlacement() && !window.editSelectedWallDimension(QStringLiteral("1"),QStringLiteral("1"),QStringLiteral("4"),
        QStringLiteral("#123456"),false,false,true,QStringLiteral("0")) && window.document().revision()==newer.revision() &&
        window.document().snapshot().entities()==newer.entities(),"read-only placement and style commands must refuse all mutations");
}

void named_horizontal_wall_measurement_placement() {
    for(const auto direction_z:{-1.0,1.0}) {
        QTemporaryDir directory;MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
        window.setMetricUnits(true);const auto wall=window.createStraightWall({0,0},{4,0});require(!wall.isEmpty(),"named plan needs real wall");display(window);
        auto sheet=window.document().snapshot().entities().at("sheet-view-1");const auto model=sketch::decode_sheet_view_entity(sheet);auto views=model.views();
        auto view=std::find_if(views.begin(),views.end(),[](const auto& value){return value.id=="view-plan";});require(view!=views.end(),"named fixture needs saved plan");
        const auto angle=std::numbers::pi/6;view->origin_m={10,-4,direction_z<0 ? 10.0 : -10.0};
        view->direction={0,0,direction_z};view->up={-std::sin(angle),std::cos(angle),0};
        sheet.properties["model"]=sketch::SheetViewModel::create(views,model.sheets(),model.schedule_ids(),model.sheet_order()).to_json();
        window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),{sketch::EntityChange::upsert(sheet)}, {}, "named wall dimension frame"});
        window.setWorkspace(sketch::desktop::Workspace::architectural);auto* chooser=window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
        require(chooser,"named wall fixture needs actual view chooser");const auto index=chooser->findData(QStringLiteral("view-plan"),Qt::UserRole+1);
        require(index>=3,"named plan must remain selectable");chooser->setCurrentIndex(index);
        auto& drawing=canvas(window,"architecturalPlanCanvas");drawing.setOverviewMapEnabled(false);drawing.setSnapEnabled(false);drawing.fitView();QApplication::processEvents();
        const auto automatic=label(drawing,wall);
        require(automatic && automatic->automatic_linear_placement &&
            std::abs(automatic->rotation_radians)<=std::numbers::pi/2+1e-12 && !automatic->wall_dimension_manual_rotation,
            "automatic wall measurements must stay upright with projected baselines in rotated/reflected named plans");
        const auto projected_start=automatic->automatic_linear_placement->anchor.start;
        require(std::abs(projected_start.x-(-direction_z*(-10*std::cos(angle)+4*std::sin(angle))))<1e-8 &&
            std::abs(projected_start.y-(10*std::sin(angle)+4*std::cos(angle)))<1e-8,
            "named-plan automatic placement must use the projected baseline rather than world coordinates");
        require(window.selectEntity(wall) && window.beginSelectedPlanLabelPlacement(),"named horizontal wall must accept callout placement");
        drawing.setSnapEnabled(false); // Selection refresh restores the workspace's snap preference.
        const auto before=window.document().snapshot();const sketch::Vec2 world{2.5,1.0};
        const auto x=world.x-view->origin_m[0],y=world.y-view->origin_m[1];
        const sketch::Vec2 projected{-direction_z*(x*std::cos(angle)+y*std::sin(angle)),-x*std::sin(angle)+y*std::cos(angle)};
        click(drawing,projected);const auto after=window.document().snapshot();assert_model_unchanged(before,after);
        const auto record=presentation(window,wall);const auto projected_label=label(drawing,wall);
        require(after.revision()==before.revision()+1 && record.plan_label_offset && std::abs(record.plan_label_offset->x-.5)<1e-8 &&
            std::abs(record.plan_label_offset->y-1)<1e-8 && projected_label && std::abs(projected_label->position.x-projected.x)<1e-8 &&
            std::abs(projected_label->position.y-projected.y)<1e-8,"rotated/reflected plan placement must unproject clicks and project the same derived callout back");
        require(window.editSelectedWallDimension(QStringLiteral("2.5 m"),QStringLiteral("1 m"),QStringLiteral("4"),
            QStringLiteral("#123456"),false,false,true,QStringLiteral("45")),"authored wall rotation must remain editable in named plans");
        const auto authored=label(drawing,wall);
        const auto expected_angle=std::atan2(std::sin(std::numbers::pi/4-angle),-direction_z*std::cos(std::numbers::pi/4-angle));
        require(authored && authored->wall_dimension_manual_rotation && std::abs(authored->rotation_radians-expected_angle)<1e-10,
            "explicit wall text angles must be projected faithfully instead of being upright-normalized");
    }
}

void curved_outline_selection_preserves_wall_and_exterior_callouts() {
    QTemporaryDir directory;require(directory.isValid(),"curved outline selection needs a temporary project directory");
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
    display(window);
    const auto arc_wall=window.createCurvedWall({-2,0},{2,0},QStringLiteral("180 deg"),QStringLiteral("exterior"));
    const QStringList walls{arc_wall,
        window.createStraightWall({2,0},{2,3},QStringLiteral("exterior")),
        window.createStraightWall({2,3},{-2,3},QStringLiteral("exterior")),
        window.createStraightWall({-2,3},{-2,0},QStringLiteral("exterior"))};
    require(std::all_of(walls.begin(),walls.end(),[](const auto& id){return !id.isEmpty();}),
        "the callout fixture needs one physical semicircle and its three-wall rectangular return");
    for(qsizetype i=0;i<walls.size();++i)
        require(window.selectEntity(walls.at(i),i!=0),"select every physical wall in the curved shell");
    const auto source=window.document().snapshot();
    require(std::abs(source.entities().at(arc_wall.toStdString()).properties.at("baseline")
        .at("sweep_radians").get<double>()-std::numbers::pi)<1e-12,
        "the physical curved wall retains its exact semicircular baseline");
    const auto boundary_id=window.createMeasurementBoundaryFromSelectedWalls();
    require(!boundary_id.isEmpty(),"the physical curved shell creates its derived exterior measurement");
    auto measured=window.document().snapshot();
    const auto identified=sketch::decode_identified_boundary_entity(
        measured.entities().at(boundary_id.toStdString()));
    require(identified.segments.size()==4 && std::count_if(identified.segments.begin(),identified.segments.end(),
        [](const auto& edge){return std::abs(edge.segment.sweep_radians)>1e-9;})==1,
        "the measured exterior retains one analytical curve alongside the rectangular return");
    const auto outside_arc=curved_exterior_dimension_id(measured,boundary_id);
    const auto outside_value=visible_canvas_label(window,outside_arc);
    require(outside_value.text==QStringLiteral("6.503 m"),
        "the measured exterior callout uses the curved physical face length");

    require(window.selectEntity(arc_wall) && window.editSelectedWallDimension(
        QStringLiteral("0 m"),QStringLiteral("-2.8 m"),QStringLiteral("5.5"),
        QStringLiteral("#A12B34"),true,true,true,QStringLiteral("32")),
        "the source curve accepts an authored manual rotated paper-font callout");
    QApplication::processEvents();
    const auto styled_snapshot=window.document().snapshot();
    const auto baseline_before=visible_label(window,arc_wall);
    const auto exterior_before=visible_canvas_label(window,outside_arc);
    require(baseline_before.text==QStringLiteral("6.283 m") &&
        std::abs(baseline_before.position.x)<1e-9 && std::abs(baseline_before.position.y+2.8)<1e-9 &&
        baseline_before.paper_height_mm==5.5 && baseline_before.color==QColor(QStringLiteral("#A12B34")) &&
        baseline_before.bold && baseline_before.italic && baseline_before.wall_dimension_manual_rotation &&
        std::abs(baseline_before.rotation_radians-32.0*std::numbers::pi/180.0)<1e-12 &&
        !baseline_before.automatic_linear_placement,
        "the source-wall callout shows its derived arc length and custom manual paper typography");
    const auto annotation_before=presentation(window,arc_wall);
    auto& drawing=canvas(window);
    drawing.fitView();QApplication::processEvents();
    capture(window,QStringLiteral("curved-outline-before-selection.png"));

    require(window.selectEntity(boundary_id),"select the measured exterior with both callouts visible");
    QApplication::processEvents();
    require(window.document().revision()==styled_snapshot.revision() &&
        window.document().snapshot().entities()==styled_snapshot.entities() && drawing.selectionBounds().has_value(),
        "selecting the derived outline must only show selection controls and must not edit model or annotation records");
    const auto baseline_selected=visible_label(window,arc_wall);
    const auto exterior_selected=visible_canvas_label(window,outside_arc);
    require_label_content_and_presentation_unchanged(baseline_before,baseline_selected);
    require_label_content_and_presentation_unchanged(exterior_before,exterior_selected);
    const auto selected_annotation=presentation(window,arc_wall);
    require(same_optional_point(selected_annotation.plan_label_offset,annotation_before.plan_label_offset) &&
        presentation(window,arc_wall).paper_text_height_mm==annotation_before.paper_text_height_mm &&
        presentation(window,arc_wall).plan_label_rotation_radians==annotation_before.plan_label_rotation_radians,
        "outline selection leaves the saved wall callout offset, paper height and rotation unchanged");
    capture(window,QStringLiteral("curved-outline-selected-before-navigation.png"));

    const auto scale_before=drawing.viewScale();
    drawing.zoomBy(1.35,QRectF(drawing.rect()).center());
    const auto center_before_pan=drawing.viewCenter();
    const auto center=QRectF(drawing.rect()).center();
    middle_pan(drawing,center,center+QPointF(44,-27));
    QApplication::processEvents();
    require(drawing.viewScale()>scale_before &&
        (std::abs(drawing.viewCenter().x-center_before_pan.x)>1e-8 ||
         std::abs(drawing.viewCenter().y-center_before_pan.y)>1e-8),
        "the selected curved outline remains stable through real canvas zoom and pan gestures");
    require(window.document().revision()==styled_snapshot.revision() &&
        window.document().snapshot().entities()==styled_snapshot.entities(),
        "zooming and panning around the selected outline do not persist presentation changes");
    require_label_content_and_presentation_unchanged(baseline_before,visible_label(window,arc_wall));
    require_label_content_and_presentation_unchanged(exterior_before,visible_canvas_label(window,outside_arc));
    capture(window,QStringLiteral("curved-outline-selected-after-navigation.png"));

    const auto project_path=directory.filePath(QStringLiteral("curved-outline-callouts.bldproj"));
    require(window.saveProjectAs(project_path),"selected curved wall callouts must save natively");
    MainWindow reopened({},nullptr,directory.filePath(QStringLiteral("missing-library.json")));
    require(reopened.openProject(project_path),"the curved wall callout project must reopen");
    require(reopened.document().snapshot().entities()==styled_snapshot.entities(),
        "native reopen restores the exact curved model and dimension-presentation records");
    display(reopened);
    require(reopened.selectEntity(arc_wall),"reopen must retain the curved source wall selection");
    const auto reopened_baseline=visible_label(reopened,arc_wall);
    const auto reopened_annotation=presentation(reopened,arc_wall);
    require(reopened_baseline.text==baseline_before.text &&
        reopened_baseline.paper_height_mm==baseline_before.paper_height_mm &&
        reopened_baseline.color==baseline_before.color && reopened_baseline.bold==baseline_before.bold &&
        reopened_baseline.italic==baseline_before.italic &&
        reopened_baseline.rotation_radians==baseline_before.rotation_radians &&
        same_optional_point(reopened_annotation.plan_label_offset,annotation_before.plan_label_offset) &&
        reopened_annotation.paper_text_height_mm==annotation_before.paper_text_height_mm &&
        reopened_annotation.plan_label_rotation_radians==annotation_before.plan_label_rotation_radians,
        "native reopen restores the wall callout value, manual placement, rotation and paper typography");
    require(reopened.selectEntity(boundary_id),"reopen must retain and select the measured exterior outline");
    const auto reopened_exterior=visible_canvas_label(reopened,outside_arc);
    require(reopened_exterior.text==exterior_before.text && reopened_exterior.position.x==exterior_before.position.x &&
        reopened_exterior.position.y==exterior_before.position.y,
        "native reopen restores the analytical exterior curve dimension at its saved position");
    capture(reopened,QStringLiteral("curved-outline-selected-after-reopen.png"));

    const auto text=pdf_text(reopened,directory.filePath(QStringLiteral("curved-outline-callouts.pdf")),
        QStringLiteral("curved-outline-callouts-native-pdf"));
    require(text.contains(QStringLiteral("6.283 m")) && text.contains(QStringLiteral("6.503 m")),
        "native PDF output contains both the source-baseline and derived-exterior curve measurements");
    require(!text.contains(QStringLiteral("× D ")),
        "native PDF output omits the screen-only selection size badge");
}
}

int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled font must load for actual rendered measurements");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        actual_wall_controls_preserve_geometry_and_raw_siblings();place_pan_cancel_automatic_and_fences();named_horizontal_wall_measurement_placement();
        curved_outline_selection_preserves_wall_and_exterior_callouts();
        std::cout<<"wall_dimension_desktop_tests passed\n";return 0;
    } catch(const std::exception& error){std::cerr<<"wall_dimension_desktop_tests: "<<error.what()<<'\n';return 1;}
}
