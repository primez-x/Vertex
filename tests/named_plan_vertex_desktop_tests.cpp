#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_construction.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/quantity.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool close_enough(double a, double b) { return std::abs(a-b)<1e-7; }
bool close_enough(Vec2 a, Vec2 b) { return close_enough(a.x,b.x) && close_enough(a.y,b.y); }
bool same_path(const Boundary& a, const Boundary& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto& x,const auto& y) {
        return close_enough(x.start,y.start) && close_enough(x.end,y.end) && close_enough(x.sweep_radians,y.sweep_radians);
    });
}
Vec2 project(Vec2 p, const CoordinatedView& view) {
    const auto length=std::hypot(view.up[0],view.up[1]);
    const auto sign=-view.direction[2];
    const auto x=p.x-view.origin_m[0],y=p.y-view.origin_m[1];
    return {sign*(x*view.up[1]-y*view.up[0])/length,
            (x*view.up[0]+y*view.up[1])/length};
}
Boundary project_path(Boundary path, const CoordinatedView& view) {
    for (auto& edge : path) {
        edge.start=project(edge.start,view);
        edge.end=project(edge.end,view);
        if (view.direction[2]>0) edge.sweep_radians=-edge.sweep_radians;
    }
    return path;
}
void mouse(PlanCanvas& canvas,QEvent::Type type,Vec2 point) {
    const auto center=QRectF(canvas.rect()).center();
    const auto view=canvas.viewCenter();
    const QPointF pixel{center.x()+(point.x-view.x)*canvas.viewScale(),
                       center.y()-(point.y-view.y)*canvas.viewScale()};
    QMouseEvent event(type,pixel,canvas.mapToGlobal(pixel.toPoint()),
        type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
void wait_preview(MainWindow& window, bool require_active=true) {
    auto* timer=window.findChild<QTimer*>("boundaryVertexPreviewPoll");
    require(timer,"corner drag must have its deferred exact preview timer");
    if (!require_active && !timer->isActive()) return;
    require(timer->isActive(),"corner drag must request a deferred exact preview");
    QEventLoop loop;
    QTimer observe,deadline;
    observe.setInterval(10);
    deadline.setSingleShot(true);
    QObject::connect(&observe,&QTimer::timeout,&loop,[&] { if (!timer->isActive()) loop.quit(); });
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    observe.start(); deadline.start(15000); loop.exec();
    require(!timer->isActive(),"named plan exact preview did not finish");
}
Entity measured_rectangle() {
    BoundaryAuthoringOptions options;
    BoundaryAuthoringSession drawing(BoundaryAuthoringMode::draw_first,options);
    (void)drawing.anchor({0,0});
    (void)drawing.add_line_rise_run(parse_quantity("0 ft"),parse_quantity("12 ft"));
    (void)drawing.add_line_rise_run(parse_quantity("8 ft"),parse_quantity("0 ft"));
    (void)drawing.add_line_rise_run(parse_quantity("0 ft"),parse_quantity("-12 ft"));
    (void)drawing.add_line_rise_run(parse_quantity("-8 ft"),parse_quantity("0 ft"));
    drawing.classify_current_chain("living");
    const auto accepted=drawing.close_chain();
    auto entity=encode_identified_boundary_entity(accepted.boundary);
    entity.properties["classification"]="living";
    entity.properties["name"]="Dining";
    entity.properties["boundary_authoring"]=boundary_construction_envelope(accepted,options);
    // Boundary v1 owns its identified segments, not an optional vendor field
    // with the same name as native room/slab holes.
    entity.properties["holes"]=nlohmann::json::array({nlohmann::json::array({
        {{"start",{.5,.5}},{"end",{1,.5}},{"sweep_radians",0}},
        {{"start",{1,.5}},{"end",{1,1}},{"sweep_radians",0}},
        {{"start",{1,1}},{"end",{.5,1}},{"sweep_radians",0}},
        {{"start",{.5,1}},{"end",{.5,.5}},{"sweep_radians",0}}})});
    entity.extensions["vendor"]={{"retain","named-plan-corner"}};
    entity.required=true;
    return entity;
}
void exercise(double direction_z, bool depth_slice=false) {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1450,1000); window.show();
    const auto area=measured_rectangle();
    const auto initial=decode_identified_boundary_entity(area);
    const auto vertex=initial.segments[0].end_vertex_id;
    const auto start=initial.segments[0].segment.end;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(area)}, {}, "named-plan measured corner fixture"});
    const auto wall=window.createStraightWall(start,{6.6576,0});
    require(!wall.isEmpty(),"named-plan joined wall fixture");
    require(window.selectEntity(wall),"named-plan hosted door fixture selection");
    const auto door=depth_slice
        ? window.createHostedOpening("window","0.4 m","0.8 m","0.8 m","1.2 m")
        : window.createHostedOpening("door","0.4 m","0.8 m","0 m","2 m",
            std::nullopt,DoorOperation{});
    require(!door.isEmpty(),"named-plan hosted door fixture");
    const auto dimension=window.createLengthDimension(QString::fromStdString(area.id),
        QString::fromStdString(initial.segments[0].segment_id),{1.8,-0.5});
    require(!dimension.isEmpty(),"named-plan dependent dimension fixture");
    PersistentConstraint relation{"named-vertex-coincidence",ConstraintRelationKind::coincident,
        {{area.id,WallEndpointRole::end,initial.segments[0].segment_id,vertex},
         {wall.toStdString(),WallEndpointRole::start}}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(encode_constraint_entity(relation))}, {}, "named-plan corner relationship"});
    auto sheet=window.document().snapshot().entities().at("sheet-view-1");
    const auto model=decode_sheet_view_entity(sheet);
    auto views=model.views();
    auto view=std::find_if(views.begin(),views.end(),[](const auto& v) { return v.id=="view-plan"; });
    require(view!=views.end(),"named-plan fixture view");
    view->origin_m={10,-4,direction_z<0 ? 10.0 : -10.0};
    view->direction={0,0,direction_z};
    if (depth_slice) {
        // Far depth is a one-sided clipping plane. View downward above the
        // window head, or upward below its sill, while retaining the wall.
        view->origin_m[2]=direction_z<0 ? 2.3 : 0.0;
        view->presentation.far_depth_m=direction_z<0 ? .1 : .4;
        view->presentation.cut_depth_m=.05;
    }
    const auto angle=std::numbers::pi/6;
    view->up={-std::sin(angle),std::cos(angle),0};
    const auto frame=*view;
    sheet.properties["model"]=SheetViewModel::create(views,model.sheets(),model.schedule_ids(),model.sheet_order()).to_json();
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(sheet)}, {}, "named-plan corner frame"});
    window.setWorkspace(Workspace::architectural);
    require(window.selectEntity(QString::fromStdString(area.id)),"named-plan area must select");
    auto* choices=window.findChild<QComboBox*>("architecturalView");
    require(choices,"named-plan view picker");
    const auto index=choices->findData(QStringLiteral("view-plan"),Qt::UserRole+1);
    require(index>=3,"named-plan persisted choice");
    choices->setCurrentIndex(index);
    require(window.selectEntity(QString::fromStdString(area.id)),"named-plan selection must refresh");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    require(canvas,"named-plan canvas");
    canvas->setSnapEnabled(false); canvas->setOverviewMapEnabled(false); canvas->fitView(); canvas->zoomBy(.8);
    QApplication::processEvents();
    const auto retained=std::find_if(canvas->entities().begin(),canvas->entities().end(),
        [&](const auto& value) { return value.id.toStdString()==area.id; });
    require(retained!=canvas->entities().end() && retained->vertex_handles.size()==initial.segments.size(),
            "named horizontal plan must retain editable measurement corner handles");
    require(retained->holes.empty(),"preserved vendor hole metadata must not become authoritative measurement geometry");
    const auto handle=std::find_if(retained->vertex_handles.begin(),retained->vertex_handles.end(),
        [&](const auto& value) { return value.id.toStdString()==vertex; });
    require(handle!=retained->vertex_handles.end() && close_enough(handle->position,project(start,frame)),
            "named-plan corner handle must retain identity and projected location");
    const auto has_door=std::any_of(canvas->entities().begin(),canvas->entities().end(),
        [&](const auto& value) { return value.id==door; });
    require(has_door!=depth_slice,"view depth must distinguish the hosted assembly from the retained wall");
    const auto label_for_area=[&]() {
        const auto found=std::find_if(canvas->labels().begin(),canvas->labels().end(),
            [&](const auto& value){return value.id.toStdString()==area.id && value.avoid_components;});
        require(found!=canvas->labels().end(),"named plan retains derived area text");
        return *found;
    };
    const Vec2 first_label{8,3};
    require(window.setSelectedPlanLabelPosition(first_label) &&
                close_enough(label_for_area().position,project(first_label,frame)) &&
                label_for_area().leader_start.has_value(),
            "saved placement and leader must project through rotated/reflected plan axes");
    const Vec2 clicked_label{7,4};
    require(window.beginSelectedPlanLabelPlacement(),"named plan supports one-click label placement");
    mouse(*canvas,QEvent::MouseButtonPress,project(clicked_label,frame));
    mouse(*canvas,QEvent::MouseButtonRelease,project(clicked_label,frame));
    require(close_enough(label_for_area().position,project(clicked_label,frame)) &&
                window.document().snapshot().entities().at(area.id)==area,
            "named-plan placement click must map to world coordinates without changing owner geometry");
    canvas->setSnapEnabled(false); // Placement refresh restores the workspace's snapping preference.
    const auto source=window.document().snapshot();
    const Vec2 target{4.5,0.4};
    mouse(*canvas,QEvent::MouseButtonPress,project(start,frame));
    mouse(*canvas,QEvent::MouseMove,project(target,frame));
    wait_preview(window);
    const auto preview=canvas->boundaryVertexPreviewEntities();
    const auto labels=canvas->boundaryVertexPreviewLabels();
    const auto metrics=canvas->boundaryVertexPreviewMetrics();
    require(std::any_of(labels.begin(),labels.end(),[&](const auto& value) {
                return value.id.toStdString()==area.id; }) &&
            std::any_of(labels.begin(),labels.end(),[&](const auto& value) { return value.id==dimension; }),
            "named-plan preview must explicitly retain area and dimension labels");
    require(window.document().snapshot().entities()==source.entities() && metrics,
            "named-plan preview must be detached and retain full model metrics");
    const auto boundary_preview=std::find_if(preview.begin(),preview.end(),
        [&](const auto& value) { return value.id.toStdString()==area.id; });
    require(boundary_preview!=preview.end(),"named-plan preview must include measured boundary");
    if (!depth_slice) require(std::any_of(preview.begin(),preview.end(),
        [&](const auto& value) { return value.id==door && !value.segments.empty(); }),
        "named-plan live preview must retain the moved hosted door assembly");
    const auto moved_handle=std::find_if(boundary_preview->vertex_handles.begin(),boundary_preview->vertex_handles.end(),
        [&](const auto& value) { return value.id.toStdString()==vertex; });
    require(moved_handle!=boundary_preview->vertex_handles.end() && close_enough(moved_handle->position,project(target,frame)),
            "named-plan preview handle must follow the actual view target");
    const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture.isEmpty()) require(QDir().mkpath(capture) &&
        canvas->grab().save(QDir(capture).filePath(QStringLiteral("named-corner-%1-%2.png")
            .arg(direction_z<0 ? "down" : "up").arg(depth_slice ? "slice" : "full"))),
        "named-plan live preview screenshot");
    mouse(*canvas,QEvent::MouseButtonRelease,project(target,frame));
    if (!window.lastError().isEmpty()) throw std::runtime_error(window.lastError().toStdString());
    const auto after=window.document().snapshot();
    const auto changed=decode_identified_boundary_entity(after.entities().at(area.id));
    require(after.revision()==source.revision()+1 && close_enough(changed.segments[0].segment.end,target) &&
                close_enough(after.entities().at(wall.toStdString()).properties.at("baseline").at("start")[0].get<double>(),target.x) &&
                close_enough(after.entities().at(wall.toStdString()).properties.at("baseline").at("start")[1].get<double>(),target.y),
            "named-plan release must inverse-map and move the joined corner and wall once");
    require(changed.segments[0].segment_id==initial.segments[0].segment_id &&
                changed.segments[0].end_vertex_id==vertex &&
                after.entities().at(area.id).extensions.at("vendor")==area.extensions.at("vendor") &&
                after.entities().at(area.id).properties.at("holes")==area.properties.at("holes") &&
                !after.entities().at(area.id).properties.contains("boundary_authoring") &&
                after.entities().at(area.id).extensions.at("boundary_geometry_derivation")
                    .at("source_boundary_authoring")==area.properties.at("boundary_authoring"),
            "named-plan corner edit must preserve original receipts, metadata and stable identity");
    require(same_path(boundary_preview->segments,project_path(boundary_geometry(changed),frame)) &&
                close_enough(metrics->area_square_metres,std::abs(signed_area(boundary_geometry(changed)))) &&
                close_enough(metrics->perimeter_metres,perimeter(boundary_geometry(changed))),
            "named-plan boundary preview must match model commit and analytical totals");
    for (const auto& projected : preview) {
        const auto committed=std::find_if(canvas->entities().begin(),canvas->entities().end(),
            [&](const auto& value) { return value.id==projected.id; });
        require(committed!=canvas->entities().end() && same_path(projected.segments,committed->segments),
                "named-plan related geometry and dimensions must match after commit");
    }
    for (const auto& label : labels) {
        const auto committed=std::find_if(canvas->labels().begin(),canvas->labels().end(),
            [&](const auto& value) { return value.id==label.id; });
        require(committed!=canvas->labels().end() && label.text==committed->text && close_enough(label.position,committed->position),
                "named-plan generated labels must match after commit");
    }
    require(window.undoCommand() && window.document().snapshot().entities()==source.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==after.entities(),
            "named-plan edit must undo/redo exact related states");
    QTemporaryDir directory;
    MainWindow reopened;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("named-corner.bldproj")) &&
                reopened.openProject(directory.filePath("named-corner.bldproj")) &&
                reopened.document().snapshot().entities()==after.entities(),
            "named-plan corner proof and saved view must replay after reopen");

    require(window.undoCommand() && window.selectEntity(QString::fromStdString(area.id)),
            "named-plan cancelled gesture must restore its source");
    canvas->setSnapEnabled(false);
    const auto cancelled=window.document().snapshot();
    mouse(*canvas,QEvent::MouseButtonPress,project(start,frame));
    mouse(*canvas,QEvent::MouseMove,project(target,frame));
    wait_preview(window);
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(canvas,&escape);
    mouse(*canvas,QEvent::MouseButtonRelease,project(target,frame));
    require(window.document().snapshot().entities()==cancelled.entities() &&
                window.document().revision()==cancelled.revision() && canvas->boundaryVertexPreviewEntities().empty(),
            "Escape must cancel a named-plan corner preview and release without a commit");

    // Replace the scene while the worker has a live request, then deliver the
    // old release. A completed worker cannot repopulate the new view or commit.
    mouse(*canvas,QEvent::MouseButtonPress,project(start,frame));
    mouse(*canvas,QEvent::MouseMove,project(target,frame));
    auto* poll=window.findChild<QTimer*>("boundaryVertexPreviewPoll");
    require(poll && poll->isActive(),"stale-view test must enqueue a live preview");
    choices->setCurrentIndex(0);
    wait_preview(window,false);
    mouse(*canvas,QEvent::MouseButtonRelease,project(target,frame));
    require(window.document().snapshot().entities()==cancelled.entities() &&
                window.document().revision()==cancelled.revision() && canvas->boundaryVertexPreviewEntities().empty(),
            "changing plan view must discard late preview and stale release");

    if (!depth_slice) {
        auto cropped_sheet=window.document().snapshot().entities().at("sheet-view-1");
        const auto cropped_model=decode_sheet_view_entity(cropped_sheet);
        auto cropped_views=cropped_model.views();
        const auto crop_bounds=boundary_bounds(project_path(boundary_geometry(initial),frame));
        const Bounds2 crop{{crop_bounds.minimum.x-.05,crop_bounds.minimum.y-.1},
                           {crop_bounds.maximum.x+.05,crop_bounds.maximum.y+.1}};
        auto cropped_view=std::find_if(cropped_views.begin(),cropped_views.end(),
            [](const auto& value) { return value.id=="view-plan"; });
        cropped_view->presentation.crop=ViewCrop{crop.minimum.x,crop.maximum.x,crop.minimum.y,crop.maximum.y};
        cropped_sheet.properties["model"]=SheetViewModel::create(cropped_views,cropped_model.sheets(),
            cropped_model.schedule_ids(),cropped_model.sheet_order()).to_json();
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(cropped_sheet)}, {}, "named-plan corner crop fixture"});
        require(window.selectEntity(QString::fromStdString(area.id)),"cropped named area must select");
        choices->setCurrentIndex(choices->findData(QStringLiteral("view-plan"),Qt::UserRole+1));
        canvas->setSnapEnabled(false); canvas->fitView(); canvas->zoomBy(.8);
        const auto contained=std::find_if(canvas->entities().begin(),canvas->entities().end(),
            [&](const auto& value) { return value.id.toStdString()==area.id; });
        require(contained!=canvas->entities().end() && contained->vertex_handles.size()==initial.segments.size(),
                "a contained named-plan measurement must retain projected corner handles");
        const auto crop_source=window.document().snapshot();
        const Vec2 outside_target{5.2,.4};
        const auto screen_target=project(outside_target,frame);
        require(screen_target.x<crop.minimum.x || screen_target.x>crop.maximum.x,
                "cropped gesture fixture must move outside the view crop");
        mouse(*canvas,QEvent::MouseButtonPress,project(start,frame));
        mouse(*canvas,QEvent::MouseMove,screen_target);
        wait_preview(window);
        const auto cropped_preview=canvas->boundaryVertexPreviewEntities();
        const auto cropped_metrics=canvas->boundaryVertexPreviewMetrics();
        const auto area_preview=std::find_if(cropped_preview.begin(),cropped_preview.end(),
            [&](const auto& value) { return value.id.toStdString()==area.id; });
        require(area_preview!=cropped_preview.end() && !area_preview->filled && cropped_metrics,
                "named cropped preview must retain clipped strokes and full-model metrics");
        for (const auto& edge : area_preview->segments)
            for (const auto& point : {edge.start,edge.end})
                require(point.x>=crop.minimum.x-1e-7 && point.x<=crop.maximum.x+1e-7 &&
                            point.y>=crop.minimum.y-1e-7 && point.y<=crop.maximum.y+1e-7,
                        "named vertex preview must clip in view coordinates");
        require(window.document().snapshot().entities()==crop_source.entities(),
                "cropping a live named-plan preview must leave full model geometry unchanged");
        mouse(*canvas,QEvent::MouseButtonRelease,screen_target);
        if (!window.lastError().isEmpty()) throw std::runtime_error(window.lastError().toStdString());
        const auto crop_after=window.document().snapshot();
        const auto full_geometry=decode_identified_boundary_entity(crop_after.entities().at(area.id));
        require(crop_after.revision()==crop_source.revision()+1 &&
                    close_enough(full_geometry.segments[0].segment.end,outside_target) &&
                    close_enough(cropped_metrics->area_square_metres,std::abs(signed_area(boundary_geometry(full_geometry)))),
                "named cropped release must retain the unclipped authoritative corner and area");
        const auto clipped=std::find_if(canvas->entities().begin(),canvas->entities().end(),
            [&](const auto& value) { return value.id.toStdString()==area.id; });
        require(clipped!=canvas->entities().end() && clipped->vertex_handles.empty() &&
                    same_path(clipped->segments,area_preview->segments),
                "clipped named outlines must match preview and suppress misleading corner handles");
        require(window.undoCommand() && window.document().snapshot().entities()==crop_source.entities(),
                "named-plan cropped edit must undo full geometry and relationships");
    }
}
void exercise_curve(double direction_z) {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1450,1000); window.show();
    const auto area=window.createBoundary(
        {{{0,0},{4,0},std::numbers::pi},{{4,0},{0,0},0}},"patio");
    require(!area.isEmpty(),"curved named-plan area fixture");
    const auto source_geometry=decode_identified_boundary_entity(
        window.document().snapshot().entities().at(area.toStdString()));
    const auto wall=window.createCurvedWall({4,0},{8,0},"45 deg");
    require(!wall.isEmpty(),"curved named-plan connected wall fixture");
    const PersistentConstraint relation{"curved-named-join",ConstraintRelationKind::coincident,
        {{area.toStdString(),WallEndpointRole::end,source_geometry.segments.front().segment_id,
            source_geometry.segments.front().end_vertex_id},
         {wall.toStdString(),WallEndpointRole::start}}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(encode_constraint_entity(relation))}, {}, "join curved plan owners"});
    const auto dimension=window.createLengthDimension(area,
        QString::fromStdString(source_geometry.segments.front().segment_id),{2,.5});
    require(!dimension.isEmpty(),"curved named-plan dimension fixture");
    auto sheet=window.document().snapshot().entities().at("sheet-view-1");
    const auto model=decode_sheet_view_entity(sheet);
    auto views=model.views();
    auto view=std::find_if(views.begin(),views.end(),[](const auto& value) { return value.id=="view-plan"; });
    require(view!=views.end(),"curved named-plan view fixture");
    view->origin_m={-8,6,direction_z<0 ? 10.0 : -10.0};
    view->direction={0,0,direction_z};
    view->up={-.6,.8,0};
    const auto frame=*view;
    sheet.properties["model"]=SheetViewModel::create(views,model.sheets(),model.schedule_ids(),model.sheet_order()).to_json();
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(sheet)}, {}, "curved named-plan frame"});
    window.setWorkspace(Workspace::architectural);
    require(window.selectEntity(area),"curved named area selection");
    auto* choices=window.findChild<QComboBox*>("architecturalView");
    require(choices,"curved named-plan view picker");
    choices->setCurrentIndex(choices->findData(QStringLiteral("view-plan"),Qt::UserRole+1));
    require(window.selectEntity(area),"curved named area refresh");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    require(canvas,"curved named-plan canvas");
    canvas->setSnapEnabled(false); canvas->setOverviewMapEnabled(false); canvas->fitView(); canvas->zoomBy(.8);
    const Vec2 target{5,.5};
    const auto source=window.document().snapshot();
    mouse(*canvas,QEvent::MouseButtonPress,project({4,0},frame));
    mouse(*canvas,QEvent::MouseMove,project(target,frame));
    wait_preview(window);
    const auto previews=canvas->boundaryVertexPreviewEntities();
    const auto metrics=canvas->boundaryVertexPreviewMetrics();
    const auto preview=std::find_if(previews.begin(),previews.end(),
        [&](const auto& value) { return value.id==area; });
    require(preview!=previews.end() && metrics && window.document().snapshot().entities()==source.entities(),
            "curved named preview must be analytical and detached");
    require(std::any_of(previews.begin(),previews.end(),[&](const auto& value) {
        return value.id==wall && std::any_of(value.segments.begin(),value.segments.end(),
            [](const auto& segment) { return segment.sweep_radians!=0.0; });
    }),"connected curved wall must appear as true arcs in the live named-plan preview");
    mouse(*canvas,QEvent::MouseButtonRelease,project(target,frame));
    if (!window.lastError().isEmpty()) throw std::runtime_error(window.lastError().toStdString());
    const auto after=window.document().snapshot();
    const auto changed=decode_identified_boundary_entity(after.entities().at(area.toStdString()));
    require(after.revision()==source.revision()+1 && close_enough(changed.segments.front().segment.end,target) &&
                close_enough(changed.segments.front().segment.sweep_radians,std::numbers::pi) &&
                same_path(preview->segments,project_path(boundary_geometry(changed),frame)) &&
                close_enough(metrics->area_square_metres,std::abs(signed_area(boundary_geometry(changed)))),
            "curved named edit must preserve model arc sweep and reflect only its projected handedness");
    const auto& moved_wall=after.entities().at(wall.toStdString()).properties.at("baseline");
    require(close_enough(moved_wall.at("start")[0].get<double>(),target.x) &&
        close_enough(moved_wall.at("start")[1].get<double>(),target.y) &&
        moved_wall.at("sweep_radians")==source.entities().at(wall.toStdString()).properties.at("baseline").at("sweep_radians"),
        "named-plan corner movement must propagate to the related wall without flattening its arc");
    require(window.undoCommand() && window.document().snapshot().entities()==source.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==after.entities(),
            "curved named edit must undo and redo exactly");
    QTemporaryDir directory;
    MainWindow reopened;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("curved-named-corner.bldproj")) &&
                reopened.openProject(directory.filePath("curved-named-corner.bldproj")) &&
                reopened.document().snapshot().entities()==after.entities(),
            "curved named edit and its proof must replay after reopening");
}
void exercise_revealed_door(double direction_z, bool excluded=false) {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1450,1000); window.show();
    const auto area=measured_rectangle();
    const auto geometry=decode_identified_boundary_entity(area);
    const auto start=geometry.segments.front().segment.end;
    const auto vertex=geometry.segments.front().end_vertex_id;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(area)}, {}, "cropped door corner fixture"});
    const auto wall=window.createStraightWall(start,{6.6576,0});
    require(!wall.isEmpty() && window.selectEntity(wall),"cropped door host fixture");
    const auto door=window.createHostedOpening("door","0.4 m","0.8 m","0 m","2 m",
        std::nullopt,DoorOperation{});
    require(!door.isEmpty(),"cropped door assembly fixture");
    const PersistentConstraint relation{"cropped-door-coincidence",ConstraintRelationKind::coincident,
        {{area.id,WallEndpointRole::end,geometry.segments.front().segment_id,vertex},
         {wall.toStdString(),WallEndpointRole::start}}};
    auto sheet=window.document().snapshot().entities().at("sheet-view-1");
    const auto model=decode_sheet_view_entity(sheet);
    auto views=model.views();
    auto view=std::find_if(views.begin(),views.end(),[](const auto& value) { return value.id=="view-plan"; });
    require(view!=views.end(),"cropped door named view");
    view->origin_m={10,-4,direction_z<0 ? 10.0 : -10.0};
    view->direction={0,0,direction_z}; view->up={0,1,0};
    if (excluded) {
        view->object_ids={area.id};
        view->restrict_to_objects=true;
    }
    const auto bounds=boundary_bounds(project_path(boundary_geometry(geometry),*view));
    view->presentation.crop=ViewCrop{bounds.minimum.x-.15,bounds.maximum.x+.15,
                                   bounds.minimum.y-.5,bounds.maximum.y+.1};
    const auto frame=*view;
    sheet.properties["model"]=SheetViewModel::create(views,model.sheets(),model.schedule_ids(),model.sheet_order()).to_json();
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(sheet),EntityChange::upsert(encode_constraint_entity(relation))}, {},
        "cropped door view and relationship"});
    window.setWorkspace(Workspace::architectural);
    require(window.selectEntity(QString::fromStdString(area.id)),"cropped door area selection");
    auto* choices=window.findChild<QComboBox*>("architecturalView");
    require(choices,"cropped door view picker");
    choices->setCurrentIndex(choices->findData(QStringLiteral("view-plan"),Qt::UserRole+1));
    require(window.selectEntity(QString::fromStdString(area.id)),"cropped door area refresh");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    require(canvas,"cropped door canvas");
    canvas->setSnapEnabled(false); canvas->setGridEnabled(false);
    canvas->setOverviewMapEnabled(false); canvas->fitView(); canvas->zoomBy(.8);
    require(std::none_of(canvas->entities().begin(),canvas->entities().end(),
        [&](const auto& value) { return value.id==door; }),"door must begin wholly outside the crop");
    const auto output=[&] {
        QImage image(canvas->size(),QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        canvas->renderSceneAt(painter,QRectF(canvas->rect()),canvas->viewScale(),canvas->viewCenter(),Qt::white);
        painter.end(); return image;
    };
    const auto source=window.document().snapshot();
    const auto original_output=output();
    const Vec2 target{2.8,.4};
    mouse(*canvas,QEvent::MouseButtonPress,project(start,frame));
    mouse(*canvas,QEvent::MouseMove,project(target,frame));
    wait_preview(window);
    const auto preview=canvas->boundaryVertexPreviewEntities();
    const auto labels=canvas->boundaryVertexPreviewLabels();
    const auto metrics=canvas->boundaryVertexPreviewMetrics();
    require(metrics && std::any_of(labels.begin(),labels.end(),[&](const auto& value) {
        return value.id.toStdString()==area.id; }),"cropped live preview must retain metrics and area label");
    if (excluded) {
        require(std::none_of(preview.begin(),preview.end(),[&](const auto& value) {
            return value.id==wall || value.id==door; }),
            "model dependencies excluded by the saved view must not leak into preview");
    } else {
    require(std::any_of(preview.begin(),preview.end(),[&](const auto& value) {
        return value.id==door && !value.segments.empty(); }),
        "a door moving into the crop must appear in the exact live preview");
    require(window.document().snapshot().entities()==source.entities() && output()==original_output,
        "newly visible preview geometry must not change the document or export");
    auto without_door=preview;
    std::erase_if(without_door,[&](const auto& value) { return value.id==door; });
    // Probe the same paint path with a separate canvas. Start real gestures
    // through the public callback; do not bypass the deferred lifecycle guard.
    PlanCanvas painted;
    painted.setAttribute(Qt::WA_DontShowOnScreen,true);
    painted.resize(canvas->size()); painted.show();
    painted.setEntities(canvas->entities());
    painted.setSnapEnabled(false); painted.setGridEnabled(false);
    painted.setOverviewMapEnabled(false); painted.fitView(); painted.zoomBy(.8);
    painted.setBoundaryVertexMoveRequested([](QString,QString,Vec2,std::uint64_t) { return true; });
    bool include_door=true;
    painted.setBoundaryVertexPreviewRequested([&](QString,QString,Vec2,std::uint64_t) {
        return std::optional{include_door ? preview : without_door};
    });
    mouse(painted,QEvent::MouseButtonPress,project(start,frame));
    mouse(painted,QEvent::MouseMove,project(target,frame));
    const auto complete=painted.grab().toImage();
    include_door=false;
    mouse(painted,QEvent::MouseMove,project(target,frame));
    require(painted.grab().toImage()!=complete,"newly visible door must actually paint on the interactive canvas");
    }
    mouse(*canvas,QEvent::MouseButtonRelease,project(target,frame));
    if (!window.lastError().isEmpty()) throw std::runtime_error(window.lastError().toStdString());
    const auto after=window.document().snapshot();
    require(after.revision()==source.revision()+1,"cropped door gesture must commit once");
    for (const auto& id : {QString::fromStdString(area.id),wall,door}) {
        const auto proposed=std::find_if(preview.begin(),preview.end(),[&](const auto& value) { return value.id==id; });
        const auto committed=std::find_if(canvas->entities().begin(),canvas->entities().end(),
            [&](const auto& value) { return value.id==id; });
        if (excluded && id!=QString::fromStdString(area.id)) {
            require(proposed==preview.end() && committed==canvas->entities().end(),
                "saved view exclusions must agree before and after the joined edit");
            continue;
        }
        require(proposed!=preview.end() && committed!=canvas->entities().end() &&
            same_path(proposed->segments,committed->segments),"affected visible geometry must match preview in both directions");
    }
    require(window.undoCommand() && window.document().snapshot().entities()==source.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==after.entities(),
        "newly visible door edit must undo and redo exactly");
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc,argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    try {
        exercise(-1); exercise(1); exercise(-1,true); exercise(1,true);
        exercise_curve(-1); exercise_curve(1);
        exercise_revealed_door(-1); exercise_revealed_door(1);
        exercise_revealed_door(-1,true); exercise_revealed_door(1,true);
        std::cout << "named_plan_vertex_desktop_tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "named_plan_vertex_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
